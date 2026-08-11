#include "include/SATEngine/Primitives.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <mockturtle/algorithms/equivalence_checking.hpp>
#include <mockturtle/algorithms/miter.hpp>
#include <mockturtle/algorithms/simulation.hpp>
#include <mockturtle/algorithms/cut_enumeration.hpp>
#include <mockturtle/views/topo_view.hpp>
#include <mockturtle/views/window_view.hpp>
#include <kitty/kitty.hpp>
#include "include/SATEngine/SatEngine.h"
#include "include/SATEngine/Fraig.h"
#include "include/SATEngine/SatTime.h"
#include "include/core/Netlist.h"

namespace eqeng {

namespace {

using SteadyClock = std::chrono::steady_clock;

double remaining_seconds(const SteadyClock::time_point& deadline) {
    return std::chrono::duration<double>(deadline - SteadyClock::now()).count();
}

// 無時限查詢用的「實質無限」預算。走同一條 CaDiCaL 路徑，
// 避免無時限與 timed 版本用不同 solver 而給出不一致的 Unknown 邊界。
constexpr double kNoTimeLimit = 1e9;

} // namespace

// ============================================================
//  呼叫樣板（本檔所有 public method 一律遵守）
//
//    ensure_fresh();                  ① 先同步 AIG（可能 rebuild）
//    if (strict && !can_prove()) ...   ② 全域健康（預設關閉）
//    tainted_any / require_trusted     ③ per-signal 污染
//    const Sig a = unwrap(ra);         ④ 再檢查 generation
//    ...                               ⑤ 實作只碰 raw Sig
//    return stamp(result, tainted);    ⑥ 回傳時蓋 generation + 傳播污染
//
//  順序不可顛倒：unwrap 需要「當前」generation，而 ensure_fresh 可能改變它。
//
//  三態 API（*_checked / *_under）遇到污染回 Unknown；
//  bool 與建構類 API 一律 require_trusted → 丟 UnsoundModel。
//    絕不可讓污染走 UnknownPolicy 折疊：AsEqual 會把「不可信」變成 true，
//    那正是把 model-health gate 繞過去的漏洞。
//
//  私有方法（equiv_via_miter / equiv_via_cadical / build_cofactor /
//  in_structural_cone / truth_of_raw）一律吃 raw Sig、不做任何檢查 ——
//  在單次呼叫中途觸發 rebuild 會讓手上的 raw Sig 全部失效。
// ============================================================

// ============================================================
//  污染閘門
// ============================================================

bool Primitives::tainted_any(std::initializer_list<SigRef> rs) {
    for (const auto& r : rs) {
        if (r.tainted()) {
            if (cfg_.count_stats) ++stats_.tainted_rejected;
            return true;
        }
    }
    return false;
}

void Primitives::require_trusted(std::initializer_list<SigRef> rs) {
    if (!tainted_any(rs)) return;
    throw UnsoundModel(model_ ? model_->taint_summary()
                              : std::string("model not built"));
}

// ============================================================
//  等價 / 常數
// ============================================================

// Phase A 的等價判斷（legacy 路徑，僅在 CaDiCaL 不可用時保留）。
//
// tmp 本身就是一顆合法的 miter：單一 PO = XOR(a, b)，
// equivalence_checking 的語意正是「這顆網路的 PO 是否恆為 0」。
// 舊版另外建 zero 網路再呼叫 miter<Ntk>() 會多做一次完整網路複製 ——
// 在大 cone 上那個複製的成本會蓋過 solve 本身。
EquivResult Primitives::equiv_via_miter(Sig a, Sig b) {
    ++stats_.miters_built;
    Ntk& A = aig();

    Ntk tmp;
    std::unordered_map<uint64_t, Sig> nodeMap;   // 主 AIG node index -> tmp signal

    std::vector<Node> order;
    std::unordered_set<uint64_t> seen;

    // 迭代式 DFS post-order（100 萬 gate 不能遞迴，會爆 stack）。
    {
        std::vector<std::pair<Node, bool>> stk;
        stk.emplace_back(A.get_node(a), false);
        stk.emplace_back(A.get_node(b), false);

        while (!stk.empty()) {
            const Node n        = stk.back().first;
            const bool expanded = stk.back().second;
            const uint64_t idx  = A.node_to_index(n);

            if (expanded) {
                stk.pop_back();
                if (seen.insert(idx).second) order.push_back(n);
                continue;
            }
            if (seen.count(idx)) { stk.pop_back(); continue; }

            stk.back().second = true;
            if (!A.is_constant(n) && !A.is_pi(n)) {
                A.foreach_fanin(n, [&](auto f) {
                    const Node fn = A.get_node(f);
                    if (!seen.count(A.node_to_index(fn)))
                        stk.emplace_back(fn, false);
                });
            }
        }
    }

    nodeMap[A.node_to_index(A.get_node(A.get_constant(false)))] = tmp.get_constant(false);

    auto lift = [&](Sig s) -> Sig {
        const uint64_t idx = A.node_to_index(A.get_node(s));
        auto it = nodeMap.find(idx);
        assert(it != nodeMap.end() && "fanin visited before its user");
        return it->second ^ A.is_complemented(s);
    };

    for (const Node n : order) {
        const uint64_t idx = A.node_to_index(n);
        if (A.is_constant(n)) continue;
        if (A.is_pi(n)) { nodeMap[idx] = tmp.create_pi(); continue; }

        std::vector<Sig> fins;
        A.foreach_fanin(n, [&](auto f) { fins.push_back(lift(f)); });
        assert(fins.size() == 2);
        nodeMap[idx] = tmp.create_and(fins[0], fins[1]);
    }

    tmp.create_po(tmp.create_xor(lift(a), lift(b)));   // 這就是 miter

    mockturtle::equivalence_checking_stats st;
    const auto res = mockturtle::equivalence_checking(tmp, {}, &st);

    if (!res) return EquivResult::Unknown;            // 打到內部 conflict limit
    return *res ? EquivResult::Equal : EquivResult::NotEqual;
}

// Deadline-aware Phase A proof：union cone 直接 encode 進 CaDiCaL，
// 前處理與 solve 共用同一份 wall-clock 預算。
// 沒有網路複製 —— 這是無時限與 timed 查詢共用的主路徑。
EquivResult Primitives::equiv_via_cadical(
    Sig a, Sig b, double time_limit_seconds) {
    lastProofTimedOut_ = false;
    if (time_limit_seconds <= 0.0) {
        lastProofTimedOut_ = true;
        return EquivResult::Unknown;
    }

    const auto deadline = SteadyClock::now() +
        std::chrono::duration_cast<SteadyClock::duration>(
            std::chrono::duration<double>(time_limit_seconds));
    auto expired = [&]() {
        if (SteadyClock::now() < deadline) return false;
        lastProofTimedOut_ = true;
        return true;
    };

    Ntk& network = aig();

    // CNF 變數編號 = node index + 1，因此假設常數節點的 index 為 0。
    // mockturtle 的 aig_network 保證如此；若哪天不成立，下面的
    // solver.add(-1) 會把錯誤的變數釘成 false，是靜默的災難。
    assert(network.node_to_index(network.get_node(network.get_constant(false))) == 0
           && "constant node must be index 0");

    std::vector<Node> stack;
    stack.reserve(1024);
    stack.push_back(network.get_node(a));
    stack.push_back(network.get_node(b));

    std::vector<Node> coneNodes;
    std::unordered_set<uint64_t> seen;
    while (!stack.empty()) {
        if (expired()) return EquivResult::Unknown;
        const Node node = stack.back();
        stack.pop_back();
        const uint64_t index = network.node_to_index(node);
        if (!seen.insert(index).second) continue;
        coneNodes.push_back(node);
        if (network.is_constant(node) || network.is_pi(node)) continue;
        network.foreach_fanin(node, [&](Sig fanin) {
            stack.push_back(network.get_node(fanin));
        });
    }

    CaDiCaL::Solver solver;
    solver.set("factor", 0);

    auto variableForNode = [&](Node node) -> int {
        const uint64_t index = network.node_to_index(node);
        if (index >= static_cast<uint64_t>(std::numeric_limits<int>::max()))
            return 0;
        return static_cast<int>(index) + 1;
    };
    auto literalForSignal = [&](Sig signal) -> int {
        const int variable = variableForNode(network.get_node(signal));
        if (variable == 0) return 0;
        return network.is_complemented(signal) ? -variable : variable;
    };

    // 變數 1 對應常數節點，釘成 false；其補數即為 true。
    solver.add(-1);
    solver.add(0);

    size_t encodedCount = 0;
    for (Node node : coneNodes) {
        if ((encodedCount++ & 0xffu) == 0u && expired())
            return EquivResult::Unknown;
        if (network.is_constant(node) || network.is_pi(node)) continue;

        std::vector<Sig> fanins;
        fanins.reserve(2);
        network.foreach_fanin(node, [&](Sig fanin) { fanins.push_back(fanin); });
        if (fanins.size() != 2) return EquivResult::Unknown;

        const int output = variableForNode(node);
        const int inputA = literalForSignal(fanins[0]);
        const int inputB = literalForSignal(fanins[1]);
        if (output == 0 || inputA == 0 || inputB == 0)
            return EquivResult::Unknown;

        // output <-> (inputA & inputB)：每個 AND 節點固定 3 clause
        solver.add(-inputA); solver.add(-inputB); solver.add(output); solver.add(0);
        solver.add(inputA);  solver.add(-output); solver.add(0);
        solver.add(inputB);  solver.add(-output); solver.add(0);
    }

    const int literalA = literalForSignal(a);
    const int literalB = literalForSignal(b);
    if (literalA == 0 || literalB == 0) return EquivResult::Unknown;

    // 問「a XOR b 是否可滿足」：SAT = 有反例 = 不等價
    solver.add(literalA);  solver.add(literalB);  solver.add(0);
    solver.add(-literalA); solver.add(-literalB); solver.add(0);

    const double solveBudget = remaining_seconds(deadline);
    if (solveBudget <= 0.0) {
        lastProofTimedOut_ = true;
        return EquivResult::Unknown;
    }

    TimeLimitTerminator terminator(solveBudget);
    solver.connect_terminator(&terminator);
    const int solverResult = solver.solve();
    solver.disconnect_terminator();

    // solver 返回後再檢查一次：terminator 的 polling 有間隔，
    // 極短時限可能在下一次 polling 之前就已越過。
    if (terminator.wasTerminated() || expired()) {
        lastProofTimedOut_ = true;
        return EquivResult::Unknown;
    }
    if (solverResult == 20) return EquivResult::Equal;
    if (solverResult == 10) return EquivResult::NotEqual;
    return EquivResult::Unknown;
}

// 無時限版：轉呼叫 timed 版並給實質無限的預算。
// 兩條路徑合一，避免同一個問題在兩個 solver 上給出不同的 Unknown 邊界。
EquivResult Primitives::equiv_checked(SigRef ra, SigRef rb) {
    return equiv_checked(ra, rb, kNoTimeLimit);
}

EquivResult Primitives::equiv_checked(
    SigRef ra, SigRef rb, double time_limit_seconds) {
    lastProofTimedOut_ = false;
    const auto startedAt = SteadyClock::now();

    ensure_fresh();
    if (cfg_.strict_global_health && !model_can_prove())
        return EquivResult::Unknown;
    // per-signal 污染：三態 API 回 Unknown，不走 UnknownPolicy。
    if (tainted_any({ra, rb})) return EquivResult::Unknown;

    // rebuild 也吃預算：lazy 第一次建 AIG 可能就用掉大半時間。
    const double remaining = time_limit_seconds -
        std::chrono::duration<double>(SteadyClock::now() - startedAt).count();
    if (remaining <= 0.0) {
        lastProofTimedOut_ = true;
        return EquivResult::Unknown;
    }

    const Sig a = unwrap(ra);
    const Sig b = unwrap(rb);
    Ntk& A = aig();

    if (cfg_.count_stats) ++stats_.equiv_calls;

    // ---- 0. 零成本短路 ----
    // signal 完全相同（strash / BUF alias / 重複查詢）。
    if (a == b) {
        if (cfg_.count_stats) ++stats_.equiv_trivial;
        return EquivResult::Equal;
    }
    // 同 node 但 phase 相反 → 必定不等價。
    if (A.get_node(a) == A.get_node(b)) {
        if (cfg_.count_stats) ++stats_.equiv_trivial;
        return EquivResult::NotEqual;
    }

    // ---- 1. Phase B：FRAIG 查表 ----
    // 前置條件：兩者都必須是 sweep 當下就存在的節點。
    // cofactor 物化出來的新節點在 watermark 之後沒被 sweep 過，
    // 查表會恆回「不同類」→ 靜默的錯誤答案，必須退回 SAT。
    if (fraig_ != nullptr && fraig_->is_swept()) {
        const Node na = A.get_node(a);
        const Node nb = A.get_node(b);
        if (fraig_->is_swept_node(na) && fraig_->is_swept_node(nb)) {
            if (cfg_.count_stats) ++stats_.equiv_by_lookup;
            return fraig_->same_class(a, b) ? EquivResult::Equal
                                            : EquivResult::NotEqual;
        }
    }

    // ---- 2. SAT ----
    EquivResult r;
    if (sat_ != nullptr) {
        r = sat_->are_equal(a, b);          // Phase B（尚未支援 deadline）
    } else {
        if (cfg_.count_stats) ++stats_.miters_built;
        r = equiv_via_cadical(a, b, remaining);
    }

    if (cfg_.count_stats) {
        ++stats_.equiv_by_sat;
        if (r == EquivResult::Unknown) ++stats_.equiv_unknown;
    }
    return r;
}

EquivResult Primitives::is_const_checked(SigRef ra, bool val) {
    return is_const_checked(ra, val, kNoTimeLimit);
}

EquivResult Primitives::is_const_checked(
    SigRef ra, bool val, double time_limit_seconds) {
    lastProofTimedOut_ = false;
    const auto startedAt = SteadyClock::now();

    ensure_fresh();
    if (cfg_.strict_global_health && !model_can_prove())
        return EquivResult::Unknown;
    if (tainted_any({ra})) return EquivResult::Unknown;

    const double remaining = time_limit_seconds -
        std::chrono::duration<double>(SteadyClock::now() - startedAt).count();
    if (remaining <= 0.0) {
        lastProofTimedOut_ = true;
        return EquivResult::Unknown;
    }

    const Sig signal = unwrap(ra);
    Ntk& network = aig();
    const Node node = network.get_node(signal);

    if (cfg_.count_stats) ++stats_.equiv_calls;

    // 已經是常數節點：直接判定。
    if (network.is_constant(node)) {
        if (cfg_.count_stats) ++stats_.equiv_trivial;
        const bool actual = network.is_complemented(signal);   // const0 + compl = 1
        return (actual == val) ? EquivResult::Equal : EquivResult::NotEqual;
    }

    if (fraig_ != nullptr && fraig_->is_swept() && fraig_->is_swept_node(node)) {
        bool known = false;
        if (fraig_->is_known_const(signal, known)) {
            if (cfg_.count_stats) ++stats_.equiv_by_lookup;
            return (known == val) ? EquivResult::Equal : EquivResult::NotEqual;
        }
    }

    EquivResult result;
    if (sat_ != nullptr) {
        result = sat_->is_const(signal, val);
    } else {
        if (cfg_.count_stats) ++stats_.miters_built;
        result = equiv_via_cadical(signal, network.get_constant(val), remaining);
    }
    if (cfg_.count_stats) {
        ++stats_.equiv_by_sat;
        if (result == EquivResult::Unknown) ++stats_.equiv_unknown;
    }
    return result;
}

bool Primitives::resolve_policy(EquivResult r) {
    switch (r) {
        case EquivResult::Equal:    return true;
        case EquivResult::NotEqual: return false;
        default:
            // Unknown = solver 資源上限。這裡「只」折疊 solver 的不確定性；
            // 模型不可信（污染）在更上層就已經丟 UnsoundModel，不會走到這裡。
            switch (cfg_.unknown_policy) {
                case UnknownPolicy::AsEqual: return true;
                case UnknownPolicy::Throw:   throw EngineUnknown{};
                default:                     return false;
            }
    }
}

bool Primitives::equiv(SigRef a, SigRef b) {
    ensure_fresh();
    require_usable_model();
    require_trusted({a, b});          // 污染必須在 resolve_policy 之前擋掉
    return resolve_policy(equiv_checked(a, b));
}

bool Primitives::is_const(SigRef a, bool val) {
    ensure_fresh();
    require_usable_model();
    require_trusted({a});
    return resolve_policy(is_const_checked(a, val));
}

// ============================================================
//  Cofactor
// ============================================================

// 把 var 設為 val，在同一顆 AIG 內物化並 strash。
// 只重建「var 的 transitive fanout ∩ f 的 fanin cone」，其餘節點原封不動共用。
Sig Primitives::build_cofactor(Sig f, Sig var, bool val) {
    Ntk& A = aig();

    const Node vnode = A.get_node(var);
    const Sig  vsub  = A.get_constant(A.is_complemented(var) ? !val : val);

    if (A.get_node(f) == vnode)                  // f 就是 var 本身
        return vsub ^ A.is_complemented(f);

    std::unordered_map<uint64_t, Sig> sub;       // node index -> 替換後的 signal
    sub[A.node_to_index(vnode)] = vsub;

    std::vector<Node> order;
    std::unordered_set<uint64_t> seen;
    {
        std::vector<std::pair<Node, bool>> stk;
        stk.emplace_back(A.get_node(f), false);
        while (!stk.empty()) {
            const Node n        = stk.back().first;
            const bool expanded = stk.back().second;
            const uint64_t idx  = A.node_to_index(n);
            if (expanded) {
                stk.pop_back();
                if (seen.insert(idx).second) order.push_back(n);
                continue;
            }
            if (seen.count(idx)) { stk.pop_back(); continue; }
            stk.back().second = true;
            if (A.is_constant(n) || A.is_pi(n)) continue;
            A.foreach_fanin(n, [&](auto fi) {
                const Node fn = A.get_node(fi);
                if (!seen.count(A.node_to_index(fn)))
                    stk.emplace_back(fn, false);
            });
        }
    }

    auto mapped = [&](Sig s) -> Sig {
        auto it = sub.find(A.node_to_index(A.get_node(s)));
        if (it == sub.end()) return s;                       // 不受影響，直接共用
        return it->second ^ A.is_complemented(s);
    };

    for (const Node n : order) {
        const uint64_t idx = A.node_to_index(n);
        if (idx == A.node_to_index(vnode)) continue;
        if (A.is_constant(n) || A.is_pi(n)) continue;

        std::vector<Sig> fins;
        bool changed = false;
        A.foreach_fanin(n, [&](auto fi) {
            const Sig m = mapped(fi);
            if (!(m == fi)) changed = true;
            fins.push_back(m);
        });
        if (!changed) continue;                              // 整個子樹與 var 無關

        // create_and 內建 strash + 常數摺疊：var 一旦是常數，上游會自然塌縮。
        const Sig ns = A.create_and(fins[0], fins[1]);
        if (!(ns == A.make_signal(n))) {
            sub[idx] = ns;
            if (cfg_.count_stats) ++stats_.cofactors_built;
        }
    }

    return mapped(f);
}

SigRef Primitives::cofactor(SigRef rf, SigRef rvar, bool val) {
    ensure_fresh();
    require_usable_model();
    require_trusted({rf, rvar});
    const Sig f   = unwrap(rf);
    const Sig var = unwrap(rvar);

    // 前置條件：var 必須是自由變數（PI，含 DFF-Q 的 pseudo-PI）。
    // 對內部節點取 cofactor 在布林語意上沒有良好定義，直接擋掉，
    // 避免高階 API 誤用後得到看似合理但錯誤的結果。
    if (!model_->is_free_var(var)) {
        std::cerr << "[Primitives] cofactor: 'var' must be a PI "
                     "(free variable). Got an internal node.\n";
        throw std::invalid_argument("Primitives::cofactor: var is not a free variable");
    }

    // 污染傳播：兩個引數都乾淨（require_trusted 已保證），結果也乾淨。
    const CofactorKey key{f.data, var.data, val};
    auto it = cofactorCache_.find(key);
    if (it != cofactorCache_.end()) return stamp(it->second, false);

    const Sig r = build_cofactor(f, var, val);
    cofactorCache_.emplace(key, r);

    // AIG 可能長大了，讓 SAT 端有機會延展 node->var 表。
    if (sat_ != nullptr) sat_->sync();
    return stamp(r, false);
}

EquivResult Primitives::equiv_under(
        SigRef ra, SigRef rb,
        const std::vector<std::pair<SigRef, bool>>& assumptions) {
    if (assumptions.empty()) return equiv_checked(ra, rb);

    ensure_fresh();
    if (cfg_.strict_global_health && !model_can_prove())
        return EquivResult::Unknown;
    if (tainted_any({ra, rb})) return EquivResult::Unknown;
    for (const auto& kv : assumptions)
        if (tainted_any({kv.first})) return EquivResult::Unknown;

    const Sig a = unwrap(ra);
    const Sig b = unwrap(rb);

    if (sat_ != nullptr) {
        std::vector<std::pair<Sig, bool>> raw;
        raw.reserve(assumptions.size());
        for (const auto& kv : assumptions)
            raw.emplace_back(unwrap(kv.first), kv.second);
        return sat_->are_equal_under(a, b, raw);
    }

    // Phase A：沒有 assumption 機制，只能真的物化 cofactor 後再比。
    //   這代表 Phase A 的 equiv_under 與明寫 cofactor 成本相同，並非零成本；
    //   語意仍正確，Phase B 接上 incremental CaDiCaL 後才會變便宜。
    SigRef ca = ra, cb = rb;
    for (const auto& kv : assumptions) {
        ca = cofactor(ca, kv.first, kv.second);
        cb = cofactor(cb, kv.first, kv.second);
    }
    return equiv_checked(ca, cb);
}

EquivResult Primitives::is_const_under(
        SigRef ra, bool val,
        const std::vector<std::pair<SigRef, bool>>& assumptions) {
    ensure_fresh();
    if (cfg_.strict_global_health && !model_can_prove())
        return EquivResult::Unknown;
    if (tainted_any({ra})) return EquivResult::Unknown;
    return equiv_under(ra, constant(val), assumptions);
}

// ============================================================
//  功能性 support / unate / symmetry
// ============================================================

// f 是否「功能上」相依 var：!equiv(f|var=0, f|var=1)。
// 回傳語意刻意與 equiv 一致：
//   Equal    = 兩個 cofactor 相同 = f 不相依 var
//   NotEqual = f 相依 var
//   Unknown  = 不知道（絕不可當成任一邊）
EquivResult Primitives::depends_on_checked(SigRef rf, SigRef rvar) {
    ensure_fresh();
    if (cfg_.strict_global_health && !model_can_prove())
        return EquivResult::Unknown;
    if (tainted_any({rf, rvar})) return EquivResult::Unknown;

    const Sig f   = unwrap(rf);
    const Sig var = unwrap(rvar);
    Ntk& A = aig();

    if (!model_->is_free_var(var)) {
        std::cerr << "[Primitives] depends_on: 'var' must be a PI (free variable)\n";
        throw std::invalid_argument("Primitives::depends_on: var is not a free variable");
    }

    // 零成本預過濾：var 根本不在 f 的 structural fanin cone 裡 → 必定不相依。
    // structural support 是 functional support 的超集合，所以「不在」是可靠的否定。
    // 這是本函式唯一真正免費的路徑；一旦 var 在 cone 內，下面一定會物化兩個 cofactor。
    if (!in_structural_cone(f, A.get_node(var)))
        return EquivResult::Equal;

    // f 就是 var 本身（或其反相）→ 必定相依。
    if (A.get_node(f) == A.get_node(var))
        return EquivResult::NotEqual;

    // 注意：這裡不能用 are_equal_under(f, f, {var=0}) 之類的寫法，
    // 因為我們要比的是「同一個 f 在 var=0 與 var=1 兩種賦值下」的結果，
    // 那是兩個不同的世界，單一 solver call 的 assumption 表達不了。
    // 正確做法就是物化兩個 cofactor 再比 —— Phase A/B 皆然。
    const SigRef f0 = cofactor(rf, rvar, false);
    const SigRef f1 = cofactor(rf, rvar, true);
    return equiv_checked(f0, f1);
}

bool Primitives::depends_on(SigRef f, SigRef var) {
    ensure_fresh();
    require_usable_model();
    require_trusted({f, var});

    const auto r = depends_on_checked(f, var);
    switch (r) {
        case EquivResult::NotEqual: return true;    // cofactor 不同 → 相依
        case EquivResult::Equal:    return false;
        default:
            // Unknown：保守回 true（寧可多列一個 support，也不要漏掉真正的相依）。
            //   這與 equiv 的保守方向相反 —— 在 support 分析裡「漏報」才是危險的那一邊：
            //   漏掉一個相依會讓 enable/hold 偵測整個失效，而且不容易察覺。
            switch (cfg_.unknown_policy) {
                case UnknownPolicy::Throw: throw EngineUnknown{};
                default:                   return true;
            }
    }
}

// 判斷 node 是否落在 f 的 structural fanin cone 內（迭代 DFS + 快取）。
bool Primitives::in_structural_cone(Sig f, Node target) {
    Ntk& A = aig();
    const uint64_t rootIdx = A.node_to_index(A.get_node(f));

    // 快取整個 cone 的 node 集合：同一個 f 常被拿來對多個候選變數反覆詢問
    // （enable/hold 就是這個模式），重算 cone 會是 O(support × cone)。
    // rebuild 時 coneCache_ 必須清空（見 Session.cpp::rebuild）。
    auto it = coneCache_.find(rootIdx);
    if (it == coneCache_.end()) {
        std::unordered_set<uint64_t> cone;
        std::vector<Node> stk;
        stk.push_back(A.get_node(f));
        cone.insert(rootIdx);

        while (!stk.empty()) {
            const Node n = stk.back();
            stk.pop_back();
            if (A.is_constant(n) || A.is_pi(n)) continue;
            A.foreach_fanin(n, [&](auto fi) {
                const Node fn = A.get_node(fi);
                if (cone.insert(A.node_to_index(fn)).second)
                    stk.push_back(fn);
            });
        }
        it = coneCache_.emplace(rootIdx, std::move(cone)).first;
    }
    return it->second.count(A.node_to_index(target)) > 0;
}

std::vector<SigRef> Primitives::functional_support(
        SigRef f, const std::vector<SigRef>& candidates) {
    ensure_fresh();
    require_usable_model();
    require_trusted({f});

    std::vector<SigRef> support;
    support.reserve(candidates.size());

    for (const SigRef var : candidates) {
        if (var.tainted()) continue;                  // 靜默略過不可信的候選
        const Sig rawVar = unwrap(var);
        if (!model_->is_free_var(rawVar)) continue;   // 靜默略過非自由變數
        if (depends_on(f, var)) support.push_back(var);
    }
    return support;
}

std::vector<SigRef> Primitives::functional_support(SigRef rf) {
    ensure_fresh();
    require_usable_model();
    require_trusted({rf});
    const Sig f = unwrap(rf);
    Ntk& A = aig();

    // 候選 = f 的 structural cone 內所有 PI。
    // structural support 是 functional support 的超集合，不會漏。
    //
    //   成本警告：這裡會對每個候選各跑一次 depends_on，
    //   也就是最多 2 × |cone 內 PI 數| 個物化 cofactor。
    //   寬 cone 上這是整組 API 最重的一個呼叫。
    std::vector<SigRef> candidates;
    std::unordered_set<uint64_t> seen;
    std::vector<Node> stk;

    stk.push_back(A.get_node(f));
    seen.insert(A.node_to_index(A.get_node(f)));

    while (!stk.empty()) {
        const Node n = stk.back();
        stk.pop_back();
        if (A.is_constant(n)) continue;
        if (A.is_pi(n)) {
            // PI 是自由變數，本身永遠可信（污染只會出現在有 driver 的 net 上）。
            candidates.push_back(stamp(A.make_signal(n), false));
            continue;
        }
        A.foreach_fanin(n, [&](auto fi) {
            const Node fn = A.get_node(fi);
            if (seen.insert(A.node_to_index(fn)).second) stk.push_back(fn);
        });
    }
    return functional_support(rf, candidates);
}

// var 對 f 是否單調：
//   positive unate : f|var=0  ≤  f|var=1   ⇔  (f|var=0 ∧ ¬f|var=1) 恆為 0
//   negative unate : f|var=1  ≤  f|var=0
bool Primitives::is_unate(SigRef f, SigRef var, bool positive) {
    ensure_fresh();
    require_usable_model();
    require_trusted({f, var});

    const SigRef f0 = cofactor(f, var, false);
    const SigRef f1 = cofactor(f, var, true);

    // 建一顆「違反單調」的偵測節點，問它是否恆為 0。
    const SigRef violation = positive ? make_and(f0, !f1)
                                      : make_and(f1, !f0);
    if (sat_ != nullptr) sat_->sync();

    return is_const(violation, false);
}

// f 是否對 x、y 對稱：f|x=0,y=1 == f|x=1,y=0
bool Primitives::is_symmetric(SigRef f, SigRef x, SigRef y) {
    ensure_fresh();
    require_usable_model();
    require_trusted({f, x, y});
    Ntk& A = aig();

    if (A.get_node(unwrap(x)) == A.get_node(unwrap(y)))
        return true;                                  // 同一個變數，恆真

    const SigRef f01 = cofactor(cofactor(f, x, false), y, true);
    const SigRef f10 = cofactor(cofactor(f, x, true),  y, false);
    return equiv(f01, f10);
}

// ============================================================
//  結構辨識（cut + NPN）
// ============================================================

// 精度界線：
//   小 cut（support ≤ 6）→ 建完整真值表，飛快
//   寬 cone（support 幾十）→ 千萬別建（2^support 會爆）→ 走 cofactor + equiv
// truth_of 會強制檢查這條界線。

std::vector<Cut> Primitives::enumerate_cuts(SigRef rroot, int k) {
    ensure_fresh();
    require_usable_model();
    require_trusted({rroot});
    const Sig root = unwrap(rroot);
    Ntk& A = aig();

    if (k <= 0) k = cfg_.default_cut_size;
    if (k > 6) {
        std::cerr << "[Primitives] cut size clamped to 6 "
                     "(truth table must fit in uint64)\n";
        k = 6;
    }
    if (cfg_.count_stats) ++stats_.cut_enumerations;

    std::vector<Cut> out;
    const Node rootNode = A.get_node(root);
    if (A.is_constant(rootNode) || A.is_pi(rootNode)) return out;

    //   Phase A 效能痛點：mockturtle 的 cut_enumeration 是對「整顆網路」跑的，
    //   每次呼叫都重算一遍。大電路上不要把這個函式放進迴圈。
    //   Phase B 應改成快取整份結果，或用 window_view 只跑局部。
    mockturtle::cut_enumeration_params ps;
    ps.cut_size  = static_cast<uint32_t>(k);
    ps.cut_limit = 12;

    const auto cuts = mockturtle::cut_enumeration(A, ps);

    for (const auto& cut : cuts.cuts(A.node_to_index(rootNode))) {
        Cut c;
        c.root = rootNode;
        c.leaves.reserve(cut->size());
        for (const auto leafIdx : *cut)
            c.leaves.push_back(A.index_to_node(leafIdx));

        // 略過 trivial cut（自己當自己的 leaf）
        if (c.leaves.size() == 1 && c.leaves[0] == rootNode) continue;

        // 蓋上 generation：Cut 裡的 Node 同樣依附於這一版 AIG。
        c.generation = generation_;
        // root 已通過 require_trusted，故必為 false；顯式寫出以保持欄位語意一致。
        c.tainted    = rroot.tainted();
        out.push_back(std::move(c));
    }
    return out;
}

// Cut 的 generation 檢查。與 unwrap 同樣的理由：
// rebuild 之後 node index 指向完全不同的節點，用舊 Cut 算真值表
// 不會報錯，只會靜默算出一個看似合理的錯誤函數。
void Primitives::check_cut(const Cut& cut) {
    if (cut.tainted) {
        if (cfg_.count_stats) ++stats_.tainted_rejected;
        throw UnsoundModel(model_ ? model_->taint_summary()
                                  : std::string("model not built"));
    }
    if (cut.generation == generation_) return;

    if (cfg_.count_stats) ++stats_.stale_rejected;
    switch (cfg_.stale_sig_policy) {
        case StaleSigPolicy::Ignore:
            return;
        case StaleSigPolicy::RebuildAndWarn:
            std::cerr << "[Primitives][WARN] stale Cut (gen " << cut.generation
                      << " vs current " << generation_
                      << ") -- results are meaningless; re-enumerate cuts\n";
            return;
        default:
            std::cerr << "[Primitives] stale Cut: generation " << cut.generation
                      << ", current " << generation_
                      << ". Re-run enumerate_cuts() after any netlist edit.\n";
            throw StaleSignal{};
    }
}

// 私有：不做 generation / 污染檢查，只算節點函數。
TruthTable Primitives::truth_of_raw(const Cut& cut) {
    Ntk& A = aig();
    const std::size_t n = cut.leaves.size();

    if (n > 6) {
        std::cerr << "[Primitives] truth_of: cut has " << n
                  << " leaves (> 6). Refusing to build a 2^" << n << " table.\n";
        throw std::invalid_argument("Primitives::truth_of: cut too wide");
    }

    // 對 cut 內部做直接模擬：每個 leaf 指派一條 base truth table，
    // 往 root 方向逐節點求值。
    std::unordered_map<uint64_t, TruthTable> tt;

    TruthTable zero(static_cast<uint32_t>(n));
    kitty::clear(zero);
    tt[A.node_to_index(A.get_node(A.get_constant(false)))] = zero;

    for (std::size_t i = 0; i < n; ++i) {
        TruthTable v(static_cast<uint32_t>(n));
        kitty::create_nth_var(v, static_cast<uint8_t>(i));
        tt[A.node_to_index(cut.leaves[i])] = v;
    }

    // 收集 root 到 leaves 之間的節點（leaves 之下不再展開），迭代 post-order。
    std::vector<Node> order;
    std::unordered_set<uint64_t> seen;
    std::unordered_set<uint64_t> isLeaf;
    for (const Node l : cut.leaves) isLeaf.insert(A.node_to_index(l));

    {
        std::vector<std::pair<Node, bool>> stk;
        stk.emplace_back(cut.root, false);
        while (!stk.empty()) {
            const Node nd       = stk.back().first;
            const bool expanded = stk.back().second;
            const uint64_t idx  = A.node_to_index(nd);

            if (expanded) {
                stk.pop_back();
                if (seen.insert(idx).second) order.push_back(nd);
                continue;
            }
            if (seen.count(idx)) { stk.pop_back(); continue; }

            stk.back().second = true;
            if (isLeaf.count(idx) || A.is_constant(nd) || A.is_pi(nd)) continue;
            A.foreach_fanin(nd, [&](auto fi) {
                const Node fn = A.get_node(fi);
                if (!seen.count(A.node_to_index(fn)))
                    stk.emplace_back(fn, false);
            });
        }
    }

    auto value_of = [&](Sig s) -> TruthTable {
        auto it = tt.find(A.node_to_index(A.get_node(s)));
        if (it == tt.end()) {
            // cut 不完整（有節點不經 leaves 就到達 PI）→ 這個 cut 無效
            throw std::runtime_error("Primitives::truth_of: incomplete cut");
        }
        return A.is_complemented(s) ? ~it->second : it->second;
    };

    for (const Node nd : order) {
        const uint64_t idx = A.node_to_index(nd);
        if (tt.count(idx)) continue;                  // leaf 或常數，已有值
        if (A.is_pi(nd))
            throw std::runtime_error("Primitives::truth_of: PI outside cut leaves");

        std::vector<TruthTable> fins;
        A.foreach_fanin(nd, [&](auto fi) { fins.push_back(value_of(fi)); });
        tt[idx] = fins[0] & fins[1];
    }

    return tt.at(A.node_to_index(cut.root));
}

// 節點函數：不含 cut.root 這條 signal 的 complement。
// 這是 mockturtle 的 cut 慣例 —— phase 不屬於 cut。
TruthTable Primitives::truth_of(const Cut& cut) {
    ensure_fresh();
    require_usable_model();
    check_cut(cut);
    return truth_of_raw(cut);
}

// signal 函數：節點函數再套上 root 的 complement。
//   要跟特定 hex 值比對時一律用這個版本：
//   AIG 裡 OR / NAND / NOR / XNOR 收尾的 signal 幾乎都帶 complement，
//   直接拿節點函數去比會系統性地差一個反相（而且只在部分閘上出錯，很難察覺）。
//   npn_matches 不受影響 —— NPN 本來就吸收輸出反相。
TruthTable Primitives::truth_of(const Cut& cut, SigRef rroot) {
    ensure_fresh();
    require_usable_model();
    require_trusted({rroot});
    check_cut(cut);
    const Sig root = unwrap(rroot);
    Ntk& A = aig();

    if (A.get_node(root) != cut.root) {
        std::cerr << "[Primitives] truth_of: root signal does not belong to this cut\n";
        throw std::invalid_argument("Primitives::truth_of: root signal / cut.root mismatch");
    }
    auto tt = truth_of_raw(cut);
    return A.is_complemented(root) ? ~tt : tt;
}

NpnClass Primitives::classify(const TruthTable& tt) {
    // 只吃 TruthTable，與 AIG 版本無關 → 不需要 ensure_fresh、也無污染可言。
    NpnClass out;
    out.num_vars = tt.num_vars();

    // kitty 的 exact NPN 在 n <= 6 是可接受的；再大要改 heuristic。
    // 回傳型別在 kitty 版本間變過，用 std::get 比 structured binding 穩定。
    const auto res   = kitty::exact_npn_canonization(tt);
    out.canonical    = std::get<0>(res);
    out.phase        = std::get<1>(res);
    const auto& perm = std::get<2>(res);
    out.perm.assign(perm.begin(), perm.end());
    return out;
}

bool Primitives::npn_matches(const Cut& cut, const TruthTable& tmpl) {
    ensure_fresh();
    require_usable_model();
    check_cut(cut);                                    // 污染 / stale 都在這裡擋掉

    if (cut.leaves.size() != tmpl.num_vars()) return false;

    TruthTable f;
    try {
        f = truth_of_raw(cut);                         // 不走 public 版本：
    } catch (const std::exception&) {                  //   否則這個 catch 會把
        return false;                                  //   StaleSignal / UnsoundModel
    }                                                  //   一起吞成「不匹配」
    const auto a = classify(f);
    const auto b = classify(tmpl);
    return a.canonical == b.canonical;
}

SigRef Primitives::cut_leaf(const Cut& cut, std::size_t i) {
    ensure_fresh();
    require_usable_model();
    check_cut(cut);

    if (i >= cut.leaves.size())
        throw std::out_of_range("Primitives::cut_leaf: index out of range");

    // cut 已通過 check_cut（未污染），leaf 繼承同樣的可信度。
    return stamp(aig().make_signal(cut.leaves[i]), cut.tainted);
}

std::vector<std::string> Primitives::cut_leaf_names(const Cut& cut) {
    ensure_fresh();
    require_usable_model();
    check_cut(cut);
    Ntk& A = aig();

    std::vector<std::string> out;
    out.reserve(cut.leaves.size());

    for (const Node l : cut.leaves) {
        auto nm = names_of(stamp(A.make_signal(l), cut.tainted));
        out.push_back(nm.empty() ? std::string("<unnamed>") : nm.front());
    }
    return out;
}

// ============================================================
//  名字橋接
// ============================================================

//   這是污染進入系統的唯一源頭：
//   從 AigModel 的 per-net taint 取得初始可信度，之後靠 SigRef 傳播。
SigRef Primitives::resolve(const std::string& net) {
    ensure_fresh();
    const Sig s  = names().resolve(net);       // 查無此 net 會丟例外
    const int id = nl_.getNetId(net);
    return stamp(s, !model_->is_net_trustworthy(id));
}

std::optional<SigRef> Primitives::try_resolve(const std::string& net) {
    ensure_fresh();
    auto s = names().try_resolve(net);
    if (!s) return std::nullopt;
    const int id = nl_.getNetId(net);
    return stamp(*s, !model_->is_net_trustworthy(id));
}

// 純查名字，不做任何證明 → 不需要污染檢查。
// 受污染的 net 仍然有正確的名字，只是它的「函數」不可信。
std::vector<std::string> Primitives::names_of(SigRef rs) {
    ensure_fresh();
    const Sig s = unwrap(rs);

    // FRAIG sweep 之後，NameMap 內的 signal 已被 canonical 化，
    // 所以查詢前也必須把 s 正規化，否則會查不到（拿舊 signal 查新表）。
    Sig key = s;
    if (fraig_ != nullptr && fraig_->is_swept() &&
        fraig_->is_swept_node(aig().get_node(s))) {
        key = fraig_->representative(s);
    }
    return names().names_of(key);
}

bool Primitives::is_trustworthy(const std::string& net) {
    ensure_fresh();
    const int id = nl_.getNetId(net);
    if (id < 0) return false;
    return model_->is_net_trustworthy(id);
}

// ============================================================
//  等價類報告
// ============================================================

std::vector<std::vector<std::string>> Primitives::equivalence_classes(int min_size) {
    ensure_fresh();
    require_usable_model();

    // Phase A：只反映 structural sharing（strash 合併、BUF/NOT alias）。
    // Phase B：Fraig::sweep + NameMap::remap 之後，同一個 API 回完整功能等價類。
    // 呼叫端不需要知道差別 —— 這正是介面凍結的意義。
    if (is_phase_a()) {
        std::cerr << "[Primitives] equivalence_classes: Phase A reports structural "
                     "sharing only (run FRAIG sweep for full functional classes)\n";
    }

    const auto     idClasses = names().equivalence_classes(min_size);
    const Netlist& nl        = model_->netlist();

    std::vector<std::vector<std::string>> out;
    out.reserve(idClasses.size());

    for (const auto& cls : idClasses) {
        std::vector<std::string> nm;
        nm.reserve(cls.size());
        for (const int netId : cls) {
            if (!nl.isValidNetId(netId)) continue;
            //   受污染的 net 不得出現在等價類報告裡：
            //   它跟代表節點指向同一個 signal，但那個 signal 是用替代值算出來的，
            //   「等價」這件事本身就不成立。靜默納入會產生假的等價宣稱。
            if (!model_->is_net_trustworthy(netId)) continue;
            nm.push_back(nl.getNet(netId).name);
        }
        // 剔除污染成員後可能不足 min_size，重新檢查
        if (static_cast<int>(nm.size()) >= min_size) out.push_back(std::move(nm));
    }
    return out;
}

} // namespace eqeng