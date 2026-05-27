#include "include/core/Netlist.h"

#include <algorithm>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// 保存已由名稱解析成 ID 的限制節點，供搜尋核心快速比對。
struct ResolvedPathNode {
    Netlist::PathNodeType type;
    int id;
};

// 將公開 API 接收的 PathNode 名稱轉成 net 或 gate ID；名稱不存在則回傳 false。
bool resolvePathNode(const Netlist& netlist,
                     const Netlist::PathNode& node,
                     ResolvedPathNode& resolvedNode) {
    resolvedNode.type = node.type;
    if (node.type == Netlist::PathNodeType::Net) {
        resolvedNode.id = netlist.getNetId(node.name);
    } else {
        resolvedNode.id = netlist.getGateId(node.name);
    }
    return resolvedNode.id >= 0;
}

// 判斷目前 net 是否正是指定的 net 條件節點；gate 條件在此不會命中。
bool matchesNet(const ResolvedPathNode* node, int netId) {
    return node != nullptr &&
           node->type == Netlist::PathNodeType::Net &&
           node->id == netId;
}

// 判斷目前 gate 是否正是指定的 gate 條件節點；net 條件在此不會命中。
bool matchesGate(const ResolvedPathNode* node, int gateId) {
    return node != nullptr &&
           node->type == Netlist::PathNodeType::Gate &&
           node->id == gateId;
}

// 使用 BFS 搜尋任意一條符合 required/avoided 條件的路徑並重建完整 witness。
// requiredNode 與 avoidedNode 為 nullptr 時，表示沒有對應限制條件。
Netlist::CombinationalPath findPathMatching(
    const Netlist& netlist,
    const std::string& startNet,
    const std::string& endNet,
    const ResolvedPathNode* requiredNode,
    const ResolvedPathNode* avoidedNode) {
    Netlist::CombinationalPath path;
    const int startNetId = netlist.getNetId(startNet);
    const int endNetId = netlist.getNetId(endNet);
    if (startNetId < 0 || endNetId < 0 ||
        matchesNet(avoidedNode, startNetId) ||
        matchesNet(avoidedNode, endNetId)) {
        return path;
    }

    const bool passedAtStart =
        requiredNode == nullptr || matchesNet(requiredNode, startNetId);
    if (startNetId == endNetId) {
        if (passedAtStart) {
            path.netIds.push_back(startNetId);
        }
        return path;
    }

    const size_t stateCount = netlist.getNetCount() * 2;
    const int startState = startNetId * 2 + (passedAtStart ? 1 : 0);
    std::queue<int> pendingStates;
    std::vector<bool> visitedStates(stateCount, false);
    std::vector<int> previousStates(stateCount, -1);
    std::vector<int> previousGateIds(stateCount, -1);
    pendingStates.push(startState);
    visitedStates[static_cast<size_t>(startState)] = true;

    int foundState = -1;
    while (!pendingStates.empty() && foundState < 0) {
        const int currentState = pendingStates.front();
        pendingStates.pop();
        const int currentNetId = currentState / 2;
        const bool alreadyPassedRequired = (currentState % 2) != 0;

        const Net& currentNet = netlist.getNet(currentNetId);
        for (int gateId : currentNet.loadGateIds) {
            const Gate& gate = netlist.getGate(gateId);
            if (gate.type == GateType::DFF ||
                gate.outputNetId < 0 ||
                matchesGate(avoidedNode, gateId)) {
                continue;
            }

            const int nextNetId = gate.outputNetId;
            if (matchesNet(avoidedNode, nextNetId)) {
                continue;
            }

            const bool passedRequired =
                alreadyPassedRequired ||
                matchesGate(requiredNode, gateId) ||
                matchesNet(requiredNode, nextNetId);
            const int nextState = nextNetId * 2 + (passedRequired ? 1 : 0);
            if (visitedStates[static_cast<size_t>(nextState)]) {
                continue;
            }

            visitedStates[static_cast<size_t>(nextState)] = true;
            previousStates[static_cast<size_t>(nextState)] = currentState;
            previousGateIds[static_cast<size_t>(nextState)] = gateId;
            if (nextNetId == endNetId && passedRequired) {
                foundState = nextState;
                break;
            }
            pendingStates.push(nextState);
        }
    }

    if (foundState < 0) {
        return path;
    }

    int currentState = foundState;
    path.netIds.push_back(currentState / 2);
    while (currentState != startState) {
        path.gateIds.push_back(previousGateIds[static_cast<size_t>(currentState)]);
        currentState = previousStates[static_cast<size_t>(currentState)];
        path.netIds.push_back(currentState / 2);
    }
    std::reverse(path.netIds.begin(), path.netIds.end());
    std::reverse(path.gateIds.begin(), path.gateIds.end());
    return path;
}

// 由目前 net 向下游列舉路徑；cycle guard 只限制目前分支，因此不會遺漏匯合路徑。
void enumeratePathsDepthFirst(
    const Netlist& netlist,
    int currentNetId,
    int endNetId,
    const ResolvedPathNode* requiredNode,
    const ResolvedPathNode* avoidedNode,
    bool alreadyPassedRequired,
    Netlist::CombinationalPath& currentPath,
    std::unordered_set<int>& netsInCurrentPath,
    std::vector<Netlist::CombinationalPath>& results) {
    const Net& currentNet = netlist.getNet(currentNetId);
    for (int gateId : currentNet.loadGateIds) {
        const Gate& gate = netlist.getGate(gateId);
        if (gate.type == GateType::DFF ||
            gate.outputNetId < 0 ||
            matchesGate(avoidedNode, gateId)) {
            continue;
        }

        const int nextNetId = gate.outputNetId;
        if (matchesNet(avoidedNode, nextNetId) ||
            netsInCurrentPath.count(nextNetId) != 0) {
            continue;
        }

        const bool passedRequired =
            alreadyPassedRequired ||
            matchesGate(requiredNode, gateId) ||
            matchesNet(requiredNode, nextNetId);
        currentPath.gateIds.push_back(gateId);
        currentPath.netIds.push_back(nextNetId);

        if (nextNetId == endNetId) {
            if (passedRequired) {
                results.push_back(currentPath);
            }
        } else {
            netsInCurrentPath.insert(nextNetId);
            enumeratePathsDepthFirst(netlist, nextNetId, endNetId,
                                     requiredNode, avoidedNode, passedRequired,
                                     currentPath, netsInCurrentPath, results);
            netsInCurrentPath.erase(nextNetId);
        }

        currentPath.netIds.pop_back();
        currentPath.gateIds.pop_back();
    }
}

// 列舉符合 required/avoided 條件的全部路徑；此核心供 C 類 API 共用。
std::vector<Netlist::CombinationalPath> enumeratePathsMatching(
    const Netlist& netlist,
    const std::string& startNet,
    const std::string& endNet,
    const ResolvedPathNode* requiredNode,
    const ResolvedPathNode* avoidedNode) {
    std::vector<Netlist::CombinationalPath> results;
    const int startNetId = netlist.getNetId(startNet);
    const int endNetId = netlist.getNetId(endNet);
    if (startNetId < 0 || endNetId < 0 ||
        matchesNet(avoidedNode, startNetId) ||
        matchesNet(avoidedNode, endNetId)) {
        return results;
    }

    const bool passedAtStart =
        requiredNode == nullptr || matchesNet(requiredNode, startNetId);
    Netlist::CombinationalPath currentPath;
    currentPath.netIds.push_back(startNetId);
    if (startNetId == endNetId) {
        if (passedAtStart) {
            results.push_back(currentPath);
        }
        return results;
    }

    std::unordered_set<int> netsInCurrentPath;
    netsInCurrentPath.insert(startNetId);
    enumeratePathsDepthFirst(netlist, startNetId, endNetId,
                             requiredNode, avoidedNode, passedAtStart,
                             currentPath, netsInCurrentPath, results);
    return results;
}

} // namespace

// 判斷兩條 net 之間是否至少存在一條組合路徑，且不跨越 DFF。
bool Netlist::hasCombinationalPath(const std::string& startNet,
                                   const std::string& endNet) const {
    return findAnyCombinationalPath(startNet, endNet).exists();
}

// 判斷兩條 net 之間是否至少存在一條避開指定 net 或 gate 的組合路徑。
bool Netlist::hasCombinationalPathAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const PathNode& avoidedNode) const {
    return findAnyCombinationalPathAvoiding(
        startNet, endNet, avoidedNode).exists();
}

// 判斷兩條 net 之間是否至少存在一條經過指定 net 或 gate 的組合路徑。
bool Netlist::hasCombinationalPathThrough(
    const std::string& startNet,
    const std::string& endNet,
    const PathNode& requiredNode) const {
    return findAnyCombinationalPathThrough(
        startNet, endNet, requiredNode).exists();
}

// 判斷是否存在一條同時經過 requiredNode 且避開 avoidedNode 的組合路徑。
bool Netlist::hasCombinationalPathThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const PathNode& requiredNode,
    const PathNode& avoidedNode) const {
    return findAnyCombinationalPathThroughAvoiding(
        startNet, endNet, requiredNode, avoidedNode).exists();
}

// 使用 BFS 找到任意一條兩條 net 之間的組合邏輯路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPath(
    const std::string& startNet,
    const std::string& endNet) const {
    return findPathMatching(*this, startNet, endNet, nullptr, nullptr);
}

// 使用 BFS 找到任意一條避開指定 net 或 gate 的組合邏輯路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPathAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const PathNode& avoidedNode) const {
    ResolvedPathNode avoided;
    if (!resolvePathNode(*this, avoidedNode, avoided)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, nullptr, &avoided);
}

// 使用 BFS 找到任意一條經過指定 net 或 gate 的組合邏輯路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPathThrough(
    const std::string& startNet,
    const std::string& endNet,
    const PathNode& requiredNode) const {
    ResolvedPathNode required;
    if (!resolvePathNode(*this, requiredNode, required)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, &required, nullptr);
}

// 使用 BFS 找到任意一條經過 requiredNode 且避開 avoidedNode 的組合邏輯路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPathThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const PathNode& requiredNode,
    const PathNode& avoidedNode) const {
    ResolvedPathNode required;
    ResolvedPathNode avoided;
    if (!resolvePathNode(*this, requiredNode, required) ||
        !resolvePathNode(*this, avoidedNode, avoided)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, &required, &avoided);
}

// 以 DFS backtracking 列出兩條 net 之間的所有組合邏輯路徑。
std::vector<Netlist::CombinationalPath> Netlist::enumerateCombinationalPaths(
    const std::string& startNet,
    const std::string& endNet) const {
    return enumeratePathsMatching(*this, startNet, endNet, nullptr, nullptr);
}

// 以 DFS backtracking 列出所有避開指定 net 或 gate 的組合邏輯路徑。
std::vector<Netlist::CombinationalPath>
Netlist::enumerateCombinationalPathsAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const PathNode& avoidedNode) const {
    ResolvedPathNode avoided;
    if (!resolvePathNode(*this, avoidedNode, avoided)) {
        return std::vector<CombinationalPath>();
    }
    return enumeratePathsMatching(*this, startNet, endNet, nullptr, &avoided);
}

// 以 DFS backtracking 列出所有經過指定 net 或 gate 的組合邏輯路徑。
std::vector<Netlist::CombinationalPath>
Netlist::enumerateCombinationalPathsThrough(
    const std::string& startNet,
    const std::string& endNet,
    const PathNode& requiredNode) const {
    ResolvedPathNode required;
    if (!resolvePathNode(*this, requiredNode, required)) {
        return std::vector<CombinationalPath>();
    }
    return enumeratePathsMatching(*this, startNet, endNet, &required, nullptr);
}

// 以 DFS backtracking 列出所有經過 requiredNode 且避開 avoidedNode 的組合邏輯路徑。
std::vector<Netlist::CombinationalPath>
Netlist::enumerateCombinationalPathsThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const PathNode& requiredNode,
    const PathNode& avoidedNode) const {
    ResolvedPathNode required;
    ResolvedPathNode avoided;
    if (!resolvePathNode(*this, requiredNode, required) ||
        !resolvePathNode(*this, avoidedNode, avoided)) {
        return std::vector<CombinationalPath>();
    }
    return enumeratePathsMatching(*this, startNet, endNet, &required, &avoided);
}

// 判斷所有既有路徑是否都通過 requiredNode；原本沒有路徑時回傳 false。
bool Netlist::everyPathPassesThrough(
    const std::string& startNet,
    const std::string& endNet,
    const PathNode& requiredNode) const {
    ResolvedPathNode required;
    if (!resolvePathNode(*this, requiredNode, required) ||
        !hasCombinationalPath(startNet, endNet)) {
        return false;
    }
    return !hasCombinationalPathAvoiding(startNet, endNet, requiredNode);
}

// 判斷所有既有路徑是否都避開 avoidedNode；原本沒有路徑時回傳 false。
bool Netlist::everyPathAvoids(const std::string& startNet,
                              const std::string& endNet,
                              const PathNode& avoidedNode) const {
    ResolvedPathNode avoided;
    if (!resolvePathNode(*this, avoidedNode, avoided) ||
        !hasCombinationalPath(startNet, endNet)) {
        return false;
    }
    return !hasCombinationalPathThrough(startNet, endNet, avoidedNode);
}
