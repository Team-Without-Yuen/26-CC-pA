// Primitives 的「會期管理」與修改前後等價驗證。
//   dirty → ensure_fresh → rebuild → generation 遞增
//   snapshot + name-based miter CEC
//
// 本檔與 Primitives.cpp 是同一個 class 的兩半：
//   Primitives.cpp  = 布林運算（只碰 raw Sig，不做任何檢查）
//   Session.cpp     = 版本管理與介面層（負責 ensure_fresh / unwrap / stamp）

#include "include/SATEngine/Primitives.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

#include <mockturtle/algorithms/equivalence_checking.hpp>

#include "include/SATEngine/SatEngine.h"
#include "include/SATEngine/Fraig.h"
#include "include/core/Netlist.h"

namespace eqeng {

namespace {

// ------------------------------------------------------------
//  比較點視圖：把一顆 AigModel 的輸入／輸出換算成「帶名字」的清單。
//  before / after 靠這些名字對齊，絕不靠索引。
// ------------------------------------------------------------
struct InterfaceView {
    std::vector<std::string> inputNames;
    std::vector<Sig>         inputSigs;
    std::vector<std::string> outputNames;
    std::vector<Sig>         outputSigs;
};

InterfaceView collect_interface(const AigModel& m) {
    InterfaceView v;
    const Ntk&     A  = m.aig();
    const Netlist& nl = m.netlist();
    const auto&    st = m.stats();
    const auto&    piNets = m.pi_net_ids();
    const auto&    dffs   = m.dffs();

    // ---- 輸入：真實 PI → DFF Q → 懸空自由 PI（AigBuilder 凍結的順序）----
    std::vector<Sig> piSigs;
    piSigs.reserve(piNets.size());
    A.foreach_pi([&](auto n) { piSigs.push_back(A.make_signal(n)); });

    const std::size_t nReal = st.num_real_pis;
    const std::size_t nDff  = st.num_dff;

    for (std::size_t i = 0; i < piSigs.size(); ++i) {
        std::string name;
        if (i >= nReal && i < nReal + nDff) {
            // DFF 的 Q：用 instance 名而非 Q net 名 ——
            // 最佳化重建 netlist 時 instance 名會保留，內部 net 名會被重新產生。
            const auto& d = dffs[i - nReal];
            name = std::string(kDffQPrefix) + nl.getGate(d.gateId).instName;
        } else if (i < piNets.size() && piNets[i] >= 0 && nl.isValidNetId(piNets[i])) {
            name = nl.getNet(piNets[i]).name;
        } else {
            name = "$PI:" + std::to_string(i);          // 理論上不會發生
        }
        v.inputNames.push_back(std::move(name));
        v.inputSigs.push_back(piSigs[i]);
    }

    // ---- 輸出：真實 PO + DFF next-state（已折進 RN/SN 的 eff_D）----
    std::unordered_set<std::string> seenOut;
    const auto& poSigs = m.po_signals();
    const auto& poNets = m.po_net_ids();

    for (std::size_t i = 0; i < poSigs.size() && i < poNets.size(); ++i) {
        if (poNets[i] < 0 || !nl.isValidNetId(poNets[i])) continue;
        const std::string& nm = nl.getNet(poNets[i]).name;
        if (!seenOut.insert(nm).second) continue;       // 同一條 net 被宣告成多個 PO
        v.outputNames.push_back(nm);
        v.outputSigs.push_back(poSigs[i]);
    }

    for (const auto& d : dffs) {
        std::string nm = std::string(kDffDPrefix) + nl.getGate(d.gateId).instName;
        if (!seenOut.insert(nm).second) continue;
        v.outputNames.push_back(std::move(nm));
        v.outputSigs.push_back(d.d_eff);
    }
    return v;
}

// ------------------------------------------------------------
//  把 src 中 roots 的 fanin cone 複製進 dst。
//  piMap：src 的 PI node index → dst signal。
//  迭代式 DFS（100 萬 gate 不能遞迴）。
// ------------------------------------------------------------
std::vector<Sig> copy_cone(const Ntk& src, const std::vector<Sig>& roots, Ntk& dst,
                           const std::unordered_map<uint64_t, Sig>& piMap,
                           bool& ok) {
    ok = true;
    std::unordered_map<uint64_t, Sig> map;
    map[src.node_to_index(src.get_node(src.get_constant(false)))] = dst.get_constant(false);

    std::vector<Node> order;
    std::unordered_set<uint64_t> seen;
    std::vector<std::pair<Node, bool>> stk;

    for (const Sig r : roots) stk.emplace_back(src.get_node(r), false);

    while (!stk.empty()) {
        const Node n        = stk.back().first;
        const bool expanded = stk.back().second;
        const uint64_t idx  = src.node_to_index(n);

        if (expanded) {
            stk.pop_back();
            if (seen.insert(idx).second) order.push_back(n);
            continue;
        }
        if (seen.count(idx)) { stk.pop_back(); continue; }

        stk.back().second = true;
        if (!src.is_constant(n) && !src.is_pi(n)) {
            src.foreach_fanin(n, [&](auto fi) {
                const Node fn = src.get_node(fi);
                if (!seen.count(src.node_to_index(fn))) stk.emplace_back(fn, false);
            });
        }
    }

    auto lift = [&](Sig s) -> Sig {
        auto it = map.find(src.node_to_index(src.get_node(s)));
        if (it == map.end()) { ok = false; return dst.get_constant(false); }
        return it->second ^ src.is_complemented(s);
    };

    for (const Node n : order) {
        const uint64_t idx = src.node_to_index(n);
        if (src.is_constant(n)) continue;
        if (src.is_pi(n)) {
            auto it = piMap.find(idx);
            if (it == piMap.end()) { ok = false; map[idx] = dst.get_constant(false); }
            else                     map[idx] = it->second;
            continue;
        }
        std::vector<Sig> fins;
        src.foreach_fanin(n, [&](auto fi) { fins.push_back(lift(fi)); });
        if (fins.size() != 2) { ok = false; continue; }
        map[idx] = dst.create_and(fins[0], fins[1]);
    }

    std::vector<Sig> out;
    out.reserve(roots.size());
    for (const Sig r : roots) out.push_back(lift(r));
    return out;
}

// 用一組 PI 賦值把整顆網路算過一遍，回傳每個 node index 的值。
// aig_network 的 node index 天生是拓撲序（create_and 要求 fanin 先存在）。
std::vector<char> simulate_all(const Ntk& ntk, const std::vector<bool>& piVals) {
    std::vector<char> vals(ntk.size(), 0);
    vals[ntk.node_to_index(ntk.get_node(ntk.get_constant(false)))] = 0;

    uint32_t i = 0;
    ntk.foreach_pi([&](auto n) {
        vals[ntk.node_to_index(n)] = (i < piVals.size() && piVals[i]) ? 1 : 0;
        ++i;
    });

    ntk.foreach_gate([&](auto n) {
        std::vector<char> fv;
        ntk.foreach_fanin(n, [&](auto fi) {
            const char base = vals[ntk.node_to_index(ntk.get_node(fi))];
            fv.push_back(ntk.is_complemented(fi) ? !base : base);
        });
        vals[ntk.node_to_index(n)] = (fv.size() == 2) ? (fv[0] && fv[1]) : 0;
    });
    return vals;
}

inline char sig_value(const Ntk& ntk, const std::vector<char>& vals, Sig s) {
    const char base = vals[ntk.node_to_index(ntk.get_node(s))];
    return ntk.is_complemented(s) ? !base : base;
}

} // namespace

// ============================================================
//  建構 / 解構 / 版本管理
// ============================================================

Primitives::Primitives(Netlist& nl, AigModel::Options bopt, Config cfg)
    : nl_(nl), bopt_(bopt), cfg_(cfg) {
    // 刻意不在這裡建 AIG：等第一個 query 進來才建（lazy）。
    // 這樣「讀檔 → 一連串修改 → 第一次分析」只會重建一次。
    if (is_phase_a() && cfg_.verbose_rebuild) {
        std::cerr << "[Primitives] Phase A: mockturtle equivalence_checking "
                     "(correct but slow; this is the golden reference)\n";
    }
}

Primitives::~Primitives() = default;

// 髒了就整顆重建。Phase A 的笨版本 —— 不做任何增量。
//
//   Phase B 的「連續修改先累積、等 query 再重建一次」不是效能優化，
//   而是讓這套機制在 100 萬 gate 下可用的前提。lazy 建構已經先做到一半：
//   只要修改是連續發生的，中間不會有任何 rebuild。
void Primitives::ensure_fresh() {
    if (model_ && !nl_.isDirty()) return;
    rebuild();
}

void Primitives::rebuild() {
    const auto t0 = std::chrono::steady_clock::now();

    // 銷毀順序與依賴相反：Fraig 依賴 SatEngine，SatEngine 依賴 AIG。
    fraig_.reset();
    sat_.reset();
    model_.reset();

    model_ = AigModel::build(nl_, bopt_);

    //   快取內容是舊 AIG 的 Sig 與 node index，必須全部清空。
    //   漏掉這一步會讓 cofactor 回傳指向舊節點的 Sig —— 不會報錯，只會算錯。
    cofactorCache_.clear();
    coneCache_.clear();

    ++generation_;                 // 所有舊 SigRef / Cut 就此失效
    nl_.clearDirty();

    if (wantPhaseB_) {
        sat_   = std::make_unique<SatEngine>(model_->aig());
        fraig_ = std::make_unique<Fraig>(model_->aig(), *sat_);
    }

    const double sec = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - t0).count();
    ++stats_.rebuilds;
    stats_.rebuild_seconds += sec;

    const auto& st = model_->stats();
    if (cfg_.verbose_rebuild) {
        std::cerr << "[Primitives] rebuild #" << stats_.rebuilds
                  << " (gen " << generation_ << "): aig=" << st.aig_size
                  << " pi=" << st.num_real_pis << " dff=" << st.num_dff
                  << " po=" << st.num_real_pos
                  << " in " << sec << "s\n";
    }
    if (!model_->can_prove()) {
        std::cerr << "[Primitives][ERROR] Boolean proof disabled: "
                  << model_->health_message() << "\n";
    }
}

bool Primitives::model_can_prove() const {
    return model_ != nullptr && model_->can_prove();
}

void Primitives::require_usable_model() const {
    if (!model_can_prove())
        throw UnsoundModel(model_ ? model_->health_message() : "model has not been built");
}

// SigRef → raw Sig，並檢查 generation。
// 呼叫端必須先 ensure_fresh()：unwrap 需要知道「當前」generation。
Sig Primitives::unwrap(SigRef s) {
    if (s.gen_ == generation_ && s.gen_ != kInvalidGeneration) return s.sig_;

    if (cfg_.count_stats) ++stats_.stale_rejected;

    switch (cfg_.stale_sig_policy) {
        case StaleSigPolicy::Ignore:
            return s.sig_;
        case StaleSigPolicy::RebuildAndWarn:
            std::cerr << "[Primitives][WARN] stale SigRef (gen " << s.gen_
                      << " vs current " << generation_
                      << ") -- the answer will be meaningless\n";
            return s.sig_;
        default:
            std::cerr << "[Primitives] stale SigRef: generation " << s.gen_
                      << ", current " << generation_
                      << ". Call resolve(name) again after any netlist edit.\n";
            throw StaleSignal{};
    }
}

void Primitives::enable_phase_b(bool on) {
    if (wantPhaseB_ == on) return;
    wantPhaseB_ = on;
    model_.reset();          // 強制下次 ensure_fresh 重建整條引擎
}

void Primitives::reset_stats() { stats_ = Stats{}; }

// ============================================================
//  狀態存取
// ============================================================

AigModel& Primitives::model() { ensure_fresh(); return *model_; }

const AigModel& Primitives::model() const {
    if (!model_) throw std::logic_error("Primitives::model() const: not built yet");
    return *model_;
}

ModelHealth Primitives::model_health() {
    ensure_fresh();
    return model_->health();
}

std::string Primitives::model_health_message() {
    ensure_fresh();
    return model_->health_message();
}

Ntk&     Primitives::aig()   { ensure_fresh(); return model_->aig(); }
NameMap& Primitives::names() { ensure_fresh(); return model_->names(); }
const Netlist& Primitives::netlist() const { return nl_; }

// ============================================================
//  常數與建構（讓高階 API 不必碰 raw Sig 也能組電路）
// ============================================================

SigRef Primitives::constant(bool val) {
    ensure_fresh();
    require_usable_model();
    return stamp(model_->aig().get_constant(val));
}

SigRef Primitives::make_and(SigRef a, SigRef b) {
    ensure_fresh();
    require_usable_model();
    const Sig r = model_->aig().create_and(unwrap(a), unwrap(b));
    if (sat_) sat_->sync();
    return stamp(r);
}

SigRef Primitives::make_or(SigRef a, SigRef b) {
    ensure_fresh();
    require_usable_model();
    const Sig r = model_->aig().create_or(unwrap(a), unwrap(b));
    if (sat_) sat_->sync();
    return stamp(r);
}

SigRef Primitives::make_xor(SigRef a, SigRef b) {
    ensure_fresh();
    require_usable_model();
    const Sig r = model_->aig().create_xor(unwrap(a), unwrap(b));
    if (sat_) sat_->sync();
    return stamp(r);
}

SigRef Primitives::make_mux(SigRef sel, SigRef onTrue, SigRef onFalse) {
    ensure_fresh();
    require_usable_model();
    Ntk& A = aig();
    const Sig s = unwrap(sel), t = unwrap(onTrue), f = unwrap(onFalse);
    const Sig r = A.create_or(A.create_and(s, t), A.create_and(!s, f));
    if (sat_) sat_->sync();
    return stamp(r);
}

bool Primitives::is_free_var(SigRef s) {
    ensure_fresh();
    require_usable_model();
    return model_->is_free_var(unwrap(s));
}

bool Primitives::is_constant(SigRef s) {
    ensure_fresh();
    require_usable_model();
    return model_->aig().is_constant(model_->aig().get_node(unwrap(s)));
}

bool Primitives::is_complemented(SigRef s) {
    ensure_fresh();
    require_usable_model();
    return model_->aig().is_complemented(unwrap(s));
}

Sig Primitives::raw(SigRef s) {
    ensure_fresh();
    require_usable_model();
    return unwrap(s);
}

SigRef Primitives::wrap(Sig s) {
    ensure_fresh();
    require_usable_model();
    return stamp(s);
}

// ============================================================
//  修改前後等價驗證
// ============================================================

std::vector<std::string> Primitives::comparison_points(bool include_dff_next_state) {
    ensure_fresh();
    const auto view = collect_interface(*model_);

    std::vector<std::string> out;
    for (const auto& nm : view.outputNames) {
        if (!include_dff_next_state &&
            nm.rfind(kDffDPrefix, 0) == 0) continue;
        out.push_back(nm);
    }
    return out;
}

// 修改前的快照。
// 不用 mockturtle 的 clone()：把所有比較點的 cone 重建進一顆全新的 Ntk，
// 天生就是深拷貝，也不受版本差異影響。
// 快照完全自足（名字以字串保存），之後的 rebuild 完全影響不到它。
AigSnapshot Primitives::snapshot() {
    ensure_fresh();
    ++stats_.snapshots_taken;

    if (!model_can_prove()) {
        AigSnapshot snap;
        snap.revision_ = nl_.revision();
        snap.generation_ = generation_;
        return snap;
    }

    const auto view = collect_interface(*model_);
    const Ntk& src  = model_->aig();

    AigSnapshot snap;
    snap.inputNames_ = view.inputNames;
    snap.outputNames_ = view.outputNames;

    // 在快照網路裡依同樣順序建 PI
    std::unordered_map<uint64_t, Sig> piMap;
    piMap.reserve(view.inputSigs.size());
    for (std::size_t i = 0; i < view.inputSigs.size(); ++i) {
        const Sig ps = snap.ntk_.create_pi();
        snap.inputSigs_.push_back(ps);
        piMap[src.node_to_index(src.get_node(view.inputSigs[i]))] = ps;
    }

    bool ok = true;
    snap.outputSigs_ = copy_cone(src, view.outputSigs, snap.ntk_, piMap, ok);
    for (const Sig o : snap.outputSigs_) snap.ntk_.create_po(o);

    snap.revision_   = nl_.revision();
    snap.generation_ = generation_;
    snap.valid_      = ok;

    if (!ok)
        std::cerr << "[Primitives][WARN] snapshot: cone copy incomplete "
                     "-- comparison results may be unreliable\n";

    if (cfg_.verbose_rebuild)
        std::cerr << "[Primitives] snapshot: " << snap.inputNames_.size()
                  << " input(s), " << snap.outputNames_.size()
                  << " comparison point(s), rev=" << snap.revision_ << "\n";

    return snap;
}

CecResult Primitives::equiv_to_snapshot(const AigSnapshot& before, CecOptions opt) {
    return run_cec(before, nullptr, false, opt);
}

CecResult Primitives::equiv_to_snapshot_except(const AigSnapshot& before,
                                               const std::vector<std::string>& exclude,
                                               CecOptions opt) {
    return run_cec(before, &exclude, true, opt);
}

CecResult Primitives::equiv_to_snapshot_only(const AigSnapshot& before,
                                             const std::vector<std::string>& only,
                                             CecOptions opt) {
    return run_cec(before, &only, false, opt);
}

// 核心：建 name-based miter 再求解。
//
//   這裡一定要建 miter，不能用 FRAIG 查表：
//   FRAIG 只在「單一 AIG 內部」找等價節點，修改前後是兩顆獨立的 AIG，
//   節點空間完全不同，representative() 查不到跨網路的關係。
CecResult Primitives::run_cec(const AigSnapshot& before,
                              const std::vector<std::string>* filter,
                              bool filterIsExclude,
                              const CecOptions& opt) {
    ensure_fresh();
    ++stats_.cec_runs;

    CecResult res;

    if (!model_can_prove()) {
        res.status = EquivResult::Unknown;
        res.message = "AIG model is not usable for proof: " + model_->health_message();
        return res;
    }

    if (!before.valid()) {
        res.status  = EquivResult::Unknown;
        res.message = "snapshot is invalid (taken before any build, or cone copy failed)";
        return res;
    }

    const auto after   = collect_interface(*model_);
    const Ntk& afterNtk = model_->aig();

    // ---- 名稱過濾 ----
    std::unordered_set<std::string> filterSet;
    if (filter) filterSet.insert(filter->begin(), filter->end());

    auto passes = [&](const std::string& nm) {
        if (!opt.include_dff_next_state && nm.rfind(kDffDPrefix, 0) == 0) return false;
        if (!filter) return true;
        const bool listed = filterSet.count(nm) > 0;
        return filterIsExclude ? !listed : listed;
    };

    // ---- 輸出：交集比較，差集回報 ----
    std::unordered_map<std::string, Sig> beforeOut, afterOut;
    for (std::size_t i = 0; i < before.outputNames_.size(); ++i)
        if (passes(before.outputNames_[i]))
            beforeOut.emplace(before.outputNames_[i], before.outputSigs_[i]);
    for (std::size_t i = 0; i < after.outputNames.size(); ++i)
        if (passes(after.outputNames[i]))
            afterOut.emplace(after.outputNames[i], after.outputSigs[i]);

    std::vector<std::string> common;
    for (const auto& kv : beforeOut) {
        if (afterOut.count(kv.first)) common.push_back(kv.first);
        else                           res.outputs_only_in_before.push_back(kv.first);
    }
    for (const auto& kv : afterOut)
        if (!beforeOut.count(kv.first)) res.outputs_only_in_after.push_back(kv.first);

    std::sort(common.begin(), common.end());
    std::sort(res.outputs_only_in_before.begin(), res.outputs_only_in_before.end());
    std::sort(res.outputs_only_in_after.begin(),  res.outputs_only_in_after.end());

    res.interface_mismatch = !res.outputs_only_in_before.empty() ||
                             !res.outputs_only_in_after.empty();
    res.compared_outputs   = common;

    if (res.interface_mismatch && opt.require_identical_outputs) {
        res.status  = EquivResult::Unknown;
        res.message = "output interface changed (" +
                      std::to_string(res.outputs_only_in_before.size()) + " removed, " +
                      std::to_string(res.outputs_only_in_after.size()) +
                      " added); set require_identical_outputs=false to compare the intersection";
        return res;
    }
    if (common.empty()) {
        res.status  = EquivResult::Unknown;
        res.message = "no comparison point selected -- nothing was verified";
        return res;
    }

    // ---- 輸入：取聯集。只出現在單邊的輸入不是錯誤（另一邊沒用到而已）----
    std::unordered_map<std::string, std::size_t> beforeIn, afterIn;
    for (std::size_t i = 0; i < before.inputNames_.size(); ++i)
        beforeIn.emplace(before.inputNames_[i], i);
    for (std::size_t i = 0; i < after.inputNames.size(); ++i)
        afterIn.emplace(after.inputNames[i], i);

    std::vector<std::string> allIn = before.inputNames_;
    for (const auto& nm : after.inputNames)
        if (!beforeIn.count(nm)) allIn.push_back(nm);

    for (const auto& kv : beforeIn)
        if (!afterIn.count(kv.first)) res.inputs_only_in_before.push_back(kv.first);
    for (const auto& kv : afterIn)
        if (!beforeIn.count(kv.first)) res.inputs_only_in_after.push_back(kv.first);
    std::sort(res.inputs_only_in_before.begin(), res.inputs_only_in_before.end());
    std::sort(res.inputs_only_in_after.begin(),  res.inputs_only_in_after.end());

    // ---- 建 miter ----
    Ntk miter;
    std::vector<std::string> miterInputNames;
    std::unordered_map<std::string, Sig> miterIn;

    for (const auto& nm : allIn) {
        const Sig p = miter.create_pi();
        miterIn.emplace(nm, p);
        miterInputNames.push_back(nm);
    }

    std::unordered_map<uint64_t, Sig> beforePiMap, afterPiMap;
    for (const auto& kv : beforeIn)
        beforePiMap[before.ntk_.node_to_index(
            before.ntk_.get_node(before.inputSigs_[kv.second]))] = miterIn[kv.first];
    for (const auto& kv : afterIn)
        afterPiMap[afterNtk.node_to_index(
            afterNtk.get_node(after.inputSigs[kv.second]))] = miterIn[kv.first];

    std::vector<Sig> beforeRoots, afterRoots;
    beforeRoots.reserve(common.size());
    afterRoots.reserve(common.size());
    for (const auto& nm : common) {
        beforeRoots.push_back(beforeOut[nm]);
        afterRoots.push_back(afterOut[nm]);
    }

    bool okB = true, okA = true;
    const auto bSigs = copy_cone(before.ntk_, beforeRoots, miter, beforePiMap, okB);
    const auto aSigs = copy_cone(afterNtk,    afterRoots,  miter, afterPiMap,  okA);

    if (!okB || !okA) {
        res.status  = EquivResult::Unknown;
        res.message = "failed to copy cones into the miter (unmapped PI?)";
        return res;
    }

    // 逐點 XOR，全部 OR 起來 → 單一 PO。miter 恆為 0 ⟺ 等價。
    std::vector<Sig> diffs;
    diffs.reserve(common.size());
    for (std::size_t i = 0; i < common.size(); ++i)
        diffs.push_back(miter.create_xor(bSigs[i], aSigs[i]));

    Sig acc = miter.get_constant(false);
    for (const Sig d : diffs) acc = miter.create_or(acc, d);
    miter.create_po(acc);

    // ---- 求解 ----
    // Phase B 時這裡應改走自家 sweep + incremental CaDiCaL：
    // 兩邊沒改到的區域 strash 一進去就免費合併，SAT 只剩修改點附近的 cone。
    mockturtle::equivalence_checking_stats st;
    const auto r = mockturtle::equivalence_checking(miter, {}, &st);

    if (!r) {
        res.status  = EquivResult::Unknown;
        res.message = "solver hit its internal limit (Phase A). "
                      "This is exactly what Phase B fixes.";
        return res;
    }

    if (*r) {
        res.status  = EquivResult::Equal;
        res.message = "equivalent on " + std::to_string(common.size()) +
                      " comparison point(s)";
        return res;
    }

    // ---- 不等價：解讀反例 ----
    res.status  = EquivResult::NotEqual;
    res.message = "NOT equivalent";

    const auto& ce = st.counter_example;
    if (!ce.empty()) {
        for (std::size_t i = 0; i < miterInputNames.size() && i < ce.size(); ++i)
            res.counterexample.emplace_back(miterInputNames[i], static_cast<bool>(ce[i]));

        // 用同一個反例做「一次」模擬，回推哪些比較點真的不同。
        // 比逐點各跑一次 SAT 便宜非常多。
        if (opt.identify_mismatches) {
            std::vector<bool> vals(ce.begin(), ce.end());
            const auto sim = simulate_all(miter, vals);
            for (std::size_t i = 0; i < common.size(); ++i)
                if (sig_value(miter, sim, diffs[i]))
                    res.mismatched_outputs.push_back(common[i]);

            if (!res.mismatched_outputs.empty())
                res.message += " at " + std::to_string(res.mismatched_outputs.size()) +
                               " point(s), first = " + res.mismatched_outputs.front();
        }
    }
    return res;
}

} // namespace eqeng
