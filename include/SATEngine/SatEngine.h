#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include "Types.h"

namespace CaDiCaL { class Solver; }

namespace eqeng {

// 反例：以 pi_index 為索引的 PI 賦值（含 DFF-Q 的 pseudo-PI）。
// FRAIG 會把它當新的模擬向量回收再利用。
struct Counterexample {
    std::vector<uint8_t> values;   // values[pi_index] ∈ {0,1}
    bool valid = false;
};

// 常駐 incremental CaDiCaL。
//
// 設計要點：
//   - AIG 的 Tseitin encode 極便宜：每個 AND 節點固定 3 clause。
//   - CaDiCaL 實例全程存活，絕不 per-query 重新 encode 整顆電路。
//   - lazy_encode = true 時只 encode 被查詢到的 fanin cone（COI），
//     100 萬 gate 幾乎不可能全部 encode，預設開啟。
//   - 每個 query = 一顆 miter 輔助變數 + assume；UNSAT → 等價，SAT → 讀 model 當反例。
//
// 生命週期：aig 需在本物件存活期間持續有效。AIG 只允許「增長」
//   （cofactor 會物化新節點）；增長後呼叫 sync() 或任何 API 會自動延展 node->var 表。
//   若 Netlist 被修改而重建 AIG，必須整個丟棄本物件重建。
class SatEngine {
public:
    struct Config {
        double  time_limit_sec;   // 單一 query 的 wall-clock 上限；<=0 表示不限
        int64_t conflict_limit;   // 單一 query 的 conflict 上限；<=0 表示不限
        bool    lazy_encode;      // 只 encode COI；100 萬 gate 建議維持 true

        Config()
        : time_limit_sec(0.0)
        , conflict_limit(0)
        , lazy_encode(true) {}
    };

    struct Stats {
        uint64_t queries      = 0;
        uint64_t unsat        = 0;
        uint64_t sat          = 0;
        uint64_t unknown      = 0;      // 打到 limit
        uint64_t encoded_nodes = 0;     // 已 encode 進 CNF 的 AND 節點數
        uint64_t clauses      = 0;
        double   solve_seconds = 0.0;
    };

    explicit SatEngine(const Ntk& aig, Config cfg = Config());
    ~SatEngine();

    SatEngine(const SatEngine&)            = delete;
    SatEngine& operator=(const SatEngine&) = delete;

    // ---------- 查詢 ----------
    // a、b 功能等價？（需相同 phase；a 與 !a 判為 NotEqual）
    // 內部：ensure_encoded(a,b) → 取/建 miter 變數 → assume → solve。
    EquivResult are_equal(Sig a, Sig b);

    // a 是否恆為 val？
    EquivResult is_const(Sig a, bool val);

    // 帶 assumption 的等價：等同對某些變數取 cofactor 後再比，
    // 但「不必實體 restrict」——不會在 AIG 裡長出新節點，適合大量試探性查詢
    // （例如 enable/hold 對 support 裡每個候選 e 都試一遍）。
    // assumptions 的 Sig 必須是 PI（自由變數）；bool 為指定值。
    EquivResult are_equal_under(Sig a, Sig b,
                                const std::vector<std::pair<Sig, bool>>& assumptions);

    // 直接問可滿足性：是否存在賦值使 f = val（給 decoder 互斥檢查等使用）。
    SatResult solve_for(Sig f, bool val);

    // 最近一次回 Sat 的 query 的反例；valid=false 表示上一次不是 Sat 或已被消耗。
    const Counterexample& last_counterexample() const;

    // ---------- 增量學習（FRAIG 用）----------
    // 把「已證明等價」當成永久 binary clause 灌回 solver：(¬a∨b)(a∨¬b)。
    // 效果等同把節點 merge 掉、讓下游 cone 變小，但**不動 AIG 結構**，
    // 因此既有的 node->var 表與所有 Sig 全部保持有效。
    //
    //    前置條件：必須是**無條件**的 UNSAT 結論（are_equal / is_const，
    //    不帶 assumption）。
    //
    //    絕不可餵 are_equal_under 的結果 —— 那個 UNSAT 只在該組 assumption
    //    下成立。灌成永久子句後 solver 從此被污染，之後所有查詢都可能靜默
    //    回錯誤的 UNSAT：看起來像「證明成功」，不會 crash、不會有任何症狀。
    //    這是本 class 最危險的一個 API。
    //
    //    Debug build 會用 lastSolveHadAssumptions_ 做一道自檢；
    //    Release build 沒有保護，呼叫端必須自己遵守。
    bool assert_equal(Sig a, Sig b);
    bool assert_const(Sig a, bool val);

    // ---------- 編碼控制 ----------
    void ensure_encoded(Sig s);        // 把 s 的 fanin cone 補進 CNF（lazy 模式下用）
    void ensure_encoded(Node n);
    void sync();                       // AIG 長大後延展內部表（cofactor 之後）
    bool is_encoded(Node n) const;

    const Stats& stats() const;
    void reset_stats();

    void set_limits(double time_limit_sec, int64_t conflict_limit);

private:
    // CaDiCaL 常駐實例 + TimeLimitTerminator；
    // node index -> CNF var（0 = 尚未 encode）；
    // (nodeA,nodeB,phase) -> miter 輔助變數的 cache，避免同一對重複建變數。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eqeng
