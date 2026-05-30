#include "include/core/Netlist.h"
#include "include/lib/cadical.hpp"
#include <algorithm>
#include <unordered_set>
#include <queue>
#include <chrono>

// 客製化 CaDiCaL 終止器：用來設定 Wall-clock Time 限制
class TimeLimitTerminator : public CaDiCaL::Terminator {
private:
    std::chrono::time_point<std::chrono::steady_clock> start_time;
    double time_limit_seconds;
    int call_counter; // 新增一個計數器

public:
    // 傳入想設定的秒數，並記錄當下時間
    TimeLimitTerminator(double limit) : time_limit_seconds(limit) {
        start_time = std::chrono::steady_clock::now();
    }

    // CaDiCaL 內部會在解題過程中頻繁呼叫這個函式
    // 如果回傳 true，CaDiCaL 就會立刻中斷並回傳 UNKNOWN (0)
    bool terminate() override {
        // 每被呼叫 1000 次，才真正去讀取一次系統時間，以減少頻繁讀取時間帶來的效能影響
        if (++call_counter < 1000) {
            return false; 
        }
        
        call_counter = 0; // 重置計數器

        // 真正檢查時間
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - start_time;
        return elapsed.count() >= time_limit_seconds; 
    }
};

// 統計每一種 gate type 的數量，供「gate count breakdown」類 prompt 使用
std::map<GateType, int> Netlist::countGatesByType() const {
    std::map<GateType, int> counts;
    counts[GateType::AND] = 0;
    counts[GateType::OR] = 0;
    counts[GateType::NOT] = 0;
    counts[GateType::NAND] = 0;
    counts[GateType::NOR] = 0;
    counts[GateType::XOR] = 0;
    counts[GateType::XNOR] = 0;
    counts[GateType::BUF] = 0;
    counts[GateType::DFF] = 0;

    for (size_t i = 0; i < getGateCount(); ++i) {
        const Gate& gate = getGate(static_cast<int>(i));
        ++counts[gate.type];
    }
    return counts;
}

// 回傳指定 gate type 的所有 gate ID，供「list all XOR gates」等 prompt 使用
std::vector<int> Netlist::getGatesByType(GateType type) const {
    std::vector<int> result;
    for (size_t i = 0; i < getGateCount(); ++i) {
        const Gate& gate = getGate(static_cast<int>(i));
        if (gate.type == type) {
            result.push_back(gate.id);
        }
    }
    return result;
}

// 找出 input 接到常數 0/1 的 gates；可選擇限定 gate type 與常數值
std::vector<int> Netlist::findGatesWithConstInput(GateType type, int constValue) const {
    std::vector<int> result;
    const std::string wantedConst =
        (constValue == 0) ? "1'b0" :
        (constValue == 1) ? "1'b1" : "";

    for (size_t i = 0; i < getGateCount(); ++i) {
        const Gate& gate = getGate(static_cast<int>(i));
        if (type != GateType::UNKNOWN && gate.type != type) {
            continue;
        }

        bool matched = false;
        for (int netId : gate.inputNetIds) {
            const Net& net = getNet(netId);
            if (!net.isConst) {
                continue;
            }
            if (wantedConst.empty() || net.name == wantedConst) {
                matched = true;
                break;
            }
        }
        if (matched) {
            result.push_back(gate.id);
        }
    }
    return result;
}

// 回傳指定 wire / bus 被多少個 gate input pins 直接使用
// return which gate input pins this wire is connected to.
std::vector<int> Netlist::getWireLoads(const std::string& wireName) const {
    // Case A: A single-bit wire, or a specific bit is selected (e.g., "clk" or "n32[31]")
    auto it = netNameToId.find(wireName);
    if (it != netNameToId.end()) {
        return nets[it->second].loadGateIds;
    }

    // Case B: A multi-bit bus (e.g., "n32", which includes "n32[31]", "n32[30]").
    std::vector<int> allLoadGates;
    bool isBusFound = false;
    std::string busPrefix = wireName + "[";

    for (const auto& pair : netNameToId) {
        // If the wire name starts with "n32["
        if (pair.first.find(busPrefix) == 0) {
            const std::vector<int>& loads = nets[pair.second].loadGateIds;
            
            // 把這條 bit 線的 loads 全部塞進 allLoadGates 陣列的尾端
            allLoadGates.insert(allLoadGates.end(), loads.begin(), loads.end());
            isBusFound = true;
        }
    }

    if (isBusFound) {
        // 【重要】去除重複的 Gate ID
        // 因為 Bus 的多個 bit 很有可能接到同一個模組/Gate，不去除的話 ID 會重複
        std::sort(allLoadGates.begin(), allLoadGates.end());
        auto last = std::unique(allLoadGates.begin(), allLoadGates.end());
        allLoadGates.erase(last, allLoadGates.end());

        return allLoadGates;
    }

    // If the wire not found, return an empty vector to indicate an error or no loads.
    return {};
}

// 回傳指定 gate output net 直接驅動多少個 gate input pins
// return which gate input pins are connected to this gate's output
std::vector<int> Netlist::getGateFanout(const std::string& gateInstName) const {
    // 1. Find the ID of this gate.
    auto it = gateNameToId.find(gateInstName);
    if (it == gateNameToId.end()) {
        return {}; // Return an empty vector if the gate not found
    }

    const Gate& gate = gates[it->second];

    // 2. Check whether this gate has an output net. 
    // (If outputNetId is -1, the output is unconnected.)
    if (gate.outputNetId == -1) {
        return {}; // Return an empty vector if unconnected
    }

    // 3. Return the vector of gate IDs connected to this wire.
    const Net& outNet = nets[gate.outputNetId];
    return outNet.loadGateIds;
}

// Count the number of logic gates of specific types
size_t Netlist::getGateCountByType(GateType type) const {
    size_t count = getGatesByType(type).size();
    return count;
}

// 邏輯等價性檢查 (LEC) using CaDiCaL SAT solver
bool Netlist::checkEquivalence(const std::string& nameA, const std::string& nameB) const {
    std::vector<int> netsA = expandNetToBits(nameA);
    std::vector<int> netsB = expandNetToBits(nameB);

    // 找不到線，或是兩邊 bit 數量不一樣，絕對不等價
    if (netsA.empty() || netsB.empty() || netsA.size() != netsB.size()) {
        return false; 
    }

    // 向後 BFS 展開邏輯錐 (Logic Cone)
    std::unordered_set<int> visitedNets;
    std::unordered_set<int> gatesToEncode;
    std::queue<int> q;

    for (int n : netsA) { q.push(n); visitedNets.insert(n); }
    for (int n : netsB) { if (visitedNets.insert(n).second) q.push(n); }

    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        const Net& net = nets[currNetId];
        if (net.driverGateId != -1) {
            const Gate& driverGate = gates[net.driverGateId];
            
            // DFF 的輸出視為邏輯錐的起點 (Pseudo-PI)，不穿透
            if (driverGate.type != GateType::DFF) {
                // 如果這個 Gate 還沒被加入過，就加入並展開它的 Input
                if (gatesToEncode.insert(driverGate.id).second) {
                    for (int inNetId : driverGate.inputNetIds) {
                        if (visitedNets.insert(inNetId).second) {
                            q.push(inNetId);
                        }
                    }
                }
            }
        }
    }

    // 初始化 CaDiCaL 引擎
    CaDiCaL::Solver solver;
    // 關閉 factor 演算法
    solver.set("factor", 0); 
    // 提前宣告最大的變數 ID
    int maxSolverVar = nets.size() + netsA.size();
    solver.resize(maxSolverVar);

    // 處理常數線 (1'b0, 1'b1)
    for (int netId : visitedNets) {
        const Net& net = nets[netId];
        if (net.isConst) {
            int lit = netId + 1; // SAT Solver 的變數不能是 0，所以全部 +1
            if (net.name == "1'b0") solver.add(-lit);
            else if (net.name == "1'b1") solver.add(lit);
            solver.add(0); // 0 代表 Clause 結束
        }
    }

    // Tseitin Transformation (將 Gate 轉為 CNF)
    for (int gateId : gatesToEncode) {
        const Gate& gate = gates[gateId];
        int outLit = gate.outputNetId + 1;
        const auto& in = gate.inputNetIds;

        if (gate.type == GateType::AND) {
            for (int id : in) { solver.add(id + 1); solver.add(-outLit); solver.add(0); }
            for (int id : in) { solver.add(-(id + 1)); } solver.add(outLit); solver.add(0);
        } 
        else if (gate.type == GateType::OR) {
            for (int id : in) { solver.add(-(id + 1)); solver.add(outLit); solver.add(0); }
            for (int id : in) { solver.add(id + 1); } solver.add(-outLit); solver.add(0);
        } 
        else if (gate.type == GateType::NAND) {
            for (int id : in) { solver.add(id + 1); solver.add(outLit); solver.add(0); }
            for (int id : in) { solver.add(-(id + 1)); } solver.add(-outLit); solver.add(0);
        } 
        else if (gate.type == GateType::NOR) {
            for (int id : in) { solver.add(-(id + 1)); solver.add(-outLit); solver.add(0); }
            for (int id : in) { solver.add(id + 1); } solver.add(outLit); solver.add(0);
        } 
        else if (gate.type == GateType::NOT) {
            int inLit = in[0] + 1;
            solver.add(inLit); solver.add(outLit); solver.add(0);
            solver.add(-inLit); solver.add(-outLit); solver.add(0);
        } 
        else if (gate.type == GateType::BUF) {
            int inLit = in[0] + 1;
            solver.add(-inLit); solver.add(outLit); solver.add(0);
            solver.add(inLit); solver.add(-outLit); solver.add(0);
        } 
        else if (gate.type == GateType::XOR) { // 假設 Gate-Level XOR 為 2-input
            int a = in[0] + 1, b = in[1] + 1;
            solver.add(-a); solver.add(-b); solver.add(-outLit); solver.add(0);
            solver.add(a); solver.add(b); solver.add(-outLit); solver.add(0);
            solver.add(a); solver.add(-b); solver.add(outLit); solver.add(0);
            solver.add(-a); solver.add(b); solver.add(outLit); solver.add(0);
        } 
        else if (gate.type == GateType::XNOR) {
            int a = in[0] + 1, b = in[1] + 1;
            solver.add(-a); solver.add(-b); solver.add(outLit); solver.add(0);
            solver.add(a); solver.add(b); solver.add(outLit); solver.add(0);
            solver.add(a); solver.add(-b); solver.add(-outLit); solver.add(0);
            solver.add(-a); solver.add(b); solver.add(-outLit); solver.add(0);
        }
    }

    // 建立 Miter 電路 (比對 Diff)
    int maxVar = nets.size(); 
    std::vector<int> diffLits;

    for (size_t i = 0; i < netsA.size(); ++i) {
        int aLit = netsA[i] + 1;
        int bLit = netsB[i] + 1;
        
        // 幫每一對 bit 建立一個新的 Diff 變數 (dLit = aLit XOR bLit)
        int dLit = ++maxVar; 
        diffLits.push_back(dLit);

        solver.add(-aLit); solver.add(-bLit); solver.add(-dLit); solver.add(0);
        solver.add(aLit); solver.add(bLit); solver.add(-dLit); solver.add(0);
        solver.add(aLit); solver.add(-bLit); solver.add(dLit); solver.add(0);
        solver.add(-aLit); solver.add(bLit); solver.add(dLit); solver.add(0);
    }

    // 找出「是否有任何情況」會讓其中一個 Diff 為 1
    // (dLit0 OR dLit1 OR dLit2 ...)
    for (int dLit : diffLits) {
        solver.add(dLit);
    }
    solver.add(0);

    TimeLimitTerminator terminator(30.0); 
    solver.connect_terminator(&terminator); // 把計時器接上 Solver

    // 呼叫求解工具，查看是否存在讓 Diff 為 1 的輸入組合
    int res = solver.solve();

    solver.disconnect_terminator();

    const int SAT = 10;
    const int UNSAT = 20;
    const int UNKNOWN = 0;

    if (res == UNSAT) {
        return true;  // 找不到反例，代表永遠等價
    } else if (res == SAT) {
        return false; // 找到反例，代表不等價
    } else {
        // res == 0 的情況，通常是 solver 被手動中斷，或設定了時間/資源上限
        // 這裡保守起見回傳 false
        return false; 
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  最長組合邏輯路徑 (getLongestPath)
//
//  策略：DFS + 記憶化
//  - 從 startNet 出發，沿 loadGateIds → outputNetId 往終點走
//  - 每穿越一個 gate，深度 +1
//  - 遇到 DFF 不穿越
//  - memo 記錄每個 net 到終點的最長深度(first)與路徑(second)，避免重複計算
//  - inStack 偵測組合邏輯迴路（防護用）
//  兩層設計：
//  - dfsLongest（內部）：用 net ID 做遞迴計算，速度快
//  - getLongestPath（外部介面）：接受 net name，轉成 ID 後呼叫內部函式，結果再轉回 name 回傳
// ─────────────────────────────────────────────────────────────────────────────

static std::pair<int, std::vector<int>> dfsLongest(
    int currNetId,
    int endNetId,
    const std::vector<Gate>& gates,
    const std::vector<Net>& nets,
    std::unordered_map<int, std::pair<int, std::vector<int>>>& memo,
    std::unordered_set<int>& inStack,
    const std::unordered_set<int>& blockedNets
) {
    // 遇到黑名單節點，直接當作死路回傳
    if (blockedNets.count(currNetId)) {
        return {-1, {}};
    }
    
    // 找到終點
    if (currNetId == endNetId) {
        return std::make_pair(0, std::vector<int>(1, currNetId));
    }

    // 已計算過:直接回傳結果
    std::unordered_map<int, std::pair<int, std::vector<int>>>::iterator it = memo.find(currNetId);
    if (it != memo.end()) {
        return it->second;
    }

    // 偵測loop:回傳 -1
    if (inStack.count(currNetId)) {
        return std::make_pair(-1, std::vector<int>());
    }

    inStack.insert(currNetId);

    const Net& net = nets[currNetId];
    std::pair<int, std::vector<int>> best = std::make_pair(-1, std::vector<int>());

    for (int i = 0; i < (int)net.loadGateIds.size(); i++) { // 每條子路徑都試試看，只保留最長的那條
        int gateId = net.loadGateIds[i];
        const Gate& gate = gates[gateId];

        // DFF 不穿越
        if (gate.type == GateType::DFF) continue;

        int outNetId = gate.outputNetId;
        if (outNetId < 0) continue;
 
        auto result = dfsLongest(outNetId, endNetId, gates, nets, memo, inStack, blockedNets);
        int depth = result.first;
        std::vector<int>& path = result.second;
 
        if (depth >= 0 && depth + 1 > best.first) {
            best.first = depth + 1;
            best.second = path;
            best.second.insert(best.second.begin(), currNetId);
        }
    }

    // 離開這個 net 時，從 inStack 移除，並把結果存進 memo
    inStack.erase(currNetId);
    memo[currNetId] = best;
    return best;
}

std::pair<int, std::vector<std::string>> Netlist::getLongestPath(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<std::string>& blockedNetNames
) const {
    int startId = getNetId(startNet);
    int endId   = getNetId(endNet);
 
    if (startId < 0 || endId < 0) return {-1, {}};

    // 建立一個 unordered_set 來存放黑名單的 ID，方便快速查詢
    std::unordered_set<int> blockedNets;
    for (const auto& name : blockedNetNames) {
        int id = getNetId(name);
        if (id >= 0) {
            blockedNets.insert(id);
        }
    }

    if (startId == endId) {
        // 如果起點等於終點，但這個點剛好在黑名單裡，依然要回傳失敗
        if (blockedNets.count(startId)) return {-1, {}};
        return {0, {startNet}};
    }
 
    std::unordered_map<int, std::pair<int, std::vector<int>>> memo;
    std::unordered_set<int> inStack;
 
    auto result = dfsLongest(startId, endId, gates, nets, memo, inStack, blockedNets);
    int depth = result.first;
    std::vector<int>& netIdPath = result.second;
 
    if (depth < 0) return {-1, {}};
 
    std::vector<std::string> namePath;
    namePath.reserve(netIdPath.size());
    for (int i = 0; i < (int)netIdPath.size(); i++)
        namePath.push_back(nets[netIdPath[i]].name);

    return std::make_pair(depth, namePath);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Transitive Fanin Cone
//  從指定 net 往回追，找出所有影響它的 net（往 PI 方向）
//  DFF 視為邊界，不穿越
//
//  樹狀結構：children[A] = {B, C} 表示 A 是由 B、C 驅動的
// ─────────────────────────────────────────────────────────────────────────────
ConeResult Netlist::getTransitiveFaninCone(const std::string& netName) const {
    ConeResult result;
    int startId = getNetId(netName);
    if (startId < 0) return result;

    result.rootNetId = startId;

    std::queue<int> q; // BFS
    q.push(startId);
    result.netIds.insert(startId);

    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        const Net& net = nets[currNetId];

        if (net.driverGateId >= 0) { // net 有沒有 driver gate:有的話取出來
            const Gate& driver = gates[net.driverGateId];

            // DFF 不穿越
            if (driver.type == GateType::DFF) continue;

            for (int i = 0; i < (int)driver.inputNetIds.size(); i++) {
                int inNetId = driver.inputNetIds[i];
                result.children[currNetId].push_back(inNetId);
                if (result.netIds.insert(inNetId).second) { // 沒訪問過就繼續往回走
                    q.push(inNetId);
                }
            }
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Transitive Fanout Cone
//  從指定 net 往前追，找出所有被它影響的 net（往 PO 方向）
//  DFF 視為邊界，不穿越
//
//  樹狀結構：children[A] = {B, C} 表示 A 驅動了 B 和 C
// ─────────────────────────────────────────────────────────────────────────────
ConeResult Netlist::getTransitiveFanoutCone(const std::string& netName) const {
    ConeResult result;
    int startId = getNetId(netName);
    if (startId < 0) return result;

    result.rootNetId = startId;

    std::queue<int> q;
    q.push(startId);
    result.netIds.insert(startId);

    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        const Net& net = nets[currNetId];

        for (int i = 0; i < (int)net.loadGateIds.size(); i++) {
            int gateId = net.loadGateIds[i];
            const Gate& gate = gates[gateId];

            // DFF 不穿越
            if (gate.type == GateType::DFF) continue;

            int outNetId = gate.outputNetId;
            if (outNetId < 0) continue;

            result.children[currNetId].push_back(outNetId);
            if (result.netIds.insert(outNetId).second) { // 沒訪問過就繼續往前追
                q.push(outNetId);
            }
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  第三種 / 第六種：找所有路徑 (getAllPaths)
//
//  策略：DFS，不使用記憶化（每條路徑都要完整記錄）
//  - 從 startNet 出發，沿 loadGateIds → outputNetId 往終點走
//  - 遇到 DFF 不穿越
//  - avoidNets：需要避開的 net，遇到就跳過
//  - 第六種：額外驗證起點是 PI、終點是 PO
// ─────────────────────────────────────────────────────────────────────────────
 
static void dfsAllPaths(
    int currNetId,
    int endNetId,
    const std::vector<Gate>& gates,
    const std::vector<Net>& nets,
    const std::unordered_set<int>& avoidIds,
    std::vector<int>& currentPath,
    std::unordered_set<int>& visited,
    std::vector<std::vector<int>>& allPaths
) {
    // 找到終點
    if (currNetId == endNetId) {
        currentPath.push_back(currNetId);
        allPaths.push_back(currentPath);
        currentPath.pop_back();
        return;
    }
 
    // 已在當前路徑上（避免迴路）
    if (visited.count(currNetId)) return;
 
    // 需要避開的 net
    if (avoidIds.count(currNetId)) return;
 
    visited.insert(currNetId);
    currentPath.push_back(currNetId);
 
    const Net& net = nets[currNetId];
    for (int i = 0; i < (int)net.loadGateIds.size(); i++) {
        int gateId = net.loadGateIds[i];
        const Gate& gate = gates[gateId];
 
        // DFF 不穿越
        if (gate.type == GateType::DFF) continue;
 
        int outNetId = gate.outputNetId;
        if (outNetId < 0) continue;
 
        // 避開指定 net
        if (avoidIds.count(outNetId)) continue;
 
        dfsAllPaths(outNetId, endNetId, gates, nets, avoidIds, currentPath, visited, allPaths);
    }
 
    currentPath.pop_back();
    visited.erase(currNetId);
}
 
std::vector<std::vector<std::string>> Netlist::getAllPaths(
    const std::string& startNet,
    const std::string& endNet,
    const std::vector<std::string>& avoidNets,
    bool requirePItoPort
) const {
    std::vector<std::vector<std::string>> result;
 
    int startId = getNetId(startNet);
    int endId   = getNetId(endNet);
 
    if (startId < 0 || endId < 0) return result;
 
    // 第六種：驗證起點是 PI、終點是 PO
    if (requirePItoPort) {
        if (!nets[startId].isPI) {
            return result; // 起點不是 PI
        }
        if (!nets[endId].isPO) {
            return result; // 終點不是 PO
        }
    }
 
    // 建立 avoidNets 的 ID 集合
    std::unordered_set<int> avoidIds;
    for (int i = 0; i < (int)avoidNets.size(); i++) {
        int id = getNetId(avoidNets[i]);
        if (id >= 0) avoidIds.insert(id);
    }
 
    std::vector<int> currentPath;
    std::unordered_set<int> visited;
    std::vector<std::vector<int>> allPathIds;
 
    dfsAllPaths(startId, endId, gates, nets, avoidIds, currentPath, visited, allPathIds);
 
    // ID 轉回 name
    for (int i = 0; i < (int)allPathIds.size(); i++) {
        std::vector<std::string> namePath;
        for (int j = 0; j < (int)allPathIds[i].size(); j++) {
            namePath.push_back(nets[allPathIds[i][j]].name);
        }
        result.push_back(namePath);
    }
 
    return result;
}
 
