#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "Types.h"

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
        uint32_t sim_words_init;       // 初始隨機向量：32 * 64 = 2048 組
        uint32_t sim_words_max;        // 反例回收累積的上限，避免記憶體爆掉
        uint32_t max_rounds;           // refine 迴圈上限
        double   sat_time_limit;       // 單一等價驗證的秒數上限
        int64_t  sat_conflict_limit;
        double   total_time_budget;    // 整個 sweep 的秒數預算；<=0 不限
        bool     use_constant_class;   // 一併掃「恆為 0/1」（最便宜，多半模擬就解掉）

        Config()
        : sim_words_init(32)
        , sim_words_max(256)
        , max_rounds(8)
        , sat_time_limit(1.0)
        , sat_conflict_limit(10000)
        , total_time_budget(0.0)
        , use_constant_class(true) {}
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
    };

    Fraig(Ntk& aig, SatEngine& sat, Config cfg = Config());
    ~Fraig();

    Fraig(const Fraig&)            = delete;
    Fraig& operator=(const Fraig&) = delete;

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
