// =============================================================================
//  SatEngine.cpp  —  常駐 incremental CaDiCaL + lazy COI Tseitin encoding
// =============================================================================
//  設計摘要
//  --------
//  1. 一顆 CaDiCaL 實例全程存活,學到的 clause 跨 query 累積。
//  2. lazy encode:只把被查詢到的 fanin cone 編進 CNF。100 萬 gate 不可能
//     全部 encode,而絕大多數 query 只碰到很小的 cone。
//  3. 每個 AND 節點固定 3 clause;常數節點一顆 unit clause。
//  4. 等價 query 用一顆 XOR 輔助變數 + assume,UNSAT → 等價。
//     輔助變數建在「node pair」上而非「signal pair」上,見 xor_var()。
//  5. 所有變數配置當下就 freeze,避免 CaDiCaL 的 inprocessing 把它消掉。
//
//  與 do_build 的耦合點
//  ----------------------
//  AigBuilder::do_build 把所有 create_pi() 集中在任何 create_and() 之前,
//  因此 PI 的 node index 連續且位於底部。本檔**不依賴**這個性質(改用
//  foreach_pi 建映射),但依賴另一個更基本的性質:
//
//      mockturtle 的 create_and 保證 fanin 的 node index 恆小於自身。
//
//  這讓 encode 完全不需要拓撲排序(見 encode_cone 的說明)。
//  只要 Fraig 維持「不呼叫 substitute_node」的非破壞性設計,此性質恆成立。
//
// =============================================================================

#include "include/SATEngine/SatEngine.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "include/lib/cadical/src/cadical.hpp"

namespace eqeng {

namespace {

// -----------------------------------------------------------------------------
//  可重複 arm 的 wall-clock terminator
// -----------------------------------------------------------------------------
class DeadlineTerminator final : public CaDiCaL::Terminator {
public:
    void arm(double limitSeconds) {
        armed_    = (limitSeconds > 0.0);
        fired_    = false;
        counter_  = 0;
        if (armed_) {
            deadline_ = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(limitSeconds));
        }
    }

    void disarm() { armed_ = false; fired_ = false; }

    bool terminate() override {
        if (!armed_) return false;
        // 每 1024 次才真的讀系統時鐘,降低 clock_gettime 的呼叫成本。
        if ((++counter_ & 0x3FFu) != 0u) return fired_;
        fired_ = std::chrono::steady_clock::now() >= deadline_;
        return fired_;
    }

    bool fired() const { return fired_; }

private:
    std::chrono::steady_clock::time_point deadline_{};
    bool     armed_   = false;
    bool     fired_   = false;
    unsigned counter_ = 0;
};

constexpr uint32_t kNoPi = 0xFFFFFFFFu;

// CaDiCaL 的回傳碼
constexpr int kSat   = 10;
constexpr int kUnsat = 20;

inline uint64_t pair_key(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
}

// 已證等價的 phase 對齊方式:
//   0 = 兩個 signal 的 complement 相同時等價
//   1 = complement 不同時等價
// 兩者互斥(一個節點不可能同時等價於另一個節點及其反相)。
// 對 (ca, cb) 對稱,所以與 pair_key 的排序無關。
inline int8_t parity_of(bool ca, bool cb) { return (ca == cb) ? 0 : 1; }

} // namespace

// =============================================================================
//  Impl
// =============================================================================

struct SatEngine::Impl {
    const Ntk&          aig;
    Config              cfg;
    CaDiCaL::Solver     solver;
    DeadlineTerminator  term;

    // node index -> CNF var(0 = 尚未配置)
    std::vector<int>     nodeVar;
    // node index -> 定義子句是否已寫出。與 nodeVar 分開:一個節點可能因為
    // 身為別人的 fanin 而先被配置變數,但定義還沒寫。**任何進到 CNF 的變數
    // 都必須有定義**,否則它是自由變數,會產生假的 SAT / 假的反例。
    std::vector<uint8_t> defined;
    // node index -> pi index(kNoPi = 非 PI)。與 AigModel::pi_net_ids() 同序。
    std::vector<uint32_t> nodeToPi;
    uint32_t              numPis = 0;

    // (nodeA, nodeB) -> XOR 輔助變數。key 已排序,同一對只建一次。
    std::unordered_map<uint64_t, int> xorVar;

    // 無條件證明帳本:(nodeA, nodeB) -> 已證明的 phase 對齊方式。
    //
    //   只有「沒有任何 assumption 的 UNSAT」才會寫進來。are_equal_under 的
    //   條件式結論**永遠不會**留下紀錄,所以 assert_equal 查不到它。
    //
    //   為什麼是 pair 而不是「上一次 solve 有沒有帶 assumption」的旗標:
    //   旗標記的是時間,assert_equal(a,b) 問的是這一對。中間插進任何一次
    //   無條件查詢就會把旗標洗掉,讓先前的條件式結論看起來合法 ——
    //   而 FRAIG 主迴圈恰恰是交錯呼叫的。
    //
    //   容量:只在 Equal 時寫入,上限為實際證出的等價對數(FRAIG 的 merge 數)。
    std::unordered_map<uint64_t, int8_t> proven;

    void record_proof(Node na, bool ca, Node nb, bool cb) {
        if (na == nb) return;                       // 結構上同節點,不需帳本
        proven[pair_key(aig.node_to_index(na), aig.node_to_index(nb))] =
            parity_of(ca, cb);
    }

    bool has_proof(Node na, bool ca, Node nb, bool cb) const {
        const auto it = proven.find(
            pair_key(aig.node_to_index(na), aig.node_to_index(nb)));
        return it != proven.end() && it->second == parity_of(ca, cb);
    }

    int  nextVar = 0;
    int  reservedVar = 0;      // 已經向 solver 宣告過的最大變數編號
    bool eager   = false;

    Counterexample cex;
    Stats          st;

    std::vector<Node> stack;     // encode 用,重複使用避免反覆配置
    uint64_t          rng = 0x9E3779B97F4A7C15ull;

    explicit Impl(const Ntk& a, Config c) : aig(a), cfg(c) {
        eager = !cfg.lazy_encode;

        // 必須關掉 factor
        solver.set("factor", 0);

        solver.connect_terminator(&term);
        term.disarm();

        const uint32_t n = static_cast<uint32_t>(aig.size());
        nodeVar.assign(n, 0);
        defined.assign(n, 0);
        nodeToPi.assign(n, kNoPi);

        aig.foreach_pi([&](Node const& node, uint32_t index) {
            const uint32_t idx = aig.node_to_index(node);
            if (idx < nodeToPi.size()) nodeToPi[idx] = index;
            numPis = std::max(numPis, index + 1u);
        });

        cex.values.assign(numPis, 0);
        cex.valid = false;

        if (eager) encode_all();
    }

    ~Impl() { solver.disconnect_terminator(); }

    // ---------------------------------------------------------------------
    //  變數配置
    // ---------------------------------------------------------------------
    int new_var() {
        const int v = ++nextVar;

        // 先向 solver 明確宣告這是 user variable,再 freeze。
        if (v > reservedVar) {
            reservedVar = v + 4095;
            solver.resize(reservedVar);
        }

        // freeze:CaDiCaL 會在兩次 solve 之間做 variable elimination。
        solver.freeze(v);
        return v;
    }

    int var_of(Node n) {
        const uint32_t idx = aig.node_to_index(n);
        if (idx >= nodeVar.size()) grow();
        int& v = nodeVar[idx];
        if (v == 0) {
            v = new_var();
            if (aig.is_constant(n)) {
                // node 0 恆為 false。於是 get_constant(false) 的 literal 為 +v
                // (恆假),get_constant(true) 為 -v(恆真),兩者自動正確。
                add_unit(-v);
                defined[idx] = 1;
            }
        }
        return v;
    }

    int lit_of(Sig s) {
        const int v = var_of(aig.get_node(s));
        return aig.is_complemented(s) ? -v : v;
    }

    void grow() {
        const uint32_t n = static_cast<uint32_t>(aig.size());
        if (n <= nodeVar.size()) return;
        nodeVar.resize(n, 0);
        defined.resize(n, 0);
        nodeToPi.resize(n, kNoPi);   // 新節點必為 AND,不會是 PI
    }

    // ---------------------------------------------------------------------
    //  clause 輸出
    // ---------------------------------------------------------------------
    void add_unit(int a) {
        solver.add(a); solver.add(0);
        ++st.clauses;
    }
    void add_bin(int a, int b) {
        solver.add(a); solver.add(b); solver.add(0);
        ++st.clauses;
    }
    void add_ter(int a, int b, int c) {
        solver.add(a); solver.add(b); solver.add(c); solver.add(0);
        ++st.clauses;
    }

    // ---------------------------------------------------------------------
    //  encode
    // ---------------------------------------------------------------------
    //
    //  為什麼不需要拓撲排序:
    //    CNF 是一個 clause 的「集合」,寫出的順序不影響語意。encode 節點 m 時
    //    引用 fanin 的變數,那些 fanin 隨後也會被推進 stack 並寫出自己的定義。
    //    唯一必須保證的不變量是:**任何被引用的變數,最終都會有定義**
    //    (常數與 PI 例外 —— 常數有 unit clause,PI 本來就是自由變數)。
    //    下面的 DFS 靠「先標 defined 再推 fanin」達成這件事。
    //
    //  用顯式 stack 而非遞迴:100 萬 gate 的 fanin cone 遞迴必定爆 stack。
    void encode_cone(Node root) {
        grow();
        const uint32_t rootIdx = aig.node_to_index(root);
        if (defined[rootIdx]) return;

        stack.clear();
        stack.push_back(root);

        while (!stack.empty()) {
            const Node m   = stack.back();
            const uint32_t idx = aig.node_to_index(m);
            stack.pop_back();

            if (defined[idx]) continue;
            defined[idx] = 1;

            const int lm = var_of(m);       // 配置(常數會在此寫 unit clause)

            if (aig.is_constant(m)) continue;
            // fanin_size == 0 → PI(含 DFF-Q 的 pseudo-PI 與懸空輸入的自由 PI)。
            // 刻意不用 is_pi():跨 mockturtle 版本 fanin_size 最穩。
            if (aig.fanin_size(m) == 0) continue;

            Sig f[2];
            uint32_t k = 0;
            aig.foreach_fanin(m, [&](Sig const& s) {
                if (k < 2) f[k] = s;
                ++k;
            });
            assert(k == 2 && "aig_network node must have exactly 2 fanins");

            const int l0 = lit_of(f[0]);
            const int l1 = lit_of(f[1]);

            // m <-> (f0 & f1):3 clause
            add_bin(-lm, l0);
            add_bin(-lm, l1);
            add_ter(lm, -l0, -l1);
            ++st.encoded_nodes;

            stack.push_back(aig.get_node(f[0]));
            stack.push_back(aig.get_node(f[1]));
        }
    }

    void encode_all() {
        grow();
        aig.foreach_node([&](Node const& n) { encode_cone(n); });
    }

    // ---------------------------------------------------------------------
    //  XOR 輔助變數
    // ---------------------------------------------------------------------
    //
    //  關鍵技巧:輔助變數建在 node pair 上,不是 signal pair 上。
    //
    //    定義 x <-> (na XOR nb),其中 na/nb 是「不含 complement 的」node 變數。
    //    對任意 signal a=(na,ca)、b=(nb,cb):
    //        a XOR b = x XOR (ca XOR cb)
    //    所以查詢時只要看 phase 的奇偶決定 assume(x) 還是 assume(-x)。
    //
    //    好處:問「a 等於 b 嗎」和「a 等於 !b 嗎」共用同一顆輔助變數與同一組
    //    定義子句。FRAIG 對同一個候選類反覆試 phase 時省下一半的變數。
    int xor_var(Node na, Node nb) {
        const uint32_t ia = aig.node_to_index(na);
        const uint32_t ib = aig.node_to_index(nb);
        const uint64_t key = pair_key(ia, ib);

        auto it = xorVar.find(key);
        if (it != xorVar.end()) return it->second;

        encode_cone(na);
        encode_cone(nb);

        const int p = var_of(na);
        const int q = var_of(nb);
        const int x = new_var();

        // x <-> (p XOR q):4 clause
        add_ter(-x,  p,  q);
        add_ter(-x, -p, -q);
        add_ter( x, -p,  q);
        add_ter( x,  p, -q);

        xorVar.emplace(key, x);
        return x;
    }

    // ---------------------------------------------------------------------
    //  solve 包裝
    // ---------------------------------------------------------------------
    int run_solve() {
        term.arm(cfg.time_limit_sec);
        if (cfg.conflict_limit > 0) {
            solver.limit("conflicts", static_cast<int>(cfg.conflict_limit));
        } else {
            solver.limit("conflicts", -1);
        }

        const auto t0 = std::chrono::steady_clock::now();
        const int  r  = solver.solve();
        const auto t1 = std::chrono::steady_clock::now();

        term.disarm();
        st.solve_seconds += std::chrono::duration<double>(t1 - t0).count();
        ++st.queries;

        if      (r == kUnsat) ++st.unsat;
        else if (r == kSat)   ++st.sat;
        else                  ++st.unknown;

        return r;
    }

    uint64_t next_rand() {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        return rng;
    }

    // SAT 之後把 model 讀成以 pi_index 為索引的反例。
    //
    // 未被 encode 到的 PI(不在本次查詢的 COI 裡)在 solver 眼中不存在,
    //   其值是 don't-care。這裡填**隨機值**而非固定 0:FRAIG 會把反例當新的
    //   模擬向量回收,若一律填 0,累積的向量會系統性偏斜,refine 效果變差。
    void capture_cex() {
        cex.values.assign(numPis, 0);
        for (uint32_t idx = 0; idx < nodeToPi.size(); ++idx) {
            const uint32_t pi = nodeToPi[idx];
            if (pi == kNoPi || pi >= numPis) continue;
            const int v = nodeVar[idx];
            if (v == 0) {
                cex.values[pi] = static_cast<uint8_t>(next_rand() & 1u);
            } else {
                cex.values[pi] = (solver.val(v) > 0) ? 1u : 0u;
            }
        }
        cex.valid = true;
    }

    void clear_cex() { cex.valid = false; }
};

// =============================================================================
//  SatEngine
// =============================================================================

SatEngine::SatEngine(const Ntk& aig, Config cfg)
    : impl_(std::make_unique<Impl>(aig, cfg)) {}

SatEngine::~SatEngine() = default;

void SatEngine::sync() {
    impl_->grow();
    if (impl_->eager) impl_->encode_all();
}

bool SatEngine::is_encoded(Node n) const {
    const uint32_t idx = impl_->aig.node_to_index(n);
    return idx < impl_->defined.size() && impl_->defined[idx] != 0;
}

void SatEngine::ensure_encoded(Node n) { impl_->encode_cone(n); }
void SatEngine::ensure_encoded(Sig s)  { impl_->encode_cone(impl_->aig.get_node(s)); }

const Counterexample& SatEngine::last_counterexample() const { return impl_->cex; }
const SatEngine::Stats& SatEngine::stats() const { return impl_->st; }
void SatEngine::reset_stats() { impl_->st = Stats{}; }

// -----------------------------------------------------------------------------
//  are_equal
// -----------------------------------------------------------------------------
EquivResult SatEngine::are_equal(Sig a, Sig b) {
    Impl& im = *impl_;
    im.grow();
    im.clear_cex();

    const Node na = im.aig.get_node(a);
    const Node nb = im.aig.get_node(b);
    const bool ca = im.aig.is_complemented(a);
    const bool cb = im.aig.is_complemented(b);

    // 結構捷徑:同一個節點時答案由 phase 決定,連 CNF 都不用碰。
    // (AIG 節點不可能等價於自己的反相,所以 phase 不同必為 NotEqual。)
    if (na == nb) return (ca == cb) ? EquivResult::Equal : EquivResult::NotEqual;

    const int x = im.xor_var(na, nb);
    // a XOR b = x XOR (ca XOR cb);問「是否可能不同」→ assume 該式為真。
    im.solver.assume((ca == cb) ? x : -x);

    const int r = im.run_solve();
    if (r == kUnsat) {
        im.record_proof(na, ca, nb, cb);   // 無條件證明 → 可供 assert_equal 使用
        return EquivResult::Equal;
    }
    if (r == kSat)  { im.capture_cex(); return EquivResult::NotEqual; }
    return EquivResult::Unknown;
}

// -----------------------------------------------------------------------------
//  is_const
// -----------------------------------------------------------------------------
EquivResult SatEngine::is_const(Sig a, bool val) {
    Impl& im = *impl_;
    im.grow();
    im.clear_cex();

    const Node na = im.aig.get_node(a);
    if (im.aig.is_constant(na)) {
        // node 0 為 false;complement 決定實際值。
        const bool actual = im.aig.is_complemented(a);
        return (actual == val) ? EquivResult::Equal : EquivResult::NotEqual;
    }

    im.encode_cone(na);
    // 問「是否存在賦值使 a != val」。不需要輔助變數 —— 這是最便宜的 query,
    // FRAIG 掃常數類時會大量呼叫。
    const int la = im.lit_of(a);
    im.solver.assume(val ? -la : la);

    const int r = im.run_solve();
    if (r == kUnsat) {
        // 「a 恆為 val」就是「a 等價於 get_constant(val)」,而 get_constant(val)
        // 是 node 0 帶 complement=val。用同一本帳記,assert_const 便能共用。
        im.record_proof(na, im.aig.is_complemented(a),
                        im.aig.get_node(im.aig.get_constant(false)), val);
        return EquivResult::Equal;
    }
    if (r == kSat)  { im.capture_cex(); return EquivResult::NotEqual; }
    return EquivResult::Unknown;
}

// -----------------------------------------------------------------------------
//  are_equal_under
// -----------------------------------------------------------------------------
//
//  語意警告:assumptions 的 Sig 應該是 PI(自由變數)。
//    對 PI 取 assumption 才真的等於 cofactor。
//
//    對「內部節點」下 assumption 在邏輯上仍然 sound(UNSAT 表示「在該節點
//    為此值的所有情況下 a≡b」),但**那不是 cofactor**,因為內部節點的值受
//    其 fanin 約束,不能自由指定。L3 若拿它當 cofactor 用會得到看似合理的
//    錯誤結論。這裡不強制擋下(擋下會讓某些合法的條件式查詢寫不出來),
//    但呼叫端請自行確保是 PI —— Primitives::equiv_under 應先做 is_free_var 檢查。
EquivResult SatEngine::are_equal_under(
        Sig a, Sig b, const std::vector<std::pair<Sig, bool>>& assumptions) {
    Impl& im = *impl_;
    im.grow();
    im.clear_cex();

    const Node na = im.aig.get_node(a);
    const Node nb = im.aig.get_node(b);
    const bool ca = im.aig.is_complemented(a);
    const bool cb = im.aig.is_complemented(b);

    // assumption 訊號也要進 CNF,否則 assume 一個 solver 不認識的變數。
    for (const auto& kv : assumptions) im.encode_cone(im.aig.get_node(kv.first));

    // 先檢查 assumption 本身是否自相矛盾(同一 signal 被指定成兩個值,
    // 或對常數指定了錯誤的值)。矛盾的 assumption 會讓 solver 回 UNSAT,
    // 而 UNSAT 在這裡被解讀成「等價」—— 那是**憑空冒出來的假證明**。
    std::unordered_map<uint64_t, bool> forced;
    for (const auto& kv : assumptions) {
        const Sig  s  = kv.first;
        const bool v  = kv.second;
        const Node n  = im.aig.get_node(s);
        const bool cs = im.aig.is_complemented(s);

        if (im.aig.is_constant(n)) {
            if (cs != v) return EquivResult::Unknown;   // 條件恆假,無從判斷
            continue;
        }
        // 統一換算成「node 的值」再比對,避免 s 與 !s 被當成兩個不同對象。
        const bool nodeVal = (v != cs);
        const uint64_t key = im.aig.node_to_index(n);
        auto it = forced.find(key);
        if (it == forced.end())      forced.emplace(key, nodeVal);
        else if (it->second != nodeVal) return EquivResult::Unknown;  // 自相矛盾
    }

    if (na == nb) {
        // 結構相同時 assumption 不影響結論。
        return (ca == cb) ? EquivResult::Equal : EquivResult::NotEqual;
    }

    const int x = im.xor_var(na, nb);
    im.solver.assume((ca == cb) ? x : -x);
    for (const auto& kv : forced) {
        const Node n = im.aig.index_to_node(static_cast<uint32_t>(kv.first));
        const int  v = im.var_of(n);
        im.solver.assume(kv.second ? v : -v);
    }

    const int r = im.run_solve();
    if (r == kUnsat) {
        // 條件式的 Equal **絕不寫進帳本**。它只在該 assumption 下成立,
        //   拿去 assert_equal 會把一個局部事實變成永久全域子句 —— 那正是
        //   本檔最危險的失效模式。
        //
        //   唯一例外:forced 為空表示 assumption 全被摺掉(只指定了常數且
        //   值相符),這一次求解實際上是無條件的,可以安全記帳。
        if (forced.empty()) im.record_proof(na, ca, nb, cb);
        return EquivResult::Equal;
    }
    if (r == kSat)  { im.capture_cex(); return EquivResult::NotEqual; }
    return EquivResult::Unknown;
}

// -----------------------------------------------------------------------------
//  solve_for
// -----------------------------------------------------------------------------
SatResult SatEngine::solve_for(Sig f, bool val) {
    Impl& im = *impl_;
    im.grow();
    im.clear_cex();

    const Node nf = im.aig.get_node(f);
    if (im.aig.is_constant(nf)) {
        const bool actual = im.aig.is_complemented(f);
        return (actual == val) ? SatResult::Sat : SatResult::Unsat;
    }

    im.encode_cone(nf);
    const int lf = im.lit_of(f);
    im.solver.assume(val ? lf : -lf);

    const int r = im.run_solve();
    if (r == kUnsat) return SatResult::Unsat;
    if (r == kSat)  { im.capture_cex(); return SatResult::Sat; }
    return SatResult::Unknown;
}

// -----------------------------------------------------------------------------
//  增量學習
// -----------------------------------------------------------------------------
//
//  這裡灌進去的是**永久**子句,一旦寫入就無法撤銷,所以只有無條件證明
//  才准寫。但這件事不靠呼叫端自律 —— 本函式自己查帳本:
//
//    有紀錄   → 直接寫子句(FRAIG 的 happy path,零額外求解)
//    無紀錄   → 自己跑一次 are_equal;UNSAT 才寫,否則什麼都不做並回 false
//
//  於是「拿 are_equal_under 的條件式結論來 assert」不再是災難,而是變成
//  一次多餘的求解 —— 因為條件式結論不進帳本,這裡會重新無條件驗證一遍。
//
//  自證路徑會呼叫 solve(),因而覆蓋 last_counterexample()。
//     FRAIG 若要保留反例,請在呼叫 assert_equal 之前先取走。
//
//  回傳值:true = 等價已成立(子句已寫入,或兩者結構上同一 signal)。
//          false = 未能證明等價,**沒有寫入任何子句**,呼叫端不可視為已合併。
bool SatEngine::assert_equal(Sig a, Sig b) {
    Impl& im = *impl_;
    im.grow();

    const Node na = im.aig.get_node(a);
    const Node nb = im.aig.get_node(b);
    const bool ca = im.aig.is_complemented(a);
    const bool cb = im.aig.is_complemented(b);

    if (na == nb) {
        // 同一 signal:本來就等價,不需要子句。
        // a 與 !a:宣告等價會讓 CNF 直接變 UNSAT、毀掉整顆 solver,回 false 擋下。
        return ca == cb;
    }

    if (!im.has_proof(na, ca, nb, cb)) {
        if (are_equal(a, b) != EquivResult::Equal) return false;
    }

    im.encode_cone(na);
    im.encode_cone(nb);
    const int la = im.lit_of(a);
    const int lb = im.lit_of(b);
    im.add_bin(-la,  lb);
    im.add_bin( la, -lb);
    return true;
}

bool SatEngine::assert_const(Sig a, bool val) {
    Impl& im = *impl_;
    im.grow();

    const Node na = im.aig.get_node(a);
    const bool ca = im.aig.is_complemented(a);

    if (im.aig.is_constant(na)) return ca == val;

    // 與 is_const 用同一本帳:常數 val 即 node 0 帶 complement=val。
    const Node n0 = im.aig.get_node(im.aig.get_constant(false));
    if (!im.has_proof(na, ca, n0, val)) {
        if (is_const(a, val) != EquivResult::Equal) return false;
    }

    im.encode_cone(na);
    const int la = im.lit_of(a);
    im.add_unit(val ? la : -la);
    return true;
}

void SatEngine::set_limits(double t, int64_t c) {
    impl_->cfg.time_limit_sec = t;
    impl_->cfg.conflict_limit = c;
}

} // namespace eqeng
