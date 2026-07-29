#include "include/core/Netlist.h"
#include "include/core/BitParallelSimulation.h"
#include "include/core/FunctionalPatternEngine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <unordered_set>

namespace {

struct LiteralRef {
    int signalNetId = -1;
    int baseNetId = -1;
    bool inverted = false;
    std::vector<int> evidenceGateIds;
};

struct BranchRef {
    int gateId = -1;
    std::array<LiteralRef, 2> literals;
};

using Clock = std::chrono::steady_clock;

double elapsedSeconds(const Clock::time_point& start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

bool isActiveNet(const Netlist& netlist, int netId) {
    return netlist.isValidNetId(netId) && !netlist.getNet(netId).isRemoved;
}

bool isActiveGate(const Netlist& netlist, int gateId) {
    return netlist.isValidGateId(gateId) && !netlist.isGateRemoved(gateId);
}

bool getInverterInput(const Netlist& netlist, const Gate& gate, int& inputNetId) {
    if (gate.type == GateType::NOT && gate.inputNetIds.size() == 1) {
        inputNetId = gate.inputNetIds.front();
        return isActiveNet(netlist, inputNetId);
    }
    if ((gate.type == GateType::NAND || gate.type == GateType::NOR) &&
        gate.inputNetIds.size() == 2 &&
        gate.inputNetIds[0] == gate.inputNetIds[1]) {
        inputNetId = gate.inputNetIds.front();
        return isActiveNet(netlist, inputNetId);
    }
    return false;
}

int stripBuffers(const Netlist& netlist,
                 int netId,
                 std::vector<int>* evidenceGateIds = nullptr) {
    std::unordered_set<int> visited;
    while (isActiveNet(netlist, netId) && visited.insert(netId).second) {
        const int driverId = netlist.getNet(netId).driverGateId;
        if (!isActiveGate(netlist, driverId)) {
            break;
        }
        const Gate& driver = netlist.getGate(driverId);
        if (driver.type != GateType::BUF || driver.inputNetIds.size() != 1 ||
            !isActiveNet(netlist, driver.inputNetIds.front())) {
            break;
        }
        if (evidenceGateIds != nullptr) {
            evidenceGateIds->push_back(driverId);
        }
        netId = driver.inputNetIds.front();
    }
    return netId;
}

LiteralRef normalizeLiteral(const Netlist& netlist, int signalNetId) {
    LiteralRef result;
    result.signalNetId = signalNetId;
    int netId = signalNetId;
    std::unordered_set<int> visited;

    while (isActiveNet(netlist, netId) && visited.insert(netId).second) {
        netId = stripBuffers(netlist, netId, &result.evidenceGateIds);
        if (!isActiveNet(netlist, netId)) {
            break;
        }

        const int driverId = netlist.getNet(netId).driverGateId;
        if (!isActiveGate(netlist, driverId)) {
            break;
        }
        const Gate& driver = netlist.getGate(driverId);
        int inverterInputNetId = -1;
        if (!getInverterInput(netlist, driver, inverterInputNetId)) {
            break;
        }

        result.evidenceGateIds.push_back(driverId);
        result.inverted = !result.inverted;
        netId = inverterInputNetId;
    }

    result.baseNetId = stripBuffers(netlist, netId, &result.evidenceGateIds);
    return result;
}

bool loadBranch(const Netlist& netlist,
                int branchNetId,
                GateType expectedType,
                BranchRef& branch) {
    const LiteralRef normalizedBranch = normalizeLiteral(netlist, branchNetId);
    if (normalizedBranch.inverted) {
        return false;
    }
    const int normalizedNetId = normalizedBranch.baseNetId;
    if (!isActiveNet(netlist, normalizedNetId)) {
        return false;
    }

    const int driverId = netlist.getNet(normalizedNetId).driverGateId;
    if (!isActiveGate(netlist, driverId)) {
        return false;
    }
    const Gate& gate = netlist.getGate(driverId);
    if (gate.type != expectedType || gate.inputNetIds.size() != 2) {
        return false;
    }

    branch.gateId = driverId;
    branch.literals[0] = normalizeLiteral(netlist, gate.inputNetIds[0]);
    branch.literals[1] = normalizeLiteral(netlist, gate.inputNetIds[1]);
    branch.literals[0].evidenceGateIds.insert(
        branch.literals[0].evidenceGateIds.end(),
        normalizedBranch.evidenceGateIds.begin(),
        normalizedBranch.evidenceGateIds.end());
    return branch.literals[0].baseNetId >= 0 && branch.literals[1].baseNetId >= 0;
}

void appendUnique(std::vector<int>& values, int value) {
    if (value >= 0 && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void appendLiteralEvidence(std::vector<int>& values, const LiteralRef& literal) {
    for (int gateId : literal.evidenceGateIds) {
        appendUnique(values, gateId);
    }
}

bool sameOppositeLiteral(const LiteralRef& a, const LiteralRef& b) {
    return a.baseNetId >= 0 && a.baseNetId == b.baseNetId && a.inverted != b.inverted;
}

bool isPositiveQ(const LiteralRef& literal, int qNetId) {
    return literal.baseNetId == qNetId && !literal.inverted;
}

bool matchBranchPair(const Netlist& netlist,
                     const BranchRef& holdBranch,
                     const BranchRef& dataBranch,
                     bool branchSelectedWhenLiteralTrue,
                     int rootGateId,
                     int qNetId,
                     DffInputPattern& pattern) {
    for (int qIndex = 0; qIndex < 2; ++qIndex) {
        if (!isPositiveQ(holdBranch.literals[qIndex], qNetId)) {
            continue;
        }

        const LiteralRef& holdSelect = holdBranch.literals[1 - qIndex];
        if (holdSelect.baseNetId == qNetId) {
            continue;
        }

        for (int selectIndex = 0; selectIndex < 2; ++selectIndex) {
            const LiteralRef& dataSelect = dataBranch.literals[selectIndex];
            if (!sameOppositeLiteral(holdSelect, dataSelect)) {
                continue;
            }

            const LiteralRef& data = dataBranch.literals[1 - selectIndex];
            if (data.baseNetId == qNetId || !isActiveNet(netlist, data.baseNetId)) {
                continue;
            }

            const int holdValue = branchSelectedWhenLiteralTrue
                ? (holdSelect.inverted ? 0 : 1)
                : (holdSelect.inverted ? 1 : 0);

            pattern.kind = DffInputPatternKind::MuxHold;
            pattern.detectionMethod =
                SequentialPatternDetectionMethod::StructuralCanonical;
            pattern.enableNetId = holdSelect.baseNetId;
            pattern.dataNetId = data.baseNetId;
            pattern.dataBranchNetId = data.signalNetId;
            pattern.feedbackNetId = qNetId;
            pattern.enableNetName = netlist.getNet(pattern.enableNetId).name;
            pattern.dataNetName = netlist.getNet(pattern.dataNetId).name;
            pattern.dataBranchNetName = netlist.getNet(pattern.dataBranchNetId).name;
            pattern.feedbackNetName = netlist.getNet(qNetId).name;
            pattern.activeLevel = 1 - holdValue;
            pattern.holdLevel = holdValue;
            pattern.dataInverted = data.inverted;
            pattern.dataFunctionResolved = true;
            pattern.structuralMatch = true;
            pattern.confirmed = true;
            pattern.message = "Canonical feedback multiplexer detected.";

            appendUnique(pattern.evidenceGateIds, rootGateId);
            appendUnique(pattern.evidenceGateIds, holdBranch.gateId);
            appendUnique(pattern.evidenceGateIds, dataBranch.gateId);
            appendLiteralEvidence(pattern.evidenceGateIds, holdBranch.literals[qIndex]);
            appendLiteralEvidence(pattern.evidenceGateIds, holdSelect);
            appendLiteralEvidence(pattern.evidenceGateIds, dataSelect);
            appendLiteralEvidence(pattern.evidenceGateIds, data);
            for (int gateId : pattern.evidenceGateIds) {
                if (isActiveGate(netlist, gateId)) {
                    pattern.evidenceGateNames.push_back(netlist.getGate(gateId).instName);
                }
            }
            return true;
        }
    }
    return false;
}

bool detectCanonicalMux(const Netlist& netlist,
                        int dNetId,
                        int qNetId,
                        DffInputPattern& pattern) {
    std::vector<int> rootBufferEvidence;
    const int rootNetId = stripBuffers(netlist, dNetId, &rootBufferEvidence);
    if (!isActiveNet(netlist, rootNetId)) {
        return false;
    }

    const int rootGateId = netlist.getNet(rootNetId).driverGateId;
    if (!isActiveGate(netlist, rootGateId)) {
        return false;
    }
    const Gate& root = netlist.getGate(rootGateId);
    if (root.inputNetIds.size() != 2) {
        return false;
    }

    GateType branchType = GateType::UNKNOWN;
    bool branchSelectedWhenLiteralTrue = true;
    switch (root.type) {
    case GateType::OR:
        branchType = GateType::AND;
        break;
    case GateType::NAND:
        branchType = GateType::NAND;
        break;
    case GateType::AND:
        branchType = GateType::OR;
        branchSelectedWhenLiteralTrue = false;
        break;
    case GateType::NOR:
        branchType = GateType::NOR;
        branchSelectedWhenLiteralTrue = false;
        break;
    default:
        return false;
    }

    BranchRef first;
    BranchRef second;
    if (!loadBranch(netlist, root.inputNetIds[0], branchType, first) ||
        !loadBranch(netlist, root.inputNetIds[1], branchType, second)) {
        return false;
    }

    if (!matchBranchPair(netlist,
                         first,
                         second,
                         branchSelectedWhenLiteralTrue,
                         rootGateId,
                         qNetId,
                         pattern) &&
        !matchBranchPair(netlist,
                         second,
                         first,
                         branchSelectedWhenLiteralTrue,
                         rootGateId,
                         qNetId,
                         pattern)) {
        return false;
    }

    for (int gateId : rootBufferEvidence) {
        appendUnique(pattern.evidenceGateIds, gateId);
        if (isActiveGate(netlist, gateId)) {
            pattern.evidenceGateNames.push_back(netlist.getGate(gateId).instName);
        }
    }
    return true;
}

DffInputPattern makeAndCandidate(const Netlist& netlist, int dNetId) {
    DffInputPattern pattern;
    pattern.kind = DffInputPatternKind::DataGatingWithoutHoldFeedback;
    pattern.detectionMethod = SequentialPatternDetectionMethod::StructuralCanonical;
    pattern.semanticsPending = false;
    pattern.message =
        "Direct AND-gated D input found, but no same-DFF Q feedback is present; "
        "this is data gating, not an enable/hold pattern.";

    const LiteralRef normalizedRoot = normalizeLiteral(netlist, dNetId);
    const int rootNetId = normalizedRoot.baseNetId;
    if (!isActiveNet(netlist, rootNetId)) {
        return pattern;
    }
    const int rootGateId = netlist.getNet(rootNetId).driverGateId;
    if (!isActiveGate(netlist, rootGateId)) {
        return pattern;
    }
    const Gate& root = netlist.getGate(rootGateId);
    const bool isAndFunction =
        (!normalizedRoot.inverted && root.type == GateType::AND) ||
        (normalizedRoot.inverted && root.type == GateType::NAND);
    if (!isAndFunction || root.inputNetIds.size() < 2) {
        return pattern;
    }

    pattern.structuralMatch = true;
    appendUnique(pattern.evidenceGateIds, rootGateId);
    for (int gateId : normalizedRoot.evidenceGateIds) {
        appendUnique(pattern.evidenceGateIds, gateId);
    }
    for (int inputNetId : root.inputNetIds) {
        if (isActiveNet(netlist, inputNetId)) {
            pattern.candidateInputNetNames.push_back(netlist.getNet(inputNetId).name);
        }
    }
    for (int gateId : pattern.evidenceGateIds) {
        if (isActiveGate(netlist, gateId)) {
            pattern.evidenceGateNames.push_back(netlist.getGate(gateId).instName);
        }
    }
    return pattern;
}

void mergeSatEvidence(DffInputPattern& pattern,
                      const FunctionReport& hold,
                      const FunctionReport& load) {
    pattern.solverRan = hold.solverRan || load.solverRan;
    pattern.solverTimedOut = hold.solverTimedOut || load.solverTimedOut;
    pattern.solverUnknown = hold.solverUnknown || load.solverUnknown;
    pattern.holdFunctionallyProven = hold.ok && hold.equivalent;
    pattern.loadFunctionallyProven = load.ok && load.equivalent;

    if (pattern.solverTimedOut) {
        pattern.solverStatus = "TIMEOUT";
    } else if (pattern.solverUnknown) {
        pattern.solverStatus = "UNKNOWN";
    } else if (!hold.ok || !load.ok) {
        pattern.solverStatus = "UNSUPPORTED";
    } else if (pattern.holdFunctionallyProven && pattern.loadFunctionallyProven) {
        pattern.solverStatus = "PROVEN";
        pattern.detectionMethod =
            SequentialPatternDetectionMethod::StructuralCanonicalWithSat;
    } else {
        pattern.solverStatus = "FAILED";
        pattern.confirmed = false;
        pattern.message = "Canonical pattern failed conditional-equivalence verification.";
    }
}

const FunctionalPatternRoleBinding* findRoleBinding(
    const FunctionalPatternMatch& match,
    FunctionalPatternRole role) {
    for (const FunctionalPatternRoleBinding& binding : match.bindings) {
        if (binding.role == role) {
            return &binding;
        }
    }
    return nullptr;
}

DffInputPattern makeFunctionalMuxPattern(
    const Netlist& netlist,
    const FunctionalPatternMatch& match) {
    DffInputPattern pattern;
    pattern.kind = DffInputPatternKind::MuxHold;
    pattern.detectionMethod = SequentialPatternDetectionMethod::FunctionalCofactorSat;
    pattern.activeLevel = match.activeLevel;
    pattern.holdLevel = match.holdLevel;
    pattern.dataFunctionResolved = match.dataFunctionResolved;
    pattern.dataSearchAttempted = match.dataSearchAttempted;
    pattern.dataSearchComplete = match.dataSearchComplete;
    pattern.dataSearchTimedOut = match.dataSearchTimedOut;
    pattern.dataCandidateCount = match.dataCandidateCount;
    pattern.dataCandidatesExamined = match.dataCandidatesExamined;
    pattern.holdFunctionallyProven = match.proven;
    pattern.loadFunctionallyProven = match.dataFunctionResolved;
    pattern.confirmed = match.proven;
    pattern.solverRan = std::any_of(
        match.proofs.begin(),
        match.proofs.end(),
        [](const FunctionalPatternProofResult& proof) { return proof.solverRan; });
    pattern.solverTimedOut = std::any_of(
        match.proofs.begin(),
        match.proofs.end(),
        [](const FunctionalPatternProofResult& proof) { return proof.timedOut; });
    pattern.solverUnknown = std::any_of(
        match.proofs.begin(),
        match.proofs.end(),
        [](const FunctionalPatternProofResult& proof) { return proof.unknown; });

    if (const FunctionalPatternRoleBinding* binding =
            findRoleBinding(match, FunctionalPatternRole::Control)) {
        pattern.enableNetId = binding->netId;
        pattern.enableNetName = binding->netName;
    }
    if (const FunctionalPatternRoleBinding* binding =
            findRoleBinding(match, FunctionalPatternRole::Feedback)) {
        pattern.feedbackNetId = binding->netId;
        pattern.feedbackNetName = binding->netName;
    }
    if (const FunctionalPatternRoleBinding* binding =
            findRoleBinding(match, FunctionalPatternRole::Data)) {
        pattern.dataNetId = binding->netId;
        pattern.dataNetName = binding->netName;
        pattern.dataBranchNetId = binding->netId;
        pattern.dataBranchNetName = binding->netName;
        pattern.dataInverted = binding->inverted;
    }

    for (int gateId : match.evidenceGateIds) {
        appendUnique(pattern.evidenceGateIds, gateId);
        if (isActiveGate(netlist, gateId)) {
            pattern.evidenceGateNames.push_back(netlist.getGate(gateId).instName);
        }
    }
    pattern.solverStatus = pattern.dataFunctionResolved
        ? "PROVEN"
        : "HOLD_PROVEN_DATA_UNRESOLVED";
    pattern.message = match.message;
    return pattern;
}

} // namespace

Netlist::SequentialPatternReportSet Netlist::runSequentialPatternQuery(
    const SequentialPatternQuery& query) const {
    const Clock::time_point queryStart = Clock::now();
    SequentialPatternReportSet result;
    result.type = query.type;

    if (query.type != SequentialPatternQueryType::DffEnableHold) {
        result.status = "UNSUPPORTED_QUERY";
        result.message = "Unsupported sequential pattern query type.";
        result.complete = false;
        result.elapsedSeconds = elapsedSeconds(queryStart);
        return result;
    }
    if (query.enableFunctionalFallback &&
        (query.maxFunctionalCandidates == 0 ||
         query.maxFunctionalMatchesPerDff == 0 ||
         (query.resolveFunctionalDataNets &&
          query.maxFunctionalDataCandidatesPerMatch == 0) ||
         (query.enableFunctionalSimulationFilter &&
          (query.functionalSimulationPatternCount == 0 ||
           query.functionalSimulationPatternCount > 4096)) ||
         query.functionalPerDffTimeLimitSeconds <= 0.0 ||
         query.functionalTimeLimitSeconds <= 0.0)) {
        result.status = "INVALID_ARGUMENT";
        result.message = "Functional fallback requires positive candidate, match, and time limits.";
        result.complete = false;
        result.elapsedSeconds = elapsedSeconds(queryStart);
        return result;
    }

    std::vector<std::string> targets;
    if (query.dffName.empty()) {
        targets = getDffNames();
    } else {
        const int gateId = getGateId(query.dffName);
        if (!isDffGate(gateId) || isGateRemoved(gateId)) {
            result.status = "DFF_NOT_FOUND";
            result.message = "DFF not found: " + query.dffName;
            result.complete = false;
            result.elapsedSeconds = elapsedSeconds(queryStart);
            return result;
        }
        targets.push_back(query.dffName);
    }

    result.totalDffCount = targets.size();
    if (targets.empty()) {
        result.ok = true;
        result.status = "NO_DFF";
        result.message = "The design contains no active DFF instances.";
        result.elapsedSeconds = elapsedSeconds(queryStart);
        return result;
    }

    bool partial = false;
    FunctionalPatternEngine functionalEngine;
    std::optional<BitParallelSimulationResult> functionalSimulation;
    for (const std::string& dffName : targets) {
        DffInputPatternReport report;
        report.dffName = dffName;
        report.dffGateId = getGateId(dffName);
        report.dNetId = getDffInputNetId(report.dffGateId, "D");
        report.qNetId = getDffOutputNetId(report.dffGateId);
        ++result.analyzedDffCount;

        if (!isActiveNet(*this, report.dNetId) || !isActiveNet(*this, report.qNetId)) {
            report.status = "INVALID_DFF_PINS";
            report.message = "DFF is missing an active D or Q net.";
            partial = true;
            result.reports.push_back(std::move(report));
            continue;
        }

        report.dNetName = getNet(report.dNetId).name;
        report.qNetName = getNet(report.qNetId).name;

        DffInputPattern muxPattern;
        if (detectCanonicalMux(*this, report.dNetId, report.qNetId, muxPattern)) {
            report.qFeedbackObserved = true;

            if (query.verifyCanonicalMatchesWithSat) {
                const int holdValue = 1 - muxPattern.activeLevel;

                FunctionQuery holdQuery;
                holdQuery.type = FunctionQueryType::ConditionalEquivalence;
                holdQuery.netNameA = report.dNetName;
                holdQuery.netNameB = report.qNetName;
                holdQuery.conditionNetName = muxPattern.enableNetName;
                holdQuery.conditionValue = holdValue;

                FunctionQuery loadQuery = holdQuery;
                loadQuery.netNameB = muxPattern.dataBranchNetName;
                loadQuery.conditionValue = muxPattern.activeLevel;

                mergeSatEvidence(
                    muxPattern,
                    runFunctionQuery(holdQuery),
                    runFunctionQuery(loadQuery));
                if (!muxPattern.confirmed) {
                    partial = true;
                }
            }
            report.patterns.push_back(std::move(muxPattern));
        }

        const bool canonicalConfirmed = std::any_of(
            report.patterns.begin(),
            report.patterns.end(),
            [](const DffInputPattern& pattern) { return pattern.confirmed; });
        if (query.enableFunctionalFallback && !canonicalConfirmed) {
            report.functionalFallbackAttempted = true;
            if (query.enableFunctionalSimulationFilter && !functionalSimulation.has_value()) {
                const Clock::time_point simulationStart = Clock::now();
                functionalSimulation.emplace(simulateNetlistBitParallel(
                    *this,
                    query.functionalSimulationPatternCount));
                result.functionalSimulationPatternCount =
                    query.functionalSimulationPatternCount;
                result.functionalSimulationSeconds =
                    elapsedSeconds(simulationStart);
            }
            const double remaining =
                query.functionalTimeLimitSeconds - elapsedSeconds(queryStart);
            if (remaining <= 0.0) {
                report.functionalFallbackComplete = false;
                report.functionalFallbackTimedOut = true;
                result.timedOut = true;
                partial = true;
            } else {
                FunctionalPatternSearchOptions options;
                options.maxCandidates = query.maxFunctionalCandidates;
                options.maxMatches = query.maxFunctionalMatchesPerDff;
                options.findAllMatches = query.findAllFunctionalMatches;
                options.resolveDataNets = query.resolveFunctionalDataNets;
                options.maxDataCandidatesPerMatch =
                    query.maxFunctionalDataCandidatesPerMatch;
                options.timeLimitSeconds = std::min(
                    remaining,
                    query.functionalPerDffTimeLimitSeconds);
                FunctionalPatternContext context = buildSequentialPatternContext(
                    *this,
                    report.dffGateId,
                    report.dNetId,
                    report.qNetId);
                if (functionalSimulation.has_value()) {
                    context.simulation = &functionalSimulation.value();
                }
                const FunctionalPatternSearchResult search = functionalEngine.search(
                    FunctionalPatternKind::MuxHold,
                    *this,
                    context,
                    options);

                report.functionalFallbackComplete = search.complete;
                report.functionalFallbackTimedOut = search.timedOut;
                report.functionalCandidateLimitReached = search.candidateLimitReached;
                report.functionalCandidateCount = search.candidateCount;
                report.functionalSearchableCandidateCount =
                    search.searchableCandidateCount;
                report.functionalCandidatesExamined = search.candidatesExamined;
                report.functionalUnexaminedCandidateCount =
                    search.unexaminedCandidateCount;
                report.functionalInconclusiveCandidateCount =
                    search.inconclusiveCandidateCount;
                report.functionalSimulationCandidateCount =
                    search.simulationCandidateCount;
                report.functionalSimulationRejectedCandidateCount =
                    search.simulationRejectedCandidateCount;
                report.functionalSatCheckCount = search.satCheckCount;
                result.functionalCandidateCount += search.candidateCount;
                result.functionalSearchableCandidateCount +=
                    search.searchableCandidateCount;
                result.functionalCandidatesExamined += search.candidatesExamined;
                result.functionalUnexaminedCandidateCount +=
                    search.unexaminedCandidateCount;
                result.functionalInconclusiveCandidateCount +=
                    search.inconclusiveCandidateCount;
                result.functionalSimulationCandidateCount +=
                    search.simulationCandidateCount;
                result.functionalSimulationRejectedCandidateCount +=
                    search.simulationRejectedCandidateCount;
                result.functionalSatCheckCount += search.satCheckCount;
                result.functionalMatchCount += search.matches.size();
                result.timedOut = result.timedOut || search.timedOut;
                if (!search.complete || search.timedOut) {
                    partial = true;
                }
                if (!search.matches.empty()) {
                    report.qFeedbackObserved = true;
                }
                for (const FunctionalPatternMatch& match : search.matches) {
                    report.patterns.push_back(makeFunctionalMuxPattern(*this, match));
                }
            }
        }

        if (query.includeAndGatedCandidates && report.patterns.empty()) {
            DffInputPattern andCandidate = makeAndCandidate(*this, report.dNetId);
            if (andCandidate.structuralMatch) {
                report.patterns.push_back(std::move(andCandidate));
            }
        }

        report.matched = std::any_of(
            report.patterns.begin(),
            report.patterns.end(),
            [](const DffInputPattern& pattern) { return pattern.confirmed; });
        const bool hasCandidate = std::any_of(
            report.patterns.begin(),
            report.patterns.end(),
            [](const DffInputPattern& pattern) {
                return pattern.kind == DffInputPatternKind::MuxHold;
            });
        const bool hasDiagnostic = std::any_of(
            report.patterns.begin(),
            report.patterns.end(),
            [](const DffInputPattern& pattern) {
                return pattern.kind ==
                    DffInputPatternKind::DataGatingWithoutHoldFeedback;
            });
        if (report.matched) {
            ++result.matchedDffCount;
        }
        if (hasCandidate) {
            ++result.candidateDffCount;
        }

        report.ok = !report.functionalFallbackTimedOut;
        report.status = !report.functionalFallbackComplete
            ? "PARTIAL"
            : (report.matched
                ? "ENABLE_HOLD_FOUND"
                : (hasCandidate
                    ? "CANDIDATE_FOUND"
                    : (hasDiagnostic
                        ? "DATA_GATING_WITHOUT_HOLD_FEEDBACK"
                        : "NO_PATTERN")));
        report.message = !report.functionalFallbackComplete
            ? "Functional fallback was incomplete; returned confirmed matches remain valid, but absence is inconclusive."
            : (report.matched
                ? "Confirmed DFF enable/hold pattern found."
                : (hasCandidate
                    ? "Unconfirmed enable/hold candidates were found."
                    : (hasDiagnostic
                        ? "D input has data gating but no same-DFF Q feedback, so it is not an enable/hold pattern."
                        : "No supported DFF enable/hold pattern was found.")));
        result.reports.push_back(std::move(report));
    }

    result.ok = !result.timedOut;
    result.exists = result.matchedDffCount > 0;
    result.complete = !partial;
    result.status = partial ? "PARTIAL" : "OK";
    result.message = partial
        ? "Sequential pattern analysis completed with inconclusive or invalid records."
        : "Sequential pattern analysis completed.";
    result.elapsedSeconds = elapsedSeconds(queryStart);
    return result;
}
