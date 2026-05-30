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