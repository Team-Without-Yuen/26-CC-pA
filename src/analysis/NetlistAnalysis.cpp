#include "include/core/Netlist.h"
#include "include/lib/cadical.hpp"
#include <algorithm>
#include <unordered_set>
#include <functional>
#include <queue>
#include <chrono>
#include <string>
#include <vector>

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
            if (netId < 0) continue;
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

std::vector<std::string> Netlist::getGateNamesWithConstInput(GateType type, int constValue) const {
    std::vector<std::string> result;
    // 重用底層 API 取得 Gate IDs
    std::vector<int> gateIds = findGatesWithConstInput(type, constValue);
    
    // 將 ID 轉換為 Instance Name
    result.reserve(gateIds.size());
    for (int id : gateIds) {
        result.push_back(gates[id].instName);
    }
    
    return result;
}

size_t Netlist::countGatesWithConstInput(GateType type, int constValue) const {
    return findGatesWithConstInput(type, constValue).size();
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

std::vector<std::string> Netlist::getWireLoadNames(const std::string& wireName) const {
    std::vector<std::string> result;
    // 呼叫你原有的底層 API 取得下游 Gate IDs
    std::vector<int> loadIds = getWireLoads(wireName);
    
    // 將 ID 轉換為 Instance Name
    result.reserve(loadIds.size());
    for (int id : loadIds) {
        result.push_back(gates[id].instName);
    }
    
    return result;
}

size_t Netlist::getWireLoadCount(const std::string& wireName) const {
    // 直接計算底層 API 回傳的陣列大小
    return getWireLoads(wireName).size();
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

std::vector<std::string> Netlist::getGateFanoutNames(const std::string& gateInstName) const {
    std::vector<std::string> result;
    // 呼叫你原有的底層 API 取得 Gate IDs
    std::vector<int> fanoutIds = getGateFanout(gateInstName);
    
    // 將 ID 轉換為 Instance Name
    result.reserve(fanoutIds.size());
    for (int id : fanoutIds) {
        result.push_back(gates[id].instName);
    }
    
    return result;
}

size_t Netlist::getGateFanoutCount(const std::string& gateInstName) const {
    // 直接計算底層 API 回傳的陣列大小
    return getGateFanout(gateInstName).size();
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
                        if (inNetId < 0) continue; 
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

// --- 針對 Net 的 Fanin ---
std::vector<std::string> Netlist::getTransitiveFaninConeGateNames(const std::string& netName) const {
    ConeResult cone = getTransitiveFaninCone(netName);
    std::unordered_set<int> uniqueGates;
    
    // Fanin 邏輯錐中的 Gate，就是錐內所有 Net 的 Driver Gate
    for (int netId : cone.netIds) {
        int driverId = nets[netId].driverGateId;
        if (driverId != -1) uniqueGates.insert(driverId);
    }
    
    std::vector<std::string> result;
    result.reserve(uniqueGates.size());
    for (int id : uniqueGates) result.push_back(gates[id].instName);
    return result;
}

size_t Netlist::getTransitiveFaninConeGateCount(const std::string& netName) const {
    return getTransitiveFaninConeGateNames(netName).size();
}

// --- 針對 Net 的 Fanout ---
std::vector<std::string> Netlist::getTransitiveFanoutConeGateNames(const std::string& netName) const {
    ConeResult cone = getTransitiveFanoutCone(netName);
    std::unordered_set<int> uniqueGates;
    
    // Fanout 邏輯錐中的 Gate，就是錐內所有 Net 的 Load Gates
    for (int netId : cone.netIds) {
        for (int loadId : nets[netId].loadGateIds) {
            uniqueGates.insert(loadId);
        }
    }
    
    std::vector<std::string> result;
    result.reserve(uniqueGates.size());
    for (int id : uniqueGates) result.push_back(gates[id].instName);
    return result;
}

size_t Netlist::getTransitiveFanoutConeGateCount(const std::string& netName) const {
    return getTransitiveFanoutConeGateNames(netName).size();
}

// --- 針對 Gate 的 Fanin ---
std::vector<std::string> Netlist::getGateTransitiveFaninConeGateNames(const std::string& gateName) const {
    ConeResult cone = getGateTransitiveFaninCone(gateName);
    std::unordered_set<int> uniqueGates;
    
    for (int netId : cone.netIds) {
        int driverId = nets[netId].driverGateId;
        if (driverId != -1) uniqueGates.insert(driverId);
    }
    
    std::vector<std::string> result;
    result.reserve(uniqueGates.size());
    for (int id : uniqueGates) result.push_back(gates[id].instName);
    return result;
}

size_t Netlist::getGateTransitiveFaninConeGateCount(const std::string& gateName) const {
    return getGateTransitiveFaninConeGateNames(gateName).size();
}

// --- 針對 Gate 的 Fanout ---
std::vector<std::string> Netlist::getGateTransitiveFanoutConeGateNames(const std::string& gateName) const {
    ConeResult cone = getGateTransitiveFanoutCone(gateName);
    std::unordered_set<int> uniqueGates;
    
    for (int netId : cone.netIds) {
        for (int loadId : nets[netId].loadGateIds) {
            uniqueGates.insert(loadId);
        }
    }
    
    std::vector<std::string> result;
    result.reserve(uniqueGates.size());
    for (int id : uniqueGates) result.push_back(gates[id].instName);
    return result;
}

size_t Netlist::getGateTransitiveFanoutConeGateCount(const std::string& gateName) const {
    return getGateTransitiveFanoutConeGateNames(gateName).size();
}

// 判斷指定的節點 (Net 或 Gate) 是否為終點。
// combinationalOnly = true: 如果下游只接 DFF，也視為終點。
// combinationalOnly = false: 必須真的沒有接任何東西才算終點。
bool Netlist::isEndpoint(const PathNode& node, bool combinationalOnly) const {
    int targetNetId = -1;

    // 找出要檢查的目標: Net ID
    if (node.type == PathNodeType::Net) {
        targetNetId = getNetId(node.name);
    } else {
        // 如果傳入的是 Gate，我們要檢查的是「這個 Gate 的輸出線」是不是終點
        int gateId = getGateId(node.name);
        if (gateId >= 0) {
            targetNetId = gates[gateId].outputNetId;
        } else {
            return false; // 找不到該 Gate
        }
    }

    // 如果找不到該 Net，或者 Gate 的輸出端本身就是懸空的 (-1)
    // 在圖論上，死路一條就是終點
    if (targetNetId < 0) {
        return true; 
    }

    const Net& net = nets[targetNetId];

    // 如果這條線沒有驅動任何下游 Gate，那它絕對是終點 (Absolute Endpoint)
    if (net.loadGateIds.empty()) {
        return true;
    }

    // 如果開啟了組合邏輯模式，檢查下游是不是「只有 DFF」
    if (combinationalOnly) {
        for (int gateId : net.loadGateIds) {
            // 只要發現下游有任何一個「非 DFF」的邏輯閘，就代表路還能繼續走
            if (gates[gateId].type != GateType::DFF) {
                return false; 
            }
        }
        // 迴圈跑完都沒 return，代表下游 100% 全部都是 DFF
        return true; 
    }

    // 如果是嚴格拓樸模式，且 loadGateIds 裡面有東西，就不是終點
    return false;
}

//  分析邏輯錐：尋找錐體內的最長路徑 (Local Critical Path)
//  回傳值：std::pair<深度, 路徑的 Net IDs>
std::pair<int, std::vector<int>> Netlist::findLongestPathInCone(const ConeResult& cone) const {
    // 記錄 <當前節點, <最大深度, 最佳路徑的下一個節點>>
    std::unordered_map<int, std::pair<int, int>> memo;

    // 內部 Lambda 遞迴函式：回傳只傳回「深度 (int)」
    std::function<int(int)> dfsLongest = [&](int currNetId) -> int {
        if (memo.count(currNetId)) {
            return memo[currNetId].first;
        }

        int maxLength = 0;
        int bestNextNode = -1; // -1 代表沒有下游 (自己是最底端)

        auto it = cone.children.find(currNetId);
        if (it != cone.children.end() && !it->second.empty()) {
            for (int childNetId : it->second) {
                
                int childLength = dfsLongest(childNetId);
                
                // 如果這條路更長，就更新最大深度與「下一個節點」
                if (childLength + 1 > maxLength) {
                    maxLength = childLength + 1;
                    bestNextNode = childNetId;
                }
            }
        }

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
    
    while (curr != -1) {
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
    std::vector<std::string> pathNames;
    pathNames.reserve(rawResult.second.size());
    for (int id : rawResult.second) pathNames.push_back(gates[id].instName);
    return {rawResult.first, pathNames};
}
// 針對 Net 的 Fanin Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFaninConeShortestPath(const std::string& netName) const {
    auto rawResult = findShortestPathInCone(getTransitiveFaninCone(netName));
    std::vector<std::string> pathNames;
    pathNames.reserve(rawResult.second.size());
    for (int id : rawResult.second) pathNames.push_back(gates[id].instName);
    return {rawResult.first, pathNames};
}

// 針對 Net 的 Fanout Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFanoutConeLongestPath(const std::string& netName) const {
    auto rawResult = findLongestPathInCone(getTransitiveFanoutCone(netName));
    std::vector<std::string> pathNames;
    pathNames.reserve(rawResult.second.size());
    for (int id : rawResult.second) pathNames.push_back(gates[id].instName);
    return {rawResult.first, pathNames};
}
// 針對 Net 的 Fanout Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFanoutConeShortestPath(const std::string& netName) const {
    auto rawResult = findShortestPathInCone(getTransitiveFanoutCone(netName));
    std::vector<std::string> pathNames;
    pathNames.reserve(rawResult.second.size());
    for (int id : rawResult.second) pathNames.push_back(gates[id].instName);
    return {rawResult.first, pathNames};
}

// 針對 Gate 的 Fanin Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFaninConeLongestPath(const std::string& gateName) const {
    auto rawResult = findLongestPathInCone(getGateTransitiveFaninCone(gateName));
    std::vector<std::string> pathNames;
    pathNames.reserve(rawResult.second.size());
    for (int id : rawResult.second) pathNames.push_back(gates[id].instName);
    return {rawResult.first, pathNames};
}
// 針對 Gate 的 Fanin Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFaninConeShortestPath(const std::string& gateName) const {
    auto rawResult = findShortestPathInCone(getGateTransitiveFaninCone(gateName));
    std::vector<std::string> pathNames;
    pathNames.reserve(rawResult.second.size());
    for (int id : rawResult.second) pathNames.push_back(gates[id].instName);
    return {rawResult.first, pathNames};
}

// 針對 Gate 的 Fanout Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFanoutConeLongestPath(const std::string& gateName) const {
    auto rawResult = findLongestPathInCone(getGateTransitiveFanoutCone(gateName));
    std::vector<std::string> pathNames;
    pathNames.reserve(rawResult.second.size());
    for (int id : rawResult.second) pathNames.push_back(gates[id].instName);
    return {rawResult.first, pathNames};
}

// ─────────────────────────────────────────────────────────────────────────────
//  B1 + B2: trimDeadLogic
//  移除所有不影響任何 PO 的 gate 和 net
//  策略：從所有 PO 往回 BFS，找出有用的 net 和 gate
//        沒被訪問到的 gate 就從 netlist 中移除
//  回傳移除的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::trimDeadLogic() {
    // Step 1: 從所有 PO 往回 BFS，找出所有有用的 net 和 gate
    std::unordered_set<int> usefulNets;
    std::unordered_set<int> usefulGates;
    std::queue<int> q;
 
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].isPO || nets[i].isPI || nets[i].isConst) {
            usefulNets.insert(i);
            if (nets[i].isPO) q.push(i);
        }
    }
 
    while (!q.empty()) {
        int netId = q.front(); q.pop();
        const Net& net = nets[netId];
        if (net.driverGateId >= 0) {
            const Gate& gate = gates[net.driverGateId];
            if (usefulGates.insert(gate.id).second) {
                for (int i = 0; i < (int)gate.inputNetIds.size(); i++) {
                    int inNetId = gate.inputNetIds[i];
                    if (usefulNets.insert(inNetId).second) {
                        q.push(inNetId);
                    }
                }
            }
        }
    }
 
    // Step 2: 找出要刪除的 gate
    std::unordered_set<int> deadGateSet;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!usefulGates.count(i)) deadGateSet.insert(i);
    }
    if (deadGateSet.empty()) return 0;
 
    // Step 3: 從 net 的 loadGateIds 移除 dead gate
    for (int i = 0; i < (int)nets.size(); i++) {
        std::vector<int> newLoads;
        for (int j = 0; j < (int)nets[i].loadGateIds.size(); j++) {
            if (!deadGateSet.count(nets[i].loadGateIds[j]))
                newLoads.push_back(nets[i].loadGateIds[j]);
        }
        nets[i].loadGateIds = newLoads;
    }
 
    // Step 4: 重建 gates vector，移除 dead gate，更新 ID
    std::vector<int> oldToNew(gates.size(), -1);
    std::vector<Gate> newGates;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!deadGateSet.count(i)) {
            int newId = newGates.size();
            oldToNew[i] = newId;
            newGates.push_back(gates[i]);
            newGates.back().id = newId;
        }
    }
 
    // Step 5: 更新 net 的 driverGateId 和 loadGateIds
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].driverGateId >= 0)
            nets[i].driverGateId = oldToNew[nets[i].driverGateId];
        for (int j = 0; j < (int)nets[i].loadGateIds.size(); j++)
            nets[i].loadGateIds[j] = oldToNew[nets[i].loadGateIds[j]];
    }
 
    // Step 6: 更新 gateNameToId
    gateNameToId.clear();
    for (int i = 0; i < (int)newGates.size(); i++)
        gateNameToId[newGates[i].instName] = i;
 
    int removed = (int)gates.size() - (int)newGates.size();
    gates = newGates;
    return removed;
}
 
// ─────────────────────────────────────────────────────────────────────────────
//  B3: collapseBackToBackInverters
//  找出所有 NOT → NOT 的連續對，把兩個 NOT 刪掉，直接把前面的 net 接到後面
//  例如：A → NOT1 → B → NOT2 → C  →  A → C
//  回傳移除的 inverter pair 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::collapseBackToBackInverters() {
    int collapsed = 0;
    bool changed = true;
 
    while (changed) {
        changed = false;
        for (int i = 0; i < (int)gates.size(); i++) {
            Gate& g1 = gates[i];
            if (g1.type != GateType::NOT) continue;
            if (g1.outputNetId < 0) continue;
 
            Net& midNet = nets[g1.outputNetId];
            if (midNet.loadGateIds.size() != 1) continue;
 
            int g2id = midNet.loadGateIds[0];
            Gate& g2 = gates[g2id];
            if (g2.type != GateType::NOT) continue;
            if (g2.outputNetId < 0) continue;
 
            int inNetId  = g1.inputNetIds[0];
            int outNetId = g2.outputNetId;
            Net& inNet  = nets[inNetId];
            Net& outNet = nets[outNetId];
 
            // 把所有接到 outNet 的 gate 改接到 inNet
            for (int j = 0; j < (int)outNet.loadGateIds.size(); j++) {
                int loadGateId = outNet.loadGateIds[j];
                Gate& loadGate = gates[loadGateId];
                for (int k = 0; k < (int)loadGate.inputNetIds.size(); k++) {
                    if (loadGate.inputNetIds[k] == outNetId)
                        loadGate.inputNetIds[k] = inNetId;
                }
                inNet.loadGateIds.push_back(loadGateId);
            }
 
            // 如果 outNet 是 PO，把 inNet 也標記為 PO
            if (outNet.isPO) {
                inNet.isPO = true;
                for (int pi = 0; pi < (int)primaryOutputs.size(); pi++) {
                    for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++) {
                        if (primaryOutputs[pi].netIds[pj] == outNetId)
                            primaryOutputs[pi].netIds[pj] = inNetId;
                    }
                }
            }
 
            // 從 inNet 的 loadGateIds 移除 g1
            std::vector<int> newLoads;
            for (int j = 0; j < (int)inNet.loadGateIds.size(); j++) {
                if (inNet.loadGateIds[j] != g1.id)
                    newLoads.push_back(inNet.loadGateIds[j]);
            }
            inNet.loadGateIds = newLoads;
 
            // 把 g1 和 g2 標記為 dead
            g1.type = GateType::UNKNOWN;
            g1.outputNetId = -1;
            g1.inputNetIds.clear();
            g2.type = GateType::UNKNOWN;
            g2.outputNetId = -1;
            g2.inputNetIds.clear();
            midNet.driverGateId = -1;
            midNet.loadGateIds.clear();
            outNet.driverGateId = -1;
            outNet.loadGateIds.clear();
 
            collapsed++;
            changed = true;
            break;
        }
    }
 
    if (collapsed > 0) trimDeadLogic();
    return collapsed;
}
 
// ─────────────────────────────────────────────────────────────────────────────
//  B4: insertBuffersForFanout
//  對 fanout > maxFanout 的 net 插入 buffer
//  讓每個 gate 的 fanout ≤ maxFanout，預設 maxFanout = 4
//  回傳插入的 buffer 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::insertBuffersForFanout(int maxFanout) {
    int inserted = 0;
    int bufCounter = 0;
 
    // 先記錄原本的 net 數量，只處理原本存在的 net
    int origNetSize = (int)nets.size();
 
    for (int netIdx = 0; netIdx < origNetSize; netIdx++) {
        if (nets[netIdx].isConst) continue;
 
        while ((int)nets[netIdx].loadGateIds.size() > maxFanout) {
            // 取出超過 maxFanout 的 load（每次都重新讀，避免 reference 失效）
            std::vector<int> overLoads(
                nets[netIdx].loadGateIds.begin() + maxFanout,
                nets[netIdx].loadGateIds.end()
            );
            nets[netIdx].loadGateIds.resize(maxFanout);
 
            // 產生新的 buffer 名稱
            std::string bufName    = "_ins_buf_"     + std::to_string(bufCounter);
            std::string bufNetName = "_ins_buf_net_" + std::to_string(bufCounter);
            bufCounter++;
 
            // 新增 buffer gate 和 output net
            // 注意：addGate/addNet 可能讓 nets/gates vector 重新分配
            // 所以之後不能用舊的 reference，全部用 index 存取
            int bufGateId = addGate(bufName, GateType::BUF);
            int bufNetId  = addNet(bufNetName);
 
            // 連接 buffer input（從 netIdx）
            // 注意：不把 bufGateId 加到 nets[netIdx].loadGateIds
            // 否則 while 條件不會減少，造成無限迴圈
            gates[bufGateId].inputNetIds.push_back(netIdx);
 
            // 連接 buffer output（到 bufNetId）
            gates[bufGateId].outputNetId = bufNetId;
            nets[bufNetId].driverGateId  = bufGateId;
 
            // 把超過的 load 改接到新的 buf output net
            for (int j = 0; j < (int)overLoads.size(); j++) {
                int loadGateId = overLoads[j];
                for (int k = 0; k < (int)gates[loadGateId].inputNetIds.size(); k++) {
                    if (gates[loadGateId].inputNetIds[k] == netIdx)
                        gates[loadGateId].inputNetIds[k] = bufNetId;
                }
                nets[bufNetId].loadGateIds.push_back(loadGateId);
            }
 
            inserted++;
        }
    }
 
    return inserted;
}

// ─────────────────────────────────────────────────────────────────────────────
//  C1: reconstructToAndNot
//  將整個 netlist 重新建構成只使用 AND 和 NOT gates
//  策略：先備份所有需要替換的 gate info，再做替換
//  De Morgan 定理：
//  - OR(a,b)   → NOT(AND(NOT(a), NOT(b)))
//  - NAND(a,b) → NOT(AND(a,b))
//  - NOR(a,b)  → AND(NOT(a), NOT(b))
//  - XOR(a,b)  → NOT(AND(NOT(AND(a,NOT(b))), NOT(AND(NOT(a),b))))
//  - XNOR(a,b) → NOT(XOR(a,b))
//  - BUF       → 直接連線（移除 gate）
//  - DFF       → 保留不動
//  回傳新增的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::reconstructToAndNot() {
    int newGateCount = 0;
    int synCounter = 0;
 
    // Step 1: 備份所有需要替換的 gate info
    struct GateInfo {
        int id;
        GateType type;
        std::vector<int> inputNetIds;
        int outputNetId;
    };
 
    std::vector<GateInfo> toReplace;
    for (int gi = 0; gi < (int)gates.size(); gi++) {
        GateType t = gates[gi].type;
        if (t == GateType::OR  || t == GateType::NAND || t == GateType::NOR  ||
            t == GateType::XOR || t == GateType::XNOR || t == GateType::BUF) {
            GateInfo info;
            info.id          = gi;
            info.type        = t;
            info.inputNetIds = gates[gi].inputNetIds;
            info.outputNetId = gates[gi].outputNetId;
            toReplace.push_back(info);
        }
    }
 
    // Step 2: 替換每個 gate
    for (int ri = 0; ri < (int)toReplace.size(); ri++) {
        GateInfo& info = toReplace[ri];
        int gi       = info.id;
        int outNetId = info.outputNetId;
        int a        = (info.inputNetIds.size() > 0) ? info.inputNetIds[0] : -1;
        int b        = (info.inputNetIds.size() > 1) ? info.inputNetIds[1] : -1;
 
        // 從 input net 的 loadGateIds 移除舊 gate
        for (int inNetId : info.inputNetIds) {
            std::vector<int> newLoads;
            for (int x : nets[inNetId].loadGateIds)
                if (x != gi) newLoads.push_back(x);
            nets[inNetId].loadGateIds = newLoads;
        }
        nets[outNetId].driverGateId = -1;
 
        // 清空舊 gate
        gates[gi].type = GateType::UNKNOWN;
        gates[gi].inputNetIds.clear();
        gates[gi].outputNetId = -1;
 
        std::string sid = std::to_string(synCounter++);
 
        if (info.type == GateType::BUF) {
            // BUF: 把所有接到 outNet 的 gate 改接到 a
            for (int loadGateId : nets[outNetId].loadGateIds) {
                for (int k = 0; k < (int)gates[loadGateId].inputNetIds.size(); k++) {
                    if (gates[loadGateId].inputNetIds[k] == outNetId) {
                        gates[loadGateId].inputNetIds[k] = a;
                        nets[a].loadGateIds.push_back(loadGateId);
                    }
                }
            }
            if (nets[outNetId].isPO) {
                nets[a].isPO = true;
                for (int pi = 0; pi < (int)primaryOutputs.size(); pi++)
                    for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++)
                        if (primaryOutputs[pi].netIds[pj] == outNetId)
                            primaryOutputs[pi].netIds[pj] = a;
            }
            nets[outNetId].loadGateIds.clear();
 
        } else if (info.type == GateType::OR) {
            // OR(a,b) = NOT(AND(NOT(a), NOT(b)))
            int notA_net = addNet("_na_" + sid);
            int notB_net = addNet("_nb_" + sid);
            int and_net  = addNet("_and_" + sid);
            int notA_g = addGate("_notA_" + sid, GateType::NOT);
            int notB_g = addGate("_notB_" + sid, GateType::NOT);
            int and_g  = addGate("_and_"  + sid, GateType::AND);
            int notO_g = addGate("_notO_" + sid, GateType::NOT);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(and_g, notA_net);  connectGateInput(and_g, notB_net);
            connectGateOutput(and_g, and_net);
            connectGateInput(notO_g, and_net);  connectGateOutput(notO_g, outNetId);
            nets[outNetId].driverGateId = notO_g;
            newGateCount += 4;
 
        } else if (info.type == GateType::NAND) {
            // NAND(a,b) = NOT(AND(a,b))
            int and_net = addNet("_and_" + sid);
            int and_g   = addGate("_and_" + sid, GateType::AND);
            int not_g   = addGate("_not_" + sid, GateType::NOT);
            connectGateInput(and_g, a);     connectGateInput(and_g, b);
            connectGateOutput(and_g, and_net);
            connectGateInput(not_g, and_net); connectGateOutput(not_g, outNetId);
            nets[outNetId].driverGateId = not_g;
            newGateCount += 2;
 
        } else if (info.type == GateType::NOR) {
            // NOR(a,b) = AND(NOT(a), NOT(b))
            int notA_net = addNet("_na_" + sid);
            int notB_net = addNet("_nb_" + sid);
            int notA_g = addGate("_notA_" + sid, GateType::NOT);
            int notB_g = addGate("_notB_" + sid, GateType::NOT);
            int and_g  = addGate("_and_"  + sid, GateType::AND);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(and_g, notA_net);  connectGateInput(and_g, notB_net);
            connectGateOutput(and_g, outNetId);
            nets[outNetId].driverGateId = and_g;
            newGateCount += 3;
 
        } else if (info.type == GateType::XOR) {
            // XOR(a,b) = NOT(AND(NOT(AND(a,NOT(b))), NOT(AND(NOT(a),b))))
            std::string s2  = std::to_string(synCounter++);
            int notB_net  = addNet("_nb_"  + sid);
            int notA_net  = addNet("_na_"  + sid);
            int and1_net  = addNet("_a1_"  + sid);
            int and2_net  = addNet("_a2_"  + sid);
            int notX_net  = addNet("_nx_"  + s2);
            int notY_net  = addNet("_ny_"  + s2);
            int and3_net  = addNet("_a3_"  + s2);
            int notB_g  = addGate("_notB_"  + sid, GateType::NOT);
            int notA_g  = addGate("_notA_"  + sid, GateType::NOT);
            int and1_g  = addGate("_and1_"  + sid, GateType::AND);
            int and2_g  = addGate("_and2_"  + sid, GateType::AND);
            int notX_g  = addGate("_notX_"  + s2,  GateType::NOT);
            int notY_g  = addGate("_notY_"  + s2,  GateType::NOT);
            int and3_g  = addGate("_and3_"  + s2,  GateType::AND);
            int notF_g  = addGate("_notF_"  + s2,  GateType::NOT);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(and1_g, a);        connectGateInput(and1_g, notB_net);
            connectGateOutput(and1_g, and1_net);
            connectGateInput(and2_g, notA_net); connectGateInput(and2_g, b);
            connectGateOutput(and2_g, and2_net);
            connectGateInput(notX_g, and1_net); connectGateOutput(notX_g, notX_net);
            connectGateInput(notY_g, and2_net); connectGateOutput(notY_g, notY_net);
            connectGateInput(and3_g, notX_net); connectGateInput(and3_g, notY_net);
            connectGateOutput(and3_g, and3_net);
            connectGateInput(notF_g, and3_net); connectGateOutput(notF_g, outNetId);
            nets[outNetId].driverGateId = notF_g;
            newGateCount += 8;
 
        } else if (info.type == GateType::XNOR) {
            // XNOR(a,b) = NOT(XOR(a,b)) → 先展開 XOR 再加 NOT
            std::string s2  = std::to_string(synCounter++);
            std::string s3  = std::to_string(synCounter++);
            int notB_net  = addNet("_nb_"  + sid);
            int notA_net  = addNet("_na_"  + sid);
            int and1_net  = addNet("_a1_"  + sid);
            int and2_net  = addNet("_a2_"  + sid);
            int notX_net  = addNet("_nx_"  + s2);
            int notY_net  = addNet("_ny_"  + s2);
            int and3_net  = addNet("_a3_"  + s2);
            int xor_net   = addNet("_xr_"  + s3);
            int notB_g  = addGate("_notB_"  + sid, GateType::NOT);
            int notA_g  = addGate("_notA_"  + sid, GateType::NOT);
            int and1_g  = addGate("_and1_"  + sid, GateType::AND);
            int and2_g  = addGate("_and2_"  + sid, GateType::AND);
            int notX_g  = addGate("_notX_"  + s2,  GateType::NOT);
            int notY_g  = addGate("_notY_"  + s2,  GateType::NOT);
            int and3_g  = addGate("_and3_"  + s2,  GateType::AND);
            int notF_g  = addGate("_notF_"  + s2,  GateType::NOT);
            int notXor_g = addGate("_nxor_" + s3,  GateType::NOT);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(and1_g, a);        connectGateInput(and1_g, notB_net);
            connectGateOutput(and1_g, and1_net);
            connectGateInput(and2_g, notA_net); connectGateInput(and2_g, b);
            connectGateOutput(and2_g, and2_net);
            connectGateInput(notX_g, and1_net); connectGateOutput(notX_g, notX_net);
            connectGateInput(notY_g, and2_net); connectGateOutput(notY_g, notY_net);
            connectGateInput(and3_g, notX_net); connectGateInput(and3_g, notY_net);
            connectGateOutput(and3_g, and3_net);
            connectGateInput(notF_g, and3_net); connectGateOutput(notF_g, xor_net);
            connectGateInput(notXor_g, xor_net); connectGateOutput(notXor_g, outNetId);
            nets[outNetId].driverGateId = notXor_g;
            newGateCount += 9;
        }
    }
 
    trimDeadLogic();
    return newGateCount;
}
 
// ─────────────────────────────────────────────────────────────────────────────
//  C2: mergeEquivalentGates
//  合併結構等價的 gate（相同 type + 相同 input net 集合）
//  回傳合併的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::mergeEquivalentGates() {
    int merged = 0;
    bool changed = true;
 
    while (changed) {
        changed = false;
 
        // key: (type, sorted input net ids) → 代表 gate 的 ID
        std::map<std::pair<int, std::vector<int>>, int> seen;
 
        for (int i = 0; i < (int)gates.size(); i++) {
            Gate& g = gates[i];
            if (g.type == GateType::UNKNOWN) continue;
            if (g.outputNetId < 0) continue;
 
            // 建立 key
            std::vector<int> sortedInputs = g.inputNetIds;
            std::sort(sortedInputs.begin(), sortedInputs.end());
            std::pair<int, std::vector<int>> key = std::make_pair((int)g.type, sortedInputs);
 
            std::map<std::pair<int, std::vector<int>>, int>::iterator it = seen.find(key);
            if (it == seen.end()) {
                seen[key] = i;
            } else {
                // 找到等價 gate，把 i 的 outNet 改接到 it->second 的 outNet
                int keepGateId = it->second;
                int deadGateId = i;
 
                int keepOutNet = gates[keepGateId].outputNetId;
                int deadOutNet = gates[deadGateId].outputNetId;
 
                if (keepOutNet < 0 || deadOutNet < 0) continue;
                if (keepOutNet == deadOutNet) continue;
 
                // 把所有接到 deadOutNet 的 gate 改接到 keepOutNet
                for (int j = 0; j < (int)nets[deadOutNet].loadGateIds.size(); j++) {
                    int loadGateId = nets[deadOutNet].loadGateIds[j];
                    Gate& loadGate = gates[loadGateId];
                    for (int k = 0; k < (int)loadGate.inputNetIds.size(); k++) {
                        if (loadGate.inputNetIds[k] == deadOutNet) {
                            loadGate.inputNetIds[k] = keepOutNet;
                            nets[keepOutNet].loadGateIds.push_back(loadGateId);
                        }
                    }
                }
 
                // 如果 deadOutNet 是 PO，把 keepOutNet 也標記為 PO
                if (nets[deadOutNet].isPO) {
                    nets[keepOutNet].isPO = true;
                    for (int pi = 0; pi < (int)primaryOutputs.size(); pi++) {
                        for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++) {
                            if (primaryOutputs[pi].netIds[pj] == deadOutNet)
                                primaryOutputs[pi].netIds[pj] = keepOutNet;
                        }
                    }
                }
 
                nets[deadOutNet].loadGateIds.clear();
                nets[deadOutNet].driverGateId = -1;
 
                // 把 dead gate 標記為 UNKNOWN
                gates[deadGateId].type = GateType::UNKNOWN;
                gates[deadGateId].outputNetId = -1;
                gates[deadGateId].inputNetIds.clear();
 
                merged++;
                changed = true;
                break;
            }
        }
    }
 
    if (merged > 0) trimDeadLogic();
    return merged;
}
 
 
