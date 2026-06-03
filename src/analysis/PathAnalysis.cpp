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

// ─────────────────────────────────────────────────────────────────────────────
//  使用 DFS + 動態規劃 (Memoization) 尋找最長路徑
//  回傳值：std::pair<深度, 路徑>。如果深度為 -1 代表此路不通或無法滿足條件。
// ─────────────────────────────────────────────────────────────────────────────
std::pair<int, Netlist::CombinationalPath> findLongestPathDepthFirst(
    const Netlist& netlist,
    int currentNetId,
    int endNetId,
    const std::vector<ResolvedPathNode>& requiredNodes,
    const std::vector<ResolvedPathNode>& avoidedNodes,
    std::vector<unsigned char> passedRequired,
    std::unordered_map<std::string, std::pair<int, Netlist::CombinationalPath>>& memo,
    std::unordered_set<std::string>& inStack) {
    
    // 產生當前狀態的唯一 Key (結合 Net ID 與狀態)
    std::string stateKey = makeStateKey(currentNetId, passedRequired);

    // 抵達終點的終止條件
    if (currentNetId == endNetId) {
        if (allRequiredPassed(passedRequired)) {
            Netlist::CombinationalPath basePath;
            basePath.netIds.push_back(currentNetId);
            return {0, basePath}; // 深度 0，路徑只含終點
        }
        return {-1, {}}; // 雖然到了終點，但必經條件沒滿足，算死路
    }

    // 查表 (Memoization)：確認這個狀態
    auto it = memo.find(stateKey);
    if (it != memo.end()) {
        return it->second;
    }

    // 防無窮迴圈 (Combinational Loop)
    if (inStack.count(stateKey)) {
        return {-1, {}};
    }

    inStack.insert(stateKey);

    const Net& currentNet = netlist.getNet(currentNetId);
    std::pair<int, Netlist::CombinationalPath> bestResult = {-1, {}};

    // 探索所有下游路徑
    for (int gateId : currentNet.loadGateIds) {
        const Gate& gate = netlist.getGate(gateId);
        
        // 避開 DFF, 輸出懸空, 或黑名單 Gate
        if (gate.type == GateType::DFF ||
            gate.outputNetId < 0 ||
            containsGate(avoidedNodes, gateId)) {
            continue;
        }

        const int nextNetId = gate.outputNetId;
        
        // 避開黑名單 Net
        if (containsNet(avoidedNodes, nextNetId)) {
            continue;
        }

        // 拷貝並更新下一步的打勾狀態
        std::vector<unsigned char> nextPassed = passedRequired;
        markRequiredGate(requiredNodes, gateId, nextPassed);
        markRequiredNet(requiredNodes, nextNetId, nextPassed);

        // 往下遞迴尋找最長路徑
        auto result = findLongestPathDepthFirst(
            netlist, nextNetId, endNetId, requiredNodes, avoidedNodes, 
            nextPassed, memo, inStack
        );

        // 如果這條岔路走得通，比對看看是不是目前最長的
        if (result.first >= 0) {
            if (result.first + 1 > bestResult.first) {
                bestResult.first = result.first + 1;
                bestResult.second = result.second; // 拷貝下游回傳的最佳路徑
                
                // 用 push_back，避免 O(N)
                bestResult.second.gateIds.push_back(gateId);
                bestResult.second.netIds.push_back(currentNetId);
            }
        }
    }

    // 清理並存檔
    inStack.erase(stateKey);
    memo[stateKey] = bestResult;
    return bestResult;
}

//  尋找符合 required/avoided 條件的「最長」組合邏輯路徑
Netlist::CombinationalPath findLongestPathMatching(
    const Netlist& netlist,
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<ResolvedPathNode>& requiredNodes,
    const std::vector<ResolvedPathNode>& avoidedNodes) {
    
    Netlist::CombinationalPath path;
    const int startNetId = netlist.getNetId(startNet);
    const int endNetId = netlist.getNetId(endNet);
    
    // 防呆檢查
    if (startNetId < 0 || endNetId < 0 ||
        containsNet(avoidedNodes, startNetId) ||
        containsNet(avoidedNodes, endNetId)) {
        return path;
    }

    // 初始化狀態
    std::vector<unsigned char> passedRequired(requiredNodes.size(), 0);
    markRequiredNet(requiredNodes, startNetId, passedRequired);

    // 準備記憶體與防呆堆疊
    std::unordered_map<std::string, std::pair<int, Netlist::CombinationalPath>> memo;
    std::unordered_set<std::string> inStack;

    // 啟動引擎
    auto result = findLongestPathDepthFirst(
        netlist, startNetId, endNetId, requiredNodes, avoidedNodes,
        passedRequired, memo, inStack
    );

    // 深度 < 0 代表沒找到合法路徑，回傳空的 path
    if (result.first >= 0) {
        path = result.second;
        // 因為 DFS 是由後往前 push_back，這裡做最後一次翻轉
        std::reverse(path.netIds.begin(), path.netIds.end());
        std::reverse(path.gateIds.begin(), path.gateIds.end());
    }

    return path;
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

// ─────────────────────────────────────────────────────────────────────────────
//  最長組合邏輯路徑 (Longest Combinational Path) API 
// ─────────────────────────────────────────────────────────────────────────────

//  最長路徑的布林判斷 (借用findAnyCombinationalPath，有路徑 = 有最長路徑)

// 找到兩條 net 之間的最長組合邏輯路徑 (無特殊條件)。
Netlist::CombinationalPath Netlist::findLongestCombinationalPath(
    const std::string& startNet,
    const std::string& endNet) const {
    
    // 直接呼叫我們寫好的底層引擎，傳入空的條件陣列 {}
    return findLongestPathMatching(*this, startNet, endNet, {}, {});
}

// 找到兩條 net 之間，且「避開」全部指定節點的最長組合邏輯路徑。
Netlist::CombinationalPath Netlist::findLongestCombinationalPathAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    
    std::vector<ResolvedPathNode> avoided;
    // 將外部的字串名稱轉換為內部的高速比對 ID
    if (!resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath(); // 轉換失敗 (找不到節點) 直接回傳空路徑
    }
    return findLongestPathMatching(*this, startNet, endNet, {}, avoided);
}

// 找到兩條 net 之間，且「經過」全部指定節點的最長組合邏輯路徑。
Netlist::CombinationalPath Netlist::findLongestCombinationalPathThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    
    std::vector<ResolvedPathNode> required;
    if (!resolvePathNodes(*this, requiredNodes, required)) {
        return CombinationalPath();
    }
    return findLongestPathMatching(*this, startNet, endNet, required, {});
}

// 找到兩條 net 之間，「經過」全部 requiredNodes 且「避開」全部 avoidedNodes 的最長路徑。
Netlist::CombinationalPath Netlist::findLongestCombinationalPathThroughAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes,
    const std::vector<PathNode>& avoidedNodes) const {
    
    std::vector<ResolvedPathNode> required;
    std::vector<ResolvedPathNode> avoided;
    
    // 必須 required 和 avoided 兩者都成功解析才繼續
    if (!resolvePathNodes(*this, requiredNodes, required) ||
        !resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath();
    }
    return findLongestPathMatching(*this, startNet, endNet, required, avoided);
}

// ─────────────────────────────────────────────────────────────────────────────
//  最短組合邏輯路徑 (Shortest Combinational Path) API
// ─────────────────────────────────────────────────────────────────────────────

// 找到兩條 net 之間的最短組合邏輯路徑；底層 BFS 第一個合法結果即為最短路徑。
Netlist::CombinationalPath Netlist::findShortestCombinationalPath(
    const std::string& startNet,
    const std::string& endNet) const {
    return findPathMatching(*this, startNet, endNet, {}, {});
}

// 找到兩條 net 之間，且避開全部指定節點的最短組合邏輯路徑。
Netlist::CombinationalPath Netlist::findShortestCombinationalPathAvoiding(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& avoidedNodes) const {
    std::vector<ResolvedPathNode> avoided;
    if (!resolvePathNodes(*this, avoidedNodes, avoided)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, {}, avoided);
}

// 找到兩條 net 之間，且經過全部指定節點的最短組合邏輯路徑。
Netlist::CombinationalPath Netlist::findShortestCombinationalPathThrough(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<PathNode>& requiredNodes) const {
    std::vector<ResolvedPathNode> required;
    if (!resolvePathNodes(*this, requiredNodes, required)) {
        return CombinationalPath();
    }
    return findPathMatching(*this, startNet, endNet, required, {});
}

// 找到兩條 net 之間，且經過全部 requiredNodes 並避開全部 avoidedNodes 的最短路徑。
Netlist::CombinationalPath Netlist::findShortestCombinationalPathThroughAvoiding(
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

// 取得指定 DFF 的 named input pin 所連接的 net ID；用於 DFF.D / DFF.CK 等 sequential 查詢。
int Netlist::getDffInputNetId(int dffGateId, const std::string& pinName) const {
    if (dffGateId < 0 || dffGateId >= static_cast<int>(gates.size())) {
        return -1;
    }

    const Gate& dff = gates[dffGateId];
    if (dff.type != GateType::DFF) {
        return -1;
    }

    for (size_t i = 0; i < dff.inputPinNames.size() && i < dff.inputNetIds.size(); ++i) {
        if (dff.inputPinNames[i] == pinName) {
            return dff.inputNetIds[i];
        }
    }
    return -1;
}

// 取得所有 Primary Input port 展開後的 net ID；bus port 會保留每一個 bit net。
std::vector<int> Netlist::getPrimaryInputNetIds() const {
    std::vector<int> netIds;
    for (const Port& port : primaryInputs) {
        for (int netId : port.netIds) {
            if (netId >= 0 && netId < static_cast<int>(nets.size())) {
                netIds.push_back(netId);
            }
        }
    }
    return netIds;
}

// 取得所有 Primary Output port 展開後的 net ID；bus port 會保留每一個 bit net。
std::vector<int> Netlist::getPrimaryOutputNetIds() const {
    std::vector<int> netIds;
    for (const Port& port : primaryOutputs) {
        for (int netId : port.netIds) {
            if (netId >= 0 && netId < static_cast<int>(nets.size())) {
                netIds.push_back(netId);
            }
        }
    }
    return netIds;
}

// 取得指定 DFF 的 Q/output net ID；DFF 的 outputNetId 目前代表 Q pin。
int Netlist::getDffOutputNetId(int dffGateId) const {
    if (dffGateId < 0 || dffGateId >= static_cast<int>(gates.size())) {
        return -1;
    }

    const Gate& dff = gates[dffGateId];
    if (dff.type != GateType::DFF ||
        dff.outputNetId < 0 ||
        dff.outputNetId >= static_cast<int>(nets.size())) {
        return -1;
    }
    return dff.outputNetId;
}

// 取得指定 gate 的 output net ID；primitive gate 與 DFF 都可用。
int Netlist::getGateOutputNetId(int gateId) const {
    if (gateId < 0 || gateId >= static_cast<int>(gates.size())) {
        return -1;
    }

    const int outputNetId = gates[gateId].outputNetId;
    if (outputNetId < 0 || outputNetId >= static_cast<int>(nets.size())) {
        return -1;
    }
    return outputNetId;
}

// 依 input pin 的位置取得指定 gate 的 input net ID；primitive gate 通常使用這個版本。
int Netlist::getGateInputNetId(int gateId, int pinIndex) const {
    if (gateId < 0 || gateId >= static_cast<int>(gates.size()) || pinIndex < 0) {
        return -1;
    }

    const Gate& gate = gates[gateId];
    if (pinIndex >= static_cast<int>(gate.inputNetIds.size())) {
        return -1;
    }

    const int inputNetId = gate.inputNetIds[static_cast<size_t>(pinIndex)];
    if (inputNetId < 0 || inputNetId >= static_cast<int>(nets.size())) {
        return -1;
    }
    return inputNetId;
}

// 依 named input pin 取得指定 gate 的 input net ID；DFF 的 D/CK/RN/SN 會使用這個版本。
int Netlist::getGateInputNetId(int gateId, const std::string& pinName) const {
    if (gateId < 0 || gateId >= static_cast<int>(gates.size())) {
        return -1;
    }

    const Gate& gate = gates[gateId];
    for (size_t i = 0; i < gate.inputPinNames.size() && i < gate.inputNetIds.size(); ++i) {
        if (gate.inputPinNames[i] == pinName) {
            const int inputNetId = gate.inputNetIds[i];
            if (inputNetId >= 0 && inputNetId < static_cast<int>(nets.size())) {
                return inputNetId;
            }
            return -1;
        }
    }
    return -1;
}

// 將單一抽象 PathEndpoint 解析成實際 net ID 陣列；空結果代表解析失敗或沒有對應 net。
std::vector<int> Netlist::resolvePathEndpoint(const PathEndpoint& endpoint) const {
    std::vector<int> netIds;

    auto appendValidNet = [&](int netId) {
        if (netId >= 0 && netId < static_cast<int>(nets.size())) {
            netIds.push_back(netId);
        }
    };

    auto appendPortNets = [&](const std::vector<Port>& ports) {
        for (const Port& port : ports) {
            if (!endpoint.name.empty() && port.name != endpoint.name) {
                continue;
            }
            for (int netId : port.netIds) {
                appendValidNet(netId);
            }
        }
    };

    switch (endpoint.type) {
    case PathEndpointType::SpecificNet:
        appendValidNet(getNetId(endpoint.name));
        break;
    case PathEndpointType::PrimaryInput:
        appendPortNets(primaryInputs);
        break;
    case PathEndpointType::PrimaryOutput:
        appendPortNets(primaryOutputs);
        break;
    case PathEndpointType::DffQ:
        appendValidNet(getDffOutputNetId(getGateId(endpoint.name)));
        break;
    case PathEndpointType::DffD:
        appendValidNet(getGateInputNetId(getGateId(endpoint.name), "D"));
        break;
    case PathEndpointType::DffClock:
        appendValidNet(getGateInputNetId(getGateId(endpoint.name),
                                         endpoint.pinName.empty() ? "CK" : endpoint.pinName));
        break;
    case PathEndpointType::DffReset:
        if (!endpoint.pinName.empty()) {
            appendValidNet(getGateInputNetId(getGateId(endpoint.name), endpoint.pinName));
        } else {
            appendValidNet(getGateInputNetId(getGateId(endpoint.name), "RN"));
            appendValidNet(getGateInputNetId(getGateId(endpoint.name), "SN"));
        }
        break;
    case PathEndpointType::GateOutput:
        appendValidNet(getGateOutputNetId(getGateId(endpoint.name)));
        break;
    case PathEndpointType::GateInput: {
        const int gateId = getGateId(endpoint.name);
        if (!endpoint.pinName.empty()) {
            appendValidNet(getGateInputNetId(gateId, endpoint.pinName));
        } else {
            appendValidNet(getGateInputNetId(gateId, endpoint.pinIndex));
        }
        break;
    }
    }

    return netIds;
}

// 將多個抽象 PathEndpoint 解析成 net ID，並保留首次出現順序、移除重複 net。
std::vector<int> Netlist::resolvePathEndpoints(
    const std::vector<PathEndpoint>& endpoints) const {
    std::vector<int> resolvedNetIds;
    std::unordered_set<int> seenNetIds;

    for (const PathEndpoint& endpoint : endpoints) {
        const std::vector<int> endpointNetIds = resolvePathEndpoint(endpoint);
        for (int netId : endpointNetIds) {
            if (seenNetIds.insert(netId).second) {
                resolvedNetIds.push_back(netId);
            }
        }
    }

    return resolvedNetIds;
}

// 執行統一 path query；目前支援 Exists、FindAny、EnumerateAll、MaxDepth 與 every-path 類模式。
Netlist::PathQueryResult Netlist::runPathQuery(const PathQuery& query) const {
    PathQueryResult result;
    if (!query.combinationalOnly) {
        return result;
    }

    const std::vector<int> startNetIds = resolvePathEndpoints(query.startpoints);
    const std::vector<int> endNetIds = resolvePathEndpoints(query.endpoints);
    if (startNetIds.empty() || endNetIds.empty()) {
        return result;
    }

    switch (query.mode) {
    case PathQueryMode::Exists:
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                if (hasCombinationalPathThroughAvoiding(
                        nets[startNetId].name,
                        nets[endNetId].name,
                        query.requiredNodes,
                        query.avoidedNodes)) {
                    result.exists = true;
                    return result;
                }
            }
        }
        return result;

    case PathQueryMode::FindAny:
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                CombinationalPath path = findAnyCombinationalPathThroughAvoiding(
                    nets[startNetId].name,
                    nets[endNetId].name,
                    query.requiredNodes,
                    query.avoidedNodes);
                if (path.exists()) {
                    result.exists = true;
                    result.depth = path.depth();
                    result.path = path;
                    return result;
                }
            }
        }
        return result;

    case PathQueryMode::EnumerateAll:
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                std::vector<CombinationalPath> paths =
                    enumerateCombinationalPathsThroughAvoiding(
                        nets[startNetId].name,
                        nets[endNetId].name,
                        query.requiredNodes,
                        query.avoidedNodes);
                result.paths.insert(result.paths.end(), paths.begin(), paths.end());
            }
        }
        result.exists = !result.paths.empty();
        if (result.exists) {
            result.path = result.paths.front();
            result.depth = result.path.depth();
        }
        return result;

    case PathQueryMode::MinDepth:
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                CombinationalPath path = findShortestCombinationalPathThroughAvoiding(
                    nets[startNetId].name,
                    nets[endNetId].name,
                    query.requiredNodes,
                    query.avoidedNodes);
                if (path.exists() && (!result.exists || path.depth() < result.depth)) {
                    result.exists = true;
                    result.depth = path.depth();
                    result.path = path;
                }
            }
        }
        return result;

    case PathQueryMode::MaxDepth:
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                CombinationalPath path = findLongestCombinationalPathThroughAvoiding(
                    nets[startNetId].name,
                    nets[endNetId].name,
                    query.requiredNodes,
                    query.avoidedNodes);
                if (path.exists() && path.depth() > result.depth) {
                    result.exists = true;
                    result.depth = path.depth();
                    result.path = path;
                }
            }
        }
        return result;

    case PathQueryMode::EveryPathThrough: {
        if (query.requiredNodes.empty()) {
            return result;
        }

        bool sawAnyPath = false;
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                const std::string& startName = nets[startNetId].name;
                const std::string& endName = nets[endNetId].name;
                if (!hasCombinationalPath(startName, endName)) {
                    continue;
                }
                sawAnyPath = true;
                if (!everyPathPassesThrough(startName, endName, query.requiredNodes)) {
                    result.exists = false;
                    return result;
                }
            }
        }
        result.exists = sawAnyPath;
        return result;
    }

    case PathQueryMode::EveryPathAvoids: {
        if (query.avoidedNodes.empty()) {
            return result;
        }

        bool sawAnyPath = false;
        for (int startNetId : startNetIds) {
            for (int endNetId : endNetIds) {
                const std::string& startName = nets[startNetId].name;
                const std::string& endName = nets[endNetId].name;
                if (!hasCombinationalPath(startName, endName)) {
                    continue;
                }
                sawAnyPath = true;
                if (!everyPathAvoids(startName, endName, query.avoidedNodes)) {
                    result.exists = false;
                    return result;
                }
            }
        }
        result.exists = sawAnyPath;
        return result;
    }

    }

    return result;
}

// 掃描所有 PI bit 到所有 DFF D-pin 的組合路徑，找出最大的 logic depth 與 witness path。
std::pair<int, Netlist::CombinationalPath>
Netlist::getMaximumLogicDepthFromPiToDffD() const {
    int bestDepth = -1;
    CombinationalPath bestPath;
    const std::vector<int> dffGateIds = getGatesByType(GateType::DFF);

    for (const Port& inputPort : primaryInputs) {
        for (int piNetId : inputPort.netIds) {
            if (piNetId < 0 || piNetId >= static_cast<int>(nets.size())) {
                continue;
            }

            const std::string& piNetName = nets[piNetId].name;
            for (int dffGateId : dffGateIds) {
                const int dNetId = getDffInputNetId(dffGateId, "D");
                if (dNetId < 0 || dNetId >= static_cast<int>(nets.size())) {
                    continue;
                }

                const std::string& dNetName = nets[dNetId].name;
                CombinationalPath path =
                    findLongestCombinationalPath(piNetName, dNetName);
                if (path.exists() && path.depth() > bestDepth) {
                    bestDepth = path.depth();
                    bestPath = path;
                }
            }
        }
    }

    return {bestDepth, bestPath};
}
