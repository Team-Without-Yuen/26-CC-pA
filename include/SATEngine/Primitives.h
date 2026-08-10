#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "Types.h"
#include "NameMap.h"
#include "AigBuilder.h"

class Netlist;

namespace eqeng {

class SatEngine;
class Fraig;

// ============================================================
//  修改前後等價驗證
// ============================================================

// 比較點命名規則 —— before / after 靠名字對齊，絕不靠索引。
//   真實 PI / PO        → net 名稱
//   DFF 的 Q（輸入側）   → "$Q:" + DFF instance 名
//   DFF 的 next-state    → "$D:" + DFF instance 名（已折進 RN/SN 的 eff_D）
//   懸空輸入的自由 PI    → net 名稱
// 用 instance 名而非 Q net 名：最佳化重建 netlist 時 instance 名會保留，
// 內部 net 名則會被重新產生（見 AigToNetlist）。
constexpr const char* kDffQPrefix = "$Q:";
constexpr const char* kDffDPrefix = "$D:";

// 修改前的 AIG 快照。
//
//    mockturtle 的 aig_network 內部是 shared_ptr —— 直接 copy 只複製 handle。
//    因此本類別禁止複製（只允許 move）：一旦淺拷貝，snapshot 會跟著後續修改
//    一起變，miter 永遠回報等價 —— 而且完全不會報錯，驗證形同虛設。
//
// 實作不依賴 mockturtle 的 clone()：snapshot 是把當下所有比較點的 cone
// 重新建進一顆全新的 Ntk，天生深拷貝，也不受版本差異影響。
// 快照完全自足（名字以字串保存），不受後續 rebuild 影響。
class AigSnapshot {
public:
    AigSnapshot() = default;
    AigSnapshot(AigSnapshot&&) = default;
    AigSnapshot& operator=(AigSnapshot&&) = default;
    AigSnapshot(const AigSnapshot&)            = delete;
    AigSnapshot& operator=(const AigSnapshot&) = delete;

    bool        valid()       const { return valid_; }
    uint64_t    revision()    const { return revision_; }
    uint32_t    generation()  const { return generation_; }
    std::size_t num_inputs()  const { return inputNames_.size(); }
    std::size_t num_outputs() const { return outputNames_.size(); }

    const std::vector<std::string>& input_names()  const { return inputNames_; }
    const std::vector<std::string>& output_names() const { return outputNames_; }

private:
    friend class Primitives;

    Ntk                      ntk_;
    std::vector<std::string> inputNames_;   // 與 ntk_ 的 PI 索引一一對應
    std::vector<Sig>         inputSigs_;
    std::vector<std::string> outputNames_;
    std::vector<Sig>         outputSigs_;
    uint64_t                 revision_   = 0;
    uint32_t                 generation_ = kInvalidGeneration;
    bool                     valid_      = false;
};

struct CecResult {
    EquivResult status = EquivResult::Unknown;

    // status == NotEqual 時，用反例做一次模擬回推「哪些比較點真的不同」。
    // 不額外花 SAT。
    std::vector<std::string> mismatched_outputs;
    // 反例：輸入名 → 值，可直接餵回模擬重現。
    std::vector<std::pair<std::string, bool>> counterexample;

    // 介面差異。輸入只在單邊出現是允許的（另一邊沒用到而已，miter 仍成立）；
    // 輸出只在單邊出現則無從比較，屬於介面變更。
    std::vector<std::string> inputs_only_in_before;
    std::vector<std::string> inputs_only_in_after;
    std::vector<std::string> outputs_only_in_before;
    std::vector<std::string> outputs_only_in_after;
    bool interface_mismatch = false;

    std::vector<std::string> compared_outputs;
    std::string              message;

    bool ok() const { return status == EquivResult::Equal && !interface_mismatch; }
};

struct CecOptions {
    // 輸出名字對不上時，是否直接判定為「無法比較」（status = Unknown）。
    // false → 只比交集，並標記 interface_mismatch。
    bool require_identical_outputs = true;
    // NotEqual 時是否回推 mismatched_outputs（一次模擬，很便宜）。
    bool identify_mismatches = true;
    // 是否納入 DFF next-state 當比較點。
    // 關掉會漏掉「只改到暫存器輸入」的修改，預設開啟。
    bool include_dff_next_state = true;
};

class UnsoundModel : public std::runtime_error {
public:
    explicit UnsoundModel(const std::string& reason)
        : std::runtime_error("AIG model is not usable for proof: " + reason) {}
};

// ============================================================
//  Primitives
// ============================================================

// facade：高階 API 唯一的呼叫窗口。
//
//   所有權：Primitives 擁有 AigModel（以及 Phase B 的 SatEngine / Fraig）。
//   一個 current Netlist 只能由 backend analysis context 持有一個共用 Primitives；
//   Function Search、Function Analysis、Sequential Pattern 與 CEC 等高階 API
//   必須接收並共用該 instance，不得各自長期保存另一份 Primitives。
//   每個 public method 開頭自行 ensure_fresh()：netlist 髒了就重建。
//   → 高階 API 與 LLM 完全不需要知道 dirty 存在。
//
//   生命週期：
//   rebuild 之後 generation 遞增，所有舊 SigRef 立即失效並在使用時被攔截。
//   正確寫法永遠是：每次分析開始時 resolve(name) 重新取得。
class Primitives {
public:
    enum class UnknownPolicy : uint8_t { AsNotEqual, AsEqual, Throw };

    // 收到跨 generation 的 SigRef 時的處置。
    enum class StaleSigPolicy : uint8_t {
        Throw,           // 預設：丟 StaleSignal，把靜默錯誤變成可見失敗
        RebuildAndWarn,  // 印警告後照舊使用（會給錯答案，僅供暫時止血）
        Ignore
    };

    struct Config {
        UnknownPolicy  unknown_policy;
        StaleSigPolicy stale_sig_policy;
        int  default_cut_size;
        bool count_stats;
        bool verbose_rebuild;

        Config()
        : unknown_policy(UnknownPolicy::AsNotEqual)
        , stale_sig_policy(StaleSigPolicy::Throw)
        , default_cut_size(6)
        , count_stats(true)
        , verbose_rebuild(true) {}
    };

    struct Stats {
        uint64_t equiv_calls      = 0;
        uint64_t equiv_by_lookup  = 0;
        uint64_t equiv_by_sat     = 0;
        uint64_t equiv_unknown    = 0;
        uint64_t equiv_trivial    = 0;
        uint64_t cofactors_built  = 0;
        uint64_t cut_enumerations = 0;
        uint64_t miters_built     = 0;
        uint64_t rebuilds         = 0;
        double   rebuild_seconds  = 0.0;
        uint64_t snapshots_taken  = 0;
        uint64_t cec_runs         = 0;
        uint64_t stale_rejected   = 0;
    };

    // 唯一建構方式。建構時不立刻建 AIG —— 等第一個 query 進來才建（lazy）。
    explicit Primitives(Netlist& nl,
                        AigModel::Options bopt = {},
                        Config cfg = Config());
    ~Primitives();

    Primitives(const Primitives&)            = delete;
    Primitives& operator=(const Primitives&) = delete;

    // ---------- 狀態 ----------
    uint32_t generation() const { return generation_; }
    bool     is_phase_a() const { return sat_ == nullptr; }
    void     enable_phase_b(bool on);   // 切換後下次 ensure_fresh 會重建引擎

    // 診斷／測試用。會先 ensure_fresh()。一般高階 API 不需要。
    AigModel& model();
    const AigModel& model() const;
    ModelHealth model_health();
    std::string model_health_message();
    Ntk&      aig();
    NameMap&  names();
    const Netlist& netlist() const;

    // ---------- 名字橋接（唯一能「憑空」取得 SigRef 的入口）----------
    SigRef                   resolve(const std::string& net);
    std::optional<SigRef>    try_resolve(const std::string& net);
    std::vector<std::string> names_of(SigRef s);

    // ---------- 常數與建構（讓高階 API 不必碰 raw Sig 也能組電路）----------
    SigRef constant(bool val);
    SigRef make_and(SigRef a, SigRef b);
    SigRef make_or(SigRef a, SigRef b);
    SigRef make_xor(SigRef a, SigRef b);
    // 例：Shannon 重組 f == s ? f1 : f0
    SigRef make_mux(SigRef sel, SigRef onTrue, SigRef onFalse);

    // ---------- 查詢節點性質 ----------
    bool is_free_var(SigRef s);      // 是否為 PI（cofactor 的前置條件）
    bool is_constant(SigRef s);
    bool is_complemented(SigRef s);

    // ---------- 等價 / 常數 ----------
    EquivResult equiv_checked(SigRef a, SigRef b);
    EquivResult is_const_checked(SigRef a, bool val);
    bool        equiv(SigRef a, SigRef b);
    bool        is_const(SigRef a, bool val);
    bool        is_const0(SigRef a) { return is_const(a, false); }
    bool        is_const1(SigRef a) { return is_const(a, true);  }

    // ---------- Cofactor ----------
    // 把 var 設為 val 後的函數，於同一顆 AIG 內物化並 strash。
    // 前置條件：var 必須是 PI（自由變數，含 DFF-Q 的 pseudo-PI）。
    SigRef      cofactor(SigRef f, SigRef var, bool val);

    // 不物化的版本：用 SAT assumption 直接問，不讓 AIG 長大。
    // 適合大量試探性查詢（enable/hold 對 support 每個候選都試一遍）。
    EquivResult equiv_under(SigRef a, SigRef b,
                            const std::vector<std::pair<SigRef, bool>>& assumptions);
    EquivResult is_const_under(SigRef a, bool val,
                               const std::vector<std::pair<SigRef, bool>>& assumptions);

    // ---------- 功能性 support / unate / symmetry ----------
    // Equal = 兩個 cofactor 相同 = 不相依；NotEqual = 相依。
    EquivResult         depends_on_checked(SigRef f, SigRef var);
    bool                depends_on(SigRef f, SigRef var);
    std::vector<SigRef> functional_support(SigRef f, const std::vector<SigRef>& candidates);
    std::vector<SigRef> functional_support(SigRef f);
    bool                is_unate(SigRef f, SigRef var, bool positive);
    bool                is_symmetric(SigRef f, SigRef x, SigRef y);

    // ---------- 結構辨識（cut + NPN）----------
    // 精度界線：cut+NPN 只用於 support <= k(<=6)；
    // 寬 cone 一律走 cofactor + equiv，絕不建 2^support 真值表。
    std::vector<Cut> enumerate_cuts(SigRef root, int k = 0);
    // 節點函數（不含 root signal 的 complement）。
    TruthTable       truth_of(const Cut& cut);
    // signal 函數（含 complement）。要跟特定 hex 值比對時用這個 ——
    // AIG 裡 OR/NAND/NOR/XNOR 收尾的 signal 幾乎都帶 complement。
    TruthTable       truth_of(const Cut& cut, SigRef root);
    NpnClass         classify(const TruthTable& tt);
    bool             npn_matches(const Cut& cut, const TruthTable& tmpl);
    // cut leaf 的存取（免得 L3 需要碰 Node）
    SigRef                   cut_leaf(const Cut& cut, std::size_t i);
    std::vector<std::string> cut_leaf_names(const Cut& cut);

    // ---------- 等價類報告 ----------
    std::vector<std::vector<std::string>> equivalence_classes(int min_size = 2);

    // ---------- 修改前後等價驗證 ----------
    //
    //   auto before = prim.snapshot();              // 修改前
    //   ... 修改 netlist ...
    //   auto r = prim.equiv_to_snapshot(before);    // 修改後
    //
    // 一定要建 miter，不能用 FRAIG 查表 —— FRAIG 只在單一 AIG 內部有效，
    // 修改前後是兩顆獨立的 AIG，節點空間對不上。
    AigSnapshot snapshot();

    // 全比較點須等價（驗證重構未改變功能）。
    CecResult equiv_to_snapshot(const AigSnapshot& before, CecOptions opt = {});

    // 排除指定比較點後其餘須等價（確認修改只影響目標）。
    // 名稱可用 net 名，或 "$D:instName" / "$Q:instName"。
    CecResult equiv_to_snapshot_except(const AigSnapshot& before,
                                       const std::vector<std::string>& exclude,
                                       CecOptions opt = {});

    // 只比指定比較點（確認目標「確實被改動」時，期望回 NotEqual）。
    CecResult equiv_to_snapshot_only(const AigSnapshot& before,
                                     const std::vector<std::string>& only,
                                     CecOptions opt = {});

    // 這顆電路目前所有比較點的名稱（方便高階 API 組 exclude / only 清單）。
    std::vector<std::string> comparison_points(bool include_dff_next_state = true);

    // ---------- 逃生口（有意識地繞過 generation 保護）----------
    // 只在需要直接操作 mockturtle API 時使用。raw() 會做 generation 檢查；
    // wrap() 蓋上「當下」的 generation —— 若來源其實是舊的，保護就失效了，
    // 所以 wrap 的輸入必須是同一次呼叫裡剛從 raw() 拿到的值。
    Sig    raw(SigRef s);
    SigRef wrap(Sig s);

    const Stats&  stats()  const { return stats_; }
    void          reset_stats();
    const Config& config() const { return cfg_; }

private:
    // ---------- dirty / rebuild ----------
    void ensure_fresh();
    void rebuild();
    bool model_can_prove() const;
    void require_usable_model() const;

    // 把 SigRef 解成 raw Sig，並檢查 generation。
    //   所有吃 SigRef 的 public method 都必須先 ensure_fresh()、再 unwrap()。
    //   順序不可顛倒：先重建才知道當前 generation。
    Sig  unwrap(SigRef s);
    SigRef stamp(Sig s) const { return SigRef(s, generation_); }
    void check_cut(const Cut& cut);

    // ---------- 既有內部 ----------
    bool        resolve_policy(EquivResult r);
    EquivResult equiv_via_miter(Sig a, Sig b);
    Sig         build_cofactor(Sig f, Sig var, bool val);
    bool        in_structural_cone(Sig f, Node target);
    TruthTable  truth_of_raw(const Cut& cut);

    // ---------- CEC 內部 ----------
    CecResult run_cec(const AigSnapshot& before,
                      const std::vector<std::string>* filter,
                      bool filterIsExclude,
                      const CecOptions& opt);

    Netlist&                   nl_;
    AigModel::Options          bopt_;
    Config                     cfg_;
    std::unique_ptr<AigModel>  model_;
    std::unique_ptr<SatEngine> sat_;     // Phase A 為 nullptr
    std::unique_ptr<Fraig>     fraig_;   // Phase A 為 nullptr
    bool                       wantPhaseB_ = false;
    uint32_t                   generation_ = kInvalidGeneration;
    Stats                      stats_;

    // ★ rebuild 時必須全部清空：內容是舊 AIG 的 Sig 與 node index。
    std::unordered_map<uint64_t, Sig> cofactorCache_;
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> coneCache_;
};

class EngineUnknown : public std::exception {
public:
    const char* what() const noexcept override {
        return "engine returned Unknown (resource limit)";
    }
};

// 跨 rebuild 使用了舊的 SigRef / Cut。修法一律是：重新 resolve(name)。
class StaleSignal : public std::exception {
public:
    const char* what() const noexcept override {
        return "stale SigRef/Cut used across a netlist modification "
               "-- call resolve(name) again after any edit";
    }
};

} // namespace eqeng
