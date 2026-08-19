#ifndef MOCKTURTLE_CONVERTER_H
#define MOCKTURTLE_CONVERTER_H

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <mockturtle/networks/aig.hpp>
#include <mockturtle/networks/xag.hpp>

#include "include/core/Netlist.h"

// ============================================================================
// Part A. Netlist <-> mockturtle 轉換 / Netlist 清理工具
// ============================================================================

// 將自定義的 Netlist 轉換為 Mockturtle 的 XAG 網路格式
mockturtle::xag_network NetlistToXag(const Netlist& nl);

// 將自定義的 Netlist 轉換為 Mockturtle 的 AIG 網路格式
mockturtle::aig_network NetlistToAig(const Netlist& nl);

// 將優化後的 XAG 網路轉換為全新的 Netlist
Netlist XagToNetlist(const mockturtle::xag_network& xag, const Netlist& old_nl);

// 將優化後的 AIG 網路轉換為全新的 Netlist
Netlist AigToNetlist(const mockturtle::aig_network& aig, const Netlist& old_nl);

// 合併輸入同一條 net 的重複 NOT：保留第一顆，其餘 fanout 改接、標死。
int mergeDuplicateInverters(Netlist& netlist);

// 把所有「吃 fromNet 當輸入」的閘，改成吃 toNet；並搬移 loadGateIds。
void redirectNetLoads(Netlist& netlist, int fromNet, int toNet);

// 把一顆閘標死並從其 fanin 的 loadGateIds 斷開。
void detachGate(Netlist& netlist, int gid);

// ============================================================================
// Part B. GenericLowering — 把 AIG/XAG 直接轉成「任意目標 basis」的 Netlist
// ============================================================================
//
// 取代原本的兩段式流程:
//     XagToNetlist (每條反相邊插一顆 NOT)  →  convertToBasis (逐閘 pattern 展開)
// 改用 dual-rail bubble pushing,每個節點同時追蹤正相與反相兩個到達層數,
// 在拓撲序上以 DP 選最淺的實現組合(對深度是最佳的)。
// 詳細推導見 GenericLowering.cpp 檔頭。
//
// XagToNetlist / AigToNetlist 在此框架下變成特例:
//     allowed = {AND, XOR, NOT}  等價於  XagToNetlist
//     allowed = {AND, NOT}       等價於  AigToNetlist
// ============================================================================

namespace lowering {

// ---------------------------------------------------------------------------
// 1. Basis 描述
// ---------------------------------------------------------------------------

struct LoweringBasis {
    // 以 GateType 的整數值索引 (AND, OR, NAND, NOR, NOT, BUF, XOR, XNOR, DFF)
    std::array<bool, 10> allow{};

    bool has(GateType t) const { return allow[static_cast<int>(t)]; }

    // 由白/黑名單建立。allowed 為空 = 不限制(除了 banned)。
    static LoweringBasis fromLists(const std::vector<GateType>& allowed,
                                   const std::vector<GateType>& banned);

    // 有沒有辦法做出「反相」?每種方式都是 1 層。
    //   NOT(a) / NAND(a,a) / NOR(a,a) / XOR(a,1'b1) / XNOR(a,1'b0)
    bool canInvert() const;

    // 有沒有辦法做出「兩輸入乘積」的任一極性?
    bool hasAndForm() const;

    // XOR 節點可以 1 層做掉嗎?否則要展開成 3 個 AND 節點。
    bool hasXorForm() const;

    // 需不需要常數 net?例如 {XNOR, AND} 沒有 NOT,只能靠 XNOR(a, 1'b0)。
    // (這組閘都保 1,由 Post's lattice 在沒有常數時不具功能完備性。)
    bool needsConstForInvert() const;

    bool complete() const;

    std::string describe() const;
};

// ---------------------------------------------------------------------------
// 2. Lowering 規格 (支援 cone 內外不同 basis)
// ---------------------------------------------------------------------------

struct LoweringSpec {
    LoweringBasis defaultBasis;                 // cone 外 / 全域

    // 只在「某個 cone 內部受限」的題目才設定。
    // coneRootName 必須是 PO net name 或某顆 DFF 的 D 端 net name
    // (依題目保證,net 只會是 PI / PO / pseudo I-O)。
    std::optional<LoweringBasis> coneBasis;
    std::string coneRootName;

    bool hasCone() const { return coneBasis.has_value() && !coneRootName.empty(); }
};

struct LoweringResult {
    bool ok = false;
    std::string message;
    Netlist netlist;

    int  depth = -1;          // 產出電路的最大組合深度
    int  gateCount = 0;
    size_t coneNodeCount = 0; // 被判定在 cone 內的 mockturtle 節點數

    // 沒能保留原名的 DFF D-pin net。原因通常是：兩顆 DFF 的 D 被合成同一條線、
    // D 被化簡成 PI/常數、或名字已被其他 boundary 佔用。
    std::vector<std::string> unpreservedDffNetNames;
};

// ---------------------------------------------------------------------------
// 3. 主函式
// ---------------------------------------------------------------------------
//
// 注意: 定義在 GenericLowering.cpp,並對 xag_network / aig_network 做顯式實例化。
// 若日後要支援其他 mockturtle 網路型別,請到該 .cpp 末端增加對應的
// template LoweringResult DoLowerToNetlist<...>(...); 一行。

template <typename Ntk>
LoweringResult DoLowerToNetlist(const Ntk& ntk,
                                const Netlist& old_nl,
                                const LoweringSpec& spec);

extern template LoweringResult DoLowerToNetlist<mockturtle::xag_network>(
    const mockturtle::xag_network&, const Netlist&, const LoweringSpec&);
extern template LoweringResult DoLowerToNetlist<mockturtle::aig_network>(
    const mockturtle::aig_network&, const Netlist&, const LoweringSpec&);

// ---- 便利包裝 ----
LoweringResult LowerXag(const mockturtle::xag_network& xag,
                        const Netlist& old_nl,
                        const LoweringSpec& spec);

LoweringResult LowerAig(const mockturtle::aig_network& aig,
                        const Netlist& old_nl,
                        const LoweringSpec& spec);

} // namespace lowering

#endif // MOCKTURTLE_CONVERTER_H
