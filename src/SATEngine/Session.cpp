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
//
//    outputTrusted 是局部化的關鍵：一顆壞閘只會讓它下游的比較點不可信，
//    其餘比較點照常比對。全域 health 只當診斷用。
// ------------------------------------------------------------
struct InterfaceView {
    std::vector<std::string> inputNames;
    std::vector<Sig>         inputSigs;
    std::vector<std::string> outputNames;
    std::vector<Sig>         outputSigs;
    std::vector<char>        outputTrusted;   // 與 outputNames 對齊：1 = 可信
};

InterfaceView collect_interface(const AigModel& m) {
    InterfaceView v;
    const Ntk&     A  = m.aig();
    const Netlist& nl = m.netlist();
    const auto&    st = m.stats();
    const auto&    piNets = m.pi_net_ids();
    const auto&    dffs   = m.dffs();

    // ---- 輸入：先全部按 net 名命名，再用 dffs() 覆寫 DFF Q ----
    //   不用 [num_real_pis, +num_dff) 切片 —— Q 有多重 driver 的 DFF
    //   不會建 pseudo-PI，切片會塌陷、名字整批錯位。
    std::vector<Sig> piSigs;
    piSigs.reserve(piNets.size());
    A.foreach_pi([&](auto n) { piSigs.push_back(A.make_signal(n)); });

    v.inputNames.resize(piSigs.size());
    v.inputSigs = piSigs;

    for (std::size_t i = 0; i < piSigs.size(); ++i) {
        if (i < piNets.size() && piNets[i] >= 0 && nl.isValidNetId(piNets[i]))
            v.inputNames[i] = nl.getNet(piNets[i]).name;
        else
            v.inputNames[i] = "$PI:" + std::to_string(i);
    }

    // DFF 的 Q 改用 instance 名 —— 最佳化重建 netlist 時 instance 名會保留，
    // 內部 net 名會被重新產生。
    for (const auto& d : dffs) {
        if (d.qPiIndex < 0) continue;                       // 非自由變數，不當比較點
        if (static_cast<std::size_t>(d.qPiIndex) >= v.inputNames.size()) continue;
        v.inputNames[d.qPiIndex] =
            std::string(kDffQPrefix) + nl.getGate(d.gateId).instName;
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
        v.outputTrusted.push_back(m.is_net_trustworthy(poNets[i]) ? 1 : 0);
    }

    for (std::size_t i = 0; i < dffs.size(); ++i) {
        const auto& d = dffs[i];
        std::string nm = std::string(kDffDPrefix) + nl.getGate(d.gateId).instName;
        if (!seenOut.insert(nm).second) continue;
        v.outputNames.push_back(std::move(nm));
        v.outputSigs.push_back(d.d_eff);
        // "$D:" 沒有對應的具名 net，可信度記在 dffTaint_ 裡。
        v.outputTrusted.push_back(m.is_dff_trustworthy(static_cast<int>(i)) ? 1 : 0);
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

Primitives::Primitives(const Netlist& nl, AigModel::Options bopt, Config cfg)
    : nl_(nl), bopt_(bopt), cfg_(cfg) {
    // 刻意不在這裡建 AIG：等第一個 query 進來才建（lazy）。
    // 這樣「讀檔 → 一連串修改 → 第一次分析」只會重建一次。
    if (is_phase_a() && cfg_.verbose_rebuild) {
        std::cerr << "[Primitives] Phase A: direct CaDiCaL encoding "
                     "(correct but non-incremental; this is the golden reference)\n";
    }
}

Primitives::~Primitives() = default;

// 髒了就整顆重建。Phase A 的笨版本 —— 不做任何增量。
//
// 以 revision 比對而非只看 dirty：若有另一個 cache 搶先呼叫了 clearDirty()，
// revision 仍會抓到不一致。這是「一個 Netlist 對應一個共用 Primitives」
// 這條約定的最後一道保險。
void Primitives::ensure_fresh() {
    if (model_ && builtRevision_ == nl_.revision()) {
        ensure_engines();          // 處理「電路沒變但引擎被拆掉」
        return;
    }
    rebuild();
}

// 不再急切建 Fraig。
// Phase B 實際上只有 incremental SatEngine 在運作。
// 改成延遲觸發,真正需要時才付 sweep 的成本。
void Primitives::rebuild() {
    const auto t0 = std::chrono::steady_clock::now();

    // 銷毀順序與依賴相反:Fraig 依賴 SatEngine,SatEngine 依賴 AIG。
    fraig_.reset();
    sat_.reset();
    model_.reset();

    model_ = AigModel::build(nl_, bopt_);

    //   快取內容是舊 AIG 的 Sig 與 node index,必須全部清空。
    //   漏掉這一步會讓 cofactor 回傳指向舊節點的 Sig —— 不會報錯,只會算錯。
    cofactorCache_.clear();
    coneCache_.clear();

    ++generation_;                 // 所有舊 SigRef / Cut 就此失效
    builtRevision_ = nl_.revision();
    nl_.clearDirty();

    // 每一版電路都要重新累積、重新 sweep。
    satEquivQueries_ = 0;
    fraigLevel_      = 0;
    fraigTriedLevel_ = 0;

    // 只建 SatEngine。Fraig 交給 ensure_fraig() 延遲處理。
    ensure_engines();

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

    if (model_->num_tainted_nets() > 0) {
        std::cerr << "[Primitives][WARN] " << model_->num_tainted_nets()
                  << " untrustworthy net(s); queries touching them will return "
                     "Unknown. " << model_->taint_summary() << "\n";
    }
}

// 全域健康。預設「不」當作閘門 —— 只有 strict_global_health 開啟時才生效。
bool Primitives::model_can_prove() const {
    return model_ != nullptr && model_->can_prove();
}

// 只有在 strict_global_health 開啟時才擋整顆電路。
// 預設模式下唯一的閘門是 per-signal 的 require_trusted()。
void Primitives::require_usable_model() {
    if (model_ == nullptr)
        throw UnsoundModel("model has not been built");
    if (cfg_.strict_global_health && !model_->can_prove())
        throw UnsoundModel(model_->health_message());
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
    fraig_.reset();
    sat_.reset();
    model_.reset();
    builtRevision_ = std::numeric_limits<uint64_t>::max();
    satEquivQueries_ = 0;      
    fraigLevel_      = 0;
    fraigTriedLevel_ = 0; 
    // generation_ 不重置:它只增不減,下次 rebuild 會 +1,舊 SigRef 正確失效。
    // 注意:這個函式會強迫整顆 AIG 重建 → 所有既有的 SigRef 立刻失效。
    // 建議在第一個 query 之前就設定好,不要在會期中途切換。
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

uint32_t Primitives::num_tainted_nets() {
    ensure_fresh();
    return model_->num_tainted_nets();
}

std::string Primitives::taint_summary() {
    ensure_fresh();
    return model_->taint_summary();
}

Ntk&     Primitives::aig()   { ensure_fresh(); return model_->aig(); }
NameMap& Primitives::names() { ensure_fresh(); return model_->names(); }
const Netlist& Primitives::netlist() const { return nl_; }

// ============================================================
//  常數與建構（讓高階 API 不必碰 raw Sig 也能組電路）
//
//    污染必須沿著建構運算傳播：make_and(乾淨, 受污染) 的結果不可信。
//    這裡若漏掉，污染鏈就斷了 —— 後續的 equiv 會拿到一個「看起來乾淨」
//    但實際上是用替代值算出來的 signal，靜默給出錯誤的確定答案。
// ============================================================

SigRef Primitives::constant(bool val) {
    ensure_fresh();
    require_usable_model();
    return stamp(model_->aig().get_constant(val), false);   // 常數永遠可信
}

SigRef Primitives::make_and(SigRef a, SigRef b) {
    ensure_fresh();
    require_usable_model();
    const bool tainted = a.tainted() || b.tainted();
    const Sig r = model_->aig().create_and(unwrap(a), unwrap(b));
    if (sat_) sat_->sync();
    return stamp(r, tainted);
}

SigRef Primitives::make_or(SigRef a, SigRef b) {
    ensure_fresh();
    require_usable_model();
    const bool tainted = a.tainted() || b.tainted();
    const Sig r = model_->aig().create_or(unwrap(a), unwrap(b));
    if (sat_) sat_->sync();
    return stamp(r, tainted);
}

SigRef Primitives::make_xor(SigRef a, SigRef b) {
    ensure_fresh();
    require_usable_model();
    const bool tainted = a.tainted() || b.tainted();
    const Sig r = model_->aig().create_xor(unwrap(a), unwrap(b));
    if (sat_) sat_->sync();
    return stamp(r, tainted);
}

SigRef Primitives::make_mux(SigRef sel, SigRef onTrue, SigRef onFalse) {
    ensure_fresh();
    require_usable_model();
    const bool tainted = sel.tainted() || onTrue.tainted() || onFalse.tainted();
    Ntk& A = aig();
    const Sig s = unwrap(sel), t = unwrap(onTrue), f = unwrap(onFalse);
    const Sig r = A.create_or(A.create_and(s, t), A.create_and(!s, f));
    if (sat_) sat_->sync();
    return stamp(r, tainted);
}

// ---------- 結構性查詢：不做布林證明，因此不需要 require_trusted ----------
// 「這個 signal 是不是 PI」與它的函數可不可信無關 ——
// 受污染的 signal 仍然有明確的結構位置。

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

// ---------- 逃生口 ----------
// raw() 保留污染資訊無從表達（Sig 沒有欄位可放），所以受污染時直接擋下：
// 拿一個不可信的 raw Sig 去直接操作 mockturtle，會完全繞過所有保護。
Sig Primitives::raw(SigRef s) {
    ensure_fresh();
    require_usable_model();
    require_trusted({s});
    return unwrap(s);
}

// wrap() 蓋上當下的 generation，並一律視為可信 ——
// 呼叫端必須保證輸入是同一次呼叫裡剛從 raw() 拿到、且未混入不可信來源的值。
SigRef Primitives::wrap(Sig s) {
    ensure_fresh();
    require_usable_model();
    return stamp(s, false);
}

// ============================================================
//  修改前後等價驗證
// ============================================================

std::vector<std::string> Primitives::comparison_points(bool include_dff_next_state) {
    ensure_fresh();
    const auto view = collect_interface(*model_);

    std::vector<std::string> out;
    for (const auto& nm : view.outputNames) {
        if (!include_dff_next_state && nm.rfind(kDffDPrefix, 0) == 0) continue;
        out.push_back(nm);
    }
    return out;
}

// 修改前的快照。
//
// 不用 mockturtle 的 clone()：把所有比較點的 cone 重建進一顆全新的 Ntk，
// 天生就是深拷貝，也不受版本差異影響。
// 快照完全自足（名字與可信度以值保存），之後的 rebuild 完全影響不到它。
//
// 局部化：不再因為全域 health == Invalid 就整份作廢。
//   壞閘只會讓它下游的比較點不可信，其餘照樣拍進快照、照樣可以比對。
//   valid_ = false 只保留給「連 cone 都複製不出來」這種真正無法使用的情況。
AigSnapshot Primitives::snapshot() {
    ensure_fresh();
    ++stats_.snapshots_taken;

    AigSnapshot snap;
    snap.revision_   = nl_.revision();
    snap.generation_ = generation_;

    if (model_ == nullptr) {
        std::cerr << "[Primitives][WARN] snapshot: model has not been built\n";
        return snap;                                   // valid_ 維持 false
    }
    if (cfg_.strict_global_health && !model_->can_prove()) {
        std::cerr << "[Primitives][WARN] snapshot: strict_global_health is on and the "
                     "model is Invalid -- snapshot marked unusable\n";
        return snap;
    }

    const auto view = collect_interface(*model_);
    const Ntk& src  = model_->aig();

    snap.inputNames_    = view.inputNames;
    snap.outputNames_   = view.outputNames;
    snap.outputTrusted_ = view.outputTrusted;

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

    snap.valid_ = ok;

    if (!ok)
        std::cerr << "[Primitives][WARN] snapshot: cone copy incomplete "
                     "-- comparison results would be unreliable\n";

    if (cfg_.verbose_rebuild) {
        std::size_t untrusted = 0;
        for (const char t : snap.outputTrusted_) if (!t) ++untrusted;
        std::cerr << "[Primitives] snapshot: " << snap.inputNames_.size()
                  << " input(s), " << snap.outputNames_.size()
                  << " comparison point(s)";
        if (untrusted > 0)
            std::cerr << " (" << untrusted << " untrustworthy)";
        std::cerr << ", rev=" << snap.revision_ << "\n";
    }

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
// 這裡一定要建 miter，不能用 FRAIG 查表：
//   FRAIG 只在「單一 AIG 內部」找等價節點，修改前後是兩顆獨立的 AIG，
//   節點空間完全不同，representative() 查不到跨網路的關係。
//
// 污染局部化：任一側不可信的比較點會被移進 untrusted_outputs 並排除，
//   其餘照常比對。絕不靜默略過 —— CecResult::ok() 會把 untrusted_outputs
//   納入判斷，否則「悄悄跳過三個點然後回 Equal」會讓使用者以為修改完全安全。
CecResult Primitives::run_cec(const AigSnapshot& before,
                              const std::vector<std::string>* filter,
                              bool filterIsExclude,
                              const CecOptions& opt) {
    ensure_fresh();
    ++stats_.cec_runs;

    CecResult res;

    if (model_ == nullptr) {
        res.status  = EquivResult::Unknown;
        res.message = "AIG model has not been built";
        return res;
    }
    if (cfg_.strict_global_health && !model_->can_prove()) {
        res.status  = EquivResult::Unknown;
        res.message = "AIG model is not usable for proof: " + model_->health_message();
        return res;
    }
    if (!before.valid()) {
        res.status  = EquivResult::Unknown;
        res.message = "snapshot is invalid (taken before any build, or cone copy failed)";
        return res;
    }

    const auto  after    = collect_interface(*model_);
    const Ntk&  afterNtk = model_->aig();

    // ---- 名稱過濾 ----
    std::unordered_set<std::string> filterSet;
    if (filter) filterSet.insert(filter->begin(), filter->end());

    auto passes = [&](const std::string& nm) {
        if (!opt.include_dff_next_state && nm.rfind(kDffDPrefix, 0) == 0) return false;
        if (!filter) return true;
        const bool listed = filterSet.count(nm) > 0;
        return filterIsExclude ? !listed : listed;
    };

    // ---- 輸出：交集比較，差集回報；同時帶出兩側的可信度 ----
    struct OutEntry { Sig sig; bool trusted; };
    std::unordered_map<std::string, OutEntry> beforeOut, afterOut;

    for (std::size_t i = 0; i < before.outputNames_.size(); ++i) {
        if (!passes(before.outputNames_[i])) continue;
        const bool trusted = i < before.outputTrusted_.size()
                           ? before.outputTrusted_[i] != 0
                           : true;                     // 舊快照沒有這欄時保守視為可信
        beforeOut.emplace(before.outputNames_[i],
                          OutEntry{before.outputSigs_[i], trusted});
    }
    for (std::size_t i = 0; i < after.outputNames.size(); ++i) {
        if (!passes(after.outputNames[i])) continue;
        const bool trusted = i < after.outputTrusted.size()
                           ? after.outputTrusted[i] != 0
                           : true;
        afterOut.emplace(after.outputNames[i],
                         OutEntry{after.outputSigs[i], trusted});
    }

    std::vector<std::string> common;
    for (const auto& kv : beforeOut) {
        if (afterOut.count(kv.first)) common.push_back(kv.first);
        else                          res.outputs_only_in_before.push_back(kv.first);
    }
    for (const auto& kv : afterOut)
        if (!beforeOut.count(kv.first)) res.outputs_only_in_after.push_back(kv.first);

    std::sort(common.begin(), common.end());
    std::sort(res.outputs_only_in_before.begin(), res.outputs_only_in_before.end());
    std::sort(res.outputs_only_in_after.begin(),  res.outputs_only_in_after.end());

    res.interface_mismatch = !res.outputs_only_in_before.empty() ||
                             !res.outputs_only_in_after.empty();

    if (res.interface_mismatch && opt.require_identical_outputs) {
        res.status  = EquivResult::Unknown;
        res.message = "output interface changed (" +
                      std::to_string(res.outputs_only_in_before.size()) + " removed, " +
                      std::to_string(res.outputs_only_in_after.size()) +
                      " added); set require_identical_outputs=false to compare the intersection";
        return res;
    }

    // ---- 剔除任一側不可信的比較點 ----
    // 受污染的比較點是用替代值算出來的，「等價」這件事本身就不成立，
    // 比了只會得到假的確定答案。明確回報，不靜默略過。
    std::vector<std::string> usable;
    usable.reserve(common.size());

    std::size_t untrustedFromBefore = 0;
    std::size_t untrustedFromAfter  = 0;

    for (const auto& nm : common) {
        const bool tb = beforeOut[nm].trusted;
        const bool ta = afterOut[nm].trusted;
        if (tb && ta) { usable.push_back(nm); continue; }
        if (!tb) ++untrustedFromBefore;
        if (!ta) ++untrustedFromAfter;
        res.untrusted_outputs.push_back(nm);
    }
    res.compared_outputs = usable;

    // 順序穩定,方便逐字比對。
    std::sort(res.untrusted_outputs.begin(), res.untrusted_outputs.end());

    // 污染來源的可讀說明,兩條訊息共用。
    auto taint_origin = [&]() {
        std::string s = "(" + std::to_string(untrustedFromBefore) +
                        " already unusable in the snapshot, " +
                        std::to_string(untrustedFromAfter) +
                        " in the current netlist). ";
        s += (untrustedFromAfter > 0)
           ? model_->taint_summary()
           : std::string("the current netlist itself is clean, so the problem predates "
                         "this modification");
        return s;
    };

    if (usable.empty()) {
        res.status = EquivResult::Unknown;
        if (!res.untrusted_outputs.empty())
            res.message = "every selected comparison point is untrustworthy (" +
                          std::to_string(res.untrusted_outputs.size()) + " point(s)) " +
                          taint_origin();
        else
            res.message = "no comparison point selected -- nothing was verified";
        return res;
    }

    // 有污染就不給確定答案(可由 CecOptions::untrusted_is_failure 關掉)。
    //   關掉的話,「三個點被跳過、其餘全部相等」會回 status = Equal ——
    //   只檢查 status 的呼叫端就會誤判「這個修改是安全的」。
    if (opt.untrusted_is_failure && !res.untrusted_outputs.empty()) {
        res.status  = EquivResult::Unknown;
        res.message = std::to_string(res.untrusted_outputs.size()) + " of " +
                      std::to_string(res.untrusted_outputs.size() + usable.size()) +
                      " comparison point(s) are untrustworthy " + taint_origin() +
                      "  First: " + res.untrusted_outputs.front() +
                      ".  Set CecOptions::untrusted_is_failure=false to compare the "
                      "clean subset anyway.";
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
    miterInputNames.reserve(allIn.size());

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
    beforeRoots.reserve(usable.size());
    afterRoots.reserve(usable.size());
    for (const auto& nm : usable) {
        beforeRoots.push_back(beforeOut[nm].sig);
        afterRoots.push_back(afterOut[nm].sig);
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
    diffs.reserve(usable.size());
    for (std::size_t i = 0; i < usable.size(); ++i)
        diffs.push_back(miter.create_xor(bSigs[i], aSigs[i]));

    Sig acc = miter.get_constant(false);
    for (const Sig d : diffs) acc = miter.create_or(acc, d);
    miter.create_po(acc);

    // ---- 求解 ----
    // copy_cone 把 before / after 兩側都建進同一顆 Ntk,而 create_and 內建
    // strash —— 兩側結構相同的區域會直接共用同一個節點,對應的 create_xor
    // 於是塌成常數 0。一個只改了三顆閘的修改,幾千個比較點裡可能有
    // 99.9% 在「建完 miter 的當下」就已經證明完畢,連 solver 都不用進。
    //
    // 所以第一件事永遠是檢查 acc 是不是已經是常數 0,而不是急著送 SAT。
    // resolved_by_strash 這個統計就是在量這件事。

    // ---- D1. strash 已經解決了多少 ----
    // copy_cone 把 before / after 兩側建進同一顆 Ntk,create_and 內建 strash,
    // 兩側結構相同的區域直接共用節點 → 對應的 XOR 塌成常數 0。
    // 只改幾顆閘的修改,絕大多數比較點在這裡就已經證完。
    std::vector<std::size_t> pending;          // 還需要求解的比較點索引
    pending.reserve(diffs.size());

    for (std::size_t i = 0; i < diffs.size(); ++i) {
        const Node dn = miter.get_node(diffs[i]);
        if (miter.is_constant(dn)) {
            if (miter.is_complemented(diffs[i])) {
                // XOR 恆為 1 → 結構上就確定不同(例如一側常數 0、另一側常數 1)
                res.mismatched_outputs.push_back(usable[i]);
            } else {
                ++res.resolved_by_strash;
            }
            continue;
        }
        pending.push_back(i);
    }

    auto tail = [&]() {
        std::string s;
        if (res.resolved_by_strash > 0)
            s += " [" + std::to_string(res.resolved_by_strash) + "/" +
                 std::to_string(usable.size()) + " resolved structurally]";
        if (!res.untrusted_outputs.empty())
            s += " (" + std::to_string(res.untrusted_outputs.size()) +
                 " point(s) skipped as untrustworthy)";
        return s;
    };

    // ---- D2. 全部被 strash 解掉 ----
    if (pending.empty() && res.mismatched_outputs.empty()) {
        res.status  = EquivResult::Equal;
        res.message = "equivalent on " + std::to_string(usable.size()) +
                      " comparison point(s), proved structurally" + tail();
        return res;
    }

    // ---- D3. Phase B:對 miter 開一顆專屬的 incremental solver ----
    if (sat_ != nullptr) {
        // ★★ 絕對不能用成員的 sat_ ★★
        //   sat_ 綁在主 AIG 上,node -> CNF var 表是主 AIG 的節點編號。
        //   miter 是獨立的 Ntk,編號空間完全不同 —— 拿主 AIG 的變數編號去
        //   解讀 miter 的節點,會得到語法合法、語意毫無意義的答案,而且不報錯。
        SatEngine::Config mcfg;
        mcfg.lazy_encode    = true;
        mcfg.time_limit_sec = 0.0;      // 由下面的全域 deadline 逐次指定
        mcfg.conflict_limit = 0;
        SatEngine msat(miter, mcfg);

        // 全域 deadline。逐點求解會呼叫很多次,若讓每次各吃一份
        // opt.time_limit_seconds,總時間會變成 N 倍。
        const auto  solveStart = std::chrono::steady_clock::now();
        const bool  bounded    = (opt.time_limit_seconds > 0.0);
        auto remaining = [&]() -> double {
            if (!bounded) return 0.0;   // 0 = SatEngine 的「不限」
            const double used = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - solveStart).count();
            return opt.time_limit_seconds - used;
        };
        auto out_of_time = [&]() { return bounded && remaining() <= 0.0; };

        // ---- 選用:先 sweep miter ----
        //
        // 對「結構被大幅改寫但功能不變」的最佳化(例如 XAG round-trip)特別有效:
        // sweep 把兩側功能相同的內部節點證成等價並灌成永久子句,
        // 頂端的 XOR 於是靠 unit propagation 就塌掉。
        // 只給部分預算。原本 fc.total_time_budget = opt.time_limit_seconds,
        //   sweep 可以吃光整份預算,之後求解又重新武裝一份 → 最壞兩倍。
        std::unique_ptr<Fraig> mfr;
        if (opt.fraig_miter) {
            Fraig::Config fc;
            fc.total_time_budget       = bounded ? opt.time_limit_seconds * 0.6 : 0.0;
            fc.sat_time_limit          = fraigSatTimeLimit_;
            fc.sat_conflict_limit      = fraigSatConflicts_;
            fc.sim_memory_budget_bytes = fraigSimMemory_;
            fc.max_rounds              = fraigMaxRounds_;

            mfr = std::make_unique<Fraig>(miter, msat, fc);
            mfr->sweep();
            msat.set_limits(0.0, 0);         // sweep 改過,還原
        }

        // ---- 逐點求解 ----
        std::size_t byLookup = 0, bySat = 0;
        bool timedOut = false;

        for (const std::size_t i : pending) {
            // FRAIG 查表:sweep 已經證過的點連 solver 都不用進
            if (mfr && mfr->is_swept_node(miter.get_node(diffs[i]))) {
                bool v = false;
                if (mfr->is_known_const(diffs[i], v)) {
                    ++byLookup;
                    if (v) res.mismatched_outputs.push_back(usable[i]);
                    continue;
                }
            }

            // SAT
            if (out_of_time()) { timedOut = true; break; }
            msat.set_limits(remaining(), 0);

            const EquivResult one = msat.is_const(diffs[i], false);
            ++bySat;

            if (one == EquivResult::NotEqual) {
                res.mismatched_outputs.push_back(usable[i]);
                // 反例只在第一次不同時記錄一份,供呼叫端重現。
                if (res.counterexample.empty()) {
                    const auto& cx = msat.last_counterexample();
                    if (cx.valid) {
                        for (std::size_t k = 0;
                             k < miterInputNames.size() && k < cx.values.size(); ++k)
                            res.counterexample.emplace_back(miterInputNames[k],
                                                            cx.values[k] != 0);
                    }
                }
                // 不提早結束:呼叫端要的是完整的不同點清單。
                // 若清單已經夠長,再問下去對診斷沒有幫助。
                if (res.mismatched_outputs.size() >= 64) { timedOut = false; break; }
            } else if (one == EquivResult::Unknown) {
                timedOut = true;
                break;
            }
        }

        if (timedOut) {
            res.status  = EquivResult::Unknown;
            res.message = "solver ran out of budget after checking " +
                          std::to_string(byLookup + bySat) + " of " +
                          std::to_string(pending.size()) + " unresolved point(s)";
            if (!res.mismatched_outputs.empty())
                res.message += "; " + std::to_string(res.mismatched_outputs.size()) +
                               " mismatch(es) already found, so the circuits are "
                               "very likely NOT equivalent";
            res.message += tail();
            return res;
        }

        if (res.mismatched_outputs.empty()) {
            res.status  = EquivResult::Equal;
            res.message = "equivalent on " + std::to_string(usable.size()) +
                          " comparison point(s) [" + std::to_string(byLookup) +
                          " by FRAIG lookup, " + std::to_string(bySat) + " by SAT]" + tail();
        } else {
            res.status  = EquivResult::NotEqual;
            res.message = "NOT equivalent at " +
                          std::to_string(res.mismatched_outputs.size()) +
                          " point(s), first = " + res.mismatched_outputs.front() + tail();
        }
        return res;
    }

    // ---- D4. Phase A:維持 mockturtle 路徑當 golden ----
    //   刻意不改:Phase B 的輸出要能跟 Phase A 的 golden 逐字比對。
    {
        mockturtle::equivalence_checking_stats st;
        const auto r = mockturtle::equivalence_checking(miter, {}, &st);

        if (!r) {
            res.status  = EquivResult::Unknown;
            res.message = "solver hit its internal limit (Phase A). "
                          "This is exactly what Phase B fixes." + tail();
            return res;
        }
        if (*r) {
            res.status  = EquivResult::Equal;
            res.message = "equivalent on " + std::to_string(usable.size()) +
                          " comparison point(s)" + tail();
            return res;
        }

        res.status  = EquivResult::NotEqual;
        res.message = "NOT equivalent";

        const auto& ce = st.counter_example;
        if (!ce.empty()) {
            std::vector<bool> vals(ce.begin(), ce.end());
            for (std::size_t k = 0; k < miterInputNames.size() && k < vals.size(); ++k)
                res.counterexample.emplace_back(miterInputNames[k], vals[k]);

            if (opt.identify_mismatches) {
                const auto sim = simulate_all(miter, vals);
                for (std::size_t i = 0; i < usable.size(); ++i)
                    if (sig_value(miter, sim, diffs[i]))
                        res.mismatched_outputs.push_back(usable[i]);
                if (!res.mismatched_outputs.empty())
                    res.message += " at " + std::to_string(res.mismatched_outputs.size()) +
                                   " point(s), first = " + res.mismatched_outputs.front();
            }
        }
        res.message += tail();
        return res;
    }
}

} // namespace eqeng
