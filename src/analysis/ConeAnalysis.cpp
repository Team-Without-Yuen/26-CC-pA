#include "include/core/Netlist.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 將 cone path 回傳的 net ID 序列轉成 net name 序列。
// findLongestPathInCone / findShortestPathInCone 的 raw path 存的是 net ID，不是 gate ID。
static std::vector<std::string> coneNetPathToNames(
    const Netlist& netlist,
    const std::vector<int>& netPath) {
    std::vector<std::string> pathNames;
    pathNames.reserve(netPath.size());
    for (int netId : netPath) {
        if (netId >= 0 && netId < static_cast<int>(netlist.getNetCount())) {
            pathNames.push_back(netlist.getNet(netId).name);
        }
    }
    return pathNames;
}

//  支援 Bus 的 Transitive Fanin Cone (多源 BFS)
ConeResult Netlist::getTransitiveFaninCone(const std::string& netName) const {
    ConeResult result;
    
    // 展開 Bus，取得所有起點
    std::vector<int> startIds = expandNetToBits(netName);
    if (startIds.empty()) return result;

    result.rootNetIds = startIds;
    std::queue<int> q; // BFS

    // 將所有起點同時推入 Queue 並標記為已訪問
    for (int id : startIds) {
        q.push(id);
        result.netIds.insert(id);
    }

    // BFS 核心邏輯
    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        const Net& net = nets[currNetId];

        if (net.driverGateId >= 0) {
            const Gate& driver = gates[net.driverGateId];

            if (driver.type == GateType::DFF) continue;

            for (int i = 0; i < (int)driver.inputNetIds.size(); i++) {
                int inNetId = driver.inputNetIds[i];
                if (inNetId < 0) continue;
                result.children[currNetId].push_back(inNetId);
                
                if (result.netIds.insert(inNetId).second) { 
                    q.push(inNetId);
                }
            }
        }
    }

    return result;
}

//  支援 Bus 的 Transitive Fanout Cone (多源 BFS)
ConeResult Netlist::getTransitiveFanoutCone(const std::string& netName) const {
    ConeResult result;
    
    // 展開 Bus，取得所有起點
    std::vector<int> startIds = expandNetToBits(netName);
    if (startIds.empty()) return result;

    result.rootNetIds = startIds;
    std::queue<int> q;

    // 將所有起點同時推入 Queue
    for (int id : startIds) {
        q.push(id);
        result.netIds.insert(id);
    }

    // BFS 核心邏輯
    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        const Net& net = nets[currNetId];

        for (int i = 0; i < (int)net.loadGateIds.size(); i++) {
            int gateId = net.loadGateIds[i];
            const Gate& gate = gates[gateId];

            if (gate.type == GateType::DFF) continue;

            int outNetId = gate.outputNetId;
            if (outNetId < 0) continue;

            result.children[currNetId].push_back(outNetId);
            
            if (result.netIds.insert(outNetId).second) { 
                q.push(outNetId);
            }
        }
    }

    return result;
}

ConeResult Netlist::getGateTransitiveFaninCone(const std::string& gateName) const {
    int gateId = getGateId(gateName);
    if (gateId < 0) return ConeResult(); // 找不到該 Gate

    const Gate& gate = gates[gateId];
    
    // 如果這個 Gate 沒有輸出線 (例如輸出懸空)
    if (gate.outputNetId < 0) return ConeResult(); 

    // Gate 的 Fanin 錐，就是它「輸出線」的 Fanin 錐
    return getTransitiveFaninCone(nets[gate.outputNetId].name);
}

ConeResult Netlist::getGateTransitiveFanoutCone(const std::string& gateName) const {
    int gateId = getGateId(gateName);
    if (gateId < 0) return ConeResult();

    const Gate& gate = gates[gateId];
    if (gate.outputNetId < 0) return ConeResult();

    // Gate 的 Fanout 錐，就是它「輸出線」的 Fanout 錐
    return getTransitiveFanoutCone(nets[gate.outputNetId].name);
}

// 將 ConeResult 內的 net set 轉成排序後的 net ID 陣列，讓回傳結果穩定。
std::vector<int> Netlist::getConeNetIds(const ConeResult& cone) const {
    std::vector<int> netIds;
    netIds.reserve(cone.netIds.size());
    for (int netId : cone.netIds) {
        if (isValidNetId(netId)) {
            netIds.push_back(netId);
        }
    }
    std::sort(netIds.begin(), netIds.end());
    return netIds;
}

// 將 ConeResult 內的 net IDs 轉成 net names。
std::vector<std::string> Netlist::getConeNetNames(const ConeResult& cone) const {
    std::vector<std::string> netNames;
    const std::vector<int> netIds = getConeNetIds(cone);
    netNames.reserve(netIds.size());
    for (int netId : netIds) {
        netNames.push_back(nets[netId].name);
    }
    return netNames;
}

// 計算 ConeResult 內有效 net 數量。
size_t Netlist::getConeNetCount(const ConeResult& cone) const {
    return getConeNetIds(cone).size();
}

// 從 cone 的 net-to-net edge 反推參與 cone 的 gate IDs。
std::vector<int> Netlist::getConeGateIds(const ConeResult& cone) const {
    std::unordered_set<int> gateSet;
    for (const auto& edgeList : cone.children) {
        const int fromNetId = edgeList.first;
        if (!isValidNetId(fromNetId)) {
            continue;
        }

        for (int toNetId : edgeList.second) {
            if (!isValidNetId(toNetId)) {
                continue;
            }

            const int fromDriverId = nets[fromNetId].driverGateId;
            if (isValidGateId(fromDriverId)) {
                const Gate& gate = gates[fromDriverId];
                if (gate.type != GateType::DFF &&
                    std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), toNetId) !=
                        gate.inputNetIds.end()) {
                    gateSet.insert(fromDriverId);
                    continue;
                }
            }

            const int toDriverId = nets[toNetId].driverGateId;
            if (isValidGateId(toDriverId)) {
                const Gate& gate = gates[toDriverId];
                if (gate.type != GateType::DFF &&
                    std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), fromNetId) !=
                        gate.inputNetIds.end()) {
                    gateSet.insert(toDriverId);
                }
            }
        }
    }

    std::vector<int> gateIds(gateSet.begin(), gateSet.end());
    std::sort(gateIds.begin(), gateIds.end());
    return gateIds;
}

// 將 ConeResult 內參與 cone 的 gate IDs 轉成 gate names。
std::vector<std::string> Netlist::getConeGateNames(const ConeResult& cone) const {
    std::vector<std::string> gateNames;
    const std::vector<int> gateIds = getConeGateIds(cone);
    gateNames.reserve(gateIds.size());
    for (int gateId : gateIds) {
        gateNames.push_back(gates[gateId].instName);
    }
    return gateNames;
}

// 計算 ConeResult 內參與 cone 的 gate 數量。
size_t Netlist::getConeGateCount(const ConeResult& cone) const {
    return getConeGateIds(cone).size();
}

// --- 針對 Net 的 Fanin ---
std::vector<std::string> Netlist::getTransitiveFaninConeGateNames(const std::string& netName) const {
    return getConeGateNames(getTransitiveFaninCone(netName));
}

size_t Netlist::getTransitiveFaninConeGateCount(const std::string& netName) const {
    return getConeGateCount(getTransitiveFaninCone(netName));
}

// --- 針對 Net 的 Fanout ---
std::vector<std::string> Netlist::getTransitiveFanoutConeGateNames(const std::string& netName) const {
    return getConeGateNames(getTransitiveFanoutCone(netName));
}

size_t Netlist::getTransitiveFanoutConeGateCount(const std::string& netName) const {
    return getConeGateCount(getTransitiveFanoutCone(netName));
}

// --- 針對 Gate 的 Fanin ---
std::vector<std::string> Netlist::getGateTransitiveFaninConeGateNames(const std::string& gateName) const {
    return getConeGateNames(getGateTransitiveFaninCone(gateName));
}

size_t Netlist::getGateTransitiveFaninConeGateCount(const std::string& gateName) const {
    return getConeGateCount(getGateTransitiveFaninCone(gateName));
}

// --- 針對 Gate 的 Fanout ---
std::vector<std::string> Netlist::getGateTransitiveFanoutConeGateNames(const std::string& gateName) const {
    return getConeGateNames(getGateTransitiveFanoutCone(gateName));
}

size_t Netlist::getGateTransitiveFanoutConeGateCount(const std::string& gateName) const {
    return getConeGateCount(getGateTransitiveFanoutCone(gateName));
}

// 執行統一 ConeQuery；建立 transitive cone 並整理成高階 ConeReport。
Netlist::ConeReport Netlist::runConeQuery(const ConeQuery& query) const {
    ConeReport report;
    report.type = query.type;

    switch (query.type) {
    case ConeQueryType::NetTransitiveFanin:
        if (query.netName.empty()) {
            report.message = "NetTransitiveFanin requires netName";
            return report;
        }
        if (expandNetToBits(query.netName).empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        report.sourceName = query.netName;
        report.sourceId = getNetId(query.netName);
        report.cone = getTransitiveFaninCone(query.netName);
        report.message = "Net transitive fanin cone";
        break;

    case ConeQueryType::NetTransitiveFanout:
        if (query.netName.empty()) {
            report.message = "NetTransitiveFanout requires netName";
            return report;
        }
        if (expandNetToBits(query.netName).empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        report.sourceName = query.netName;
        report.sourceId = getNetId(query.netName);
        report.cone = getTransitiveFanoutCone(query.netName);
        report.message = "Net transitive fanout cone";
        break;

    case ConeQueryType::GateTransitiveFanin:
        if (query.gateName.empty()) {
            report.message = "GateTransitiveFanin requires gateName";
            return report;
        }
        report.sourceId = getGateId(query.gateName);
        if (!isValidGateId(report.sourceId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.sourceName = query.gateName;
        report.cone = getGateTransitiveFaninCone(query.gateName);
        report.message = "Gate transitive fanin cone";
        break;

    case ConeQueryType::GateTransitiveFanout:
        if (query.gateName.empty()) {
            report.message = "GateTransitiveFanout requires gateName";
            return report;
        }
        report.sourceId = getGateId(query.gateName);
        if (!isValidGateId(report.sourceId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.sourceName = query.gateName;
        report.cone = getGateTransitiveFanoutCone(query.gateName);
        report.message = "Gate transitive fanout cone";
        break;
    }

    if (report.cone.rootNetIds.empty()) {
        report.message += " is empty";
        return report;
    }

    report.ok = true;
    report.exists = true;
    report.netCount = getConeNetCount(report.cone);
    report.gateCount = getConeGateCount(report.cone);

    if (query.includeIds) {
        report.rootNetIds = report.cone.rootNetIds;
        report.netIds = getConeNetIds(report.cone);
        report.gateIds = getConeGateIds(report.cone);
    }

    if (query.includeNames) {
        for (int rootNetId : report.cone.rootNetIds) {
            if (isValidNetId(rootNetId)) {
                report.rootNetNames.push_back(nets[rootNetId].name);
            }
        }
        report.netNames = getConeNetNames(report.cone);
        report.gateNames = getConeGateNames(report.cone);
    }

    if (query.includeLocalPaths) {
        const std::pair<int, std::vector<int>> longest = findLongestPathInCone(report.cone);
        const std::pair<int, std::vector<int>> shortest = findShortestPathInCone(report.cone);
        report.longestDepth = longest.first;
        report.shortestDepth = shortest.first;

        if (query.includeIds) {
            report.longestPathNetIds = longest.second;
            report.shortestPathNetIds = shortest.second;
        }
        if (query.includeNames) {
            report.longestPathNetNames = coneNetPathToNames(*this, longest.second);
            report.shortestPathNetNames = coneNetPathToNames(*this, shortest.second);
        }
    }

    return report;
}

//  分析邏輯錐：尋找錐體內的最長路徑 (Local Critical Path)
//  回傳值：std::pair<深度, 路徑的 Net IDs>
std::pair<int, std::vector<int>> Netlist::findLongestPathInCone(const ConeResult& cone) const {
    // 記錄 <當前節點, <最大深度, 最佳路徑的下一個節點>>
    std::unordered_map<int, std::pair<int, int>> memo;
    std::unordered_set<int> visiting;

    // 內部 Lambda 遞迴函式：回傳「從目前 net 往下走的最大深度」。
    // 若偵測到 combinational loop，回傳 -1，避免無限遞迴。
    std::function<int(int)> dfsLongest = [&](int currNetId) -> int {
        if (memo.count(currNetId)) {
            return memo[currNetId].first;
        }

        if (visiting.count(currNetId) != 0) {
            return -1;
        }

        visiting.insert(currNetId);

        int maxLength = -1;
        int bestNextNode = -1; // -1 代表沒有下游 (自己是最底端)

        auto it = cone.children.find(currNetId);
        if (it != cone.children.end() && !it->second.empty()) {
            for (int childNetId : it->second) {
                
                int childLength = dfsLongest(childNetId);
                if (childLength < 0) {
                    continue;
                }
                
                // 如果這條路更長，就更新最大深度與「下一個節點」
                if (childLength + 1 > maxLength) {
                    maxLength = childLength + 1;
                    bestNextNode = childNetId;
                }
            }
        } else {
            maxLength = 0;
        }

        visiting.erase(currNetId);

        // 存檔並回傳
        memo[currNetId] = {maxLength, bestNextNode};
        return maxLength;
    };

    // 啟動引擎
    int globalMaxLength = -1;
    int bestRootId = -1;

    for (int rootId : cone.rootNetIds) {
        int length = dfsLongest(rootId);
        if (length > globalMaxLength) {
            globalMaxLength = length;
            bestRootId = rootId;
        }
    }

    // 防呆：如果沒有找到任何路徑
    if (globalMaxLength == -1) {
        return {-1, {}};
    }

    // 路徑重建 (只執行一次)
    std::vector<int> longestPath;
    int curr = bestRootId;
    std::unordered_set<int> rebuiltPath;
    
    while (curr != -1) {
        if (!rebuiltPath.insert(curr).second) {
            return {-1, {}};
        }
        longestPath.push_back(curr);
        curr = memo[curr].second; // 順藤摸瓜找下一個節點
    }

    return {globalMaxLength, longestPath};
}

std::pair<int, std::vector<int>> Netlist::findShortestPathInCone(const ConeResult& cone) const {
    if (cone.rootNetIds.empty()) return {-1, {}};

    std::queue<int> q;
    std::unordered_set<int> visited;
    std::unordered_map<int, int> parent; // 紀錄是誰擴展到當前節點的，用於重建路徑

    // 初始化多源 BFS
    for (int rootId : cone.rootNetIds) {
        q.push(rootId);
        visited.insert(rootId);
        parent[rootId] = -1; // -1 代表起點
    }

    int targetLeaf = -1;
    int currentDepth = 0;

    // BFS 擴展
    while (!q.empty()) {
        int levelSize = q.size();
        
        for (int i = 0; i < levelSize; ++i) {
            int curr = q.front();
            q.pop();

            auto it = cone.children.find(curr);
            
            // 如果這是一個葉節點 (沒有 downstream)
            if (it == cone.children.end() || it->second.empty()) {
                targetLeaf = curr;
                break; // 找到第一個葉節點就是全局最短路徑，直接中斷內圈！
            }

            for (int childNetId : it->second) {
                if (visited.find(childNetId) == visited.end()) {
                    visited.insert(childNetId);
                    parent[childNetId] = curr; // 紀錄路徑來源
                    q.push(childNetId);
                }
            }
        }

        if (targetLeaf != -1) {
            break; // 中斷外圈
        }
        currentDepth++;
    }

    if (targetLeaf == -1) return {-1, {}};

    // 路徑重建
    std::vector<int> path;
    int curr = targetLeaf;
    while (curr != -1) {
        path.push_back(curr);
        curr = parent[curr];
    }
    std::reverse(path.begin(), path.end()); // 因為是從葉節點往回追溯，所以需要反轉

    return {currentDepth, path};
}

// 針對 Net 的 Fanin Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFaninConeLongestPath(const std::string& netName) const {
    auto rawResult = findLongestPathInCone(getTransitiveFaninCone(netName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}
// 針對 Net 的 Fanin Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFaninConeShortestPath(const std::string& netName) const {
    auto rawResult = findShortestPathInCone(getTransitiveFaninCone(netName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}

// 針對 Net 的 Fanout Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFanoutConeLongestPath(const std::string& netName) const {
    auto rawResult = findLongestPathInCone(getTransitiveFanoutCone(netName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}
// 針對 Net 的 Fanout Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFanoutConeShortestPath(const std::string& netName) const {
    auto rawResult = findShortestPathInCone(getTransitiveFanoutCone(netName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}

// 針對 Gate 的 Fanin Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFaninConeLongestPath(const std::string& gateName) const {
    auto rawResult = findLongestPathInCone(getGateTransitiveFaninCone(gateName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}
// 針對 Gate 的 Fanin Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFaninConeShortestPath(const std::string& gateName) const {
    auto rawResult = findShortestPathInCone(getGateTransitiveFaninCone(gateName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}

// 針對 Gate 的 Fanout Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFanoutConeLongestPath(const std::string& gateName) const {
    auto rawResult = findLongestPathInCone(getGateTransitiveFanoutCone(gateName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}
// 針對 Gate 的 Fanout Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFanoutConeShortestPath(const std::string& gateName) const {
    auto rawResult = findShortestPathInCone(getGateTransitiveFanoutCone(gateName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}
