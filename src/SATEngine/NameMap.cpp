#include "include/SATEngine/NameMap.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include "include/core/Netlist.h"

namespace eqeng {

namespace {

// Sig 的雜湊鍵：mockturtle 的 signal 內部就是一個 uint64（index<<1 | complement），
// 直接拿 .data 當 key 即可，node 與 phase 都涵蓋在內。
inline uint64_t sig_key(Sig s) { return s.data; }

} // namespace

NameMap::NameMap(const Ntk& aig, const Netlist& nl)
    : aig_(&aig), nl_(&nl) {
    // 100 萬 gate 時 rehash 成本可觀，先按 net 數預留。
    const std::size_t n = nl.getNetCount();
    if (n > 0) {
        netToSig_.reserve(n);
        nodeToNets_.reserve(n / 2 + 1);   // strash/alias 後 node 數必然少於 net 數
    }
}

void NameMap::clear() {
    netToSig_.clear();
    nodeToNets_.clear();
    nodeRemap_.clear();
}

void NameMap::bind(int netId, Sig s) {
    if (netId < 0 || !nl_->isValidNetId(netId)) return;

    auto it = netToSig_.find(netId);
    if (it != netToSig_.end()) {
        // 重複綁定：先把舊的反向索引拆掉，避免同一 net 出現在兩個 node 底下。
        // 正常流程（AigBuilder 的 hasSig 檢查）不會走到這裡，屬防呆。
        if (it->second == s) return;
        const uint64_t oldNode = aig_->node_to_index(aig_->get_node(it->second));
        auto rit = nodeToNets_.find(oldNode);
        if (rit != nodeToNets_.end()) {
            auto& v = rit->second;
            v.erase(std::remove(v.begin(), v.end(), netId), v.end());
            if (v.empty()) nodeToNets_.erase(rit);
        }
        it->second = s;
    } else {
        netToSig_.emplace(netId, s);
    }

    const uint64_t node = aig_->node_to_index(aig_->get_node(s));
    nodeToNets_[node].push_back(netId);
}

void NameMap::rebuild_reverse_index() {
    nodeToNets_.clear();
    nodeToNets_.reserve(netToSig_.size() / 2 + 1);

    for (const auto& kv : netToSig_) {
        const uint64_t node = aig_->node_to_index(aig_->get_node(kv.second));
        nodeToNets_[node].push_back(kv.first);
    }
    // 排序以確保報告順序穩定：輸出要能逐字比對。
    for (auto& kv : nodeToNets_) {
        std::sort(kv.second.begin(), kv.second.end());
    }
}

// ---------- 正向查詢 ----------

bool NameMap::contains(const std::string& netName) const {
    const int netId = nl_->getNetId(netName);
    return netId >= 0 && netToSig_.find(netId) != netToSig_.end();
}

Sig NameMap::resolve(const std::string& netName) const {
    auto s = try_resolve(netName);
    if (!s) {
        // 刻意不回 const0：靜默的 const0 會讓不等價的電路被判為等價，
        // 是最難追的一類錯誤。寧可讓呼叫端當場炸掉。
        std::cerr << "[NameMap] resolve failed: net '" << netName << "' not bound\n";
        throw std::out_of_range("NameMap::resolve: unknown net '" + netName + "'");
    }
    return *s;
}

std::optional<Sig> NameMap::try_resolve(const std::string& netName) const {
    const int netId = nl_->getNetId(netName);
    if (netId < 0) return std::nullopt;
    return try_resolve(netId);
}

std::optional<Sig> NameMap::try_resolve(int netId) const {
    auto it = netToSig_.find(netId);
    if (it == netToSig_.end()) return std::nullopt;
    return it->second;
}

// ---------- 反向查詢 ----------
//
// 只回「phase 完全相同」的 net。
// 一條 net 與它的 NOT 會落在同一個 node、但 data 差一個 bit，
// 因此不會被混為一談 —— 這是功能等價的正確語意。

std::vector<int> NameMap::net_ids_of(Sig s) const {
    s = canonicalize(s);
    std::vector<int> out;

    const uint64_t node = aig_->node_to_index(aig_->get_node(s));
    auto it = nodeToNets_.find(node);
    if (it == nodeToNets_.end()) return out;

    for (const int netId : it->second) {
        auto sit = netToSig_.find(netId);
        if (sit == netToSig_.end()) continue;
        if (!(sit->second == s)) continue;                  // phase 必須一致
        if (!nl_->isValidNetId(netId)) continue;
        if (nl_->getNet(netId).isRemoved) continue;         // tombstone 不報
        out.push_back(netId);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> NameMap::names_of(Sig s) const {
    const std::vector<int> ids = net_ids_of(s);

    std::vector<std::string> names;
    names.reserve(ids.size());
    for (const int netId : ids) names.push_back(nl_->getNet(netId).name);
    return names;
}

// ---------- 等價類報告 ----------
//
// 依「代表 signal（node + phase）」把 net 分組。
// remap 之前：只反映 structural sharing（strash 合併、BUF/NOT alias）。
// remap 之後：反映完整的功能等價類。兩者共用同一套機制。

std::vector<std::vector<int>> NameMap::equivalence_classes(int min_size) const {
    std::unordered_map<uint64_t, std::vector<int>> bySig;
    bySig.reserve(netToSig_.size() / 2 + 1);

    for (const auto& kv : netToSig_) {
        const int netId = kv.first;
        if (!nl_->isValidNetId(netId)) continue;
        if (nl_->getNet(netId).isRemoved) continue;
        bySig[sig_key(kv.second)].push_back(netId);
    }

    std::vector<std::vector<int>> classes;
    classes.reserve(bySig.size());

    for (auto& kv : bySig) {
        if (static_cast<int>(kv.second.size()) < min_size) continue;
        std::sort(kv.second.begin(), kv.second.end());
        classes.push_back(std::move(kv.second));
    }

    // unordered_map 的走訪順序不保證，這裡排序讓輸出可重現
    // （Phase B 要跟 Phase A 的 golden 逐字比對）。
    std::sort(classes.begin(), classes.end(),
              [](const std::vector<int>& a, const std::vector<int>& b) {
                  if (a.size() != b.size()) return a.size() > b.size();   // 大類在前
                  return a < b;                                           // 字典序
              });
    return classes;
}

// ---------- FRAIG 整合 ----------
//
// 把每條 net 的 signal 改寫成 canonical：
//     canonical(s) = reprByNode[node(s)] ^ is_complemented(s)
// 之後 resolve() 直接回代表 signal，equiv 退化為 signal 比對。
//
// 呼叫端契約：remap 之後，names_of / net_ids_of 必須以 canonical signal 查詢
// （Primitives 會先過 Fraig::representative）。拿舊的非 canonical signal 來查會查不到。

void NameMap::remap(const std::vector<Sig>& reprByNode) {
    if (reprByNode.empty()) return;

    // 建立節點層級的對照,供 canonicalize 使用
    nodeRemap_.clear();
    for (uint32_t i = 0; i < reprByNode.size(); ++i) {
        const Sig self = aig_->make_signal(aig_->index_to_node(i));
        if (!(reprByNode[i] == self)) nodeRemap_.emplace(i, reprByNode[i]);
    }

    std::size_t remapped = 0, skipped = 0;

    for (auto& kv : netToSig_) {
        const Sig      s     = kv.second;
        const uint64_t node  = aig_->node_to_index(aig_->get_node(s));

        // sweep watermark 之後才物化的節點（cofactor 產物）不在表內，維持原樣。
        if (node >= reprByNode.size()) { ++skipped; continue; }

        const Sig canonical = reprByNode[node] ^ aig_->is_complemented(s);
        if (!(canonical == s)) {
            kv.second = canonical;
            ++remapped;
        }
    }

    // signal 換了 node，反向索引整個作廢，必須重建。
    rebuild_reverse_index();

    if (remapped > 0 || skipped > 0) {
        std::cerr << "[NameMap] remap: " << remapped << " net(s) canonicalized, "
                  << skipped << " beyond sweep watermark\n";
    }
}

Sig NameMap::canonicalize(Sig s) const {
    if (nodeRemap_.empty()) return s;
    auto it = nodeRemap_.find(aig_->node_to_index(aig_->get_node(s)));
    if (it == nodeRemap_.end()) return s;
    return it->second ^ aig_->is_complemented(s);
}

} // namespace eqeng
