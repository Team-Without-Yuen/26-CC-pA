#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "Types.h"
#include "NameMap.h"

class Netlist;

namespace eqeng {

// 一顆 DFF 的組合抽象結果。
struct DffInfo {
    int gateId   = -1;
    int qNetId   = -1;      // Q 的 net
    int dNetId   = -1;      // D pin 接的 net（未折 RN/SN 前）
    int rnNetId  = -1;      // -1 表示未接 → 視為恆 1（無 reset）
    int snNetId  = -1;
    Sig q;                  // pseudo-PI
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

    // PI 順序（反例解讀、cofactor 合法性檢查都靠它）：
    //   [0, num_real_pis)                        → 宣告順序的真實 PI
    //   [num_real_pis, +num_dff)                 → DFF Q 的 pseudo-PI（gate id 遞增）
    //   之後                                      → 懸空輸入的自由 PI（net id 遞增）
    const std::vector<int>& pi_net_ids() const { return piNetIds_; }
    bool                    is_free_var(Sig s) const;   // s 是否為 PI（cofactor 前置條件）

    const std::vector<DffInfo>& dffs() const { return dffs_; }
    // 真實 PO 的驅動 signal，宣告順序（與 aig.po_at(0..num_real_pos) 對齊）。
    const std::vector<Sig>& po_signals() const { return poSigs_; }
    const std::vector<int>& po_net_ids() const { return poNetIds_; }

    const Stats& stats() const { return stats_; }

private:
    explicit AigModel(const Netlist& nl);
    void do_build(const Options& opt);

    const Netlist*       nl_;
    Ntk                  aig_;      // 宣告順序重要：names_ 建構時要用到 aig_
    NameMap              names_;
    std::vector<int>     piNetIds_;
    std::vector<DffInfo> dffs_;
    std::vector<Sig>     poSigs_;
    std::vector<int>     poNetIds_;
    Stats                stats_;
};

} // namespace eqeng