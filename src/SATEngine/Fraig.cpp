// =============================================================================
//  Fraig.cpp  —  模擬分類 → SAT 驗證 → 反例回收
// =============================================================================
//  非破壞性設計(核心前提)
//  ------------------------
//  本檔**絕不呼叫 substitute_node,絕不改動 AIG 結構**,只維護一張
//  node index -> representative signal 的表,並把已證等價灌成 solver 的
//  永久 binary clause(SatEngine::assert_equal)。
//
//  於是:
//    - 所有既有的 Sig 與 NameMap 綁定全程有效,不需要 bump generation。
//    - 「下游 cone 縮小」的效果由 solver 的 unit propagation 承擔,
//      而不是由 AIG 的節點合併承擔。
//    - node index 的遞增順序恆為合法拓撲序(mockturtle 的 create_and 保證
//      fanin index < 自身 index,而我們不動結構,所以此性質不會被破壞)。
//      → 模擬只要由小到大掃一遍,不需要另外排序。
//
//  演算法
//  ------
//    1. bit-parallel 隨機模擬(一個 uint64 同時跑 64 組向量)。
//    2. 依 simulation signature 分「候選等價類」。signature 以 word0 的 bit0
//       正規化(見 simPhase_),使 f 與 ~f 落在同一類,靠 phase 位區分。
//    3. 由小到大逐對送 SAT:
//         Equal    → 記進 repr_ 表 + SatEngine::assert_equal
//         NotEqual → 反例存進 buffer;滿 64 個就併成一個新 word、
//                    重新模擬該 word、一口氣切開所有候選類 
//         Unknown  → 保守放棄這一對(不合併),計入 stats().gave_up
//    4. 反覆到沒有未決候選,或打到 max_rounds / 時間預算。
//
//  安全性:任何提早中止都只會讓結果「不完整」,不會讓結果「錯誤」。
//  已合併的都是 SAT 證出的,沒證出的就維持各自獨立。
//
// =============================================================================

#include "include/SATEngine/Fraig.h"
#include "include/SATEngine/SatEngine.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace eqeng {

namespace {

constexpr uint32_t kNoClass = 0xFFFFFFFFu;

// 一次 flush 最多併入的反例數 = 一個 uint64 的位元數。
constexpr uint32_t kCexPerWord = 64;

enum : uint8_t { kOpen = 0, kMerged = 1, kGivenUp = 2 };

inline uint64_t xorshift64(uint64_t& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

} // namespace

// =============================================================================
//  Impl
// =============================================================================

struct Fraig::Impl {
    Ntk&       aig;
    SatEngine& sat;
    Config     cfg;
    Stats      st;

    bool     swept     = false;
    uint32_t numNodes  = 0;      // = sweep 當下的 aig.size()
    uint32_t numPis    = 0;

    // ---- 拓撲快取(建一次,模擬時反覆掃)----
    // 不在內層迴圈呼叫 foreach_fanin:1.3M 節點 × 數十 word 的 lambda 呼叫
    // 成本非常可觀。攤平成陣列後內層是一條沒有分支的直線。
    std::vector<uint32_t> faninIdx;     // 2*i, 2*i+1
    std::vector<uint8_t>  faninCompl;   // 2*i, 2*i+1
    std::vector<uint8_t>  isLeaf;       // 常數節點與 PI
    std::vector<uint32_t> piNodeIdx;    // pi index -> node index

    // ---- 模擬 ----
    // word-major 佈局:sim[w * numNodes + i]。
    // 選 word-major 而非 node-major 的理由:反例回收要「追加一個 word」,
    // node-major 需要改 stride 並整份搬移,word-major 只要在尾巴 resize。
    // 而且單一 word 的模擬是對節點的線性掃描,cache 行為最好。
    std::vector<uint64_t> sim;
    uint32_t              words    = 0;   // 目前已用的 word 數
    uint32_t              maxWords = 0;   // 記憶體預算 clamp 後的上限
    std::vector<uint8_t>  simPhase;        // word0 的 bit0;決定 signature 正規化方向
    uint64_t              rng = 0x9E3779B97F4A7C15ull;

    // ---- 等價類 ----
    std::vector<Sig>                   repr;      // node index -> 代表 signal
    std::vector<uint8_t>               state;     // kOpen / kMerged / kGivenUp
    std::vector<std::vector<uint32_t>> classes;   // 每個 class 的成員(index 遞增)
    std::vector<uint32_t>              classOf;   // node index -> class id

    // ---- 反例緩衝 ----
    // pendingNodes 與 pendingCex 一一對齊:無法再吸收反例時要把這些節點
    // 標成放棄,否則下一輪掃描會再測一次同樣的配對 → 無窮迴圈。
    std::vector<std::vector<uint8_t>> pendingCex;
    std::vector<uint32_t>             pendingNodes;

    std::chrono::steady_clock::time_point t0;

    Impl(Ntk& a, SatEngine& s, Config c) : aig(a), sat(s), cfg(c) {}

    // ---------------------------------------------------------------------
    bool out_of_budget() const {
        if (cfg.total_time_budget <= 0.0) return false;
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - t0).count() >= cfg.total_time_budget;
    }

    Sig sig_of(uint32_t i) const { return aig.make_signal(aig.index_to_node(i)); }

    // 代表選擇順序:leaf(常數節點、PI)優先,同級取索引小者。
    //
    // leaf 一定要優先:代表永遠不會被併掉,所以「leaf 當代表」等價於
    //   「PI 永遠不會被併進內部節點」,而內部節點**可以**被併進 PI ——
    //   合成後 (a&b)|(a&!b) 這種化簡完等於 a 的節點很常見,DFF 的 Q 又是
    //   pseudo-PI,「這條 net 就等於某個暫存器輸出」正是會被問到的問題。
    //   若把 PI 排除在候選類外,這一整類等價會被靜默漏掉。
    //
    //   一個類最多只有一個 leaf:兩個相異 PI(或 PI 與常數)在數千組隨機向量下
    //   signature 相同的機率可以忽略,所以代表沒有歧義。
    bool rep_less(uint32_t a, uint32_t b) const {
        const int la = isLeaf[a] ? 0 : 1;
        const int lb = isLeaf[b] ? 0 : 1;
        if (la != lb) return la < lb;
        return a < b;
    }

    // ---------------------------------------------------------------------
    //  拓撲快取
    // ---------------------------------------------------------------------
    void build_topology() {
        numNodes = static_cast<uint32_t>(aig.size());

        isLeaf.assign(numNodes, 0);
        isLeaf[0] = 1;                       // 常數節點

        piNodeIdx.clear();
        aig.foreach_pi([&](Node const& n) {
            const uint32_t i = aig.node_to_index(n);
            if (i < numNodes) isLeaf[i] = 1;
            piNodeIdx.push_back(i);
        });
        numPis = static_cast<uint32_t>(piNodeIdx.size());

        faninIdx.assign(2ull * numNodes, 0);
        faninCompl.assign(2ull * numNodes, 0);

        for (uint32_t i = 1; i < numNodes; ++i) {
            if (isLeaf[i]) continue;
            uint32_t k = 0;
            aig.foreach_fanin(aig.index_to_node(i), [&](Sig const& s) {
                if (k < 2) {
                    faninIdx[2ull * i + k]   = aig.node_to_index(aig.get_node(s));
                    faninCompl[2ull * i + k] = aig.is_complemented(s) ? 1u : 0u;
                }
                ++k;
            });
        }
    }

    // ---------------------------------------------------------------------
    //  模擬
    // ---------------------------------------------------------------------
    //
    //  piWord == nullptr → PI 值用亂數(初始向量)
    //  piWord != nullptr → PI 值取自反例緩衝(回收向量)
    //
    //  bias:1 的密度。0 = 均勻(p=0.5);k>0 → p=2^-(k+1);k<0 → p=1-2^-(|k|+1)。
    uint64_t biased_word(int bias) {
        uint64_t w = xorshift64(rng);
        for (int i = 0; i < bias; ++i)  w &= xorshift64(rng);
        for (int i = 0; i > bias; --i)  w |= xorshift64(rng);
        return w;
    }

    void simulate_word(uint32_t w, const std::vector<uint64_t>* piWord, int bias = 0) {
        uint64_t* base = &sim[static_cast<size_t>(w) * numNodes];

        base[0] = 0ull;                       // 常數節點恆為 0
        for (uint32_t p = 0; p < numPis; ++p) {
            base[piNodeIdx[p]] = piWord ? (*piWord)[p] : biased_word(bias);
        }

        // node index 遞增 = 合法拓撲序(見檔頭)。
        for (uint32_t i = 1; i < numNodes; ++i) {
            if (isLeaf[i]) continue;
            const uint32_t i0 = faninIdx[2ull * i];
            const uint32_t i1 = faninIdx[2ull * i + 1];
            uint64_t v0 = base[i0];
            uint64_t v1 = base[i1];
            if (faninCompl[2ull * i])     v0 = ~v0;
            if (faninCompl[2ull * i + 1]) v1 = ~v1;
            base[i] = v0 & v1;
        }
    }

    void full_simulate(uint32_t nwords) {
        const auto s0 = std::chrono::steady_clock::now();
        words = nwords;
        sim.assign(static_cast<size_t>(words) * numNodes, 0ull);
        const uint32_t nBiased = 0;
        static const int kBias[8] = {-3, 3, -4, 4, -2, 2, -5, 5};

        for (uint32_t w = 0; w < words; ++w) {
            const int bias = (nBiased > 0 && w >= words - nBiased)
                           ? kBias[(w - (words - nBiased)) % 8]
                           : 0;
            simulate_word(w, nullptr, bias);
        }
        st.sim_seconds +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
        ++st.sim_rounds;

        // signature 的正規化方向由 word0 的 bit0 決定,而 word0 一旦產生就
        // 不再改變 → simPhase 只算一次,之後 repartition 直接沿用。
        simPhase.assign(numNodes, 0);
        const uint64_t* w0 = sim.data();
        for (uint32_t i = 0; i < numNodes; ++i)
            simPhase[i] = static_cast<uint8_t>(w0[i] & 1ull);
    }

    // 追加一個 word(反例回收)。回傳 false 表示已達記憶體上限。
    bool append_word(const std::vector<uint64_t>& piWord) {
        if (words >= maxWords) return false;
        const auto s0 = std::chrono::steady_clock::now();
        sim.resize(static_cast<size_t>(words + 1) * numNodes, 0ull);
        simulate_word(words, &piWord);
        ++words;
        st.sim_seconds +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
        ++st.sim_rounds;
        return true;
    }

    // 追加 extra 個均勻隨機字,只模擬新增的部分(自適應加碼用)。
    //
    // simPhase 不重算:它由 word 0 的 bit 0 決定,而 word 0 不會變。
    //   重算會讓所有 signature 的正規化方向改變,先前的分類全部作廢。
    void grow_simulation(uint32_t extra) {
        if (extra == 0) return;
        const auto s0 = std::chrono::steady_clock::now();
        const uint32_t old = words;
        sim.resize(static_cast<std::size_t>(old + extra) * numNodes, 0ull);
        for (uint32_t w = old; w < old + extra; ++w) simulate_word(w, nullptr, 0);
        words = old + extra;
        st.sim_seconds +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
        ++st.sim_rounds;
    }

    // 目前分類下,驗證階段最少要跑幾次 SAT:
    // 每個類都要把 size-1 個成員各對代表驗一次。
    uint64_t pending_pairs() const {
        uint64_t n = 0;
        for (const auto& c : classes) n += c.size() - 1;
        return n;
    }
    uint64_t sig_hash(uint32_t i) const {
        const bool ph = simPhase[i] != 0;
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t w = 0; w < words; ++w) {
            uint64_t v = sim[static_cast<size_t>(w) * numNodes + i];
            if (ph) v = ~v;
            h ^= v;
            h *= 0x100000001b3ull;
        }
        return h;
    }

    // hash 相同不代表 signature 相同。做一次精確比對,避免把 hash 碰撞的節點
    // 送進 SAT —— 那會白跑一次 solve 再靠反例把它們分開,純浪費。
    bool sig_equal(uint32_t a, uint32_t b) const {
        const bool pa = simPhase[a] != 0;
        const bool pb = simPhase[b] != 0;
        for (uint32_t w = 0; w < words; ++w) {
            uint64_t va = sim[static_cast<size_t>(w) * numNodes + a];
            uint64_t vb = sim[static_cast<size_t>(w) * numNodes + b];
            if (pa) va = ~va;
            if (pb) vb = ~vb;
            if (va != vb) return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------
    //  候選等價類
    // ---------------------------------------------------------------------
    void commit_classes(std::vector<std::vector<uint32_t>>& groups) {
        classes.clear();
        classOf.assign(numNodes, kNoClass);
        for (auto& g : groups) {
            if (g.size() < 2) continue;      // 單元素不是候選
            const uint32_t cid = static_cast<uint32_t>(classes.size());
            for (uint32_t n : g) classOf[n] = cid;
            classes.push_back(std::move(g));
        }
    }

    void build_classes() {
        // 候選節點 = 常數節點 + PI + 所有仍 Open 的內部節點。
        //
        // PI 納入是為了讓內部節點能被併進 PI(見 rep_less 的說明)。
        // 兩個相異 PI 永遠不會被證出等價,但它們的 signature 也幾乎不可能相同,
        // 所以根本不會湊成候選對 —— 納入 PI 的實際 SAT 成本接近零。
        std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;
        buckets.reserve(numNodes / 2 + 16);

        for (uint32_t i = 0; i < numNodes; ++i) {
            if (i == 0 && !cfg.use_constant_class) continue;
            if (i != 0 && !isLeaf[i] && state[i] != kOpen) continue;
            buckets[sig_hash(i)].push_back(i);
        }

        std::vector<std::vector<uint32_t>> groups;
        for (auto& kv : buckets) {
            auto& b = kv.second;
            if (b.size() < 2) continue;
            std::sort(b.begin(), b.end(),
                      [&](uint32_t x, uint32_t y) { return rep_less(x, y); });

            // 同一個 hash bucket 內再依精確 signature 細分。
            std::vector<char> taken(b.size(), 0);
            for (size_t x = 0; x < b.size(); ++x) {
                if (taken[x]) continue;
                std::vector<uint32_t> g{b[x]};
                for (size_t y = x + 1; y < b.size(); ++y) {
                    if (taken[y]) continue;
                    if (sig_equal(b[x], b[y])) { taken[y] = 1; g.push_back(b[y]); }
                }
                if (g.size() >= 2) groups.push_back(std::move(g));
            }
        }
        commit_classes(groups);
    }

    // 追加一個 word 之後,只用「新 word」把現有類切開。
    // 不必重算整條 signature —— 新增位元只會分裂,不會合併。
    void repartition_last_word() {
        const uint64_t* base = &sim[static_cast<size_t>(words - 1) * numNodes];
        std::vector<std::vector<uint32_t>> groups;

        for (auto& c : classes) {
            std::unordered_map<uint64_t, std::vector<uint32_t>> split;
            for (uint32_t n : c) {
                if (!isLeaf[n] && n != 0 && state[n] != kOpen) continue;
                uint64_t v = base[n];
                if (simPhase[n]) v = ~v;
                split[v].push_back(n);
            }
            for (auto& kv : split) {
                if (kv.second.size() < 2) continue;
                std::sort(kv.second.begin(), kv.second.end(),
                          [&](uint32_t x, uint32_t y) { return rep_less(x, y); });
                groups.push_back(std::move(kv.second));
            }
        }
        commit_classes(groups);
    }

    // ---------------------------------------------------------------------
    //  反例回收
    // ---------------------------------------------------------------------
    //
    //  把 buffer 裡最多 64 個反例併成一個 word:第 k 個反例佔第 k 個位元。
    //  沒填滿的位元用亂數補,不浪費這個 word 的模擬頻寬。
    bool flush_cex() {
        if (pendingCex.empty()) return false;

        std::vector<uint64_t> piWord(numPis, 0ull);
        for (uint32_t p = 0; p < numPis; ++p) piWord[p] = xorshift64(rng);

        const uint32_t n = static_cast<uint32_t>(
            std::min<size_t>(pendingCex.size(), kCexPerWord));
        for (uint32_t k = 0; k < n; ++k) {
            const auto& cx = pendingCex[k];
            const uint64_t bit  = 1ull << k;
            const uint64_t mask = ~bit;
            for (uint32_t p = 0; p < numPis; ++p) {
                const bool v = (p < cx.size()) && (cx[p] != 0);
                piWord[p] = (piWord[p] & mask) | (v ? bit : 0ull);
            }
        }
        // append 失敗時必須保留 pendingNodes，讓呼叫端的
        // abandon_pending() 能將這批候選標成 GivenUp。若先清空，這些
        // 候選會保持 Open，下一輪便會重做同一批 SAT 直到總時限耗盡。
        if (!append_word(piWord)) return false;   // 記憶體上限
        pendingCex.clear();
        pendingNodes.clear();
        repartition_last_word();
        return true;
    }

    // 無法再吸收反例時(額度用盡或模擬字數達上限),把緩衝裡的節點全部標成放棄。
    // 不做這件事,下一輪掃描會重測同樣的配對,而反例又進不去 → 無窮迴圈。
    void abandon_pending() {
        for (const uint32_t n : pendingNodes) {
            if (state[n] == kOpen) { state[n] = kGivenUp; ++st.gave_up; }
        }
        pendingCex.clear();
        pendingNodes.clear();
    }

    // ---------------------------------------------------------------------
    //  合併
    // ---------------------------------------------------------------------
    void merge_into(uint32_t member, Sig target) {
        repr[member] = target;
        state[member] = kMerged;
        ++st.merged_nodes;
        if (aig.get_node(target) == aig.get_node(aig.get_constant(false)))
            ++st.const_nodes;
    }

    // ---------------------------------------------------------------------
    //  主迴圈
    // ---------------------------------------------------------------------
    void run() {
        t0 = std::chrono::steady_clock::now();
        st.completed = false;

        build_topology();
        if (numNodes == 0) { st.completed = true; swept = true; return; }

        // 記憶體預算反推字數。sim 是 words * numNodes 個 uint64,
        // 100 萬 gate(約 1.3M 節點)照預設的 32 word 會吃掉 333MB。
        // budget == 0 表示不設限,完全聽 sim_words_max 的。
        const uint64_t perWord = 8ull * numNodes;
        uint32_t byBudget = cfg.sim_words_max;
        if (cfg.sim_memory_budget_bytes > 0) {
            byBudget = static_cast<uint32_t>(std::max<uint64_t>(
                1ull, cfg.sim_memory_budget_bytes / std::max<uint64_t>(1ull, perWord)));
        }
        maxWords = std::max(1u, std::min(cfg.sim_words_max, byBudget));
        const uint32_t initWords = std::max(1u, std::min(cfg.sim_words_init, maxWords));

        repr.resize(numNodes);
        for (uint32_t i = 0; i < numNodes; ++i) repr[i] = sig_of(i);
        state.assign(numNodes, kOpen);

        full_simulate(initWords);
        build_classes();

        // ---- 自適應向量數 ----
        //
        // 候選就是「在目前向量下 signature 相同、但可能不等價」的配對,
        // 每一對都要花一次 SAT 去判決。向量越多,假候選越少。
        //
        // 實測 112k gate:
        //     32 字(2048 向量) → 12790 候選,SAT 13.5s
        //    128 字(8192 向量) →  1938 候選,SAT  1.04s
        // 四倍向量換到十三倍的 SAT 加速 —— 但那個 128 是手調出來的,
        // 換一顆電路就不一定對。這裡改成從 sim_words_init 起跳、
        // 依實際候選數倍增,讓小電路不必付大電路的代價。
        //
        // 兩個停止條件:
        //   1. 候選已經夠少(相對節點數)
        //   2. 加倍向量之後候選沒有明顯下降 → 剩下的都是真正近似等價的配對,
        //      再加字也只是浪費模擬與分類的時間,那些本來就該交給 SAT
        //
        // 上限是 maxWords/2,另一半留給反例回收。用光的話 append_word 會失敗,
        // 所有反例無處可去,直接變成 gave_up。
        // simulate_only 模式不做自適應。
        //   自適應是為了減少候選、進而減少 SAT 呼叫 —— 而那個模式根本不跑 SAT。
        //   多出來的向量只讓 provably_different 多幾個命中(實測 396/400 → 400/400),
        //   卻要付 2.5 倍的模擬與分類成本(0.20s → 0.51s)。
        //   那 4 個 miss 退回 SAT 只要 24ms,遠比 0.31s 便宜。
        if (cfg.adaptive_sim && !cfg.simulate_only) {
            const uint64_t target  = std::max<uint64_t>(64ull, numNodes / 50ull);
            const uint32_t growCap = std::max(initWords, maxWords / 2u);

            while (words < growCap) {
                const uint64_t before = pending_pairs();
                if (before <= target) break;

                const uint32_t add = std::min(words, growCap - words);
                if (add == 0) break;
                grow_simulation(add);
                build_classes();

                // 下降不到三成 → 收斂了,停手。
                if (pending_pairs() * 10 > before * 7) break;
            }
        }

        // ---- simulate_only:只做模擬與分類,跳過整段 SAT 驗證 ----
        //
        // 依據:signature 不同 ⇒ 存在一組模擬向量使兩者取值不同 ⇒ 必定不等價。
        // 那是一個實實在在的反例,與 sweep 有沒有跑完全無關。
        // 而真實 workload 裡「不等價」是絕大多數查詢的答案,
        // 所以這個模式用約 1/30 的成本換到近乎全部的查詢加速。
        //
        // completed 必須維持 false:相同 signature 的候選**沒有**被驗證過,
        //   fraig_complete() 一旦為 true,Primitives 會把「同類但未合併」
        //   誤讀成不等價 —— 那是靜默的錯誤答案。
        //   否定結論一律走 provably_different(),不走 completed 那條路。
        if (cfg.simulate_only) {
            st.sat_seconds += 0.0;
            st.completed = false;
            swept = true;
            return;
        }

        sat.set_limits(cfg.sat_time_limit, cfg.sat_conflict_limit);
        const double satBefore = sat.stats().solve_seconds;

        uint32_t rounds = 0;
        bool     finished = false;

        while (true) {
            if (out_of_budget()) break;

            bool refined = false;   // 本輪是否因反例而需要重新分類

            // 由小到大掃:低層節點先合併,其永久子句能讓上層的 SAT 更省力。
            for (uint32_t i = 0; i < numNodes && !refined; ++i) {
                if (isLeaf[i]) continue;          // leaf 永遠是代表,不會被併掉
                if (state[i] != kOpen) continue;
                const uint32_t cid = classOf[i];
                if (cid == kNoClass) continue;

                const uint32_t rep = classes[cid][0];
                if (rep == i) continue;                  // 自己是代表
                if (!isLeaf[rep] && state[rep] != kOpen) continue;

                if (out_of_budget()) { refined = false; goto done; }

                ++st.candidate_pairs;

                // phase:同類中 simPhase 相異者,候選關係是 rep ≡ ¬i。
                const bool opposite = (simPhase[rep] != simPhase[i]);

                EquivResult r;
                if (rep == 0) {
                    // 常數類:node 0 的 simPhase 恆為 0,故 opposite == simPhase[i],
                    // 也就是「i 的候選常數值」。用 is_const 比 XOR 輔助變數便宜。
                    r = sat.is_const(sig_of(i), opposite);
                } else {
                    const Sig a = sig_of(rep);
                    const Sig b = opposite ? !sig_of(i) : sig_of(i);
                    r = sat.are_equal(a, b);
                }

                if (r == EquivResult::Equal) {
                    ++st.proved_equal;
                    if (rep == 0) {
                        const bool val = opposite;
                        sat.assert_const(sig_of(i), val);
                        merge_into(i, aig.get_constant(val));
                    } else {
                        const Sig target = opposite ? !sig_of(rep) : sig_of(rep);
                        sat.assert_equal(sig_of(i), target);
                        merge_into(i, target);
                    }
                } else if (r == EquivResult::NotEqual) {
                    ++st.refuted;
                    const Counterexample& cx = sat.last_counterexample();
                    if (cx.valid) {
                        pendingCex.push_back(cx.values);
                        pendingNodes.push_back(i);
                    } else {
                        ++st.gave_up;
                        state[i] = kGivenUp;   // 沒反例可回收,只能放棄
                    }

                    if (pendingCex.size() >= kCexPerWord) {
                        // 反例回收:一口氣切開所有候選類,而不是只處理這一對。
                        //
                        // 額度檢查必須放在 flush **之前**。放在之後的話,
                        // 最後一次 flush 會立刻 break,連「確認已經沒有候選」
                        // 的那一趟掃描都跑不到 —— 明明全部解完,卻回報
                        // completed=false,而 fraig_complete() 一旦是 false,
                        // 所有否定結論都會退回 SAT,查表的效益整個消失。
                        //
                        // 額度用盡時改成「放棄這些配對」,由 gave_up 誠實反映,
                        // 而不是讓整趟 sweep 看起來沒跑完。
                        if (rounds >= cfg.max_rounds) {
                            abandon_pending();
                        } else if (flush_cex()) {
                            ++rounds;
                            refined = true;
                        } else {
                            abandon_pending();     // 已達模擬字數上限
                        }
                    }
                } else {
                    // Unknown:打到 time / conflict limit。保守起見不合併。
                    ++st.gave_up;
                    state[i] = kGivenUp;
                }
            }

            // 重新分類之後從頭掃(見下方說明)。
            // 這裡不再檢查 max_rounds —— 額度已經在 flush 之前擋過了。
            if (refined) continue;

            // 掃完一輪且沒觸發 flush:把殘餘反例吐出去再確認一次。
            if (!pendingCex.empty()) {
                if (rounds < cfg.max_rounds && flush_cex()) {
                    ++rounds;
                    continue;
                }
                abandon_pending();
                continue;                    // 讓下一趟掃描確認真的沒有候選了
            }

            finished = true;                 // 沒有未決候選了
            break;
        }

    done:
        st.sat_seconds += sat.stats().solve_seconds - satBefore;
        st.completed = finished && !out_of_budget();
        swept = true;
    }
};

// -----------------------------------------------------------------------------
//  每次 repartition 之後要「從頭掃」而不是接著往下:
//
//    分裂只會把一個類切小,但切出來的新子類可能由兩個「索引都在游標之前」的
//    節點組成 —— 那是一組全新的、還沒測過的候選對。若游標只往前走就會漏掉。
//    重掃的成本很低:已 Merged / GivenUp 的節點是 O(1) 跳過。
//
//    終止性:每個反例都保證能把「剛被否證的那一對」分開(反例就是使兩者取值
//    不同的賦值),所以同一對不會被反覆送進 SAT。加上 max_rounds 與時間預算,
//    迴圈必定結束。
// -----------------------------------------------------------------------------

// =============================================================================
//  Fraig
// =============================================================================

Fraig::Fraig(Ntk& aig, SatEngine& sat, Config cfg)
    : impl_(std::make_unique<Impl>(aig, sat, cfg)) {}

Fraig::~Fraig() = default;

void Fraig::sweep() { impl_->run(); }

bool Fraig::is_swept() const { return impl_->swept; }

uint64_t Fraig::sweep_watermark() const { return impl_->numNodes; }

bool Fraig::is_swept_node(Node n) const {
    return impl_->swept && impl_->aig.node_to_index(n) < impl_->numNodes;
}

// canonical(s) = repr[node(s)] ^ is_complemented(s)
//
// 未經 sweep、或 sweep 之後才長出來的節點(cofactor 物化)一律回傳 s 自身 ——
// 呼叫端因此必須先用 is_swept_node 判斷,否則會把「還沒分析過」誤讀成
// 「已分析且自成一類」。
Sig Fraig::representative(Sig s) const {
    const Impl& im = *impl_;
    if (!im.swept) return s;
    const uint32_t i = im.aig.node_to_index(im.aig.get_node(s));
    if (i >= im.numNodes) return s;
    const Sig r = im.repr[i];
    return im.aig.is_complemented(s) ? !r : r;
}

bool Fraig::same_class(Sig a, Sig b) const {
    return representative(a) == representative(b);
}

// a、b 是否已經被某一組模擬向量分開。
//
// 這是**不需要 SAT 的否定證明**:兩者在某組向量下取值不同,那組向量
//   本身就是反例。這個判斷與 sweep 有沒有跑完、有沒有驗證過完全無關,
//   所以即使是 simulate_only 模式、或預算中途耗盡,它依然可信。
//
// 回傳 false 只代表「模擬沒能分開」,不代表等價 —— 那時仍須 SAT。
bool Fraig::provably_different(Sig a, Sig b) const {
    const Impl& im = *impl_;
    if (!im.swept || im.sim.empty() || im.words == 0) return false;

    const uint32_t ia = im.aig.node_to_index(im.aig.get_node(a));
    const uint32_t ib = im.aig.node_to_index(im.aig.get_node(b));
    if (ia >= im.numNodes || ib >= im.numNodes) return false;   // sweep 之後才長出來的節點

    const bool ca = im.aig.is_complemented(a);
    const bool cb = im.aig.is_complemented(b);
    if (ia == ib) return ca != cb;      // 同節點反相 → 必定不同

    for (uint32_t w = 0; w < im.words; ++w) {
        uint64_t va = im.sim[static_cast<std::size_t>(w) * im.numNodes + ia];
        uint64_t vb = im.sim[static_cast<std::size_t>(w) * im.numNodes + ib];
        if (ca) va = ~va;
        if (cb) vb = ~vb;
        if (va != vb) return true;
    }
    return false;
}

bool Fraig::is_known_const(Sig s, bool& val) const {
    const Impl& im = *impl_;
    const Sig r = representative(s);
    if (im.aig.get_node(r) != im.aig.get_node(im.aig.get_constant(false)))
        return false;
    val = im.aig.is_complemented(r);
    return true;
}

const std::vector<Sig>& Fraig::representatives_by_node() const {
    return impl_->repr;
}

std::vector<EquivClass> Fraig::classes(int min_size) const {
    const Impl& im = *impl_;
    std::vector<EquivClass> out;
    if (!im.swept) return out;

    // 依「代表 signal」分組。常數 0 與常數 1 會落在不同組(代表分別是
    // get_constant(false) 與 get_constant(true)),這是正確的 —— 它們是
    // 兩個不同的等價類。
    std::unordered_map<uint64_t, size_t> where;
    for (uint32_t i = 0; i < im.numNodes; ++i) {
        const Sig self = im.sig_of(i);
        const Sig r    = im.repr[i];
        if (r == self) continue;                 // 自己就是代表,不算成員

        auto it = where.find(r.data);
        if (it == where.end()) {
            where.emplace(r.data, out.size());
            out.push_back(EquivClass{r, {self}});
        } else {
            out[it->second].members.push_back(self);
        }
    }

    if (min_size > 1) {
        const size_t need = static_cast<size_t>(min_size) - 1;   // 代表本身佔一個
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const EquivClass& c) {
                                     return c.members.size() < need;
                                 }),
                  out.end());
    }
    return out;
}

const Fraig::Stats& Fraig::stats() const { return impl_->st; }

} // namespace eqeng
