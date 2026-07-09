#include "include/core/Netlist.h"
#include "include/lib/cadical/cadical.hpp"
#include <algorithm>
#include <unordered_set>
#include <functional>
#include <queue>
#include <chrono>
#include <string>
#include <vector>

// 把邏輯閘轉換為 CNF 格式
void Netlist::encodeGateToCNF(CaDiCaL::Solver& solver, const Gate& gate) const {
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

// 將一個 combinational gate 的布林功能轉成 CNF clause，加入 SAT solver。
// 回傳 false 代表 gate 腳位不完整或 gate type 不支援，呼叫端應保守視為分析失敗。
static bool addGateCnfClauses(CaDiCaL::Solver& solver, const Gate& gate) {
    if (gate.outputNetId < 0) {
        return false;
    }

    const int outLit = gate.outputNetId + 1;
    const auto& in = gate.inputNetIds;

    if (gate.type == GateType::AND) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        for (int id : in) { solver.add(id + 1); solver.add(-outLit); solver.add(0); }
        for (int id : in) { solver.add(-(id + 1)); } solver.add(outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::OR) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        for (int id : in) { solver.add(-(id + 1)); solver.add(outLit); solver.add(0); }
        for (int id : in) { solver.add(id + 1); } solver.add(-outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::NAND) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        for (int id : in) { solver.add(id + 1); solver.add(outLit); solver.add(0); }
        for (int id : in) { solver.add(-(id + 1)); } solver.add(-outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::NOR) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        for (int id : in) { solver.add(-(id + 1)); solver.add(-outLit); solver.add(0); }
        for (int id : in) { solver.add(id + 1); } solver.add(outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::NOT) {
        if (in.size() != 1 || in[0] < 0) return false;
        const int inLit = in[0] + 1;
        solver.add(inLit); solver.add(outLit); solver.add(0);
        solver.add(-inLit); solver.add(-outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::BUF) {
        if (in.size() != 1 || in[0] < 0) return false;
        const int inLit = in[0] + 1;
        solver.add(-inLit); solver.add(outLit); solver.add(0);
        solver.add(inLit); solver.add(-outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::XOR) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        const int a = in[0] + 1;
        const int b = in[1] + 1;
        solver.add(-a); solver.add(-b); solver.add(-outLit); solver.add(0);
        solver.add(a); solver.add(b); solver.add(-outLit); solver.add(0);
        solver.add(a); solver.add(-b); solver.add(outLit); solver.add(0);
        solver.add(-a); solver.add(b); solver.add(outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::XNOR) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        const int a = in[0] + 1;
        const int b = in[1] + 1;
        solver.add(-a); solver.add(-b); solver.add(outLit); solver.add(0);
        solver.add(a); solver.add(b); solver.add(outLit); solver.add(0);
        solver.add(a); solver.add(-b); solver.add(-outLit); solver.add(0);
        solver.add(-a); solver.add(b); solver.add(-outLit); solver.add(0);
        return true;
    }

    return false;
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
    // solver.set("factor", 0); 
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
        // 呼叫獨立的 Function 來處理每一個 Gate 的 CNF 編碼
        encodeGateToCNF(solver, gates[gateId]);
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

// 語意化 wrapper：目前直接重用既有 checkEquivalence()。
// 保留這個名字是為了讓高階 Function Analysis API 讀起來更直覺。
bool Netlist::areNetsEquivalent(const std::string& netA, const std::string& netB) const {
    return checkEquivalence(netA, netB);
}

// 使用 SAT 判斷一條 scalar net 是否「可能」等於指定 value。
// 做法：
// 1. 先把 netName 解析成 net ID；第一版只接受 scalar，所以 bus 會回傳 false。
// 2. 從 target net 往 fanin 做 BFS，收集需要編碼的 combinational gates。
// 3. 遇到 DFF driver 時停止，因為 DFF.Q 在組合分析裡視為 pseudo PI。
// 4. 把 cone 內常數 net 與 primitive gate 轉成 CNF。
// 5. 額外加上一個 target=value 的限制，讓 SAT solver 判斷是否存在可行輸入。
bool Netlist::canNetBeValue(const std::string& netName, int value) const {
    if (value != 0 && value != 1) {
        return false;
    }

    const std::vector<int> targetBits = expandNetToBits(netName);
    if (targetBits.size() != 1) {
        return false;
    }

    const int targetNetId = targetBits.front();
    if (!isValidNetId(targetNetId)) {
        return false;
    }

    std::unordered_set<int> visitedNets;
    std::unordered_set<int> gatesToEncode;
    std::queue<int> q;

    visitedNets.insert(targetNetId);
    q.push(targetNetId);

    while (!q.empty()) {
        const int currNetId = q.front();
        q.pop();

        if (!isValidNetId(currNetId)) {
            continue;
        }

        const Net& net = nets[currNetId];
        if (!isValidGateId(net.driverGateId)) {
            continue;
        }

        const Gate& driverGate = gates[net.driverGateId];
        if (driverGate.type == GateType::DFF) {
            continue;
        }

        if (gatesToEncode.insert(driverGate.id).second) {
            for (int inputNetId : driverGate.inputNetIds) {
                if (!isValidNetId(inputNetId)) {
                    continue;
                }
                if (visitedNets.insert(inputNetId).second) {
                    q.push(inputNetId);
                }
            }
        }
    }

    CaDiCaL::Solver solver;
    solver.set("factor", 0);
    solver.resize(static_cast<int>(nets.size()));

    for (int netId : visitedNets) {
        const Net& net = nets[netId];
        if (!net.isConst) {
            continue;
        }

        const int lit = netId + 1;
        if (net.name == "1'b0") {
            solver.add(-lit);
            solver.add(0);
        } else if (net.name == "1'b1") {
            solver.add(lit);
            solver.add(0);
        }
    }

    for (int gateId : gatesToEncode) {
        if (!isValidGateId(gateId)) {
            return false;
        }
        if (!addGateCnfClauses(solver, gates[gateId])) {
            return false;
        }
    }

    const int targetLit = targetNetId + 1;
    solver.add(value == 1 ? targetLit : -targetLit);
    solver.add(0);

    TimeLimitTerminator terminator(30.0);
    solver.connect_terminator(&terminator);
    const int result = solver.solve();
    solver.disconnect_terminator();

    const int SAT = 10;
    return result == SAT;
}

// 使用反例檢查判斷 scalar net 是否恆等於 constValue。
// 若 constValue=0，代表「不存在讓 net=1 的 assignment」。
// 若 constValue=1，代表「不存在讓 net=0 的 assignment」。
bool Netlist::isNetConstantFunction(const std::string& netName, int constValue) const {
    if (constValue != 0 && constValue != 1) {
        return false;
    }

    const std::vector<int> targetBits = expandNetToBits(netName);
    if (targetBits.size() != 1) {
        return false;
    }

    return !canNetBeValue(netName, constValue == 0 ? 1 : 0);
}

// 判斷 scalar net 是否恆為 0。
bool Netlist::isNetAlwaysZero(const std::string& netName) const {
    return isNetConstantFunction(netName, 0);
}

// 判斷 scalar net 是否恆為 1。
bool Netlist::isNetAlwaysOne(const std::string& netName) const {
    return isNetConstantFunction(netName, 1);
}

// 執行統一 FunctionQuery；這層只負責檢查參數與 dispatch 到底層 Boolean helper。
// Report 會保留主要 yes/no 結果，也會保留 canBeZero/canBeOne/status 方便 LLM 生成回答。
Netlist::FunctionReport Netlist::runFunctionQuery(const FunctionQuery& query) const {
    FunctionReport report;
    report.netNameA = query.netNameA;
    report.netNameB = query.netNameB;
    report.constValue = query.constValue;

    switch (query.type) {
    case FunctionQueryType::Equivalence:
        if (query.netNameA.empty() || query.netNameB.empty()) {
            report.message = "Equivalence requires netNameA and netNameB";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (expandNetToBits(query.netNameA).empty()) {
            report.message = "Net not found: " + query.netNameA;
            report.status = "NET_A_NOT_FOUND";
            return report;
        }
        if (expandNetToBits(query.netNameB).empty()) {
            report.message = "Net not found: " + query.netNameB;
            report.status = "NET_B_NOT_FOUND";
            return report;
        }

        report.ok = true;
        report.equivalent = areNetsEquivalent(query.netNameA, query.netNameB);
        report.exists = report.equivalent;
        report.netIdA = getNetId(query.netNameA);
        report.netIdB = getNetId(query.netNameB);
        report.message = report.equivalent ? "Functions are equivalent"
                                           : "Functions are not equivalent";
        report.status = report.equivalent ? "EQUIVALENT" : "NOT_EQUIVALENT";
        return report;

    case FunctionQueryType::CanBeValue:
        if (query.netNameA.empty() || (query.constValue != 0 && query.constValue != 1)) {
            report.message = "CanBeValue requires netNameA and constValue 0 or 1";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (expandNetToBits(query.netNameA).size() != 1) {
            report.message = "CanBeValue requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.exists = canNetBeValue(query.netNameA, query.constValue);
        report.canBeZero = query.constValue == 0 ? report.exists : canNetBeValue(query.netNameA, 0);
        report.canBeOne = query.constValue == 1 ? report.exists : canNetBeValue(query.netNameA, 1);
        report.message = report.exists ? "Target value is satisfiable"
                                       : "Target value is not satisfiable";
        report.status = report.exists ? "SATISFIABLE_VALUE" : "UNSATISFIABLE_VALUE";
        return report;

    case FunctionQueryType::ConstantFunction:
        if (query.netNameA.empty() || (query.constValue != 0 && query.constValue != 1)) {
            report.message = "ConstantFunction requires netNameA and constValue 0 or 1";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (expandNetToBits(query.netNameA).size() != 1) {
            report.message = "ConstantFunction requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.exists = isNetConstantFunction(query.netNameA, query.constValue);
        report.isConstant = report.exists;
        report.canBeZero = canNetBeValue(query.netNameA, 0);
        report.canBeOne = canNetBeValue(query.netNameA, 1);
        report.message = report.exists ? "Net is the requested constant function"
                                       : "Net is not the requested constant function";
        report.status = report.exists ? (query.constValue == 0 ? "ALWAYS_ZERO" : "ALWAYS_ONE")
                                      : "NOT_REQUESTED_CONSTANT";
        return report;

    case FunctionQueryType::AlwaysZero:
        if (query.netNameA.empty() || expandNetToBits(query.netNameA).size() != 1) {
            report.message = "AlwaysZero requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.constValue = 0;
        report.exists = isNetAlwaysZero(query.netNameA);
        report.isConstant = report.exists;
        report.canBeZero = canNetBeValue(query.netNameA, 0);
        report.canBeOne = canNetBeValue(query.netNameA, 1);
        report.message = report.exists ? "Net is always 0" : "Net is not always 0";
        report.status = report.exists ? "ALWAYS_ZERO" : "NOT_ALWAYS_ZERO";
        return report;

    case FunctionQueryType::AlwaysOne:
        if (query.netNameA.empty() || expandNetToBits(query.netNameA).size() != 1) {
            report.message = "AlwaysOne requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.constValue = 1;
        report.exists = isNetAlwaysOne(query.netNameA);
        report.isConstant = report.exists;
        report.canBeZero = canNetBeValue(query.netNameA, 0);
        report.canBeOne = canNetBeValue(query.netNameA, 1);
        report.message = report.exists ? "Net is always 1" : "Net is not always 1";
        report.status = report.exists ? "ALWAYS_ONE" : "NOT_ALWAYS_ONE";
        return report;

    case FunctionQueryType::TruthStatus:
        if (query.netNameA.empty() || expandNetToBits(query.netNameA).size() != 1) {
            report.message = "TruthStatus requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.canBeZero = canNetBeValue(query.netNameA, 0);
        report.canBeOne = canNetBeValue(query.netNameA, 1);
        if (report.canBeZero && !report.canBeOne) {
            report.exists = true;
            report.isConstant = true;
            report.constValue = 0;
            report.status = "ALWAYS_ZERO";
            report.message = "Net is always 0";
        } else if (!report.canBeZero && report.canBeOne) {
            report.exists = true;
            report.isConstant = true;
            report.constValue = 1;
            report.status = "ALWAYS_ONE";
            report.message = "Net is always 1";
        } else if (report.canBeZero && report.canBeOne) {
            report.exists = true;
            report.isConstant = false;
            report.constValue = -1;
            report.status = "NON_CONSTANT";
            report.message = "Net can be both 0 and 1";
        } else {
            report.exists = false;
            report.isConstant = false;
            report.constValue = -1;
            report.status = "UNSAT_OR_UNSUPPORTED";
            report.message = "Net cannot be proven satisfiable for either value";
        }
        return report;
    }

    report.message = "Unsupported FunctionQueryType";
    report.status = "UNSUPPORTED_QUERY_TYPE";
    return report;
}

