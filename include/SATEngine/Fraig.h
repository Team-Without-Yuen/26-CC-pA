#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "Types.h"
#include "include/core/RequestTimeBudget.h"

namespace eqeng {

class SatEngine;

// 一個功能等價類：members 全部等價於 representative（同 phase）。
struct EquivClass {
    Sig              representative;
    std::vector<Sig> members;       // 不含 representative 本身
};

// FRAIG(Functionally Reduced AIG) sweep：
// 對整顆電路做**一次**掃描，之後所有 equiv / is_const 查詢都變成查表。
// 10 萬 query 從 10 萬次 SAT 變 10 萬次查表。
//
// 演算法：
//   1. strash 後 structural 相同者已免費合併（mockturtle create_and 內建）。
//   2. bit-parallel 隨機模擬（一個 uint64 跑 64 組向量）→ 依 signature 分候選等價類。
//   3. 每類選代表，其餘對代表跑 SatEngine 驗證：
//        Unsat    → 認定等價，記進 repr 表 + SatEngine::assert_equal
//        Sat      → 把 counterexample 當新模擬向量，一口氣 refine 所有候選類 ★反例回收
//        Unknown  → 放棄這一對（保守：不合併），計入 stats().gave_up
//   4. 依 topological order（fanin → fanout）進行，反覆到穩定。
//
// 非破壞性設計（重要）：
//   sweep **不呼叫 substitute_node、不改動 AIG 結構**，只維護一張
//   node -> representative signal 的表，並把已證等價灌成 solver 的 binary clause。
//   → 所有既有 Sig 與 NameMap 綁定全程保持有效；下游 cone 縮小的效果由 solver 承擔。
class Fraig {
public:
    struct Config {
        uint32_t sim_words_init;
        uint32_t sim_words_max;
        // 模擬向量的總記憶體上限;實際字數 = min(sim_words_max, 預算 / (8 * 節點數))。
        // 0 = 不限,完全聽 sim_words_max 的。
        uint64_t sim_memory_budget_bytes;
        uint32_t max_rounds;
        double   sat_time_limit;
        int64_t  sat_conflict_limit;
        double   total_time_budget;
        // 因為 sweep 只是 request 的一個階段，用自己的碼表會超出整體預算。
        const request_time_budget::RequestDeadline* deadline;
        bool     use_constant_class;
        // 只做模擬與分類,跳過整段 SAT 驗證。
        bool simulate_only;
        // 依實際候選數自動加碼向量:從 sim_words_init 起跳,倍增到
        // 候選夠少或收斂為止,上限是 sim_words_max/2(另一半留給反例回收)。
        // 關掉的話就固定用 sim_words_init。
        bool adaptive_sim;

        Config()
        : sim_words_init(32)
        , sim_words_max(512)
        , sim_memory_budget_bytes(8ull * 1024 * 1024 * 1024)
        , max_rounds(64)
        , sat_time_limit(1.0)
        , sat_conflict_limit(10000)
        , total_time_budget(0.0)
        , deadline(nullptr)
        , use_constant_class(true)
        , simulate_only(false)
        , adaptive_sim(true) {}
    };

    struct Stats {
        uint64_t candidate_pairs = 0;
        uint64_t proved_equal    = 0;
        uint64_t refuted         = 0;      // SAT，反例已回收
        uint64_t gave_up         = 0;      // Unknown（打到 limit），保守不合併
        uint64_t sim_rounds      = 0;
        uint64_t merged_nodes    = 0;
        uint64_t const_nodes     = 0;
        double   sim_seconds     = 0.0;
        double   sat_seconds     = 0.0;
        bool     completed       = false;  // false = 預算耗盡，結果不完整但仍安全
        // 前置階段（topology / simulate / classify）實際完成到哪個 node index。
        // 時間預算在這些階段耗盡時 < total_node_count；此時 index 大於等於
        // 這個值的節點沒有可信的模擬資料，所有查表一律退回 SAT。
        uint64_t swept_node_count = 0;
        uint64_t total_node_count = 0;
        bool     sweep_truncated  = false;
    };

    Fraig(Ntk& aig, SatEngine& sat, Config cfg = Config());
    ~Fraig();

    Fraig(const Fraig&)            = delete;
    Fraig& operator=(const Fraig&) = delete;

    // 前置階段是否被時間預算截斷。true 代表部分節點沒有模擬資料，
    // 查表會 miss 但不會給錯誤答案。
    bool sweep_truncated() const;
    uint64_t swept_node_count() const;

    // 執行 sweep。可重複呼叫（例如追加預算再跑一輪）。
    void sweep();

    bool        is_swept() const;
    // sweep 當下的 aig.size()。之後 cofactor 物化出來的新節點 index >= 這個值，
    // 它們沒有經過 sweep → Primitives 必須對它們走 SAT，不可只比 representative。
    uint64_t    sweep_watermark() const;
    bool        is_swept_node(Node n) const;

    // 查代表：canonical(s) = repr[node(s)] ^ is_complemented(s)。
    // 未經 sweep 或未被歸類的節點回傳 s 自身。
    Sig representative(Sig s) const;

    // 快速判定；前置條件：兩者皆 is_swept_node，否則結果無意義（要走 SAT）。
    bool same_class(Sig a, Sig b) const;
    // 是否已知為常數；回傳 nullopt 表示未知（需走 SAT）。
    bool is_known_const(Sig s, bool& val) const;

    // 全部等價類（含常數類；常數類的 representative 為 aig.get_constant(...)）。
    std::vector<EquivClass> classes(int min_size = 2) const;

    // 兩者是否已被某組模擬向量分開。true ⇒ 必定不等價,且不需要 SAT。
    // 與 sweep 是否完成無關,simulate_only 模式下同樣可信。
    bool provably_different(Sig a, Sig b) const;

    // 給 NameMap::remap 使用：以 node index 為索引的代表 signal，
    // 未受影響的 node 填自身。長度 = sweep_watermark()。
    const std::vector<Sig>& representatives_by_node() const;

    const Stats& stats() const;

private:
    // partial_simulator / sim signature -> 候選類；union-find 或 repr 陣列；
    // SatEngine 驗證 + 反例回收。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eqeng
