#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include "Types.h"

class Netlist;   // 前置宣告

namespace eqeng {

// 名字對照層：net 名 <-> AIG signal。
//
// 設計原則：
//   - 名字只在 Netlist 與這張表；高階 API 全程只碰 Sig。
//   - 每顆閘只有一個輸出 net → 綁定在 gate-output 這層天生 1:1。
//   - 多對一的唯一來源：strash 合併結構相同的閘、BUF/NOT 造成 net alias、
//     以及 FRAIG 合併功能等價節點 → 多個 net 名掛到同一 node。
//     這就是「報告等價類」的天然機制。
//
// 生命週期鐵律：
//   本表對應「某一次 AIG 建構後的快照」。只要 Netlist 被修改
//   （compactRemovedGates 會位移 net id），就必須連同 AIG 一起重建這張表；
//   絕不可跨修改沿用舊 Sig。這是最容易產生「對不上」heisenbug 的地方。
class NameMap {
public:
    // aig 與 nl 皆需在本物件存活期間持續有效（唯讀）。
    NameMap(const Ntk& aig, const Netlist& nl);

    // === 建構期：translator 逐 gate 呼叫 ===
    // 把「原始 net」綁到它在 AIG 裡對應的 signal。
    // 同一 node 可被不同 net 重複綁定（alias / merge）→ 反向索引多對一。
    void bind(int netId, Sig s);

    void clear();                    // 重建前呼叫
    void rebuild_reverse_index();    // 由 netToSig_ 重算 node->nets（remap 內部會用）

    // === 正向查詢（LLM 用名字提問）===
    bool contains(const std::string& netName) const;
    // 前置條件：contains(netName)；找不到會 assert，不確定時用 try_resolve。
    Sig  resolve(const std::string& netName) const;
    std::optional<Sig> try_resolve(const std::string& netName) const;
    std::optional<Sig> try_resolve(int netId) const;   // 內部路徑用 id 較快

    // === 反向查詢（報告用）===
    // 回傳「精確對應到這個 signal（含 phase）」的所有 net。
    // net 與它的 NOT 落在同一 node、但 phase 不同 → 不會被混在一起。
    std::vector<std::string> names_of(Sig s) const;
    std::vector<int>         net_ids_of(Sig s) const;

    // === 等價類報告（FRAIG remap 後最完整；未 remap 時只反映結構共享）===
    // 依「代表 signal（node + phase）」把 net 分組。
    // 一個 net 與其反相會落在不同組（各自等價於 f 與 !f）。
    // min_size 過濾單元素組（預設只回報真正的等價類）。
    std::vector<std::vector<int>> equivalence_classes(int min_size = 2) const;

    // === FRAIG 整合 ===
    // sweep 後把「每個 node 的代表 signal」傳進來，將 name2sig 全部改寫成 canonical：
    //   canonical(s) = reprByNode[get_node(s)] ^ is_complemented(s)
    // 之後 resolve() 直接回代表 signal，equiv 即為 signal 比對。
    // reprByNode 以 node index 為索引，未受影響的 node 填自身。
    void remap(const std::vector<Sig>& reprByNode);

    std::size_t size() const { return netToSig_.size(); }

private:
    const Ntk*     aig_;   // get_node / is_complemented（唯讀）
    const Netlist* nl_;    // name <-> net id、net id -> name（唯讀）

    std::unordered_map<int, Sig>                   netToSig_;    // net id -> signal
    std::unordered_map<uint64_t, std::vector<int>> nodeToNets_;  // node index -> net ids
};

} // namespace eqeng