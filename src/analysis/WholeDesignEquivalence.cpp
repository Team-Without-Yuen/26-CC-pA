#include "include/core/Netlist.h"
#include "include/SATEngine/SatTime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct NamedNet {
    std::string name;
    int netId = -1;
};

struct InterfaceDiff {
    std::vector<std::string> missing;
    std::vector<std::string> extra;
};

std::vector<NamedNet> collectPortBits(const Netlist& netlist, const std::vector<Port>& ports) {
    std::vector<NamedNet> result;
    for (const Port& port : ports) {
        for (int netId : port.netIds) {
            if (!netlist.isValidNetId(netId)) continue;
            const Net& net = netlist.getNet(netId);
            if (net.isRemoved) continue;
            result.push_back({net.name, netId});
        }
    }
    return result;
}

std::vector<NamedNet> collectDffDInputs(const Netlist& netlist) {
    std::vector<NamedNet> result;
    for (const std::string& dffName : netlist.getDffNames()) {
        const int gateId = netlist.getGateId(dffName);
        int dNetId = netlist.getDffInputNetId(gateId, "D");
        if (netlist.isValidNetId(dNetId) && netlist.getNet(dNetId).isRemoved) {
            dNetId = -1;
        }
        // Keep every active DFF in the interface set. An invalid D pin is
        // reported as an unsupported endpoint instead of being silently skipped.
        result.push_back({dffName, dNetId});
    }
    return result;
}

std::set<std::string> toNameSet(const std::vector<NamedNet>& nets) {
    std::set<std::string> result;
    for (const NamedNet& item : nets) {
        result.insert(item.name);
    }
    return result;
}

InterfaceDiff diffNames(const std::set<std::string>& originalNames,
                        const std::set<std::string>& currentNames) {
    InterfaceDiff diff;
    for (const std::string& name : originalNames) {
        if (!currentNames.count(name)) diff.missing.push_back(name);
    }
    for (const std::string& name : currentNames) {
        if (!originalNames.count(name)) diff.extra.push_back(name);
    }
    return diff;
}

std::map<std::string, int> toNameIdMap(const std::vector<NamedNet>& nets) {
    std::map<std::string, int> result;
    for (const NamedNet& item : nets) {
        result[item.name] = item.netId;
    }
    return result;
}

bool isDffOutputNet(const Netlist& netlist, int netId) {
    if (!netlist.isValidNetId(netId)) return false;
    const int driverId = netlist.getNet(netId).driverGateId;
    return netlist.isValidGateId(driverId) &&
           !netlist.isGateRemoved(driverId) &&
           netlist.getGate(driverId).type == GateType::DFF;
}

struct CrossSatContext {
    CaDiCaL::Solver solver;
    int nextVar = 0;
    std::unordered_map<std::string, int> sharedLeafVars;
    std::unordered_map<int, int> originalVars;
    std::unordered_map<int, int> currentVars;
    std::vector<std::string> errors;

    int newVar() {
        ++nextVar;
        solver.resize(nextVar);
        return nextVar;
    }
};

std::string sharedLeafKey(const Netlist& netlist, int netId) {
    if (!netlist.isValidNetId(netId)) return "";
    const Net& net = netlist.getNet(netId);
    if (net.isPI) return "PI:" + net.name;
    if (isDffOutputNet(netlist, netId)) {
        return "DFFQ:" + netlist.getGate(net.driverGateId).instName;
    }
    const bool hasActiveDriver =
        netlist.isValidGateId(net.driverGateId) &&
        !netlist.isGateRemoved(net.driverGateId) &&
        netlist.getGate(net.driverGateId).type != GateType::UNKNOWN;
    if (!net.isConst && !hasActiveDriver) {
        return "UNDRIVEN:" + net.name;
    }
    return "";
}

int netVar(CrossSatContext& ctx, const Netlist& netlist, int netId, bool originalSide) {
    if (!netlist.isValidNetId(netId)) {
        ctx.errors.push_back("Invalid net id in SAT encoding.");
        return -1;
    }

    const std::string sharedKey = sharedLeafKey(netlist, netId);
    if (!sharedKey.empty()) {
        auto it = ctx.sharedLeafVars.find(sharedKey);
        if (it != ctx.sharedLeafVars.end()) return it->second;
        const int var = ctx.newVar();
        ctx.sharedLeafVars[sharedKey] = var;
        return var;
    }

    std::unordered_map<int, int>& vars = originalSide ? ctx.originalVars : ctx.currentVars;
    auto it = vars.find(netId);
    if (it != vars.end()) return it->second;

    const int var = ctx.newVar();
    vars[netId] = var;
    return var;
}

void addClause(CaDiCaL::Solver& solver, const std::vector<int>& lits) {
    for (int lit : lits) solver.add(lit);
    solver.add(0);
}

bool addGateClauses(CrossSatContext& ctx, const Netlist& netlist, const Gate& gate, bool originalSide) {
    if (gate.type == GateType::UNKNOWN || gate.type == GateType::DFF) return true;
    if (gate.outputNetId < 0) {
        ctx.errors.push_back("Gate has no output: " + gate.instName);
        return false;
    }

    const int out = netVar(ctx, netlist, gate.outputNetId, originalSide);
    if (out < 0) return false;

    std::vector<int> inputs;
    for (int inputNetId : gate.inputNetIds) {
        const int var = netVar(ctx, netlist, inputNetId, originalSide);
        if (var < 0) return false;
        inputs.push_back(var);
    }

    if (gate.type == GateType::BUF || gate.type == GateType::NOT) {
        if (inputs.size() != 1) {
            ctx.errors.push_back("Unary gate has invalid input count: " + gate.instName);
            return false;
        }
        const int a = inputs[0];
        if (gate.type == GateType::BUF) {
            addClause(ctx.solver, {-a, out});
            addClause(ctx.solver, {a, -out});
        } else {
            addClause(ctx.solver, {a, out});
            addClause(ctx.solver, {-a, -out});
        }
        return true;
    }

    if (inputs.empty()) {
        ctx.errors.push_back("Gate has no inputs: " + gate.instName);
        return false;
    }

    if (gate.type == GateType::AND || gate.type == GateType::NAND) {
        for (int input : inputs) {
            addClause(ctx.solver, {input, gate.type == GateType::AND ? -out : out});
        }
        std::vector<int> clause;
        for (int input : inputs) clause.push_back(-input);
        clause.push_back(gate.type == GateType::AND ? out : -out);
        addClause(ctx.solver, clause);
        return true;
    }

    if (gate.type == GateType::OR || gate.type == GateType::NOR) {
        for (int input : inputs) {
            addClause(ctx.solver, {-input, gate.type == GateType::OR ? out : -out});
        }
        std::vector<int> clause;
        for (int input : inputs) clause.push_back(input);
        clause.push_back(gate.type == GateType::OR ? -out : out);
        addClause(ctx.solver, clause);
        return true;
    }

    if (gate.type == GateType::XOR || gate.type == GateType::XNOR) {
        auto addXorRelation = [&](int a, int b, int result, bool inverted) {
            if (!inverted) {
                addClause(ctx.solver, {-a, -b, -result});
                addClause(ctx.solver, {a, b, -result});
                addClause(ctx.solver, {a, -b, result});
                addClause(ctx.solver, {-a, b, result});
            } else {
                addClause(ctx.solver, {-a, -b, result});
                addClause(ctx.solver, {a, b, result});
                addClause(ctx.solver, {a, -b, -result});
                addClause(ctx.solver, {-a, b, -result});
            }
        };

        if (inputs.size() == 1) {
            const int a = inputs.front();
            if (gate.type == GateType::XOR) {
                addClause(ctx.solver, {-a, out});
                addClause(ctx.solver, {a, -out});
            } else {
                addClause(ctx.solver, {a, out});
                addClause(ctx.solver, {-a, -out});
            }
            return true;
        }

        int parity = inputs.front();
        for (size_t i = 1; i < inputs.size(); ++i) {
            const bool isLast = i + 1 == inputs.size();
            const int result = isLast ? out : ctx.newVar();
            addXorRelation(
                parity,
                inputs[i],
                result,
                isLast && gate.type == GateType::XNOR);
            parity = result;
        }
        return true;
    }

    ctx.errors.push_back("Unsupported gate type in whole-design SAT: " + gate.instName);
    return false;
}

bool encodeDesign(CrossSatContext& ctx, const Netlist& netlist, bool originalSide) {
    for (size_t i = 0; i < netlist.getNetCount(); ++i) {
        const Net& net = netlist.getNet(static_cast<int>(i));
        if (net.isRemoved) continue;
        if (!net.isConst) continue;
        const int var = netVar(ctx, netlist, static_cast<int>(i), originalSide);
        if (var < 0) return false;
        if (net.constVal == 0) addClause(ctx.solver, {-var});
        else if (net.constVal == 1) addClause(ctx.solver, {var});
        else {
            ctx.errors.push_back("Constant net has invalid value: " + net.name);
            return false;
        }
    }

    for (size_t i = 0; i < netlist.getGateCount(); ++i) {
        const Gate& gate = netlist.getGate(static_cast<int>(i));
        if (!addGateClauses(ctx, netlist, gate, originalSide)) {
            return false;
        }
    }
    return true;
}

enum class EndpointKind {
    PrimaryOutput,
    DffD
};

struct EndpointPair {
    EndpointKind kind = EndpointKind::PrimaryOutput;
    std::string name;
    int originalNetId = -1;
    int currentNetId = -1;
};

struct BatchEndpointSatResult {
    bool ok = false;
    bool equivalent = false;
    bool timeBudgetExceeded = false;
    std::vector<EndpointPair> matched;
    std::vector<EndpointPair> mismatched;
    std::vector<EndpointPair> skipped;
    std::vector<std::string> errors;
};

BatchEndpointSatResult compareEndpointsBySat(
    const Netlist& original,
    const Netlist& current,
    const std::vector<EndpointPair>& endpoints,
    double totalTimeBudgetSeconds) {
    BatchEndpointSatResult result;
    if (endpoints.empty()) {
        result.ok = true;
        result.equivalent = true;
        return result;
    }

    const auto startTime = std::chrono::steady_clock::now();
    auto remainingSeconds = [&]() {
        if (totalTimeBudgetSeconds <= 0.0) {
            return request_time_budget::kGeneralToolBudgetSeconds;
        }
        const std::chrono::duration<double> elapsed =
            std::chrono::steady_clock::now() - startTime;
        return totalTimeBudgetSeconds - elapsed.count();
    };

    CrossSatContext ctx;
    if (!encodeDesign(ctx, original, true) || !encodeDesign(ctx, current, false)) {
        result.errors.insert(result.errors.end(), ctx.errors.begin(), ctx.errors.end());
        result.skipped = endpoints;
        return result;
    }

    std::vector<int> diffLits;
    diffLits.reserve(endpoints.size());
    for (const EndpointPair& endpoint : endpoints) {
        const int a = netVar(ctx, original, endpoint.originalNetId, true);
        const int b = netVar(ctx, current, endpoint.currentNetId, false);
        if (a <= 0 || b <= 0) {
            result.errors.push_back("Invalid endpoint net while building whole-design miter: " + endpoint.name);
            result.skipped = endpoints;
            return result;
        }
        const int diff = ctx.newVar();
        diffLits.push_back(diff);
        addClause(ctx.solver, {-a, -b, -diff});
        addClause(ctx.solver, {a, b, -diff});
        addClause(ctx.solver, {a, -b, diff});
        addClause(ctx.solver, {-a, b, diff});
    }

    const int globalSelector = ctx.newVar();
    std::vector<int> globalDiffClause;
    globalDiffClause.reserve(diffLits.size() + 1);
    globalDiffClause.push_back(-globalSelector);
    globalDiffClause.insert(globalDiffClause.end(), diffLits.begin(), diffLits.end());
    addClause(ctx.solver, globalDiffClause);

    ctx.solver.resize(ctx.nextVar);

    const int SAT = 10;
    const int UNSAT = 20;
    bool lastSolveTimedOut = false;
    auto solveWithAssumption = [&](int assumption, double maximumSolveSeconds) {
        lastSolveTimedOut = false;
        const double remaining = remainingSeconds();
        if (totalTimeBudgetSeconds > 0.0 && remaining <= 0.0) {
            return 0;
        }
        const double solveLimit = std::max(
            0.001,
            std::min(remaining, maximumSolveSeconds));
        TimeLimitTerminator terminator(solveLimit);
        ctx.solver.connect_terminator(&terminator);
        ctx.solver.assume(assumption);
        const int solveResult = ctx.solver.solve();
        ctx.solver.disconnect_terminator();
        lastSolveTimedOut = terminator.wasTerminated();
        return solveResult;
    };

    const int globalResult = solveWithAssumption(
        globalSelector,
        remainingSeconds());
    if (globalResult == UNSAT) {
        result.ok = true;
        result.equivalent = true;
        result.matched = endpoints;
        return result;
    }
    if (globalResult != SAT) {
        result.timeBudgetExceeded = lastSolveTimedOut ||
            (totalTimeBudgetSeconds > 0.0 && remainingSeconds() <= 0.0);
        result.errors.push_back(
            result.timeBudgetExceeded
                ? "Whole-design equivalence time budget exceeded during the global endpoint miter."
                : "SAT solver returned UNKNOWN during the global endpoint miter.");
        result.skipped = endpoints;
        return result;
    }

    for (size_t i = 0; i < endpoints.size(); ++i) {
        if (totalTimeBudgetSeconds > 0.0 && remainingSeconds() <= 0.0) {
            result.timeBudgetExceeded = true;
            result.skipped.insert(result.skipped.end(), endpoints.begin() + i, endpoints.end());
            break;
        }

        const size_t remainingEndpointCount = endpoints.size() - i;
        const double endpointShare =
            remainingSeconds() / static_cast<double>(remainingEndpointCount);
        const int endpointResult = solveWithAssumption(
            diffLits[i],
            endpointShare);
        if (endpointResult == UNSAT) {
            result.matched.push_back(endpoints[i]);
        } else if (endpointResult == SAT) {
            result.mismatched.push_back(endpoints[i]);
        } else {
            result.errors.push_back("SAT solver returned UNKNOWN while comparing endpoint: " + endpoints[i].name);
            result.skipped.push_back(endpoints[i]);
        }
    }

    result.ok = result.errors.empty() && result.skipped.empty();
    result.equivalent = result.ok && result.mismatched.empty();
    return result;
}

} // namespace

WholeDesignEquivalenceReport Netlist::checkWholeDesignEquivalence(
    const Netlist& original,
    double totalTimeBudgetSeconds) const {
    WholeDesignEquivalenceReport report;
    report.method = EquivalenceCheckMethod::WholeDesignSat;
    report.timeBudgetSeconds = totalTimeBudgetSeconds;
    if (!std::isfinite(totalTimeBudgetSeconds) || totalTimeBudgetSeconds <= 0.0) {
        report.ok = false;
        report.equivalent = false;
        report.message = "Whole-design equivalence requires a finite, positive time budget.";
        return report;
    }
    const std::vector<NamedNet> originalInputs = collectPortBits(original, original.getPrimaryInputs());
    const std::vector<NamedNet> currentInputs = collectPortBits(*this, getPrimaryInputs());
    const std::vector<NamedNet> originalOutputs = collectPortBits(original, original.getPrimaryOutputs());
    const std::vector<NamedNet> currentOutputs = collectPortBits(*this, getPrimaryOutputs());
    const std::vector<NamedNet> originalDffDs = collectDffDInputs(original);
    const std::vector<NamedNet> currentDffDs = collectDffDInputs(*this);

    const InterfaceDiff inputDiff = diffNames(toNameSet(originalInputs), toNameSet(currentInputs));
    const InterfaceDiff outputDiff = diffNames(toNameSet(originalOutputs), toNameSet(currentOutputs));
    const InterfaceDiff dffDiff = diffNames(toNameSet(originalDffDs), toNameSet(currentDffDs));
    report.missingInputNames = inputDiff.missing;
    report.extraInputNames = inputDiff.extra;
    report.missingOutputNames = outputDiff.missing;
    report.extraOutputNames = outputDiff.extra;
    report.missingDffNames = dffDiff.missing;
    report.extraDffNames = dffDiff.extra;

    if (!report.missingInputNames.empty() || !report.extraInputNames.empty() ||
        !report.missingOutputNames.empty() || !report.extraOutputNames.empty() ||
        !report.missingDffNames.empty() || !report.extraDffNames.empty()) {
        report.ok = false;
        report.equivalent = false;
        report.message = "Interface mismatch; whole-design equivalence was not attempted for all endpoints.";
        return report;
    }

    if (originalOutputs.empty() && originalDffDs.empty()) {
        report.ok = false;
        report.equivalent = false;
        report.message = "Original design has no primary outputs or DFF.D endpoints to compare.";
        return report;
    }

    if (original.getGateCountByType(GateType::DFF) > 0 || getGateCountByType(GateType::DFF) > 0) {
        report.warnings.push_back(
            "DFF outputs are treated as sequential boundary leaves; DFF.D next-state functions are compared, but initial state is not analyzed.");
    }

    const std::map<std::string, int> originalOutputByName = toNameIdMap(originalOutputs);
    const std::map<std::string, int> currentOutputByName = toNameIdMap(currentOutputs);
    const std::map<std::string, int> originalDffDByName = toNameIdMap(originalDffDs);
    const std::map<std::string, int> currentDffDByName = toNameIdMap(currentDffDs);

    std::vector<EndpointPair> endpoints;
    endpoints.reserve(originalOutputByName.size() + originalDffDByName.size());
    for (const auto& item : originalOutputByName) {
        endpoints.push_back({
            EndpointKind::PrimaryOutput,
            item.first,
            item.second,
            currentOutputByName.at(item.first)});
    }
    for (const auto& item : originalDffDByName) {
        endpoints.push_back({
            EndpointKind::DffD,
            item.first + ".D",
            item.second,
            currentDffDByName.at(item.first)});
    }

    const BatchEndpointSatResult satResult = compareEndpointsBySat(
        original,
        *this,
        endpoints,
        totalTimeBudgetSeconds);

    auto appendEndpoint = [&](const EndpointPair& endpoint,
                              std::vector<std::string>& outputNames,
                              std::vector<std::string>& dffNames) {
        if (endpoint.kind == EndpointKind::PrimaryOutput) {
            outputNames.push_back(endpoint.name);
        } else {
            dffNames.push_back(endpoint.name);
        }
    };
    for (const EndpointPair& endpoint : satResult.matched) {
        appendEndpoint(endpoint, report.matchedOutputNames, report.matchedDffDNames);
    }
    for (const EndpointPair& endpoint : satResult.mismatched) {
        appendEndpoint(endpoint, report.mismatchedOutputNames, report.mismatchedDffDNames);
    }
    for (const EndpointPair& endpoint : satResult.skipped) {
        appendEndpoint(endpoint, report.skippedOutputNames, report.skippedDffDNames);
    }

    report.comparedOutputCount = static_cast<int>(
        report.matchedOutputNames.size() + report.mismatchedOutputNames.size());
    report.comparedDffDCount = static_cast<int>(
        report.matchedDffDNames.size() + report.mismatchedDffDNames.size());
    report.skippedOutputCount = static_cast<int>(report.skippedOutputNames.size());
    report.skippedDffDCount = static_cast<int>(report.skippedDffDNames.size());
    report.timeBudgetExceeded = satResult.timeBudgetExceeded;
    report.unsupportedReasons = satResult.errors;

    report.ok = satResult.ok;
    report.equivalent = satResult.equivalent;
    if (report.equivalent) {
        report.message = "Whole-design equivalence proved for all primary outputs and DFF.D endpoints.";
    } else if (!report.ok) {
        report.message = "Whole-design equivalence check encountered unsupported logic or SAT timeout.";
    } else {
        report.message = "Whole-design equivalence failed for one or more primary outputs or DFF.D endpoints.";
    }
    return report;
}
