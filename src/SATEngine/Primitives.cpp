#include "include/SATEngine/Primitives.h"
#include <algorithm>
#include <cassert>
#include <iostream>
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
#include "include/core/Netlist.h"

namespace eqeng {

namespace {

// (f, var, val) 的快取鍵。f/var 都是 signal（含 phase），val 一位。
inline uint64_t cofactor_key(Sig f, Sig var, bool val) {
    // signal.data 實際只用到低 ~33 bits，這樣拼不會撞。
    return (f.data * 1000003ull) ^ (var.data * 31ull) ^ (val ? 1ull : 0ull);
}

} // namespace

// ============================================================
//  呼叫樣板（本檔所有 public method 一律遵守）
//
//    ensure_fresh();              ① 先確保 AIG 與 netlist 同步（可能 rebuild）
//    const Sig a = unwrap(ra);    ② 再檢查 generation
//    ... 原本的實作只碰 raw Sig ...
//    return stamp(result);        ③ 回傳 SigRef 時蓋上當前 generation
//
//  順序不可顛倒：unwrap 需要知道「當前」generation，而 ensure_fresh 可能改變它。
//  私有方法（equiv_via_miter / build_cofactor / in_structural_cone /
//  truth_of_raw）一律吃 raw Sig、不做任何檢查 —— 在單次呼叫中途觸發 rebuild
//  會讓手上的 raw Sig 全部失效，那是比 stale SigRef 更難查的錯誤。
// ============================================================

// ============================================================
//  等價 / 常數
// ============================================================

// Phase A 的等價判斷：對「a XOR b」建一顆單輸出 miter，問它是否恆為 0。
//
// 為什麼不直接用 mockturtle::miter<Ntk>(ntk1, ntk2)：
//   那個 API 比的是「兩顆完整網路的所有 PO」，而我們要比的是
//   「同一顆 AIG 內的任意兩條 signal」。所以自己組一顆小網路：
//   把 a、b 的 fanin cone 抽出來，以 XOR 收尾，再問 equivalence_checking
//   它是否等價於常數 0。
//
// 慢，但正確且極簡 —— 這正是 stub 該有的樣子。Phase B 會整條換掉。
EquivResult Primitives::equiv_via_miter(Sig a, Sig b) {
    ++stats_.miters_built;
    Ntk& A = aig();

    // 在一顆「臨時網路」裡重建 a、b 的 cone，避免污染主 AIG。
    Ntk tmp;
    std::unordered_map<uint64_t, Sig> nodeMap;   // 主 AIG node index -> tmp signal

    // 收集 a、b 聯集的 fanin cone（含常數與 PI）。
    std::vector<Node> order;
    std::unordered_set<uint64_t> seen;

    // 迭代式 DFS post-order（100 萬 gate 不能遞迴，會爆 stack）。
    {
        std::vector<std::pair<Node, bool>> stk;   // (node, children_expanded)
        stk.emplace_back(A.get_node(a), false);
        stk.emplace_back(A.get_node(b), false);

        while (!stk.empty()) {
            const Node n         = stk.back().first;
            const bool expanded  = stk.back().second;
            const uint64_t idx   = A.node_to_index(n);

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

    // 依序在 tmp 裡重建。
    nodeMap[A.node_to_index(A.get_node(A.get_constant(false)))] = tmp.get_constant(false);

    auto lift = [&](Sig s) -> Sig {
        const uint64_t idx = A.node_to_index(A.get_node(s));
        auto it = nodeMap.find(idx);
        assert(it != nodeMap.end() && "fanin visited before its user");
        return it->second ^ A.is_complemented(s);
    };

    for (const Node n : order) {
        const uint64_t idx = A.node_to_index(n);
        if (A.is_constant(n)) continue;               // 已放進 nodeMap
        if (A.is_pi(n)) {
            nodeMap[idx] = tmp.create_pi();           // cone 內的 PI 都當自由變數
            continue;
        }
        std::vector<Sig> fins;
        A.foreach_fanin(n, [&](auto f) { fins.push_back(lift(f)); });
        assert(fins.size() == 2);
        nodeMap[idx] = tmp.create_and(fins[0], fins[1]);
    }

    const Sig ta = lift(a);
    const Sig tb = lift(b);
    tmp.create_po(tmp.create_xor(ta, tb));            // miter：相異則為 1

    // 與「恆 0」的參考網路比對。
    Ntk zero;
    const uint32_t npi = tmp.num_pis();
    for (uint32_t i = 0; i < npi; ++i) zero.create_pi();
    zero.create_po(zero.get_constant(false));

    const auto m = mockturtle::miter<Ntk>(tmp, zero);
    if (!m) {
        std::cerr << "[Primitives][WARN] miter construction failed\n";
        return EquivResult::Unknown;
    }

    mockturtle::equivalence_checking_stats st;
    const auto res = mockturtle::equivalence_checking(*m, {}, &st);

    if (!res) return EquivResult::Unknown;            // 打到內部 conflict limit
    return *res ? EquivResult::Equal : EquivResult::NotEqual;
}

EquivResult Primitives::equiv_checked(SigRef ra, SigRef rb) {
    ensure_fresh();
    const Sig a = unwrap(ra);
    const Sig b = unwrap(rb);
    Ntk& A = aig();

    if (cfg_.count_stats) ++stats_.equiv_calls;

    // ---- 0. 零成本：signal 完全相同（含 phase）----
    // strash / BUF alias / 同一條 net 重複查詢，大量 query 在這一步就解決。
    if (a == b) {
        if (cfg_.count_stats) ++stats_.equiv_trivial;
        return EquivResult::Equal;
    }
    // 同 node 但 phase 相反 → 必定不等價，不必送 SAT。
    if (A.get_node(a) == A.get_node(b)) {
        if (cfg_.count_stats) ++stats_.equiv_trivial;
        return EquivResult::NotEqual;
    }

    // ---- 1. Phase B：FRAIG 查表 ----
    // 前置條件：兩者都必須是 sweep 當下就存在的節點。
    // cofactor 物化出來的新節點在 watermark 之後，沒被 sweep 過，
    // 查表會恆回「不同類」→ 靜默的錯誤答案，所以必須退回 SAT。
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
        r = sat_->are_equal(a, b);
    } else {
        r = equiv_via_miter(a, b);          // Phase A
    }

    if (cfg_.count_stats) {
        ++stats_.equiv_by_sat;
        if (r == EquivResult::Unknown) ++stats_.equiv_unknown;
    }
    return r;
}

EquivResult Primitives::is_const_checked(SigRef ra, bool val) {
    ensure_fresh();
    const Sig a = unwrap(ra);
    Ntk& A = aig();

    const Node n = A.get_node(a);

    // 已經是常數節點：直接判定。
    if (A.is_constant(n)) {
        const bool actual = A.is_complemented(a);   // const0 節點 + compl = 1
        return (actual == val) ? EquivResult::Equal : EquivResult::NotEqual;
    }

    if (fraig_ != nullptr && fraig_->is_swept() && fraig_->is_swept_node(n)) {
        bool known = false;
        if (fraig_->is_known_const(a, known)) {
            if (cfg_.count_stats) ++stats_.equiv_by_lookup;
            return (known == val) ? EquivResult::Equal : EquivResult::NotEqual;
        }
    }

    if (sat_ != nullptr) {
        const auto r = sat_->is_const(a, val);
        if (cfg_.count_stats && r == EquivResult::Unknown) ++stats_.equiv_unknown;
        return r;
    }
    return equiv_via_miter(a, A.get_constant(val));   // Phase A
}

bool Primitives::resolve_policy(EquivResult r) {
    switch (r) {
        case EquivResult::Equal:    return true;
        case EquivResult::NotEqual: return false;
        default:
            // Unknown：預設保守回 false（不宣稱等價，寧可漏報也不假報）。
            switch (cfg_.unknown_policy) {
                case UnknownPolicy::AsEqual: return true;
                case UnknownPolicy::Throw:   throw EngineUnknown{};
                default:                     return false;
            }
    }
}

bool Primitives::equiv(SigRef a, SigRef b) {
    return resolve_policy(equiv_checked(a, b));
}

bool Primitives::is_const(SigRef a, bool val) {
    return resolve_policy(is_const_checked(a, val));
}

// ============================================================
//  Cofactor
// ============================================================

// 把 var 設為 val，在同一顆 AIG 內物化並 strash。
//
// 實作方式：只重建「var 的 transitive fanout ∩ f 的 fanin cone」，
// 其餘節點原封不動共用。重複呼叫同一組 (f,var,val) 會命中快取；
// 即使沒命中，strash 也會讓相同結構收斂到同一批節點，成長有界。
Sig Primitives::build_cofactor(Sig f, Sig var, bool val) {
    Ntk& A = aig();

    const Node vnode = A.get_node(var);
    const Sig  vsub  = A.get_constant(A.is_complemented(var) ? !val : val);

    // f 就是 var 本身
    if (A.get_node(f) == vnode)
        return vsub ^ A.is_complemented(f);

    std::unordered_map<uint64_t, Sig> sub;    // node index -> 替換後的 signal
    sub[A.node_to_index(vnode)] = vsub;

    // 收集 f 的 fanin cone（迭代 post-order）
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
    const Sig f   = unwrap(rf);
    const Sig var = unwrap(rvar);

    // 前置條件：var 必須是自由變數（PI，含 DFF-Q 的 pseudo-PI）。
    // 對內部節點取 cofactor 在布林語意上沒有良好定義，這裡直接擋掉，
    // 避免高階 API 誤用後得到看似合理但錯誤的結果。
    if (!model_->is_free_var(var)) {
        std::cerr << "[Primitives] cofactor: 'var' must be a PI "
                     "(free variable). Got an internal node.\n";
        throw std::invalid_argument("Primitives::cofactor: var is not a free variable");
    }

    const uint64_t key = cofactor_key(f, var, val);
    auto it = cofactorCache_.find(key);
    if (it != cofactorCache_.end()) return stamp(it->second);

    const Sig r = build_cofactor(f, var, val);
    cofactorCache_.emplace(key, r);

    // AIG 可能長大了，讓 SAT 端有機會延展 node->var 表。
    if (sat_ != nullptr) sat_->sync();
    return stamp(r);
}

EquivResult Primitives::equiv_under(
        SigRef ra, SigRef rb,
        const std::vector<std::pair<SigRef, bool>>& assumptions) {
    if (assumptions.empty()) return equiv_checked(ra, rb);

    ensure_fresh();
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
    // 慢，但語意等價 —— golden reference 的意義就在這裡。
    // 這裡走 public cofactor()：它會做 free-var 前置檢查與快取，
    // 而 ensure_fresh 此刻已經是 no-op（同一次呼叫內不會再 rebuild）。
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
    const Sig f   = unwrap(rf);
    const Sig var = unwrap(rvar);
    Ntk& A = aig();

    if (!model_->is_free_var(var)) {
        std::cerr << "[Primitives] depends_on: 'var' must be a PI (free variable)\n";
        throw std::invalid_argument("Primitives::depends_on: var is not a free variable");
    }

    // 零成本預過濾：var 根本不在 f 的 structural fanin cone 裡 → 必定不相依。
    // structural support 是 functional support 的超集合，所以「不在」是可靠的否定。
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
    const auto r = depends_on_checked(f, var);
    switch (r) {
        case EquivResult::NotEqual: return true;    // cofactor 不同 → 相依
        case EquivResult::Equal:    return false;
        default:
            // Unknown：保守回 true（寧可多列一個 support，也不要漏掉真正的相依）。
            // 這與 equiv 的保守方向相反 —— 這裡「漏報」才是危險的那一邊。
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
    //  rebuild 時 coneCache_ 必須清空（見 Session.cpp::rebuild）。
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

    std::vector<SigRef> support;
    support.reserve(candidates.size());

    for (const SigRef var : candidates) {
        const Sig raw = unwrap(var);
        if (!model_->is_free_var(raw)) continue;      // 靜默略過非自由變數
        if (depends_on(f, var)) support.push_back(var);
    }
    return support;
}

std::vector<SigRef> Primitives::functional_support(SigRef rf) {
    ensure_fresh();
    const Sig f = unwrap(rf);
    Ntk& A = aig();

    // 候選 = f 的 structural cone 內所有 PI。
    // structural support 是 functional support 的超集合，不會漏。
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
            candidates.push_back(stamp(A.make_signal(n)));
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

    // mockturtle 的 cut_enumeration 是對「整顆網路」跑的。
    // Phase A 直接照做（正確但重）；Phase B 應改成快取整份結果、
    // 或用 window_view 只跑局部 —— 100 萬 gate 上這是明確的痛點。
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
        out.push_back(std::move(c));
    }
    return out;
}

// Cut 的 generation 檢查。與 unwrap 同樣的理由：
// rebuild 之後 node index 指向完全不同的節點，用舊 Cut 算真值表
// 不會報錯，只會靜默算出一個看似合理的錯誤函數。
void Primitives::check_cut(const Cut& cut) {
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

// 私有：不做 generation 檢查，只算節點函數。
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
    // 只吃 TruthTable，與 AIG 版本無關 → 不需要 ensure_fresh。
    NpnClass out;
    out.num_vars = tt.num_vars();

    // kitty 的 exact NPN 在 n <= 6 是可接受的；再大要改 heuristic。
    const auto res   = kitty::exact_npn_canonization(tt);
    out.canonical    = std::get<0>(res);
    out.phase        = std::get<1>(res);
    const auto& perm = std::get<2>(res);
    out.perm.assign(perm.begin(), perm.end());
    return out;
}

bool Primitives::npn_matches(const Cut& cut, const TruthTable& tmpl) {
    ensure_fresh();
    check_cut(cut);

    if (cut.leaves.size() != tmpl.num_vars()) return false;

    TruthTable f;
    try {
        f = truth_of_raw(cut);
    } catch (const std::exception&) {
        return false;                                  // 無效 cut，視為不匹配
    }

    const auto a = classify(f);
    const auto b = classify(tmpl);
    return a.canonical == b.canonical;
}

SigRef Primitives::cut_leaf(const Cut& cut, std::size_t i) {
    ensure_fresh();
    check_cut(cut);

    if (i >= cut.leaves.size())
        throw std::out_of_range("Primitives::cut_leaf: index out of range");

    return stamp(aig().make_signal(cut.leaves[i]));
}

std::vector<std::string> Primitives::cut_leaf_names(const Cut& cut) {
    ensure_fresh();
    check_cut(cut);
    Ntk& A = aig();

    std::vector<std::string> out;
    out.reserve(cut.leaves.size());

    for (const Node l : cut.leaves) {
        auto nm = names_of(stamp(A.make_signal(l)));
        out.push_back(nm.empty() ? std::string("<unnamed>") : nm.front());
    }
    return out;
}

// ============================================================
//  名字橋接
// ============================================================

SigRef Primitives::resolve(const std::string& net) {
    ensure_fresh();
    return stamp(names().resolve(net));       // 查無此 net 會丟例外
}

std::optional<SigRef> Primitives::try_resolve(const std::string& net) {
    ensure_fresh();
    auto s = names().try_resolve(net);
    if (!s) return std::nullopt;
    return stamp(*s);
}

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

// ============================================================
//  等價類報告
// ============================================================

std::vector<std::vector<std::string>> Primitives::equivalence_classes(int min_size) {
    ensure_fresh();

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
            nm.push_back(nl.getNet(netId).name);
        }
        if (static_cast<int>(nm.size()) >= min_size) out.push_back(std::move(nm));
    }
    return out;
}

} // namespace eqeng   