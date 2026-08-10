#include "include/core/Netlist.h"
#include "include/core/BitParallelSimulation.h"
#include "include/SATEngine/SatTime.h"
#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <functional>
#include <fstream>
#include <map>
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
        if (in.empty() || std::any_of(in.begin(), in.end(), [](int id) { return id < 0; })) return false;
        for (int id : in) { solver.add(id + 1); solver.add(-outLit); solver.add(0); }
        for (int id : in) { solver.add(-(id + 1)); } solver.add(outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::OR) {
        if (in.empty() || std::any_of(in.begin(), in.end(), [](int id) { return id < 0; })) return false;
        for (int id : in) { solver.add(-(id + 1)); solver.add(outLit); solver.add(0); }
        for (int id : in) { solver.add(id + 1); } solver.add(-outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::NAND) {
        if (in.empty() || std::any_of(in.begin(), in.end(), [](int id) { return id < 0; })) return false;
        for (int id : in) { solver.add(id + 1); solver.add(outLit); solver.add(0); }
        for (int id : in) { solver.add(-(id + 1)); } solver.add(-outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::NOR) {
        if (in.empty() || std::any_of(in.begin(), in.end(), [](int id) { return id < 0; })) return false;
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

namespace {

struct DetailedSatResult {
    bool solverRan = false;
    bool sat = false;
    bool unsat = false;
    bool unknown = false;
    bool timedOut = false;
    bool unsupported = false;
    std::string solverStatus;
    std::string message;

    bool conclusive() const {
        return sat || unsat;
    }
};

struct SymmetrySatResult {
    DetailedSatResult solve;
    bool inputAInStructuralSupport = false;
    bool inputBInStructuralSupport = false;
    std::vector<std::string> mismatchedTargetBitNames;
    std::map<std::string, int> counterexampleAssignments;
    std::map<std::string, int> outputValuesBeforeSwap;
    std::map<std::string, int> outputValuesAfterSwap;
};

DetailedSatResult makeUnsupportedResult(const std::string& message) {
    DetailedSatResult result;
    result.unsupported = true;
    result.solverStatus = "UNSUPPORTED";
    result.message = message;
    return result;
}

DetailedSatResult makeSolveResult(int solverResult, const TimeLimitTerminator& terminator) {
    DetailedSatResult result;
    result.solverRan = true;

    const int SAT = 10;
    const int UNSAT = 20;
    const int UNKNOWN = 0;

    if (solverResult == SAT) {
        result.sat = true;
        result.solverStatus = "SAT";
        result.message = "SAT solver found a satisfying assignment.";
    } else if (solverResult == UNSAT) {
        result.unsat = true;
        result.solverStatus = "UNSAT";
        result.message = "SAT solver proved the constraint unsatisfiable.";
    } else if (solverResult == UNKNOWN) {
        result.unknown = true;
        result.timedOut = terminator.wasTerminated();
        result.solverStatus = result.timedOut ? "TIMEOUT" : "UNKNOWN";
        result.message = result.timedOut
            ? "SAT solver reached the time limit."
            : "SAT solver returned UNKNOWN.";
    } else {
        result.unknown = true;
        result.solverStatus = "UNKNOWN";
        result.message = "SAT solver returned an unexpected status.";
    }

    return result;
}

void mergeSolverStatus(FunctionReport& report, const DetailedSatResult& result) {
    report.solverRan = report.solverRan || result.solverRan;
    report.solverTimedOut = report.solverTimedOut || result.timedOut;
    report.solverUnknown = report.solverUnknown || result.unknown;
    report.unsupported = report.unsupported || result.unsupported;

    if (report.solverStatus.empty() || result.timedOut || result.unknown || result.unsupported) {
        report.solverStatus = result.solverStatus;
    }
}

DetailedSatResult solveCanBeValueDetailed(const Netlist& netlist,
                                           const std::string& netName,
                                           int value,
                                           double timeLimitSeconds = 30.0) {
    if (value != 0 && value != 1) {
        return makeUnsupportedResult("CanBeValue requires constValue 0 or 1.");
    }

    const std::vector<int> targetBits = netlist.expandNetToBits(netName);
    if (targetBits.size() != 1) {
        return makeUnsupportedResult("CanBeValue requires an existing scalar net.");
    }

    const int targetNetId = targetBits.front();
    if (!netlist.isValidNetId(targetNetId)) {
        return makeUnsupportedResult("Target net ID is invalid.");
    }

    std::unordered_set<int> visitedNets;
    std::unordered_set<int> gatesToEncode;
    std::queue<int> q;

    visitedNets.insert(targetNetId);
    q.push(targetNetId);

    while (!q.empty()) {
        const int currNetId = q.front();
        q.pop();

        if (!netlist.isValidNetId(currNetId)) {
            continue;
        }

        const Net& net = netlist.getNet(currNetId);
        if (!netlist.isValidGateId(net.driverGateId)) {
            continue;
        }

        const Gate& driverGate = netlist.getGate(net.driverGateId);
        if (driverGate.type == GateType::DFF) {
            continue;
        }

        if (gatesToEncode.insert(driverGate.id).second) {
            for (int inputNetId : driverGate.inputNetIds) {
                if (!netlist.isValidNetId(inputNetId)) {
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
    solver.resize(static_cast<int>(netlist.getNetCount()));

    for (int netId : visitedNets) {
        const Net& net = netlist.getNet(netId);
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
        if (!netlist.isValidGateId(gateId)) {
            return makeUnsupportedResult("Invalid gate ID in function cone.");
        }
        if (!addGateCnfClauses(solver, netlist.getGate(gateId))) {
            return makeUnsupportedResult("Unsupported or incomplete gate in function cone.");
        }
    }

    const int targetLit = targetNetId + 1;
    solver.add(value == 1 ? targetLit : -targetLit);
    solver.add(0);

    TimeLimitTerminator terminator(std::max(0.001, timeLimitSeconds));
    solver.connect_terminator(&terminator);
    const int solverResult = solver.solve();
    solver.disconnect_terminator();
    return makeSolveResult(solverResult, terminator);
}

DetailedSatResult solveEquivalenceDetailed(const Netlist& netlist,
                                           const std::string& nameA,
                                           const std::string& nameB,
                                           const std::string& conditionName = "",
                                           int conditionValue = -1,
                                           double timeLimitSeconds = 30.0) {
    const std::vector<int> netsA = netlist.expandNetToBits(nameA);
    const std::vector<int> netsB = netlist.expandNetToBits(nameB);
    const bool conditional = !conditionName.empty();
    const std::vector<int> conditionBits = conditional
        ? netlist.expandNetToBits(conditionName)
        : std::vector<int>{};

    if (netsA.empty() || netsB.empty()) {
        return makeUnsupportedResult("Equivalence requires existing nets or buses.");
    }
    if (netsA.size() != netsB.size()) {
        return makeUnsupportedResult("Equivalence requires equal bit widths.");
    }
    if (conditional && (conditionBits.size() != 1 ||
                        (conditionValue != 0 && conditionValue != 1))) {
        return makeUnsupportedResult(
            "ConditionalEquivalence requires a scalar condition net and conditionValue 0 or 1.");
    }

    std::unordered_set<int> visitedNets;
    std::unordered_set<int> gatesToEncode;
    std::queue<int> q;

    for (int netId : netsA) {
        if (visitedNets.insert(netId).second) {
            q.push(netId);
        }
    }
    for (int netId : netsB) {
        if (visitedNets.insert(netId).second) {
            q.push(netId);
        }
    }
    if (conditional && visitedNets.insert(conditionBits.front()).second) {
        q.push(conditionBits.front());
    }

    while (!q.empty()) {
        const int currNetId = q.front();
        q.pop();

        if (!netlist.isValidNetId(currNetId)) {
            continue;
        }

        const Net& net = netlist.getNet(currNetId);
        if (!netlist.isValidGateId(net.driverGateId)) {
            continue;
        }

        const Gate& driverGate = netlist.getGate(net.driverGateId);
        if (driverGate.type == GateType::DFF) {
            continue;
        }

        if (gatesToEncode.insert(driverGate.id).second) {
            for (int inputNetId : driverGate.inputNetIds) {
                if (!netlist.isValidNetId(inputNetId)) {
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
    int maxVar = static_cast<int>(netlist.getNetCount());
    solver.resize(maxVar + static_cast<int>(netsA.size()));

    for (int netId : visitedNets) {
        const Net& net = netlist.getNet(netId);
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
        if (!netlist.isValidGateId(gateId)) {
            return makeUnsupportedResult("Invalid gate ID in equivalence cone.");
        }
        if (!addGateCnfClauses(solver, netlist.getGate(gateId))) {
            return makeUnsupportedResult("Unsupported or incomplete gate in equivalence cone.");
        }
    }

    if (conditional) {
        const int conditionLit = conditionBits.front() + 1;
        solver.add(conditionValue == 1 ? conditionLit : -conditionLit);
        solver.add(0);
    }

    std::vector<int> diffLits;
    diffLits.reserve(netsA.size());
    for (size_t i = 0; i < netsA.size(); ++i) {
        const int aLit = netsA[i] + 1;
        const int bLit = netsB[i] + 1;
        const int dLit = ++maxVar;
        diffLits.push_back(dLit);

        solver.add(-aLit); solver.add(-bLit); solver.add(-dLit); solver.add(0);
        solver.add(aLit); solver.add(bLit); solver.add(-dLit); solver.add(0);
        solver.add(aLit); solver.add(-bLit); solver.add(dLit); solver.add(0);
        solver.add(-aLit); solver.add(bLit); solver.add(dLit); solver.add(0);
    }

    for (int dLit : diffLits) {
        solver.add(dLit);
    }
    solver.add(0);

    if (timeLimitSeconds <= 0.0) {
        DetailedSatResult result;
        result.unknown = true;
        result.timedOut = true;
        result.solverStatus = "TIMEOUT";
        result.message = "Equivalence search reached its time limit before SAT verification.";
        return result;
    }

    TimeLimitTerminator terminator(timeLimitSeconds);
    solver.connect_terminator(&terminator);
    const int solverResult = solver.solve();
    solver.disconnect_terminator();
    return makeSolveResult(solverResult, terminator);
}

DetailedSatResult solveFunctionalDependenceDetailed(
    const Netlist& netlist,
    const std::string& targetName,
    const std::string& inputName,
    bool& inputInStructuralSupport,
    double timeLimitSeconds) {
    inputInStructuralSupport = false;

    const std::vector<int> targetBits = netlist.expandNetToBits(targetName);
    const std::vector<int> inputBits = netlist.expandNetToBits(inputName);
    if (targetBits.size() != 1 || inputBits.size() != 1) {
        return makeUnsupportedResult(
            "FunctionalDependence requires an existing scalar target and scalar input.");
    }

    const int targetNetId = targetBits.front();
    const int inputNetId = inputBits.front();
    if (!netlist.isValidNetId(targetNetId) || !netlist.isValidNetId(inputNetId) ||
        netlist.getNet(targetNetId).isRemoved || netlist.getNet(inputNetId).isRemoved) {
        return makeUnsupportedResult("FunctionalDependence requires active target and input nets.");
    }

    const Net& inputNet = netlist.getNet(inputNetId);
    const bool isPrimaryInput = inputNet.isPI;
    const bool isPseudoPrimaryInput =
        netlist.isValidGateId(inputNet.driverGateId) &&
        netlist.getGate(inputNet.driverGateId).type == GateType::DFF;
    if (!isPrimaryInput && !isPseudoPrimaryInput) {
        return makeUnsupportedResult(
            "FunctionalDependence input must be a primary input or DFF.Q pseudo-primary input.");
    }

    std::unordered_set<int> visitedNets;
    std::unordered_set<int> gatesToEncode;
    std::queue<int> q;
    visitedNets.insert(targetNetId);
    q.push(targetNetId);

    while (!q.empty()) {
        const int currNetId = q.front();
        q.pop();
        if (!netlist.isValidNetId(currNetId) || netlist.getNet(currNetId).isRemoved) {
            return makeUnsupportedResult("Removed or invalid net found in function cone.");
        }

        if (currNetId == inputNetId) {
            inputInStructuralSupport = true;
        }

        const Net& net = netlist.getNet(currNetId);
        if (!netlist.isValidGateId(net.driverGateId)) {
            continue;
        }

        const Gate& driverGate = netlist.getGate(net.driverGateId);
        if (driverGate.type == GateType::UNKNOWN) {
            continue;
        }
        if (driverGate.type == GateType::DFF) {
            continue;
        }

        if (gatesToEncode.insert(driverGate.id).second) {
            for (int faninNetId : driverGate.inputNetIds) {
                if (!netlist.isValidNetId(faninNetId) || netlist.getNet(faninNetId).isRemoved) {
                    return makeUnsupportedResult("Removed or invalid gate input found in function cone.");
                }
                if (visitedNets.insert(faninNetId).second) {
                    q.push(faninNetId);
                }
            }
        }
    }

    if (!inputInStructuralSupport) {
        DetailedSatResult result;
        result.unsat = true;
        result.solverStatus = "NOT_NEEDED";
        result.message = "Input is outside the target's structural support.";
        return result;
    }

    const int netSlotCount = static_cast<int>(netlist.getNetCount());
    const int diffLit = 2 * netSlotCount + 1;
    CaDiCaL::Solver solver;
    solver.set("factor", 0);
    solver.resize(diffLit);

    auto copyLiteral = [netSlotCount](int netId, int copyIndex) {
        return copyIndex * netSlotCount + netId + 1;
    };

    for (int netId : visitedNets) {
        const Net& net = netlist.getNet(netId);
        const int lit0 = copyLiteral(netId, 0);
        const int lit1 = copyLiteral(netId, 1);

        if (net.isConst) {
            if (net.constVal != 0 && net.constVal != 1) {
                return makeUnsupportedResult("Constant net has an unknown value in function cone.");
            }
            solver.add(net.constVal == 1 ? lit0 : -lit0); solver.add(0);
            solver.add(net.constVal == 1 ? lit1 : -lit1); solver.add(0);
            continue;
        }

        bool isLeaf = !netlist.isValidGateId(net.driverGateId);
        if (!isLeaf) {
            const GateType driverType = netlist.getGate(net.driverGateId).type;
            isLeaf = driverType == GateType::DFF || driverType == GateType::UNKNOWN;
        }
        if (isLeaf && netId != inputNetId) {
            solver.add(-lit0); solver.add(lit1); solver.add(0);
            solver.add(lit0); solver.add(-lit1); solver.add(0);
        }
    }

    solver.add(-copyLiteral(inputNetId, 0)); solver.add(0);
    solver.add(copyLiteral(inputNetId, 1)); solver.add(0);

    for (int gateId : gatesToEncode) {
        if (!netlist.isValidGateId(gateId)) {
            return makeUnsupportedResult("Invalid gate ID in dependence cone.");
        }
        const Gate& originalGate = netlist.getGate(gateId);
        for (int copyIndex = 0; copyIndex < 2; ++copyIndex) {
            Gate copiedGate = originalGate;
            const int offset = copyIndex * netSlotCount;
            copiedGate.outputNetId += offset;
            for (int& inputId : copiedGate.inputNetIds) {
                inputId += offset;
            }
            if (!addGateCnfClauses(solver, copiedGate)) {
                return makeUnsupportedResult(
                    "Unsupported or incomplete gate in functional-dependence cone.");
            }
        }
    }

    const int output0 = copyLiteral(targetNetId, 0);
    const int output1 = copyLiteral(targetNetId, 1);
    solver.add(-output0); solver.add(-output1); solver.add(-diffLit); solver.add(0);
    solver.add(output0); solver.add(output1); solver.add(-diffLit); solver.add(0);
    solver.add(output0); solver.add(-output1); solver.add(diffLit); solver.add(0);
    solver.add(-output0); solver.add(output1); solver.add(diffLit); solver.add(0);
    solver.add(diffLit); solver.add(0);

    TimeLimitTerminator terminator(timeLimitSeconds);
    solver.connect_terminator(&terminator);
    const int solverResult = solver.solve();
    solver.disconnect_terminator();
    return makeSolveResult(solverResult, terminator);
}

SymmetrySatResult solveSymmetryDetailed(
    const Netlist& netlist,
    const std::string& targetName,
    const std::string& inputNameA,
    const std::string& inputNameB,
    double timeLimitSeconds) {
    SymmetrySatResult result;

    const std::vector<int> targetBits = netlist.expandNetToBits(targetName);
    const std::vector<int> inputBitsA = netlist.expandNetToBits(inputNameA);
    const std::vector<int> inputBitsB = netlist.expandNetToBits(inputNameB);
    if (targetBits.empty()) {
        result.solve = makeUnsupportedResult(
            "Symmetry requires an existing scalar or bus target.");
        return result;
    }
    if (inputBitsA.size() != 1 || inputBitsB.size() != 1) {
        result.solve = makeUnsupportedResult(
            "Symmetry requires two existing scalar input nets.");
        return result;
    }

    const int inputNetIdA = inputBitsA.front();
    const int inputNetIdB = inputBitsB.front();
    if (inputNetIdA == inputNetIdB) {
        result.solve = makeUnsupportedResult(
            "Symmetry requires two distinct input nets.");
        return result;
    }

    auto isActiveNet = [&netlist](int netId) {
        return netlist.isValidNetId(netId) && !netlist.getNet(netId).isRemoved;
    };
    for (int targetNetId : targetBits) {
        if (!isActiveNet(targetNetId)) {
            result.solve = makeUnsupportedResult(
                "Symmetry target contains a removed or invalid net.");
            return result;
        }
    }
    if (!isActiveNet(inputNetIdA) || !isActiveNet(inputNetIdB)) {
        result.solve = makeUnsupportedResult(
            "Symmetry requires active input nets.");
        return result;
    }

    auto isIndependentInput = [&netlist](int netId) {
        const Net& net = netlist.getNet(netId);
        if (net.isPI) {
            return true;
        }
        return netlist.isValidGateId(net.driverGateId) &&
               netlist.getGate(net.driverGateId).type == GateType::DFF;
    };
    if (!isIndependentInput(inputNetIdA) || !isIndependentInput(inputNetIdB)) {
        result.solve = makeUnsupportedResult(
            "Symmetry inputs must be primary inputs or DFF.Q pseudo-primary inputs.");
        return result;
    }

    std::unordered_set<int> visitedNets;
    std::unordered_set<int> gatesToEncode;
    std::queue<int> pendingNets;
    for (int targetNetId : targetBits) {
        if (visitedNets.insert(targetNetId).second) {
            pendingNets.push(targetNetId);
        }
    }

    while (!pendingNets.empty()) {
        const int currNetId = pendingNets.front();
        pendingNets.pop();
        if (!isActiveNet(currNetId)) {
            result.solve = makeUnsupportedResult(
                "Removed or invalid net found in symmetry cone.");
            return result;
        }

        result.inputAInStructuralSupport =
            result.inputAInStructuralSupport || currNetId == inputNetIdA;
        result.inputBInStructuralSupport =
            result.inputBInStructuralSupport || currNetId == inputNetIdB;

        const Net& net = netlist.getNet(currNetId);
        if (!netlist.isValidGateId(net.driverGateId)) {
            continue;
        }

        const Gate& driverGate = netlist.getGate(net.driverGateId);
        if (driverGate.type == GateType::DFF || driverGate.type == GateType::UNKNOWN) {
            continue;
        }

        if (gatesToEncode.insert(driverGate.id).second) {
            for (int faninNetId : driverGate.inputNetIds) {
                if (!isActiveNet(faninNetId)) {
                    result.solve = makeUnsupportedResult(
                        "Removed or invalid gate input found in symmetry cone.");
                    return result;
                }
                if (visitedNets.insert(faninNetId).second) {
                    pendingNets.push(faninNetId);
                }
            }
        }
    }

    if (!result.inputAInStructuralSupport && !result.inputBInStructuralSupport) {
        result.solve.unsat = true;
        result.solve.solverStatus = "NOT_NEEDED";
        result.solve.message =
            "Both inputs are outside the target's structural support.";
        return result;
    }

    const int netSlotCount = static_cast<int>(netlist.getNetCount());
    const int firstDiffLit = 2 * netSlotCount + 1;
    const int lastDiffLit = firstDiffLit + static_cast<int>(targetBits.size()) - 1;
    CaDiCaL::Solver solver;
    solver.set("factor", 0);
    solver.resize(lastDiffLit);

    auto copyLiteral = [netSlotCount](int netId, int copyIndex) {
        return copyIndex * netSlotCount + netId + 1;
    };

    for (int netId : visitedNets) {
        const Net& net = netlist.getNet(netId);
        const int lit0 = copyLiteral(netId, 0);
        const int lit1 = copyLiteral(netId, 1);

        if (net.isConst) {
            if (net.constVal != 0 && net.constVal != 1) {
                result.solve = makeUnsupportedResult(
                    "Constant net has an unknown value in symmetry cone.");
                return result;
            }
            solver.add(net.constVal == 1 ? lit0 : -lit0); solver.add(0);
            solver.add(net.constVal == 1 ? lit1 : -lit1); solver.add(0);
            continue;
        }

        bool isLeaf = !netlist.isValidGateId(net.driverGateId);
        if (!isLeaf) {
            const GateType driverType = netlist.getGate(net.driverGateId).type;
            isLeaf = driverType == GateType::DFF || driverType == GateType::UNKNOWN;
        }
        if (isLeaf && netId != inputNetIdA && netId != inputNetIdB) {
            solver.add(-lit0); solver.add(lit1); solver.add(0);
            solver.add(lit0); solver.add(-lit1); solver.add(0);
        }
    }

    // Cofactor comparison: copy 0 uses (A, B)=(0, 1), copy 1 uses (1, 0).
    solver.add(-copyLiteral(inputNetIdA, 0)); solver.add(0);
    solver.add(copyLiteral(inputNetIdB, 0)); solver.add(0);
    solver.add(copyLiteral(inputNetIdA, 1)); solver.add(0);
    solver.add(-copyLiteral(inputNetIdB, 1)); solver.add(0);

    for (int gateId : gatesToEncode) {
        if (!netlist.isValidGateId(gateId)) {
            result.solve = makeUnsupportedResult(
                "Invalid gate ID in symmetry cone.");
            return result;
        }
        const Gate& originalGate = netlist.getGate(gateId);
        for (int copyIndex = 0; copyIndex < 2; ++copyIndex) {
            Gate copiedGate = originalGate;
            const int offset = copyIndex * netSlotCount;
            copiedGate.outputNetId += offset;
            for (int& inputId : copiedGate.inputNetIds) {
                inputId += offset;
            }
            if (!addGateCnfClauses(solver, copiedGate)) {
                result.solve = makeUnsupportedResult(
                    "Unsupported or incomplete gate in symmetry cone.");
                return result;
            }
        }
    }

    for (size_t index = 0; index < targetBits.size(); ++index) {
        const int output0 = copyLiteral(targetBits[index], 0);
        const int output1 = copyLiteral(targetBits[index], 1);
        const int diffLit = firstDiffLit + static_cast<int>(index);
        solver.add(-output0); solver.add(-output1); solver.add(-diffLit); solver.add(0);
        solver.add(output0); solver.add(output1); solver.add(-diffLit); solver.add(0);
        solver.add(output0); solver.add(-output1); solver.add(diffLit); solver.add(0);
        solver.add(-output0); solver.add(output1); solver.add(diffLit); solver.add(0);
        solver.add(diffLit);
    }
    solver.add(0);

    TimeLimitTerminator terminator(timeLimitSeconds);
    solver.connect_terminator(&terminator);
    const int solverResult = solver.solve();
    solver.disconnect_terminator();
    result.solve = makeSolveResult(solverResult, terminator);

    if (!result.solve.sat) {
        return result;
    }

    result.counterexampleAssignments[netlist.getNet(inputNetIdA).name] = 0;
    result.counterexampleAssignments[netlist.getNet(inputNetIdB).name] = 1;
    for (int netId : visitedNets) {
        const Net& net = netlist.getNet(netId);
        if (net.isConst) {
            continue;
        }
        bool isLeaf = !netlist.isValidGateId(net.driverGateId);
        if (!isLeaf) {
            const GateType driverType = netlist.getGate(net.driverGateId).type;
            isLeaf = driverType == GateType::DFF || driverType == GateType::UNKNOWN;
        }
        if (isLeaf && netId != inputNetIdA && netId != inputNetIdB) {
            result.counterexampleAssignments[net.name] =
                solver.val(copyLiteral(netId, 0)) > 0 ? 1 : 0;
        }
    }

    for (int targetNetId : targetBits) {
        const std::string& bitName = netlist.getNet(targetNetId).name;
        const int before = solver.val(copyLiteral(targetNetId, 0)) > 0 ? 1 : 0;
        const int after = solver.val(copyLiteral(targetNetId, 1)) > 0 ? 1 : 0;
        result.outputValuesBeforeSwap[bitName] = before;
        result.outputValuesAfterSwap[bitName] = after;
        if (before != after) {
            result.mismatchedTargetBitNames.push_back(bitName);
        }
    }
    return result;
}

} // namespace

// 邏輯等價性檢查 (LEC) using CaDiCaL SAT solver
bool Netlist::checkEquivalence(const std::string& nameA, const std::string& nameB) const {
    const DetailedSatResult result = solveEquivalenceDetailed(*this, nameA, nameB);
    return result.unsat;
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
    const DetailedSatResult result = solveCanBeValueDetailed(*this, netName, value);
    return result.sat;
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
    report.conditionNetName = query.conditionNetName;
    report.symmetryInputNameA = query.symmetryInputNameA;
    report.symmetryInputNameB = query.symmetryInputNameB;
    report.constValue = query.constValue;
    report.conditionValue = query.conditionValue;
    report.maxExpressionDepth = query.maxExpressionDepth;

    const bool usesConfigurableSatLimit =
        query.type == FunctionQueryType::Equivalence ||
        query.type == FunctionQueryType::ConditionalEquivalence ||
        query.type == FunctionQueryType::CanBeValue ||
        query.type == FunctionQueryType::ConstantFunction ||
        query.type == FunctionQueryType::AlwaysZero ||
        query.type == FunctionQueryType::AlwaysOne ||
        query.type == FunctionQueryType::TruthStatus ||
        query.type == FunctionQueryType::FunctionalDependence ||
        query.type == FunctionQueryType::Symmetry;
    if (usesConfigurableSatLimit && query.timeLimitSeconds <= 0.0) {
        report.status = "INVALID_ARGUMENT";
        report.message = "SAT timeLimitSeconds must be positive.";
        return report;
    }

    auto markSolverFailure = [&report](const std::string& context) {
        report.ok = false;
        report.exists = false;
        if (report.unsupported) {
            report.status = "UNSUPPORTED";
            report.message = context + " is unsupported: " + report.solverStatus;
        } else if (report.solverTimedOut) {
            report.status = "SOLVER_TIMEOUT";
            report.message = context + " could not be completed before the SAT time limit.";
        } else {
            report.status = "SOLVER_UNKNOWN";
            report.message = context + " could not be completed because the SAT solver returned UNKNOWN.";
        }
    };

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

        {
        const DetailedSatResult eq = solveEquivalenceDetailed(
            *this,
            query.netNameA,
            query.netNameB,
            "",
            -1,
            query.timeLimitSeconds);
        mergeSolverStatus(report, eq);
        if (!eq.conclusive()) {
            markSolverFailure("Equivalence query");
            return report;
        }

        report.ok = true;
        report.equivalent = eq.unsat;
        report.exists = report.equivalent;
        report.netIdA = getNetId(query.netNameA);
        report.netIdB = getNetId(query.netNameB);
        report.message = report.equivalent ? "Functions are equivalent"
                                           : "Functions are not equivalent";
        report.status = report.equivalent ? "EQUIVALENT" : "NOT_EQUIVALENT";
        return report;
        }

    case FunctionQueryType::ConditionalEquivalence:
        if (query.netNameA.empty() || query.netNameB.empty() ||
            query.conditionNetName.empty() ||
            (query.conditionValue != 0 && query.conditionValue != 1)) {
            report.message = "ConditionalEquivalence requires netNameA, netNameB, "
                             "conditionNetName, and conditionValue 0 or 1";
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
        if (expandNetToBits(query.conditionNetName).size() != 1) {
            report.message = "ConditionalEquivalence requires an existing scalar condition net";
            report.status = "SCALAR_CONDITION_REQUIRED";
            return report;
        }

        {
        const DetailedSatResult eq = solveEquivalenceDetailed(
            *this,
            query.netNameA,
            query.netNameB,
            query.conditionNetName,
            query.conditionValue,
            query.timeLimitSeconds);
        mergeSolverStatus(report, eq);
        if (!eq.conclusive()) {
            markSolverFailure("ConditionalEquivalence query");
            if (eq.unsupported && !eq.message.empty()) {
                report.message = eq.message;
            }
            return report;
        }

        report.ok = true;
        report.equivalent = eq.unsat;
        report.exists = report.equivalent;
        report.netIdA = getNetId(query.netNameA);
        report.netIdB = getNetId(query.netNameB);
        report.conditionNetId = getNetId(query.conditionNetName);
        report.status = report.equivalent
            ? "CONDITIONALLY_EQUIVALENT"
            : "NOT_CONDITIONALLY_EQUIVALENT";
        report.message = report.equivalent
            ? "Functions are equivalent under the requested condition"
            : "Functions are not equivalent under the requested condition";
        return report;
        }

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

        {
        const double perValueLimit = std::max(0.001, query.timeLimitSeconds / 2.0);
        const DetailedSatResult zero =
            solveCanBeValueDetailed(*this, query.netNameA, 0, perValueLimit);
        const DetailedSatResult one =
            solveCanBeValueDetailed(*this, query.netNameA, 1, perValueLimit);
        mergeSolverStatus(report, zero);
        mergeSolverStatus(report, one);
        if (!zero.conclusive() || !one.conclusive()) {
            markSolverFailure("CanBeValue query");
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.canBeZero = zero.sat;
        report.canBeOne = one.sat;
        report.exists = query.constValue == 0 ? report.canBeZero : report.canBeOne;
        report.solverStatus = query.constValue == 0 ? zero.solverStatus : one.solverStatus;
        report.message = report.exists ? "Target value is satisfiable"
                                       : "Target value is not satisfiable";
        report.status = report.exists ? "SATISFIABLE_VALUE" : "UNSATISFIABLE_VALUE";
        return report;
        }

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

        {
        const double perValueLimit = std::max(0.001, query.timeLimitSeconds / 2.0);
        const DetailedSatResult zero =
            solveCanBeValueDetailed(*this, query.netNameA, 0, perValueLimit);
        const DetailedSatResult one =
            solveCanBeValueDetailed(*this, query.netNameA, 1, perValueLimit);
        mergeSolverStatus(report, zero);
        mergeSolverStatus(report, one);
        if (!zero.conclusive() || !one.conclusive()) {
            markSolverFailure("ConstantFunction query");
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.canBeZero = zero.sat;
        report.canBeOne = one.sat;
        report.exists = query.constValue == 0
            ? (report.canBeZero && !report.canBeOne)
            : (!report.canBeZero && report.canBeOne);
        report.isConstant = report.exists;
        report.message = report.exists ? "Net is the requested constant function"
                                       : "Net is not the requested constant function";
        report.status = report.exists ? (query.constValue == 0 ? "ALWAYS_ZERO" : "ALWAYS_ONE")
                                      : "NOT_REQUESTED_CONSTANT";
        return report;
        }

    case FunctionQueryType::AlwaysZero:
        if (query.netNameA.empty() || expandNetToBits(query.netNameA).size() != 1) {
            report.message = "AlwaysZero requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        {
        const double perValueLimit = std::max(0.001, query.timeLimitSeconds / 2.0);
        const DetailedSatResult zero =
            solveCanBeValueDetailed(*this, query.netNameA, 0, perValueLimit);
        const DetailedSatResult one =
            solveCanBeValueDetailed(*this, query.netNameA, 1, perValueLimit);
        mergeSolverStatus(report, zero);
        mergeSolverStatus(report, one);
        if (!zero.conclusive() || !one.conclusive()) {
            markSolverFailure("AlwaysZero query");
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.constValue = 0;
        report.canBeZero = zero.sat;
        report.canBeOne = one.sat;
        report.exists = report.canBeZero && !report.canBeOne;
        report.isConstant = report.exists;
        report.message = report.exists ? "Net is always 0" : "Net is not always 0";
        report.status = report.exists ? "ALWAYS_ZERO" : "NOT_ALWAYS_ZERO";
        return report;
        }

    case FunctionQueryType::AlwaysOne:
        if (query.netNameA.empty() || expandNetToBits(query.netNameA).size() != 1) {
            report.message = "AlwaysOne requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        {
        const double perValueLimit = std::max(0.001, query.timeLimitSeconds / 2.0);
        const DetailedSatResult zero =
            solveCanBeValueDetailed(*this, query.netNameA, 0, perValueLimit);
        const DetailedSatResult one =
            solveCanBeValueDetailed(*this, query.netNameA, 1, perValueLimit);
        mergeSolverStatus(report, zero);
        mergeSolverStatus(report, one);
        if (!zero.conclusive() || !one.conclusive()) {
            markSolverFailure("AlwaysOne query");
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.constValue = 1;
        report.canBeZero = zero.sat;
        report.canBeOne = one.sat;
        report.exists = !report.canBeZero && report.canBeOne;
        report.isConstant = report.exists;
        report.message = report.exists ? "Net is always 1" : "Net is not always 1";
        report.status = report.exists ? "ALWAYS_ONE" : "NOT_ALWAYS_ONE";
        return report;
        }

    case FunctionQueryType::TruthStatus:
        if (query.netNameA.empty() || expandNetToBits(query.netNameA).size() != 1) {
            report.message = "TruthStatus requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        {
        const double perValueLimit = std::max(0.001, query.timeLimitSeconds / 2.0);
        const DetailedSatResult zero =
            solveCanBeValueDetailed(*this, query.netNameA, 0, perValueLimit);
        const DetailedSatResult one =
            solveCanBeValueDetailed(*this, query.netNameA, 1, perValueLimit);
        mergeSolverStatus(report, zero);
        mergeSolverStatus(report, one);
        if (!zero.conclusive() || !one.conclusive()) {
            markSolverFailure("TruthStatus query");
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.canBeZero = zero.sat;
        report.canBeOne = one.sat;
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

    case FunctionQueryType::FunctionalDependence:
        if (query.netNameA.empty() || query.netNameB.empty()) {
            report.message = "FunctionalDependence requires target netNameA and input netNameB";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (expandNetToBits(query.netNameA).size() != 1 ||
            expandNetToBits(query.netNameB).size() != 1) {
            report.message = "FunctionalDependence requires existing scalar target and input nets";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        {
        const DetailedSatResult dependence = solveFunctionalDependenceDetailed(
            *this,
            query.netNameA,
            query.netNameB,
            report.inputInStructuralSupport,
            query.timeLimitSeconds);
        mergeSolverStatus(report, dependence);
        if (!dependence.conclusive()) {
            markSolverFailure("FunctionalDependence query");
            if (dependence.unsupported && !dependence.message.empty()) {
                report.message = dependence.message;
            }
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.netIdB = getNetId(query.netNameB);
        report.dependsOnInput = dependence.sat;
        report.exists = report.dependsOnInput;
        report.status = report.dependsOnInput
            ? "FUNCTIONALLY_DEPENDENT"
            : "FUNCTIONALLY_INDEPENDENT";
        report.message = report.dependsOnInput
            ? "Target function depends on the selected input"
            : "Target function does not depend on the selected input";
        return report;
        }

    case FunctionQueryType::Symmetry:
        if (query.netNameA.empty() || query.symmetryInputNameA.empty() ||
            query.symmetryInputNameB.empty()) {
            report.message =
                "Symmetry requires target netNameA and two symmetry input names";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (expandNetToBits(query.netNameA).empty()) {
            report.message = "Symmetry target not found: " + query.netNameA;
            report.status = "TARGET_NOT_FOUND";
            return report;
        }
        if (expandNetToBits(query.symmetryInputNameA).size() != 1 ||
            expandNetToBits(query.symmetryInputNameB).size() != 1) {
            report.message = "Symmetry requires two existing scalar input nets";
            report.status = "SCALAR_INPUT_REQUIRED";
            return report;
        }

        {
        const SymmetrySatResult symmetry = solveSymmetryDetailed(
            *this,
            query.netNameA,
            query.symmetryInputNameA,
            query.symmetryInputNameB,
            query.timeLimitSeconds);
        mergeSolverStatus(report, symmetry.solve);
        report.symmetryInputAInStructuralSupport =
            symmetry.inputAInStructuralSupport;
        report.symmetryInputBInStructuralSupport =
            symmetry.inputBInStructuralSupport;
        if (!symmetry.solve.conclusive()) {
            markSolverFailure("Symmetry query");
            if (symmetry.solve.unsupported && !symmetry.solve.message.empty()) {
                report.message = symmetry.solve.message;
            }
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.symmetryInputNetIdA = getNetId(query.symmetryInputNameA);
        report.symmetryInputNetIdB = getNetId(query.symmetryInputNameB);
        report.symmetric = symmetry.solve.unsat;
        report.exists = report.symmetric;
        report.counterexampleFound = symmetry.solve.sat;
        report.mismatchedTargetBitNames = symmetry.mismatchedTargetBitNames;
        report.counterexampleAssignments = symmetry.counterexampleAssignments;
        report.outputValuesBeforeSwap = symmetry.outputValuesBeforeSwap;
        report.outputValuesAfterSwap = symmetry.outputValuesAfterSwap;
        report.status = report.symmetric ? "SYMMETRIC" : "NOT_SYMMETRIC";
        report.message = report.symmetric
            ? "Target function is symmetric with respect to the selected inputs"
            : "Target function is not symmetric with respect to the selected inputs";
        return report;
        }

    case FunctionQueryType::BooleanExpression:
        if (query.netNameA.empty()) {
            report.message = "BooleanExpression requires netNameA";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (getNetId(query.netNameA) < 0) {
            report.message = "Net not found: " + query.netNameA;
            report.status = "NET_NOT_FOUND";
            return report;
        }

        {
        bool expressionTruncated = false;
        report.ok = true;
        report.exists = true;
        report.netIdA = getNetId(query.netNameA);
        report.expression = getBooleanExpression(query.netNameA, &expressionTruncated);
        report.expressionLength = report.expression.size();
        report.maxExpressionDepth = -1;
        // getBooleanExpression() 內部仍有一個 depth/size 安全上限（見
        // Booleanexpression.cpp），避免 reconvergent fanout 電路讓字串長度指數
        // 爆炸。expressionTruncated 反映這次是否真的觸發了該上限；如實回報，
        // 不能一律回 false，否則呼叫端會誤以為拿到的是完整、忠實的展開。
        report.expressionDepthLimited = expressionTruncated;
        const PrimaryInputSupport support = getPrimaryInputSupportBreakdown(query.netNameA);
        report.supportPrimaryInputs = support.all;
        report.supportRealPrimaryInputs = support.realPrimaryInputs;
        report.supportDffPseudoInputs = support.dffPseudoInputs;
        report.supportUndrivenLeaves = support.undrivenLeaves;
        report.message = expressionTruncated
            ? "Boolean expression generated, but an internal expansion safety limit was reached; "
              "some deep sub-expressions are shown as net names instead of being fully expanded."
            : "Boolean expression generated";
        report.status = "BOOLEAN_EXPRESSION";
        }
        return report;

    case FunctionQueryType::SimplifiedBooleanExpression:
        if (query.netNameA.empty() || query.maxExpressionDepth < 0) {
            report.message = "SimplifiedBooleanExpression requires netNameA and non-negative maxExpressionDepth";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (getNetId(query.netNameA) < 0) {
            report.message = "Net not found: " + query.netNameA;
            report.status = "NET_NOT_FOUND";
            return report;
        }

        {
        report.ok = true;
        report.exists = true;
        report.netIdA = getNetId(query.netNameA);
        report.expression = getSimplifiedBooleanExpression(query.netNameA, query.maxExpressionDepth);
        report.expressionLength = report.expression.size();
        report.maxExpressionDepth = query.maxExpressionDepth;
        report.expressionDepthLimited = true;
        const PrimaryInputSupport support = getPrimaryInputSupportBreakdown(query.netNameA);
        report.supportPrimaryInputs = support.all;
        report.supportRealPrimaryInputs = support.realPrimaryInputs;
        report.supportDffPseudoInputs = support.dffPseudoInputs;
        report.supportUndrivenLeaves = support.undrivenLeaves;
        report.message = "Depth-limited Boolean expression generated";
        report.status = "SIMPLIFIED_BOOLEAN_EXPRESSION";
        }
        return report;

    case FunctionQueryType::PrimaryInputsOfNet:
        if (query.netNameA.empty()) {
            report.message = "PrimaryInputsOfNet requires netNameA";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (getNetId(query.netNameA) < 0) {
            report.message = "Net not found: " + query.netNameA;
            report.status = "NET_NOT_FOUND";
            return report;
        }

        {
        report.ok = true;
        report.exists = true;
        report.netIdA = getNetId(query.netNameA);
        const PrimaryInputSupport support = getPrimaryInputSupportBreakdown(query.netNameA);
        report.supportPrimaryInputs = support.all;
        report.supportRealPrimaryInputs = support.realPrimaryInputs;
        report.supportDffPseudoInputs = support.dffPseudoInputs;
        report.supportUndrivenLeaves = support.undrivenLeaves;
        report.message = "Primary input support collected";
        report.status = "PRIMARY_INPUT_SUPPORT";
        }
        return report;
    }

    report.message = "Unsupported FunctionQueryType";
    report.status = "UNSUPPORTED_QUERY_TYPE";
    return report;
}

namespace {

using SimulationSignature = BitParallelSimulationSignature;

std::uint64_t splitMix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

bool isSupportedSimulationGate(const Gate& gate) {
    switch (gate.type) {
    case GateType::AND:
    case GateType::OR:
    case GateType::NAND:
    case GateType::NOR:
        return !gate.inputNetIds.empty();
    case GateType::NOT:
    case GateType::BUF:
        return gate.inputNetIds.size() == 1;
    case GateType::XOR:
    case GateType::XNOR:
        return gate.inputNetIds.size() == 2;
    default:
        return false;
    }
}

SimulationSignature makeLeafSignature(int netId, size_t wordCount) {
    SimulationSignature signature(wordCount, 0);
    for (size_t word = 0; word < wordCount; ++word) {
        const std::uint64_t seed =
            (static_cast<std::uint64_t>(static_cast<unsigned int>(netId)) << 32) ^
            static_cast<std::uint64_t>(word) ^ 0x6a09e667f3bcc909ULL;
        signature[word] = splitMix64(seed);
    }
    return signature;
}

bool evaluateSimulationGate(const Gate& gate,
                            const std::vector<SimulationSignature>& signatures,
                            std::uint64_t lastWordMask,
                            SimulationSignature& output) {
    if (!isSupportedSimulationGate(gate) || gate.outputNetId < 0) {
        return false;
    }

    const size_t wordCount = output.size();
    for (int inputNetId : gate.inputNetIds) {
        if (inputNetId < 0 || static_cast<size_t>(inputNetId) >= signatures.size() ||
            signatures[inputNetId].size() != wordCount) {
            return false;
        }
    }

    for (size_t word = 0; word < wordCount; ++word) {
        std::uint64_t value = 0;
        switch (gate.type) {
        case GateType::AND:
        case GateType::NAND:
            value = ~std::uint64_t{0};
            for (int inputNetId : gate.inputNetIds) value &= signatures[inputNetId][word];
            if (gate.type == GateType::NAND) value = ~value;
            break;
        case GateType::OR:
        case GateType::NOR:
            value = 0;
            for (int inputNetId : gate.inputNetIds) value |= signatures[inputNetId][word];
            if (gate.type == GateType::NOR) value = ~value;
            break;
        case GateType::NOT:
            value = ~signatures[gate.inputNetIds.front()][word];
            break;
        case GateType::BUF:
            value = signatures[gate.inputNetIds.front()][word];
            break;
        case GateType::XOR:
        case GateType::XNOR:
            value = signatures[gate.inputNetIds[0]][word] ^
                    signatures[gate.inputNetIds[1]][word];
            if (gate.type == GateType::XNOR) value = ~value;
            break;
        default:
            return false;
        }
        output[word] = value;
    }

    if (!output.empty()) {
        output.back() &= lastWordMask;
    }
    return true;
}

using SimulationResult = BitParallelSimulationResult;

SimulationResult simulateNetlist(const Netlist& netlist, size_t patternCount) {
    const size_t wordCount = (patternCount + 63) / 64;
    const size_t remainingBits = patternCount % 64;
    const std::uint64_t lastWordMask = remainingBits == 0
        ? ~std::uint64_t{0}
        : ((std::uint64_t{1} << remainingBits) - 1);

    SimulationResult result;
    result.patternCount = patternCount;
    result.lastWordMask = lastWordMask;
    result.signatures.resize(netlist.getNetCount());
    result.known.assign(netlist.getNetCount(), false);

    std::vector<int> unresolvedInputs(netlist.getGateCount(), -1);
    std::vector<std::vector<int>> waitingGates(netlist.getNetCount());
    std::queue<int> readyGates;

    auto assignLeaf = [&](int netId, const SimulationSignature& signature) {
        result.signatures[netId] = signature;
        result.known[netId] = true;
    };

    for (size_t index = 0; index < netlist.getNetCount(); ++index) {
        const int netId = static_cast<int>(index);
        const Net& net = netlist.getNet(netId);
        if (net.isRemoved) {
            continue;
        }
        if (net.isConst) {
            SimulationSignature constant(wordCount, net.constVal == 1 ? ~std::uint64_t{0} : 0);
            if (!constant.empty()) constant.back() &= lastWordMask;
            assignLeaf(netId, constant);
            continue;
        }

        const bool hasDriver = netlist.isValidGateId(net.driverGateId) &&
                               netlist.getGate(net.driverGateId).type != GateType::UNKNOWN;
        const bool dffOutput = hasDriver &&
                               netlist.getGate(net.driverGateId).type == GateType::DFF;
        if (!hasDriver || dffOutput) {
            assignLeaf(netId, makeLeafSignature(netId, wordCount));
        }
    }

    for (size_t index = 0; index < netlist.getGateCount(); ++index) {
        const int gateId = static_cast<int>(index);
        const Gate& gate = netlist.getGate(gateId);
        if (!netlist.isCombinationalGate(gateId) ||
            !isSupportedSimulationGate(gate) ||
            !netlist.isValidNetId(gate.outputNetId) ||
            netlist.getNet(gate.outputNetId).isRemoved) {
            continue;
        }

        int unresolved = 0;
        bool invalidInput = false;
        for (int inputNetId : gate.inputNetIds) {
            if (!netlist.isValidNetId(inputNetId) || netlist.getNet(inputNetId).isRemoved) {
                invalidInput = true;
                break;
            }
            if (!result.known[inputNetId]) {
                ++unresolved;
                waitingGates[inputNetId].push_back(gateId);
            }
        }
        if (invalidInput) {
            continue;
        }
        unresolvedInputs[gateId] = unresolved;
        if (unresolved == 0) {
            readyGates.push(gateId);
        }
    }

    while (!readyGates.empty()) {
        const int gateId = readyGates.front();
        readyGates.pop();
        if (!netlist.isValidGateId(gateId)) {
            continue;
        }

        const Gate& gate = netlist.getGate(gateId);
        if (!netlist.isValidNetId(gate.outputNetId) || result.known[gate.outputNetId]) {
            continue;
        }

        SimulationSignature output(wordCount, 0);
        if (!evaluateSimulationGate(gate, result.signatures, lastWordMask, output)) {
            continue;
        }
        result.signatures[gate.outputNetId] = std::move(output);
        result.known[gate.outputNetId] = true;

        for (int waitingGateId : waitingGates[gate.outputNetId]) {
            if (waitingGateId < 0 ||
                static_cast<size_t>(waitingGateId) >= unresolvedInputs.size() ||
                unresolvedInputs[waitingGateId] <= 0) {
                continue;
            }
            --unresolvedInputs[waitingGateId];
            if (unresolvedInputs[waitingGateId] == 0) {
                readyGates.push(waitingGateId);
            }
        }
    }

    return result;
}

bool signatureContainsRequiredOnes(const SimulationSignature& candidate,
                                   const SimulationSignature& requiredOnes) {
    if (candidate.size() != requiredOnes.size()) return false;
    for (size_t word = 0; word < candidate.size(); ++word) {
        if ((candidate[word] & requiredOnes[word]) != requiredOnes[word]) return false;
    }
    return true;
}

bool nandSignatureMatches(const SimulationSignature& a,
                          const SimulationSignature& b,
                          const SimulationSignature& target,
                          std::uint64_t lastWordMask) {
    if (a.size() != b.size() || a.size() != target.size()) return false;
    for (size_t word = 0; word < a.size(); ++word) {
        std::uint64_t value = ~(a[word] & b[word]);
        if (word + 1 == a.size()) value &= lastWordMask;
        if (value != target[word]) return false;
    }
    return true;
}

DetailedSatResult solveNandPairEquivalenceDetailed(const Netlist& netlist,
                                                   int targetNetId,
                                                   int netIdA,
                                                   int netIdB,
                                                   double timeLimitSeconds) {
    if (!netlist.isValidNetId(targetNetId) || !netlist.isValidNetId(netIdA) ||
        !netlist.isValidNetId(netIdB)) {
        return makeUnsupportedResult("NAND pair equivalence requires valid scalar net IDs.");
    }
    if (timeLimitSeconds <= 0.0) {
        DetailedSatResult result;
        result.unknown = true;
        result.timedOut = true;
        result.solverStatus = "TIMEOUT";
        result.message = "Function search reached its time limit before SAT verification.";
        return result;
    }

    std::unordered_set<int> visitedNets;
    std::unordered_set<int> gatesToEncode;
    std::queue<int> pending;
    for (int netId : {targetNetId, netIdA, netIdB}) {
        if (visitedNets.insert(netId).second) pending.push(netId);
    }

    while (!pending.empty()) {
        const int netId = pending.front();
        pending.pop();
        const Net& net = netlist.getNet(netId);
        if (!netlist.isValidGateId(net.driverGateId)) continue;

        const Gate& driver = netlist.getGate(net.driverGateId);
        if (driver.type == GateType::DFF) continue;
        if (!netlist.isCombinationalGate(driver.id) || !isSupportedSimulationGate(driver)) {
            return makeUnsupportedResult("Unsupported gate in NAND pair equivalence cone.");
        }
        if (!gatesToEncode.insert(driver.id).second) continue;
        for (int inputNetId : driver.inputNetIds) {
            if (!netlist.isValidNetId(inputNetId)) {
                return makeUnsupportedResult("Invalid input net in NAND pair equivalence cone.");
            }
            if (visitedNets.insert(inputNetId).second) pending.push(inputNetId);
        }
    }

    CaDiCaL::Solver solver;
    solver.set("factor", 0);
    int maxVar = static_cast<int>(netlist.getNetCount());
    solver.resize(maxVar + 2);

    for (int netId : visitedNets) {
        const Net& net = netlist.getNet(netId);
        if (!net.isConst) continue;
        const int lit = netId + 1;
        solver.add(net.constVal == 1 ? lit : -lit);
        solver.add(0);
    }
    for (int gateId : gatesToEncode) {
        if (!addGateCnfClauses(solver, netlist.getGate(gateId))) {
            return makeUnsupportedResult("Unsupported or incomplete gate in NAND pair equivalence cone.");
        }
    }

    const int aLit = netIdA + 1;
    const int bLit = netIdB + 1;
    const int nandLit = ++maxVar;
    solver.add(aLit); solver.add(nandLit); solver.add(0);
    solver.add(bLit); solver.add(nandLit); solver.add(0);
    solver.add(-aLit); solver.add(-bLit); solver.add(-nandLit); solver.add(0);

    const int targetLit = targetNetId + 1;
    const int diffLit = ++maxVar;
    solver.add(-targetLit); solver.add(-nandLit); solver.add(-diffLit); solver.add(0);
    solver.add(targetLit); solver.add(nandLit); solver.add(-diffLit); solver.add(0);
    solver.add(targetLit); solver.add(-nandLit); solver.add(diffLit); solver.add(0);
    solver.add(-targetLit); solver.add(nandLit); solver.add(diffLit); solver.add(0);
    solver.add(diffLit);
    solver.add(0);

    TimeLimitTerminator terminator(timeLimitSeconds);
    solver.connect_terminator(&terminator);
    const int solverResult = solver.solve();
    solver.disconnect_terminator();
    return makeSolveResult(solverResult, terminator);
}

bool collectFunctionSearchScopeGates(
    const Netlist& netlist,
    const FunctionSearchQuery& query,
    std::vector<int>& gateIds,
    std::string& error) {
    if (query.scope == FunctionSearchScope::WholeDesign) {
        gateIds.reserve(netlist.getGateCount());
        for (size_t index = 0; index < netlist.getGateCount(); ++index) {
            gateIds.push_back(static_cast<int>(index));
        }
        return true;
    }
    if (query.scopeName.empty()) {
        error = "A non-whole Function Search scope requires scopeName.";
        return false;
    }

    Netlist::ConeQuery coneQuery;
    switch (query.scope) {
    case FunctionSearchScope::NetFanin:
        coneQuery.type = Netlist::ConeQueryType::NetTransitiveFanin;
        coneQuery.netName = query.scopeName;
        break;
    case FunctionSearchScope::NetFanout:
        coneQuery.type = Netlist::ConeQueryType::NetTransitiveFanout;
        coneQuery.netName = query.scopeName;
        break;
    case FunctionSearchScope::GateFanin:
        coneQuery.type = Netlist::ConeQueryType::GateTransitiveFanin;
        coneQuery.gateName = query.scopeName;
        break;
    case FunctionSearchScope::GateFanout:
        coneQuery.type = Netlist::ConeQueryType::GateTransitiveFanout;
        coneQuery.gateName = query.scopeName;
        break;
    case FunctionSearchScope::WholeDesign:
        break;
    }

    const Netlist::ConeReport cone = netlist.runConeQuery(coneQuery);
    if (!cone.ok) {
        error = cone.message;
        return false;
    }
    gateIds = cone.gateIds;
    std::sort(gateIds.begin(), gateIds.end());
    gateIds.erase(std::unique(gateIds.begin(), gateIds.end()), gateIds.end());
    return true;
}

FunctionSearchMatch makeEquivalentGatePairMatch(
    const Netlist& netlist,
    int gateIdA,
    int gateIdB) {
    FunctionSearchMatch match;
    const Gate& gateA = netlist.getGate(gateIdA);
    const Gate& gateB = netlist.getGate(gateIdB);
    match.gateIdA = gateIdA;
    match.gateIdB = gateIdB;
    match.netIdA = gateA.outputNetId;
    match.netIdB = gateB.outputNetId;
    match.gateNameA = gateA.instName;
    match.gateNameB = gateB.instName;
    match.netNameA = netlist.getNet(gateA.outputNetId).name;
    match.netNameB = netlist.getNet(gateB.outputNetId).name;
    match.provenEquivalent = true;
    match.proofMethod = "SAT_EQUIVALENCE_CLASS";
    match.solverStatus = "UNSAT";
    return match;
}

FunctionSearchEquivalenceClass makeEquivalenceClassReport(
    const Netlist& netlist,
    const std::vector<int>& gateIds) {
    FunctionSearchEquivalenceClass result;
    result.gateIds = gateIds;
    result.provenEquivalent = gateIds.size() >= 2;
    result.proofMethod = "SAT_EQUIVALENCE_CLASS";
    result.netIds.reserve(gateIds.size());
    result.gateNames.reserve(gateIds.size());
    result.netNames.reserve(gateIds.size());
    for (int gateId : gateIds) {
        const Gate& gate = netlist.getGate(gateId);
        result.netIds.push_back(gate.outputNetId);
        result.gateNames.push_back(gate.instName);
        result.netNames.push_back(netlist.getNet(gate.outputNetId).name);
    }
    return result;
}

bool openFunctionSearchOutput(const FunctionSearchQuery& query,
                              FunctionSearchReport& report,
                              std::ofstream& output) {
    if (!query.writeMatchesToFile) {
        return true;
    }
    report.outputFilePath = query.outputFilePath.empty()
        ? "function_search_output.txt"
        : query.outputFilePath;
    output.open(report.outputFilePath, std::ios::out | std::ios::trunc);
    if (!output) {
        report.status = "OUTPUT_ERROR";
        report.message = "Failed to open Function Search output file: " +
                         report.outputFilePath;
        return false;
    }
    report.wroteMatchesToFile = true;
    output << "Function search matches\n\n";
    return true;
}

bool writeFunctionSearchMatchRecord(std::ostream& output,
                                    const FunctionSearchMatch& match,
                                    size_t index) {
    output << "Match " << index << "\n";
    if (match.gateIdA >= 0 || match.gateIdB >= 0) {
        output << "  gate_a: " << match.gateNameA
               << " (id=" << match.gateIdA << ")\n";
        output << "  gate_b: " << match.gateNameB
               << " (id=" << match.gateIdB << ")\n";
    }
    output << "  net_a: " << match.netNameA
           << " (id=" << match.netIdA << ")\n";
    output << "  net_b: " << match.netNameB
           << " (id=" << match.netIdB << ")\n";
    output << "  proof_method: " << match.proofMethod << "\n";
    output << "  solver_status: " << match.solverStatus << "\n\n";
    return static_cast<bool>(output);
}

bool writeFunctionSearchClassRecord(
    std::ostream& output,
    const FunctionSearchEquivalenceClass& equivalentClass,
    size_t index) {
    output << "Equivalence class " << index << "\n";
    output << "  proof_method: " << equivalentClass.proofMethod << "\n";
    output << "  member_count: " << equivalentClass.gateIds.size() << "\n";
    for (size_t member = 0; member < equivalentClass.gateIds.size(); ++member) {
        output << "  member " << (member + 1) << ": "
               << equivalentClass.gateNames[member]
               << " (gate_id=" << equivalentClass.gateIds[member]
               << ", net=" << equivalentClass.netNames[member]
               << ", net_id=" << equivalentClass.netIds[member] << ")\n";
    }
    output << "\n";
    return static_cast<bool>(output);
}

void finalizeFunctionSearchOutput(std::ofstream& output,
                                  FunctionSearchReport& report) {
    if (!output.is_open()) {
        return;
    }
    output << "Total matches: " << report.matchCount << "\n";
    output << "Complete: " << (report.complete ? "yes" : "no") << "\n";
    output << "Status: " << report.status << "\n";
    output.flush();
    if (!output) {
        report.ok = false;
        report.complete = false;
        report.status = "OUTPUT_ERROR";
        report.message = "Failed to finalize Function Search output file: " +
                         report.outputFilePath;
    }
}

FunctionSearchReport searchEquivalentGatePairs(
    const Netlist& netlist,
    const FunctionSearchQuery& query) {
    FunctionSearchReport report;
    report.queryType = query.type;
    report.scope = query.scope;
    report.scopeName = query.scopeName;
    report.gateTypeFilter = query.gateTypeFilter;
    report.simulationPatternCount = query.simulationPatternCount;

    const auto startedAt = std::chrono::steady_clock::now();
    std::ofstream matchOutput;
    auto elapsedSeconds = [&]() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startedAt).count();
    };
    auto finish = [&]() -> FunctionSearchReport {
        report.elapsedSeconds = elapsedSeconds();
        finalizeFunctionSearchOutput(matchOutput, report);
        return report;
    };

    const bool validGateTypeFilter =
        query.gateTypeFilter == GateType::UNKNOWN ||
        query.gateTypeFilter == GateType::AND ||
        query.gateTypeFilter == GateType::OR ||
        query.gateTypeFilter == GateType::NAND ||
        query.gateTypeFilter == GateType::NOR ||
        query.gateTypeFilter == GateType::NOT ||
        query.gateTypeFilter == GateType::BUF ||
        query.gateTypeFilter == GateType::XOR ||
        query.gateTypeFilter == GateType::XNOR;
    if (query.simulationPatternCount == 0 || query.simulationPatternCount > 4096 ||
        query.timeLimitSeconds <= 0.0 || !validGateTypeFilter) {
        report.status = "INVALID_ARGUMENT";
        report.message = "Equivalent gate-pair search requires 1..4096 simulation patterns, "
                         "positive timeLimitSeconds, and a supported optional "
                         "combinational gate-type filter.";
        return finish();
    }

    std::vector<int> scopeGateIds;
    std::string scopeError;
    if (!collectFunctionSearchScopeGates(netlist, query, scopeGateIds, scopeError)) {
        report.status = "SCOPE_NOT_FOUND";
        report.message = "Function Search scope could not be resolved: " + scopeError;
        return finish();
    }
    if (!openFunctionSearchOutput(query, report, matchOutput)) {
        return finish();
    }

    const SimulationResult simulation =
        simulateNetlist(netlist, query.simulationPatternCount);
    if (elapsedSeconds() >= query.timeLimitSeconds) {
        report.status = "TIMEOUT";
        report.message = "Equivalent gate-pair search reached its time limit during simulation.";
        report.timedOut = true;
        return finish();
    }

    std::vector<int> candidateGateIds;
    std::map<SimulationSignature, std::vector<int>> buckets;
    for (int gateId : scopeGateIds) {
        if (!netlist.isValidGateId(gateId) || !netlist.isCombinationalGate(gateId)) {
            continue;
        }
        const Gate& gate = netlist.getGate(gateId);
        if (query.gateTypeFilter != GateType::UNKNOWN &&
            gate.type != query.gateTypeFilter) {
            continue;
        }
        if (!netlist.isValidNetId(gate.outputNetId)) {
            ++report.unsupportedSignalCount;
            continue;
        }
        const Net& output = netlist.getNet(gate.outputNetId);
        if (output.isRemoved || output.isConst || output.name.empty() ||
            output.driverGateId != gateId) {
            ++report.unsupportedSignalCount;
            continue;
        }

        candidateGateIds.push_back(gateId);
        if (static_cast<size_t>(gate.outputNetId) >= simulation.known.size() ||
            !simulation.known[gate.outputNetId]) {
            ++report.unsupportedSignalCount;
            continue;
        }
        buckets[simulation.signatures[gate.outputNetId]].push_back(gateId);
    }
    report.candidateGateCount = candidateGateIds.size();
    report.candidateSignalCount = candidateGateIds.size();
    for (const auto& bucket : buckets) {
        report.simulationEligibleSignalCount += bucket.second.size();
    }
    report.simulationBucketCount = buckets.size();
    const size_t allEligiblePairs = report.simulationEligibleSignalCount < 2
        ? 0
        : report.simulationEligibleSignalCount *
              (report.simulationEligibleSignalCount - 1) / 2;
    size_t sameBucketPairs = 0;
    for (const auto& bucket : buckets) {
        if (bucket.second.size() >= 2) {
            sameBucketPairs += bucket.second.size() * (bucket.second.size() - 1) / 2;
        }
    }
    report.candidatePairsRejectedBySimulation = allEligiblePairs - sameBucketPairs;

    std::vector<std::vector<int>> provenClasses;
    bool stop = false;
    for (const auto& bucketEntry : buckets) {
        const std::vector<int>& bucket = bucketEntry.second;
        if (bucket.size() < 2) {
            continue;
        }

        std::vector<std::vector<int>> bucketClasses;
        for (int candidateGateId : bucket) {
            if (elapsedSeconds() >= query.timeLimitSeconds) {
                report.timedOut = true;
                stop = true;
                break;
            }

            bool matched = false;
            bool unresolved = false;
            for (std::vector<int>& equivalentClass : bucketClasses) {
                const int representativeGateId = equivalentClass.front();
                const Gate& representative = netlist.getGate(representativeGateId);
                const Gate& candidate = netlist.getGate(candidateGateId);
                ++report.candidatePairsConsidered;

                const double remaining = query.timeLimitSeconds - elapsedSeconds();
                const DetailedSatResult proof = solveEquivalenceDetailed(
                    netlist,
                    netlist.getNet(representative.outputNetId).name,
                    netlist.getNet(candidate.outputNetId).name,
                    "",
                    -1,
                    remaining);
                ++report.satChecks;
                if (!proof.conclusive()) {
                    ++report.satUnknownCount;
                    report.timedOut = report.timedOut || proof.timedOut;
                    report.unsupported = report.unsupported || proof.unsupported;
                    unresolved = true;
                    if (proof.timedOut) {
                        stop = true;
                        break;
                    }
                    continue;
                }
                if (!proof.unsat) {
                    continue;
                }

                equivalentClass.push_back(candidateGateId);
                matched = true;
                if (query.mode == FunctionSearchMode::FindAny) {
                    const FunctionSearchMatch match = makeEquivalentGatePairMatch(
                        netlist, representativeGateId, candidateGateId);
                    report.matches.push_back(match);
                    report.matchCount = 1;
                    report.equivalenceClasses.push_back(makeEquivalenceClassReport(
                        netlist, equivalentClass));
                    report.equivalenceClassCount = 1;
                    report.equivalentPairCount = 1;
                    report.found = true;
                    report.ok = true;
                    report.complete = true;
                    report.status = "MATCH_FOUND";
                    report.message = "Found a SAT-proven functionally equivalent gate pair.";
                    return finish();
                }
                break;
            }
            if (stop) {
                break;
            }
            if (!matched && !unresolved) {
                bucketClasses.push_back({candidateGateId});
            }
        }

        for (std::vector<int>& equivalentClass : bucketClasses) {
            if (equivalentClass.size() >= 2) {
                provenClasses.push_back(std::move(equivalentClass));
            }
        }
        if (stop) {
            break;
        }
    }

    for (const std::vector<int>& equivalentClass : provenClasses) {
        report.equivalenceClasses.push_back(
            makeEquivalenceClassReport(netlist, equivalentClass));
        ++report.equivalenceClassCount;
        report.equivalentPairCount +=
            equivalentClass.size() * (equivalentClass.size() - 1) / 2;
    }
    report.matchCount = report.equivalentPairCount;

    if (matchOutput.is_open()) {
        for (size_t index = 0; index < report.equivalenceClasses.size(); ++index) {
            if (!writeFunctionSearchClassRecord(
                    matchOutput, report.equivalenceClasses[index], index + 1)) {
                report.status = "OUTPUT_ERROR";
                report.message = "Failed to write Function Search equivalence classes to: " +
                                 report.outputFilePath;
                return finish();
            }
        }
    }

    if (query.expandEquivalentPairs) {
        size_t emittedPairCount = 0;
        for (const std::vector<int>& equivalentClass : provenClasses) {
            for (size_t i = 0; i < equivalentClass.size(); ++i) {
                for (size_t j = i + 1; j < equivalentClass.size(); ++j) {
                    if (query.maxResults > 0 &&
                        emittedPairCount >= query.maxResults) {
                        report.truncated = true;
                        break;
                    }
                    const FunctionSearchMatch match = makeEquivalentGatePairMatch(
                        netlist, equivalentClass[i], equivalentClass[j]);
                    ++emittedPairCount;
                    if (matchOutput.is_open() &&
                        !writeFunctionSearchMatchRecord(
                            matchOutput, match, emittedPairCount)) {
                        report.status = "OUTPUT_ERROR";
                        report.message = "Failed to write Function Search matches to: " +
                                         report.outputFilePath;
                        return finish();
                    }
                    if (query.maxStoredMatches > 0 &&
                        report.matches.size() < query.maxStoredMatches) {
                        report.matches.push_back(match);
                    }
                }
                if (report.truncated) break;
            }
            if (report.truncated) break;
        }
    }
    report.found = !report.equivalenceClasses.empty();

    if (report.timedOut) {
        report.status = "TIMEOUT";
        report.message = "Equivalent gate-pair search reached its time limit; results are partial.";
        return finish();
    }
    if (report.truncated) {
        report.status = "RESULT_LIMIT_REACHED";
        report.message = "Equivalent gate-pair search reached maxResults; results are partial.";
        return finish();
    }
    if (report.satUnknownCount > 0 || report.unsupportedSignalCount > 0 ||
        report.unsupported) {
        report.status = report.unsupported ? "UNSUPPORTED_OR_PARTIAL" : "SOLVER_UNKNOWN";
        report.message = "Equivalent gate-pair search could not classify every candidate.";
        return finish();
    }

    report.ok = true;
    report.complete = true;
    report.allCandidatesExamined = true;
    report.status = report.found ? "MATCHES_FOUND" : "NO_MATCH";
    report.message = report.found
        ? "All eligible gate outputs were classified into SAT-proven equivalence classes."
        : "No functionally equivalent combinational gate pair exists in the selected scope.";
    return finish();
}

} // namespace

BitParallelSimulationResult simulateNetlistBitParallel(
    const Netlist& netlist,
    size_t patternCount) {
    return simulateNetlist(netlist, patternCount);
}

Netlist::FunctionSearchReport Netlist::runFunctionSearchQuery(
    const FunctionSearchQuery& query) const {
    FunctionSearchReport report;
    report.queryType = query.type;
    report.scope = query.scope;
    report.scopeName = query.scopeName;
    report.gateTypeFilter = query.gateTypeFilter;
    report.targetNetName = query.targetNetName;
    report.simulationPatternCount = query.simulationPatternCount;
    const auto startedAt = std::chrono::steady_clock::now();
    std::ofstream matchOutput;
    auto elapsedSeconds = [&]() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startedAt).count();
    };
    auto finish = [&]() -> FunctionSearchReport {
        report.elapsedSeconds = elapsedSeconds();
        finalizeFunctionSearchOutput(matchOutput, report);
        return report;
    };

    if (query.type == FunctionSearchQueryType::EquivalentGatePairs) {
        return searchEquivalentGatePairs(*this, query);
    }
    if (query.type != FunctionSearchQueryType::NandEquivalentInputPairs) {
        report.status = "UNSUPPORTED_QUERY_TYPE";
        report.message = "Unsupported FunctionSearchQueryType";
        report.unsupported = true;
        return finish();
    }
    if (query.targetNetName.empty() || query.simulationPatternCount == 0 ||
        query.simulationPatternCount > 4096 || query.timeLimitSeconds <= 0.0) {
        report.status = "INVALID_ARGUMENT";
        report.message = "Function search requires a target, 1..4096 simulation patterns, "
                         "and positive timeLimitSeconds.";
        return finish();
    }

    const std::vector<int> targetBits = expandNetToBits(query.targetNetName);
    if (targetBits.empty()) {
        report.status = "TARGET_NOT_FOUND";
        report.message = "Target net not found: " + query.targetNetName;
        return finish();
    }
    if (targetBits.size() != 1) {
        report.status = "SCALAR_TARGET_REQUIRED";
        report.message = "NAND pair search requires an existing scalar target net.";
        return finish();
    }
    report.targetNetId = targetBits.front();
    if (!isValidNetId(report.targetNetId) || getNet(report.targetNetId).isRemoved) {
        report.status = "TARGET_NOT_FOUND";
        report.message = "Target net is missing or removed: " + query.targetNetName;
        return finish();
    }
    if (!openFunctionSearchOutput(query, report, matchOutput)) {
        return finish();
    }

    const SimulationResult simulation = simulateNetlist(*this, query.simulationPatternCount);
    if (!simulation.known[report.targetNetId]) {
        report.status = "UNSUPPORTED_TARGET_CONE";
        report.message = "Target function could not be simulated because its cone is unsupported or cyclic.";
        report.unsupported = true;
        return finish();
    }
    if (elapsedSeconds() >= query.timeLimitSeconds) {
        report.status = "TIMEOUT";
        report.message = "Function search reached its time limit during simulation.";
        report.timedOut = true;
        return finish();
    }

    std::vector<int> candidateNetIds;
    std::vector<int> eligibleNetIds;
    const SimulationSignature& targetSignature = simulation.signatures[report.targetNetId];
    SimulationSignature requiredOnes(targetSignature.size(), 0);
    for (size_t word = 0; word < targetSignature.size(); ++word) {
        requiredOnes[word] = ~targetSignature[word];
    }
    const size_t remainingBits = query.simulationPatternCount % 64;
    const std::uint64_t lastWordMask = remainingBits == 0
        ? ~std::uint64_t{0}
        : ((std::uint64_t{1} << remainingBits) - 1);
    requiredOnes.back() &= lastWordMask;

    for (size_t index = 0; index < getNetCount(); ++index) {
        const int netId = static_cast<int>(index);
        const Net& net = getNet(netId);
        if (netId == report.targetNetId || net.isRemoved || net.isConst || net.name.empty()) {
            continue;
        }
        const bool hasDefinedDriver = isValidGateId(net.driverGateId) &&
                                      getGate(net.driverGateId).type != GateType::UNKNOWN;
        if (query.internalSignalsOnly) {
            if (net.isPI || net.isPO || !hasDefinedDriver) continue;
        } else if (!net.isPI && !hasDefinedDriver) {
            continue;
        }

        candidateNetIds.push_back(netId);
        if (!simulation.known[netId]) {
            ++report.unsupportedSignalCount;
            continue;
        }
        if (signatureContainsRequiredOnes(simulation.signatures[netId], requiredOnes)) {
            eligibleNetIds.push_back(netId);
        }
    }
    report.candidateSignalCount = candidateNetIds.size();
    report.simulationEligibleSignalCount = eligibleNetIds.size();

    bool stopped = false;
    for (size_t i = 0; i < eligibleNetIds.size() && !stopped; ++i) {
        const size_t firstJ = query.allowSameSignalPair ? i : i + 1;
        for (size_t j = firstJ; j < eligibleNetIds.size(); ++j) {
            if ((report.candidatePairsConsidered & 0xfffU) == 0 &&
                elapsedSeconds() >= query.timeLimitSeconds) {
                report.timedOut = true;
                stopped = true;
                break;
            }

            const int netIdA = eligibleNetIds[i];
            const int netIdB = eligibleNetIds[j];
            ++report.candidatePairsConsidered;
            if (!nandSignatureMatches(simulation.signatures[netIdA],
                                      simulation.signatures[netIdB],
                                      targetSignature,
                                      lastWordMask)) {
                ++report.candidatePairsRejectedBySimulation;
                continue;
            }

            const double remaining = query.timeLimitSeconds - elapsedSeconds();
            const DetailedSatResult proof = solveNandPairEquivalenceDetailed(
                *this, report.targetNetId, netIdA, netIdB, remaining);
            ++report.satChecks;
            if (!proof.conclusive()) {
                ++report.satUnknownCount;
                report.timedOut = report.timedOut || proof.timedOut;
                report.unsupported = report.unsupported || proof.unsupported;
                if (proof.timedOut) {
                    stopped = true;
                    break;
                }
                continue;
            }
            if (!proof.unsat) {
                continue;
            }

            if (query.mode == FunctionSearchMode::FindAll &&
                query.maxResults > 0 &&
                report.matchCount >= query.maxResults) {
                report.truncated = true;
                stopped = true;
                break;
            }

            FunctionSearchMatch match;
            match.netIdA = netIdA;
            match.netIdB = netIdB;
            match.netNameA = getNet(netIdA).name;
            match.netNameB = getNet(netIdB).name;
            match.provenEquivalent = true;
            match.proofMethod = "SAT_UNSAT_MITER";
            match.solverStatus = proof.solverStatus;
            ++report.matchCount;
            if (matchOutput.is_open() &&
                !writeFunctionSearchMatchRecord(
                    matchOutput, match, report.matchCount)) {
                report.status = "OUTPUT_ERROR";
                report.message = "Failed to write Function Search matches to: " +
                                 report.outputFilePath;
                return finish();
            }
            if (query.mode == FunctionSearchMode::FindAny ||
                (query.maxStoredMatches > 0 &&
                 report.matches.size() < query.maxStoredMatches)) {
                report.matches.push_back(match);
            }
            report.found = true;

            if (query.mode == FunctionSearchMode::FindAny) {
                report.ok = true;
                report.complete = true;
                report.status = "MATCH_FOUND";
                report.message = "Found a SAT-proven internal signal pair.";
                return finish();
            }
        }
    }

    if (report.timedOut) {
        report.status = "TIMEOUT";
        report.message = "Function search reached its time limit; results are partial.";
        return finish();
    }
    if (report.truncated) {
        report.status = "RESULT_LIMIT_REACHED";
        report.message = "Function search reached maxResults; results are partial.";
        return finish();
    }
    if (report.satUnknownCount > 0 || report.unsupportedSignalCount > 0 || report.unsupported) {
        report.status = report.unsupported ? "UNSUPPORTED_OR_PARTIAL" : "SOLVER_UNKNOWN";
        report.message = "Function search could not prove a complete answer for every candidate.";
        return finish();
    }

    report.ok = true;
    report.complete = true;
    report.allCandidatesExamined = true;
    report.status = report.found ? "MATCHES_FOUND" : "NO_MATCH";
    report.message = report.found
        ? "All candidate pairs were searched and SAT-proven matches were collected."
        : "No internal signal pair satisfies the requested NAND equivalence.";
    return finish();
}
