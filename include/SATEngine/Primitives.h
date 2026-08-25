#pragma once

#include <cstdint>
#include <functional>
#include <limits>
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
#include "include/core/RequestTimeBudget.h"

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
    std::vector<char>        outputTrusted_;
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

    // 因污染而被排除、無法比較的比較點
    std::vector<std::string> untrusted_outputs;

    // 介面差異。輸入只在單邊出現是允許的（另一邊沒用到而已，miter 仍成立）；
    // 輸出只在單邊出現則無從比較，屬於介面變更。
    std::vector<std::string> inputs_only_in_before;
    std::vector<std::string> inputs_only_in_after;
    std::vector<std::string> outputs_only_in_before;
    std::vector<std::string> outputs_only_in_after;
    bool interface_mismatch = false;

    std::vector<std::string> compared_outputs;
    std::string              message;

    // 診斷:多少個比較點在建完 miter 的當下就被 strash 證明相等(完全沒進 solver)。
    // 這個數字接近 compared_outputs.size() 時,代表修改的影響範圍很小 —— 是好事。
    std::size_t resolved_by_strash = 0;

    bool ok() const {
        return status == EquivResult::Equal
            && !interface_mismatch
            && untrusted_outputs.empty();
    }
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

    // 求解的 wall-clock 預算(秒)。<=0 表示不限。
    // Phase A 的 mockturtle 路徑吃不到這個,只有 Phase B 生效。
    double time_limit_seconds = 0.0;

    // 求解前先對 miter 跑一次 FRAIG sweep。
    //
    // 適用時機很明確:驗證「結構被改寫但功能不變」的最佳化。
    // sweep 會把兩側功能相同的內部節點合併掉,頂端的 XOR 直接塌成常數 0,
    // SAT 幾乎不用做事。
    bool fraig_miter = false;

    // 有任何比較點因污染而無法比對時,直接把 status 判為 Unknown。
    //
    // 需要「明知有污染,仍想知道乾淨區域是否相等」時才關掉,
    // 並且務必改用 ok() 而不是 status。
    bool untrusted_is_failure = true;
};

class UnsoundModel : public std::runtime_error {
public:
    explicit UnsoundModel(const std::string& reason)
        : std::runtime_error("AIG model is not usable for proof: " + reason) {}
};

// ============================================================
//  Primitives
// ============================================================

// Low-level Boolean analysis utility used inside high-level API implementations.
//
//   所有權：Primitives 擁有 AigModel（以及 Phase B 的 SatEngine / Fraig）。
//   每個 public method 開頭自行 ensure_fresh()：netlist 髒了就重建。
//   高階 API 只使用 proof 結果，不得把 SigRef/AIG 細節暴露到 public report。
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
        // true  = 全域閘門：health==Invalid 時整顆電路拒絕作答（舊行為）
        // false = 預設：只拒絕受污染的訊號，乾淨區域照常證明
        bool strict_global_health;
        // 同一版電路上累積多少次「必須走 SAT 的等價查詢」之後,自動觸發一次
        // FRAIG sweep,之後同版的查詢全部變查表。
        // 門檻不宜太低:只問三五條 net 的 session 不該付 sweep 的成本;
        // 但比賽的 prompt 常常一次爆出上百個等價問題,那時 sweep 一定划算。
        // 0 = 關閉自動觸發,只能靠 request_fraig_sweep()。
        uint32_t fraig_auto_sweep_threshold;

        Config()
        : unknown_policy(UnknownPolicy::AsNotEqual)
        , stale_sig_policy(StaleSigPolicy::Throw)
        , default_cut_size(6)
        , count_stats(true)
        , verbose_rebuild(true)
        , strict_global_health(false) 
        , fraig_auto_sweep_threshold(32) {}
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
        uint64_t tainted_rejected = 0;   // 因污染而回 Unknown / 丟例外的次數
        uint64_t fraig_sweeps         = 0;
        double   fraig_sweep_seconds  = 0.0;
        uint64_t fraig_lookup_misses  = 0;   // 查表查不出來、退回 SAT 的次數
    };

    // 唯一建構方式。建構時不立刻建 AIG —— 等第一個 query 進來才建（lazy）。
    explicit Primitives(const Netlist& nl,
                        AigModel::Options bopt = {},
                        Config cfg = Config());
    ~Primitives();

    Primitives(const Primitives&)            = delete;
    Primitives& operator=(const Primitives&) = delete;

    // ---------- 狀態 ----------
    uint32_t generation() const { return generation_; }
    bool     is_phase_a() const { return sat_ == nullptr; }
    void     enable_phase_b(bool on);   // 切換後下次 ensure_fresh 會重建引擎
    // Phase B 引擎是否已就緒(SatEngine 存在)。
    bool     phase_b_ready() const { return sat_ != nullptr; }

    // 診斷／測試用。會先 ensure_fresh()。一般高階 API 不需要。
    AigModel& model();
    const AigModel& model() const;
    ModelHealth model_health();
    std::string model_health_message();
    uint32_t    num_tainted_nets();      
    std::string taint_summary();         // 污染源的人可讀說明
    Ntk&      aig();
    NameMap&  names();
    const Netlist& netlist() const;
    bool is_trustworthy(const std::string& net);
    bool is_trustworthy(SigRef s) { return !s.tainted(); }

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
    // Deadline-aware Phase A proof. The budget includes lazy rebuild and CNF
    // preparation; CaDiCaL is interrupted when the remaining wall time expires.
    EquivResult equiv_checked(SigRef a, SigRef b, double time_limit_seconds);
    EquivResult is_const_checked(SigRef a, bool val, double time_limit_seconds);
    bool        last_proof_timed_out() const { return lastProofTimedOut_; }
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
    // 完整報告：等價類 + 常數類分開，並回報被排除的不可信 net 數。
    // 建議新程式碼一律用這個 —— 「恆為 0」與「彼此等價」在回報給使用者時
    // 是兩件不同的事，混在一起會讓 LLM 產生誤導性的敘述。
    EquivClassReport equivalence_report(int min_size = 2);

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

    // ---------- FRAIG ----------
    // 診斷用的數值快照
    struct FraigProfile {
        bool     swept = false, complete = false;
        uint64_t candidate_pairs = 0, merged_nodes = 0, const_nodes = 0;
        uint64_t proved_equal = 0, refuted = 0, gave_up = 0, sim_rounds = 0;
        double   sim_seconds = 0.0, sat_seconds = 0.0;
        // SatEngine 的累計值。encoded_nodes 相對於 aig_size 就是 lazy encoding 的效益。
        uint64_t sat_queries = 0, sat_encoded_nodes = 0, sat_clauses = 0;
        double   sat_solve_seconds = 0.0;
    };
    FraigProfile fraig_profile() const;

    // needVerified = false(預設):只做模擬與分類。
    //     否定查詢立即可答,正面候選仍走 SAT。成本約完整驗證的 1/30。
    // needVerified = true:額外跑完整 SAT 驗證。
    //     只有 equivalence_report 真正需要它 —— 一般查詢不需要。
    //
    // 已經跑過較低層級時會**升級**(重跑模擬只要零點幾秒,
    // 而 SatEngine 學到的子句全部保留,不會白費)。
    bool request_fraig_sweep(bool needVerified = false);

    // 硬上限:開啟後即使呼叫端要求驗證,也只做模擬分類。
    // 給「電路太大,寧可回報不完整也不能超時」的情況用。
    // equivalence_report 會誠實把 is_complete 標成 false。
    void set_fraig_simulate_only(bool on);

    bool fraig_swept()   const;    // 至少做過模擬分類
    bool fraig_verified() const;   // 做過完整 SAT 驗證
    bool fraig_complete() const;   // 驗證過、跑完、且沒有任何放棄
    std::string fraig_summary() const;   // 診斷用

    // 設定下一次 sweep 的預算。已經 sweep 過的不受影響。
    // 刻意不直接吃 Fraig::Config —— 那會逼 Primitives.h include Fraig.h,
    // 把 mockturtle 的相依性洩漏給所有引用 Primitives.h 的檔案。
    void set_fraig_budget(double total_seconds,
                          double per_query_seconds,
                          int64_t per_query_conflicts,
                          uint64_t sim_memory_bytes,
                          uint32_t max_rounds = 0); // 0 = 沿用現值

    // 讓 sweep 使用整個 request 的 deadline，而不是自己起算。
    // 傳 nullptr 恢復成用 set_fraig_budget 的 total_seconds。
    void set_fraig_deadline(const request_time_budget::RequestDeadline* deadline);
    bool fraig_sweep_truncated() const;

private:
    // ---------- dirty / rebuild ----------
    void ensure_fresh();
    void rebuild();
    bool model_can_prove() const;
    void require_usable_model();

    // 把 SigRef 解成 raw Sig，並檢查 generation。
    //   所有吃 SigRef 的 public method 都必須先 ensure_fresh()、再 unwrap()。
    //   順序不可顛倒：先重建才知道當前 generation。
    Sig  unwrap(SigRef s);
    SigRef stamp(Sig s, bool tainted = false) const {
        return SigRef(s, generation_, tainted);
    }
    void check_cut(const Cut& cut);

    // ---------- 既有內部 ----------
    bool        resolve_policy(EquivResult r);
    EquivResult equiv_via_miter(Sig a, Sig b);
    EquivResult equiv_via_cadical(Sig a, Sig b, double time_limit_seconds);
    Sig         build_cofactor(Sig f, Sig var, bool val);
    bool        in_structural_cone(Sig f, Node target);
    TruthTable  truth_of_raw(const Cut& cut);
    // 任何一個引數受污染 → true（並累計 stats）
    bool tainted_any(std::initializer_list<SigRef> rs);
    // 無三態回傳的 API 用：受污染就丟 UnsoundModel
    void require_trusted(std::initializer_list<SigRef> rs);
    void        ensure_engines();
    // want: 1 = Simulated, 2 = Verified
    bool ensure_fraig(bool needVerified);
    EquivResult fraig_lookup(Sig a, Sig b);
    EquivResult fraig_lookup_const(Sig s, bool val);
    void        arm_sat_budget(double remaining);

    // ---------- CEC 內部 ----------
    CecResult run_cec(const AigSnapshot& before,
                      const std::vector<std::string>* filter,
                      bool filterIsExclude,
                      const CecOptions& opt);

    const Netlist&             nl_;
    AigModel::Options          bopt_;
    Config                     cfg_;
    std::unique_ptr<AigModel>  model_;
    std::unique_ptr<SatEngine> sat_;     // Phase A 為 nullptr
    std::unique_ptr<Fraig>     fraig_;   // Phase A 為 nullptr
    bool                       wantPhaseB_ = false;
    uint32_t                   generation_ = kInvalidGeneration;
    uint64_t                   builtRevision_ =
        std::numeric_limits<uint64_t>::max();
    Stats                      stats_;
    bool                       lastProofTimedOut_ = false;
    uint64_t satEquivQueries_   = 0;      // 本 generation 內退回 SAT 的等價查詢數
    double   fraigTotalBudget_  = 0.0;    // <=0 不限
    double   fraigSatTimeLimit_ = 1.0;
    int64_t  fraigSatConflicts_ = 10000;
    uint64_t fraigSimMemory_    = 8ull * 1024ull * 1024ull * 1024ull; // 8GB
    uint8_t  fraigLevel_       = 0;   // 目前這顆 Fraig 達到的層級
    uint8_t  fraigTriedLevel_  = 0;   // 已嘗試過的最高層級(失敗也算,避免反覆重試)
    bool     fraigSimOnlyCap_  = false;
    uint32_t fraigMaxRounds_   = 64;
    const request_time_budget::RequestDeadline* fraigDeadline_ = nullptr;

    struct CofactorKey {
        uint64_t functionData = 0;
        uint64_t variableData = 0;
        bool value = false;

        bool operator==(const CofactorKey& other) const noexcept {
            return functionData == other.functionData &&
                   variableData == other.variableData &&
                   value == other.value;
        }
    };

    struct CofactorKeyHash {
        std::size_t operator()(const CofactorKey& key) const noexcept {
            std::size_t seed = std::hash<uint64_t>{}(key.functionData);
            seed ^= std::hash<uint64_t>{}(key.variableData) +
                    0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
            seed ^= std::hash<bool>{}(key.value) +
                    0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    // rebuild 時必須全部清空：內容是舊 AIG 的 Sig 與 node index。
    std::unordered_map<CofactorKey, Sig, CofactorKeyHash> cofactorCache_;
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

class FraigDeadlineScope {
public:
    FraigDeadlineScope(Primitives& p,
                       const request_time_budget::RequestDeadline* d)
        : prim_(p) { prim_.set_fraig_deadline(d); }

    ~FraigDeadlineScope() { prim_.set_fraig_deadline(nullptr); }

    FraigDeadlineScope(const FraigDeadlineScope&)            = delete;
    FraigDeadlineScope& operator=(const FraigDeadlineScope&) = delete;
    FraigDeadlineScope(FraigDeadlineScope&&)                 = delete;
    FraigDeadlineScope& operator=(FraigDeadlineScope&&)      = delete;

private:
    Primitives& prim_;
};

} // namespace eqeng
