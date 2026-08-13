#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Types.h"
#include "NameMap.h"

class Netlist;

namespace eqeng {

enum class ModelHealth : uint8_t {
    Sound,
    Conservative,
    Invalid
};

// 一顆 DFF 的組合抽象結果。
struct DffInfo {
    int gateId   = -1;
    int qNetId   = -1;      // Q 的 net
    int dNetId   = -1;      // D pin 接的 net（未折 RN/SN 前）
    int rnNetId  = -1;      // -1 表示未接 → 視為恆 1（無 reset）
    int snNetId  = -1;
    // Q 對應的 PI 索引（aig.pi_at(qPiIndex)）。
    //   -1 = 這顆 DFF 的 Q 不是自由變數（多 driver 等異常）
    int qPiIndex = -1;
    Sig q;                  // pseudo-PI（qPiIndex < 0 時無意義）
    Sig d_raw;              // D pin 的函數
    Sig d_eff;              // 折進 async set/reset 後的 next-state
};

// 一次 AIG 建構的完整結果。
//
// 為什麼是 class 而不是回傳 struct：NameMap 內部持有 aig 與 netlist 的指標，
// 一旦 Ntk 被 move，指標就懸空。因此 AigModel 不可複製、不可搬移，
// 一律以 unique_ptr 持有。
//
// 生命週期鐵律：Netlist 一被修改（尤其 compactRemovedGates 會位移 ID），
// 整個 AigModel、SatEngine、Fraig 全部作廢，重建一份新的。
class AigModel {
public:
    struct Options {
        // 把 async set/reset 折進 next-state：
        //   eff_D = RN ? (SN ? D : 1) : 0   ⇔   RN & (D | ~SN)   （reset 優先）
        // 純組合等價要把 reset 行為也比進去，預設開啟。
        bool fold_async_controls;
        // 懸空輸入（無 driver、非 PI、非常數）當成新的自由 PI，而不是綁 const0。
        // 綁 const0 會讓兩顆行為不同的電路被誤判為等價，預設開啟。
        bool undriven_as_free_pi;
        bool verbose;

        Options()
        : fold_async_controls(true),
          undriven_as_free_pi(true),
          verbose(false) {}
    };

    struct Stats {
        uint32_t num_real_pis      = 0;
        uint32_t num_dff           = 0;
        uint32_t num_free_pis      = 0;   // 懸空輸入造出來的
        uint32_t num_real_pos      = 0;
        uint32_t num_bound_nets    = 0;
        uint32_t num_unresolved    = 0;
        uint32_t num_comb_gates    = 0;   // 成功排序並建出的組合閘
        uint32_t num_topo_dropped  = 0;   // 組合迴路 / 無法排序
        uint64_t aig_size          = 0;
    };

    static std::unique_ptr<AigModel> build(const Netlist& nl, Options opt = Options());

    AigModel(const AigModel&)            = delete;
    AigModel& operator=(const AigModel&) = delete;

    Ntk&           aig()       { return aig_; }
    const Ntk&     aig() const { return aig_; }
    NameMap&       names()       { return names_; }
    const NameMap& names() const { return names_; }
    const Netlist& netlist() const { return *nl_; }

    // PI 的 net id，與 aig.pi_at(i) 對齊。反例解讀靠這張表。
    //
    //   建立順序是「真實 PI → DFF Q → 懸空自由 PI」，但不要用
    //   [num_real_pis, num_real_pis + num_dff) 去切 DFF 區段：
    //   Q 有多重 driver 的 DFF 不會建 pseudo-PI，區段會塌陷、後面整批錯位。
    //   要拿某顆 DFF 的 Q，一律查 dffs()[i].qPiIndex。
    const std::vector<int>& pi_net_ids() const { return piNetIds_; }
    bool                    is_free_var(Sig s) const;   // s 是否為 PI（cofactor 前置條件）

    const std::vector<DffInfo>& dffs() const { return dffs_; }
    // 真實 PO 的驅動 signal，宣告順序（與 aig.po_at(0..num_real_pos) 對齊）。
    const std::vector<Sig>& po_signals() const { return poSigs_; }
    const std::vector<int>& po_net_ids() const { return poNetIds_; }

    // ---- 局部可信度（cone-local health）----
    //
    // health() 是「整顆電路的最壞情況」，僅供診斷；
    // 是否能證明某個查詢，一律以下列 per-net 判斷為準。
    //
    // 不變量：未污染的 net，其整個 fanin cone 必定乾淨。
    // 因此一顆壞閘只會廢掉它下游的那一塊，不會廢掉整張電路。
    bool is_net_trustworthy(int netId) const;
    bool is_dff_trustworthy(int dffIndex) const;   // dffs() 的索引

    uint32_t num_tainted_nets() const { return numTaintedNets_; }
    // 污染源（root cause）的 net id，最多保留 kMaxTaintRoots 筆
    const std::vector<int>& taint_roots() const { return taintRoots_; }
    std::string taint_summary() const;

    // 保留舊語意供診斷用；Primitives 不再拿它當全域閘門。
    const Stats& stats() const { return stats_; }
    ModelHealth  health() const { return health_; }
    bool         can_prove() const { return health_ != ModelHealth::Invalid; }
    const std::string& health_message() const { return healthMessage_; }

private:
    explicit AigModel(const Netlist& nl);
    void do_build(const Options& opt);
    void mark_conservative();
    void mark_invalid(const std::string& reason);

    const Netlist*       nl_;
    Ntk                  aig_;      // 宣告順序重要：names_ 建構時要用到 aig_
    NameMap              names_;
    std::vector<int>     piNetIds_;
    std::vector<DffInfo> dffs_;
    std::vector<Sig>     poSigs_;
    std::vector<int>     poNetIds_;
    Stats                stats_;
    ModelHealth          health_ = ModelHealth::Sound;
    std::string          healthMessage_ = "sound";
    uint32_t             invalidIssueCount_ = 0;
    static constexpr uint32_t kMaxTaintRoots = 32;

    void taint_root(int netId, const std::string& reason);  // 污染源
    void taint_derived(int netId);                          // 由上游傳播而來

    std::vector<uint8_t> netTaint_;    // 0=clean, 1=root cause, 2=propagated
    std::vector<uint8_t> dffTaint_;    // 與 dffs_ 對齊
    std::vector<int>     taintRoots_;
    std::unordered_map<int, std::string> taintReason_;
    uint32_t             numTaintedNets_ = 0;
};

} // namespace eqeng
