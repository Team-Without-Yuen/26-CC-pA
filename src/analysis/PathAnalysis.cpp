#include "include/core/Netlist.h"

#include <algorithm>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// 保存已由名稱解析成 ID 的條件節點，供搜尋時以整數快速比對。
struct ResolvedPathNode {
    Netlist::PathNodeType type;
    int id;
};

// 保存一筆 BFS 搜尋狀態；passedRequired[i] 表示是否已經過第 i 個 required node。
struct SearchState {
    int netId;
    std::vector<unsigned char> passedRequired;
    int previousStateIndex;
    int previousGateId;
};

// 將一組 PathNode 名稱轉成 ID；任一名稱不存在代表查詢條件無效。
bool resolvePathNodes(const Netlist& netlist,
                      const std::vector<Netlist::PathNode>& nodes,
                      std::vector<ResolvedPathNode>& resolvedNodes) {
    for (const Netlist::PathNode& node : nodes) {
        ResolvedPathNode resolved;
        resolved.type = node.type;
        if (node.type == Netlist::PathNodeType::Net) {
            resolved.id = netlist.getNetId(node.name);
        } else {
            resolved.id = netlist.getGateId(node.name);
        }
        if (resolved.id < 0) {
            return false;
        }
        resolvedNodes.push_back(resolved);
    }
    return true;
}

// 判斷指定 net 是否存在於一組條件節點中；gate 類型條件不會命中。
bool containsNet(const std::vector<ResolvedPathNode>& nodes, int netId) {
    for (const ResolvedPathNode& node : nodes) {
        if (node.type == Netlist::PathNodeType::Net && node.id == netId) {
            return true;
        }
    }
    return false;
}

// 判斷指定 gate 是否存在於一組條件節點中；net 類型條件不會命中。
bool containsGate(const std::vector<ResolvedPathNode>& nodes, int gateId) {
    for (const ResolvedPathNode& node : nodes) {
        if (node.type == Netlist::PathNodeType::Gate && node.id == gateId) {
            return true;
        }
    }
    return false;
}

// 在走到指定 net 時，更新哪些 required net 條件已被滿足。
void markRequiredNet(const std::vector<ResolvedPathNode>& requiredNodes,
                     int netId,
                     std::vector<unsigned char>& passedRequired) {
    for (size_t i = 0; i < requiredNodes.size(); ++i) {
        if (requiredNodes[i].type == Netlist::PathNodeType::Net &&
            requiredNodes[i].id == netId) {
            passedRequired[i] = 1;
        }
    }
}

// 在穿越指定 gate 時，更新哪些 required gate 條件已被滿足。
void markRequiredGate(const std::vector<ResolvedPathNode>& requiredNodes,
                      int gateId,
                      std::vector<unsigned char>& passedRequired) {
    for (size_t i = 0; i < requiredNodes.size(); ++i) {
        if (requiredNodes[i].type == Netlist::PathNodeType::Gate &&
            requiredNodes[i].id == gateId) {
            passedRequired[i] = 1;
        }
    }
}

// 判斷 requiredNodes 的所有條件是否都已經被目前路徑滿足。
bool allRequiredPassed(const std::vector<unsigned char>& passedRequired) {
    for (unsigned char passed : passedRequired) {
        if (passed == 0) {
            return false;
        }
    }
    return true;
}

// 將 BFS 狀態轉成 visited key；相同 net 但通過條件不同，必須視為不同狀態。
std::string makeStateKey(int netId,
                         const std::vector<unsigned char>& passedRequired) {
    std::string key = std::to_string(netId);
    key.push_back(':');
    for (unsigned char passed : passedRequired) {
        key.push_back(passed == 0 ? '0' : '1');
    }
    return key;
}

// 使用 BFS 找到任意一條經過全部 requiredNodes 且避開全部 avoidedNodes 的路徑。
// requiredNodes 為空代表沒有必經限制，avoidedNodes 為空代表沒有禁止限制。
Netlist::CombinationalPath findPathMatching(
    const Netlist& netlist,
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<ResolvedPathNode>& requiredNodes,
    const std::vector<ResolvedPathNode>& avoidedNodes) {
    Netlist::CombinationalPath path;
    const int startNetId = netlist.getNetId(startNet);
    const int endNetId = netlist.getNetId(endNet);
    if (startNetId < 0 || endNetId < 0 ||
        containsNet(avoidedNodes, startNetId) ||
        containsNet(avoidedNodes, endNetId)) {
        return path;
    }

    SearchState startState;
    startState.netId = startNetId;
    startState.passedRequired.assign(requiredNodes.size(), 0);
    startState.previousStateIndex = -1;
    startState.previousGateId = -1;
    markRequiredNet(requiredNodes, startNetId, startState.passedRequired);

    if (startNetId == endNetId) {
        if (allRequiredPassed(startState.passedRequired)) {
            path.netIds.push_back(startNetId);
        }
        return path;
    }

    std::vector<SearchState> states;
    std::queue<int> pendingStateIndices;
    std::unordered_set<std::string> visitedStates;
    states.push_back(startState);
    pendingStateIndices.push(0);
    visitedStates.insert(makeStateKey(startNetId, startState.passedRequired));

    int foundStateIndex = -1;
    while (!pendingStateIndices.empty() && foundStateIndex < 0) {
        const int currentStateIndex = pendingStateIndices.front();
        pendingStateIndices.pop();
        const SearchState currentState = states[static_cast<size_t>(currentStateIndex)];

        const Net& currentNet = netlist.getNet(currentState.netId);
        for (int gateId : currentNet.loadGateIds) {
            const Gate& gate = netlist.getGate(gateId);
            if (gate.type == GateType::DFF ||
                gate.outputNetId < 0 ||
                containsGate(avoidedNodes, gateId)) {
                continue;
            }

            const int nextNetId = gate.outputNetId;
            if (containsNet(avoidedNodes, nextNetId)) {
                continue;
            }

            SearchState nextState;
            nextState.netId = nextNetId;
            nextState.passedRequired = currentState.passedRequired;
            nextState.previousStateIndex = currentStateIndex;
            nextState.previousGateId = gateId;
            markRequiredGate(requiredNodes, gateId, nextState.passedRequired);
            markRequiredNet(requiredNodes, nextNetId, nextState.passedRequired);

            const std::string stateKey =
                makeStateKey(nextNetId, nextState.passedRequired);
            if (!visitedStates.insert(stateKey).second) {
                continue;
            }

            states.push_back(nextState);
            const int nextStateIndex = static_cast<int>(states.size() - 1);
            if (nextNetId == endNetId &&
                allRequiredPassed(nextState.passedRequired)) {
                foundStateIndex = nextStateIndex;
                break;
            }
            pendingStateIndices.push(nextStateIndex);
        }
    }

    if (foundStateIndex < 0) {
        return path;
    }

    int currentStateIndex = foundStateIndex;
    while (currentStateIndex >= 0) {
        const SearchState& state = states[static_cast<size_t>(currentStateIndex)];
        path.netIds.push_back(state.netId);
        if (state.previousGateId >= 0) {
            path.gateIds.push_back(state.previousGateId);
        }
        currentStateIndex = state.previousStateIndex;
    }
    std::reverse(path.netIds.begin(), path.netIds.end());
    std::reverse(path.gateIds.begin(), path.gateIds.end());
    return path;
}

// 由目前 net 向下游列舉路徑；目前分支的 cycle guard 可避免遞迴迴圈且保留匯合路徑。
void enumeratePathsDepthFirst(
    const Netlist& netlist,
    int currentNetId,
    int endNetId,
    const std::vector<ResolvedPathNode>& requiredNodes,
    const std::vector<ResolvedPathNode>& avoidedNodes,
    const std::vector<unsigned char>& alreadyPassedRequired,
    Netlist::CombinationalPath& currentPath,
    std::unordered_set<int>& netsInCurrentPath,
    std::vector<Netlist::CombinationalPath>& results) {
    const Net& currentNet = netlist.getNet(currentNetId);
    for (int gateId : currentNet.loadGateIds) {
        const Gate& gate = netlist.getGate(gateId);
        if (gate.type == GateType::DFF ||
            gate.outputNetId < 0 ||
            containsGate(avoidedNodes, gateId)) {
            continue;
        }

        const int nextNetId = gate.outputNetId;
        if (containsNet(avoidedNodes, nextNetId) ||
            netsInCurrentPath.count(nextNetId) != 0) {
            continue;
        }

        std::vector<unsigned char> passedRequired = alreadyPassedRequired;
        markRequiredGate(requiredNodes, gateId, passedRequired);
        markRequiredNet(requiredNodes, nextNetId, passedRequired);
        currentPath.gateIds.push_back(gateId);
        currentPath.netIds.push_back(nextNetId);

        if (nextNetId == endNetId) {
            if (allRequiredPassed(passedRequired)) {
                results.push_back(currentPath);
            }
        } else {
            netsInCurrentPath.insert(nextNetId);
            enumeratePathsDepthFirst(netlist, nextNetId, endNetId,
                                     requiredNodes, avoidedNodes, passedRequired,
                                     currentPath, netsInCurrentPath, results);
            netsInCurrentPath.erase(nextNetId);
        }

        currentPath.netIds.pop_back();
        currentPath.gateIds.pop_back();
    }
}

// 以 DFS 列舉所有經過全部 requiredNodes 且避開全部 avoidedNodes 的路徑。
std::vector<Netlist::CombinationalPath> enumeratePathsMatching(
    const Netlist& netlist,
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<ResolvedPathNode>& requiredNodes,
    const std::vector<ResolvedPathNode>& avoidedNodes) {
    std::vector<Netlist::CombinationalPath> results;
    const int startNetId = netlist.getNetId(startNet);
    const int endNetId = netlist.getNetId(endNet);
    if (startNetId < 0 || endNetId < 0 ||
        containsNet(avoidedNodes, startNetId) ||
        containsNet(avoidedNodes, endNetId)) {
        return results;
    }

    std::vector<unsigned char> passedRequired(requiredNodes.size(), 0);
    markRequiredNet(requiredNodes, startNetId, passedRequired);
    Netlist::CombinationalPath currentPath;
    currentPath.netIds.push_back(startNetId);
    if (startNetId == endNetId) {
        if (allRequiredPassed(passedRequired)) {
            results.push_back(currentPath);
        }
        return results;
    }

    std::unordered_set<int> netsInCurrentPath;
    netsInCurrentPath.insert(startNetId);
    enumeratePathsDepthFirst(netlist, startNetId, endNetId,
                             requiredNodes, avoidedNodes, passedRequired,
                             currentPath, netsInCurrentPath, results);
    return results;
}

} // namespace

// 判斷兩條 net 之間是否至少存在一條組合路徑，且不跨越 DFF。
bool Netlist::hasCombinationalPath(const std::string& startNet,
                                   const std::string& endNet) const {
    return findAnyCombinationalPath(startNet, endNet).exists();
}

// 判斷兩條 net 之間是否至少存在一條避開全部指定節點的組合路徑。
bool Netlist::hasCombinationalPathAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    return findAnyCombinationalPathAvoiding(
        startNet, endNet, avoidedNodes).exists();
}

// 判斷兩條 net 之間是否至少存在一條經過全部指定節點的組合路徑。
bool Netlist::hasCombinationalPathThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    return findAnyCombinationalPathThrough(
        startNet, endNet, requiredNodes).exists();
}

// 判斷是否存在一條經過全部 requiredNodes 且避開全部 avoidedNodes 的組合路徑。
bool Netlist::hasCombinationalPathThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes,
    const std::vector<PathNode>& avoidedNodes) const {
    return findAnyCombinationalPathThroughAvoiding(
        startNet, endNet, requiredNodes, avoidedNodes).exists();
}

// 使用 BFS 找到任意一條兩條 net 之間的組合邏輯路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPath(
    const std::string& startNet,
    const std::string& endNet) const {
    return findPathMatching(*this, startNet, endNet, {}, {});
}

// 使用 BFS 找到任意一條避開全部指定節點的組合邏輯路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPathAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, {}, avoided);
}

// 使用 BFS 找到任意一條經過全部指定節點的組合邏輯路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPathThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    std::vector<ResolvedPathNode> required;
    if (!resolvePathNodes(*this, requiredNodes, required)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, required, {});
}

// 使用 BFS 找到任意一條經過全部 requiredNodes 且避開全部 avoidedNodes 的組合路徑。
Netlist::CombinationalPath Netlist::findAnyCombinationalPathThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> required;
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, requiredNodes, required) ||
        !resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, required, avoided);
}

// 以 DFS backtracking 列出兩條 net 之間的所有組合邏輯路徑。
std::vector<Netlist::CombinationalPath> Netlist::enumerateCombinationalPaths(
    const std::string& startNet,
    const std::string& endNet) const {
    return enumeratePathsMatching(*this, startNet, endNet, {}, {});
}

// 以 DFS backtracking 列出所有避開全部指定節點的組合邏輯路徑。
std::vector<Netlist::CombinationalPath>
Netlist::enumerateCombinationalPathsAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, avoidedNodes, avoided)) {
        return std::vector<CombinationalPath>();
    }
    return enumeratePathsMatching(*this, startNet, endNet, {}, avoided);
}

// 以 DFS backtracking 列出所有經過全部指定節點的組合邏輯路徑。
std::vector<Netlist::CombinationalPath>
Netlist::enumerateCombinationalPathsThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    std::vector<ResolvedPathNode> required;
    if (!resolvePathNodes(*this, requiredNodes, required)) {
        return std::vector<CombinationalPath>();
    }
    return enumeratePathsMatching(*this, startNet, endNet, required, {});
}

// 以 DFS 列出所有經過全部 requiredNodes 且避開全部 avoidedNodes 的組合路徑。
std::vector<Netlist::CombinationalPath>
Netlist::enumerateCombinationalPathsThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> required;
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, requiredNodes, required) ||
        !resolvePathNodes(*this, avoidedNodes, avoided)) {
        return std::vector<CombinationalPath>();
    }
    return enumeratePathsMatching(*this, startNet, endNet, required, avoided);
}

// 判斷所有既有路徑是否都經過 requiredNodes 中每一個節點；無原始路徑時回傳 false。
bool Netlist::everyPathPassesThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    std::vector<ResolvedPathNode> required;
    if (!resolvePathNodes(*this, requiredNodes, required) ||
        !hasCombinationalPath(startNet, endNet)) {
        return false;
    }
    for (const PathNode& requiredNode : requiredNodes) {
        if (hasCombinationalPathAvoiding(startNet, endNet, {requiredNode})) {
            return false;
        }
    }
    return true;
}

// 判斷所有既有路徑是否都避開 avoidedNodes 中每一個節點；無原始路徑時回傳 false。
bool Netlist::everyPathAvoids(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, avoidedNodes, avoided) ||
        !hasCombinationalPath(startNet, endNet)) {
        return false;
    }
    for (const PathNode& avoidedNode : avoidedNodes) {
        if (hasCombinationalPathThrough(startNet, endNet, {avoidedNode})) {
            return false;
        }
    }
    return true;
}
