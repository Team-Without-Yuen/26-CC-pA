#include "include/core/FunctionalPatternEngine.h"

#include "include/core/BitParallelSimulation.h"
#include "include/core/Netlist.h"
#include "include/core/SatTime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace {

using Clock = std::chrono::steady_clock;

double elapsedSeconds(const Clock::time_point& start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::unordered_set<int> collectFeedbackDependentNets(
    const Netlist& netlist,
    int feedbackNetId) {
    std::unordered_set<int> dependent;
    if (!netlist.isValidNetId(feedbackNetId)) {
        return dependent;
    }
    dependent.insert(feedbackNetId);
    const ConeResult fanout =
        netlist.getTransitiveFanoutCone(netlist.getNet(feedbackNetId).name);
    dependent.insert(fanout.netIds.begin(), fanout.netIds.end());
    return dependent;
}

std::vector<int> computeDistanceToTarget(
    const Netlist& netlist,
    const FunctionalPatternContext& context) {
    std::vector<int> distance(netlist.getNetCount(), -1);
    if (!netlist.isValidNetId(context.targetNetId)) {
        return distance;
    }

    std::queue<int> pending;
    distance[context.targetNetId] = 0;
    pending.push(context.targetNetId);
    while (!pending.empty()) {
        const int netId = pending.front();
        pending.pop();
        const int driverId = netlist.getNet(netId).driverGateId;
        if (!netlist.isValidGateId(driverId) || netlist.isGateRemoved(driverId)) {
            continue;
        }
        const Gate& driver = netlist.getGate(driverId);
        if (driver.type == GateType::DFF) {
            continue;
        }
        for (int inputNetId : driver.inputNetIds) {
            if (!netlist.isValidNetId(inputNetId) ||
                netlist.getNet(inputNetId).isRemoved) {
                continue;
            }
            const int nextDistance = distance[netId] + 1;
            if (distance[inputNetId] < 0 || nextDistance < distance[inputNetId]) {
                distance[inputNetId] = nextDistance;
                pending.push(inputNetId);
            }
        }
    }
    return distance;
}

std::vector<int> collectControlCandidates(
    const Netlist& netlist,
    const FunctionalPatternContext& context,
    const std::unordered_set<int>& feedbackDependentNets) {
    const std::vector<int> distanceToTarget =
        computeDistanceToTarget(netlist, context);
    std::vector<int> candidates;
    for (int netId : context.coneNetIds) {
        if (!netlist.isValidNetId(netId) ||
            netId == context.targetNetId ||
            netId == context.feedbackNetId) {
            continue;
        }
        const Net& net = netlist.getNet(netId);
        if (net.isRemoved || net.isConst ||
            feedbackDependentNets.count(netId) != 0) {
            continue;
        }
        candidates.push_back(netId);
    }

    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [&netlist, &distanceToTarget](int a, int b) {
            const Net& netA = netlist.getNet(a);
            const Net& netB = netlist.getNet(b);
            if (netA.isPI != netB.isPI) {
                return netA.isPI;
            }
            const bool leafA = netA.driverGateId < 0;
            const bool leafB = netB.driverGateId < 0;
            if (leafA != leafB) {
                return leafA;
            }
            const int distanceA = distanceToTarget[a] >= 0
                ? distanceToTarget[a]
                : std::numeric_limits<int>::max();
            const int distanceB = distanceToTarget[b] >= 0
                ? distanceToTarget[b]
                : std::numeric_limits<int>::max();
            if (distanceA != distanceB) {
                return distanceA < distanceB;
            }
            return a < b;
        });
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

std::vector<int> collectDataCandidates(
    const Netlist& netlist,
    const FunctionalPatternContext& context,
    const std::unordered_set<int>& feedbackDependentNets) {
    std::vector<int> candidates;
    for (int netId : context.coneNetIds) {
        if (!netlist.isValidNetId(netId) ||
            netId == context.targetNetId ||
            netId == context.feedbackNetId) {
            continue;
        }
        const Net& net = netlist.getNet(netId);
        if (net.isRemoved || net.isConst ||
            feedbackDependentNets.count(netId) != 0) {
            continue;
        }
        candidates.push_back(netId);
    }
    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [&netlist](int a, int b) {
            const Net& netA = netlist.getNet(a);
            const Net& netB = netlist.getNet(b);
            if (netA.isPI != netB.isPI) {
                return netA.isPI;
            }
            const bool leafA = netA.driverGateId < 0;
            const bool leafB = netB.driverGateId < 0;
            if (leafA != leafB) {
                return leafA;
            }
            return a < b;
        });
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

FunctionReport queryReachableLevels(
    const Netlist& netlist,
    int netId,
    double timeLimitSeconds) {
    FunctionQuery query;
    query.type = FunctionQueryType::TruthStatus;
    query.netNameA = netlist.getNet(netId).name;
    query.timeLimitSeconds = std::max(0.001, timeLimitSeconds);
    return netlist.runFunctionQuery(query);
}

FunctionalPatternProofResult proveConditionalEquivalence(
    const Netlist& netlist,
    int lhsNetId,
    int rhsNetId,
    int conditionNetId,
    int conditionValue,
    double timeLimitSeconds) {
    FunctionalPatternProofResult proof;
    proof.lhsNetId = lhsNetId;
    proof.rhsNetId = rhsNetId;
    proof.conditionNetId = conditionNetId;
    proof.conditionValue = conditionValue;

    if (!netlist.isValidNetId(lhsNetId) ||
        !netlist.isValidNetId(rhsNetId) ||
        !netlist.isValidNetId(conditionNetId)) {
        proof.unsupported = true;
        proof.solverStatus = "INVALID_NET";
        return proof;
    }

    FunctionQuery query;
    query.type = FunctionQueryType::ConditionalEquivalence;
    query.netNameA = netlist.getNet(lhsNetId).name;
    query.netNameB = netlist.getNet(rhsNetId).name;
    query.conditionNetName = netlist.getNet(conditionNetId).name;
    query.conditionValue = conditionValue;
    query.timeLimitSeconds = std::max(0.001, timeLimitSeconds);
    const FunctionReport report = netlist.runFunctionQuery(query);

    proof.solverRan = report.solverRan;
    proof.proven = report.ok && report.equivalent;
    proof.timedOut = report.solverTimedOut;
    proof.unknown = report.solverUnknown;
    proof.unsupported = report.unsupported;
    proof.solverStatus = report.solverStatus;
    return proof;
}

double remainingSeconds(
    const Clock::time_point& start,
    double limitSeconds) {
    return std::max(0.0, limitSeconds - elapsedSeconds(start));
}

bool isSupportedCnfGate(const Netlist& netlist, const Gate& gate) {
    if (netlist.isGateRemoved(gate.id) ||
        !netlist.isValidNetId(gate.outputNetId) ||
        netlist.getNet(gate.outputNetId).isRemoved) {
        return false;
    }
    if (std::any_of(
            gate.inputNetIds.begin(),
            gate.inputNetIds.end(),
            [&netlist](int netId) {
                return !netlist.isValidNetId(netId) ||
                    netlist.getNet(netId).isRemoved;
            })) {
        return false;
    }

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
    case GateType::DFF:
    case GateType::UNKNOWN:
        return false;
    }
    return false;
}

class DffCofactorSatSession {
public:
    DffCofactorSatSession(
        const Netlist& netlist,
        const FunctionalPatternContext& context)
        : netlist(netlist), context(context) {}

    FunctionalPatternProofResult proveConditionalEquivalence(
        int lhsNetId,
        int rhsNetId,
        int conditionNetId,
        int conditionValue,
        double timeLimitSeconds) {
        // A one-shot query is cheaper when the first ranked candidate matches.
        // Build the reusable cone solver only after the search needs another proof.
        if (proofRequestCount++ == 0) {
            return ::proveConditionalEquivalence(
                netlist,
                lhsNetId,
                rhsNetId,
                conditionNetId,
                conditionValue,
                timeLimitSeconds);
        }
        if (!ensureBuilt()) {
            return ::proveConditionalEquivalence(
                netlist,
                lhsNetId,
                rhsNetId,
                conditionNetId,
                conditionValue,
                timeLimitSeconds);
        }

        FunctionalPatternProofResult proof;
        proof.lhsNetId = lhsNetId;
        proof.rhsNetId = rhsNetId;
        proof.conditionNetId = conditionNetId;
        proof.conditionValue = conditionValue;
        if (!netlist.isValidNetId(lhsNetId) ||
            !netlist.isValidNetId(rhsNetId) ||
            !netlist.isValidNetId(conditionNetId) ||
            (conditionValue != 0 && conditionValue != 1)) {
            proof.unsupported = true;
            proof.solverStatus = "INVALID_NET";
            return proof;
        }
        if (timeLimitSeconds <= 0.0) {
            proof.timedOut = true;
            proof.unknown = true;
            proof.solverStatus = "TIMEOUT";
            return proof;
        }

        const int diffLiteral = getOrCreateDiffLiteral(lhsNetId, rhsNetId);
        solver->assume(
            conditionValue == 1 ? conditionNetId + 1 : -(conditionNetId + 1));
        solver->assume(diffLiteral);
        TimeLimitTerminator terminator(timeLimitSeconds);
        solver->connect_terminator(&terminator);
        const int solveResult = solver->solve();
        solver->disconnect_terminator();

        proof.solverRan = true;
        if (solveResult == 20) {
            proof.proven = true;
            proof.solverStatus = "UNSAT";
        } else if (solveResult == 10) {
            proof.solverStatus = "SAT";
        } else {
            proof.unknown = true;
            proof.timedOut = terminator.wasTerminated();
            proof.solverStatus = proof.timedOut ? "TIMEOUT" : "UNKNOWN";
        }
        return proof;
    }

private:
    bool ensureBuilt() {
        if (buildAttempted) {
            return ready;
        }
        buildAttempted = true;

        solver = std::make_unique<CaDiCaL::Solver>();
        solver->set("factor", 0);
        nextVariable = static_cast<int>(netlist.getNetCount());
        solver->resize(nextVariable);

        std::unordered_set<int> encodedConstants;
        auto encodeConstant = [this, &encodedConstants](int netId) {
            if (!netlist.isValidNetId(netId) ||
                netlist.getNet(netId).isRemoved) {
                return false;
            }
            if (!encodedConstants.insert(netId).second) {
                return true;
            }
            const Net& net = netlist.getNet(netId);
            if (!net.isConst) {
                return true;
            }
            if (net.constVal != 0 && net.constVal != 1) {
                return false;
            }
            solver->add(net.constVal == 1 ? netId + 1 : -(netId + 1));
            solver->add(0);
            return true;
        };

        for (int netId : context.coneNetIds) {
            if (!encodeConstant(netId)) {
                solver.reset();
                return false;
            }
        }
        if (!encodeConstant(context.targetNetId) ||
            !encodeConstant(context.feedbackNetId)) {
            solver.reset();
            return false;
        }

        std::unordered_set<int> encodedGates;
        for (int gateId : context.coneGateIds) {
            if (!encodedGates.insert(gateId).second) {
                continue;
            }
            if (!netlist.isValidGateId(gateId) ||
                netlist.isGateRemoved(gateId)) {
                solver.reset();
                return false;
            }
            const Gate& gate = netlist.getGate(gateId);
            if (gate.type == GateType::DFF) {
                continue;
            }
            if (!isSupportedCnfGate(netlist, gate)) {
                solver.reset();
                return false;
            }
            netlist.encodeGateToCNF(*solver, gate);
        }

        ready = true;
        return true;
    }

    int getOrCreateDiffLiteral(int lhsNetId, int rhsNetId) {
        const int low = std::min(lhsNetId, rhsNetId);
        const int high = std::max(lhsNetId, rhsNetId);
        const std::uint64_t key =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(low)) << 32) |
            static_cast<std::uint32_t>(high);
        const auto found = diffLiterals.find(key);
        if (found != diffLiterals.end()) {
            return found->second;
        }

        const int diffLiteral = ++nextVariable;
        solver->resize(nextVariable);
        const int lhsLiteral = lhsNetId + 1;
        const int rhsLiteral = rhsNetId + 1;
        solver->add(-lhsLiteral); solver->add(-rhsLiteral); solver->add(-diffLiteral); solver->add(0);
        solver->add(lhsLiteral); solver->add(rhsLiteral); solver->add(-diffLiteral); solver->add(0);
        solver->add(lhsLiteral); solver->add(-rhsLiteral); solver->add(diffLiteral); solver->add(0);
        solver->add(-lhsLiteral); solver->add(rhsLiteral); solver->add(diffLiteral); solver->add(0);
        diffLiterals.emplace(key, diffLiteral);
        return diffLiteral;
    }

    const Netlist& netlist;
    const FunctionalPatternContext& context;
    bool buildAttempted = false;
    bool ready = false;
    size_t proofRequestCount = 0;
    int nextVariable = 0;
    std::unique_ptr<CaDiCaL::Solver> solver;
    std::unordered_map<std::uint64_t, int> diffLiterals;
};

enum class HoldSimulationHint {
    Unavailable,
    Reject,
    HoldZeroOnly,
    HoldOneOnly,
    BothPossible
};

HoldSimulationHint classifyHoldBySimulation(
    const FunctionalPatternContext& context,
    int controlNetId) {
    const BitParallelSimulationResult* simulation = context.simulation;
    if (simulation == nullptr ||
        context.targetNetId < 0 || context.feedbackNetId < 0 || controlNetId < 0 ||
        static_cast<size_t>(context.targetNetId) >= simulation->known.size() ||
        static_cast<size_t>(context.feedbackNetId) >= simulation->known.size() ||
        static_cast<size_t>(controlNetId) >= simulation->known.size() ||
        !simulation->known[context.targetNetId] ||
        !simulation->known[context.feedbackNetId] ||
        !simulation->known[controlNetId]) {
        return HoldSimulationHint::Unavailable;
    }

    const BitParallelSimulationSignature& target =
        simulation->signatures[context.targetNetId];
    const BitParallelSimulationSignature& feedback =
        simulation->signatures[context.feedbackNetId];
    const BitParallelSimulationSignature& control =
        simulation->signatures[controlNetId];
    if (target.empty() || target.size() != feedback.size() ||
        target.size() != control.size()) {
        return HoldSimulationHint::Unavailable;
    }

    bool seenZero = false;
    bool seenOne = false;
    bool mismatchAtZero = false;
    bool mismatchAtOne = false;
    for (size_t word = 0; word < target.size(); ++word) {
        const std::uint64_t validMask = word + 1 == target.size()
            ? simulation->lastWordMask
            : ~std::uint64_t{0};
        const std::uint64_t controlWord = control[word] & validMask;
        const std::uint64_t mismatch =
            (target[word] ^ feedback[word]) & validMask;
        seenOne = seenOne || controlWord != 0;
        seenZero = seenZero || ((~controlWord) & validMask) != 0;
        mismatchAtOne = mismatchAtOne || (mismatch & controlWord) != 0;
        mismatchAtZero = mismatchAtZero ||
            (mismatch & (~controlWord) & validMask) != 0;
    }
    if (!seenZero || !seenOne) {
        return HoldSimulationHint::Unavailable;
    }

    const bool holdZeroPossible = !mismatchAtZero;
    const bool holdOnePossible = !mismatchAtOne;
    if (!holdZeroPossible && !holdOnePossible) {
        return HoldSimulationHint::Reject;
    }
    if (holdZeroPossible && !holdOnePossible) {
        return HoldSimulationHint::HoldZeroOnly;
    }
    if (!holdZeroPossible && holdOnePossible) {
        return HoldSimulationHint::HoldOneOnly;
    }
    return HoldSimulationHint::BothPossible;
}

struct RankedControlCandidate {
    int netId = -1;
    HoldSimulationHint simulationHint = HoldSimulationHint::Unavailable;
};

int simulationHintPriority(HoldSimulationHint hint) {
    switch (hint) {
    case HoldSimulationHint::HoldZeroOnly:
    case HoldSimulationHint::HoldOneOnly:
        return 0;
    case HoldSimulationHint::BothPossible:
        return 1;
    case HoldSimulationHint::Unavailable:
        return 2;
    case HoldSimulationHint::Reject:
        return 3;
    }
    return 3;
}

std::vector<RankedControlCandidate> rankControlCandidatesBySimulation(
    const FunctionalPatternContext& context,
    const std::vector<int>& structurallyRankedCandidates) {
    std::vector<RankedControlCandidate> ranked;
    ranked.reserve(structurallyRankedCandidates.size());
    for (int netId : structurallyRankedCandidates) {
        ranked.push_back({netId, classifyHoldBySimulation(context, netId)});
    }
    std::stable_sort(
        ranked.begin(),
        ranked.end(),
        [](const RankedControlCandidate& a, const RankedControlCandidate& b) {
            return simulationHintPriority(a.simulationHint) <
                simulationHintPriority(b.simulationHint);
        });
    return ranked;
}

class MuxHoldCofactorMatcher final : public FunctionalPatternMatcher {
public:
    FunctionalPatternKind kind() const override {
        return FunctionalPatternKind::MuxHold;
    }

    FunctionalPatternSearchResult search(
        const Netlist& netlist,
        const FunctionalPatternContext& context,
        const FunctionalPatternSearchOptions& options) const override {
        FunctionalPatternSearchResult result;
        const Clock::time_point start = Clock::now();

        if (!netlist.isValidNetId(context.targetNetId) ||
            !netlist.isValidNetId(context.feedbackNetId) ||
            options.maxCandidates == 0 ||
            options.maxMatches == 0 ||
            (options.resolveDataNets && options.maxDataCandidatesPerMatch == 0) ||
            options.timeLimitSeconds <= 0.0) {
            result.status = "INVALID_ARGUMENT";
            result.message = "Functional MUX-hold search requires valid D/Q nets and positive limits.";
            result.complete = false;
            return result;
        }

        const std::unordered_set<int> feedbackDependentNets =
            collectFeedbackDependentNets(netlist, context.feedbackNetId);
        const std::vector<int> structurallyRankedCandidates =
            collectControlCandidates(netlist, context, feedbackDependentNets);
        const std::vector<RankedControlCandidate> allCandidates =
            rankControlCandidatesBySimulation(context, structurallyRankedCandidates);
        // Concrete counterexamples at both control levels are a complete
        // negative result for MUX-hold and therefore do not consume SAT quota.
        std::vector<RankedControlCandidate> searchableCandidates;
        searchableCandidates.reserve(allCandidates.size());
        const std::vector<int> dataCandidates =
            collectDataCandidates(netlist, context, feedbackDependentNets);
        result.candidateCount = allCandidates.size();
        for (const RankedControlCandidate& candidate : allCandidates) {
            if (candidate.simulationHint != HoldSimulationHint::Unavailable) {
                ++result.simulationCandidateCount;
            }
            if (candidate.simulationHint == HoldSimulationHint::Reject) {
                ++result.simulationRejectedCandidateCount;
                continue;
            }
            searchableCandidates.push_back(candidate);
        }
        result.searchableCandidateCount = searchableCandidates.size();
        const size_t candidateLimit =
            std::min(options.maxCandidates, result.searchableCandidateCount);
        result.candidateLimitReached =
            options.findAllMatches && candidateLimit < searchableCandidates.size();
        result.complete = !result.candidateLimitReached;
        DffCofactorSatSession satSession(netlist, context);

        for (size_t index = 0; index < candidateLimit; ++index) {
            if (remainingSeconds(start, options.timeLimitSeconds) <= 0.0) {
                result.timedOut = true;
                result.complete = false;
                break;
            }

            const int controlNetId = searchableCandidates[index].netId;
            ++result.candidatesExamined;
            bool candidateInconclusive = false;
            const auto markCandidateInconclusive = [&]() {
                if (!candidateInconclusive) {
                    candidateInconclusive = true;
                    ++result.inconclusiveCandidateCount;
                }
            };
            const HoldSimulationHint simulationHint =
                searchableCandidates[index].simulationHint;

            if (simulationHint == HoldSimulationHint::Unavailable) {
                const FunctionReport reachability = queryReachableLevels(
                    netlist,
                    controlNetId,
                    remainingSeconds(start, options.timeLimitSeconds));
                if (reachability.solverRan) {
                    result.satCheckCount += 2;
                }
                if (!reachability.ok) {
                    markCandidateInconclusive();
                    result.timedOut = result.timedOut || reachability.solverTimedOut;
                    result.complete = false;
                    if (result.timedOut) {
                        break;
                    }
                    continue;
                }
                if (!reachability.canBeZero || !reachability.canBeOne) {
                    continue;
                }
            }

            std::vector<int> holdLevels;
            if (simulationHint == HoldSimulationHint::HoldZeroOnly) {
                holdLevels.push_back(0);
            } else if (simulationHint == HoldSimulationHint::HoldOneOnly) {
                holdLevels.push_back(1);
            } else {
                holdLevels = {0, 1};
            }
            std::array<FunctionalPatternProofResult, 2> holdProofs;
            std::array<bool, 2> proofAttempted = {false, false};
            bool inconclusiveProof = false;
            for (int holdLevel : holdLevels) {
                const double remaining =
                    remainingSeconds(start, options.timeLimitSeconds);
                if (remaining <= 0.0) {
                    markCandidateInconclusive();
                    result.timedOut = true;
                    result.complete = false;
                    break;
                }
                FunctionalPatternProofResult proof =
                    satSession.proveConditionalEquivalence(
                        context.targetNetId,
                        context.feedbackNetId,
                        controlNetId,
                        holdLevel,
                        remaining);
                if (proof.solverRan) {
                    ++result.satCheckCount;
                }
                if (proof.timedOut || proof.unknown || proof.unsupported) {
                    markCandidateInconclusive();
                    result.timedOut = result.timedOut || proof.timedOut;
                    result.complete = false;
                    inconclusiveProof = true;
                }
                proofAttempted[static_cast<size_t>(holdLevel)] = true;
                holdProofs[static_cast<size_t>(holdLevel)] = std::move(proof);
            }
            if (result.timedOut) {
                break;
            }
            if (inconclusiveProof) {
                continue;
            }

            const bool holdAtZero = proofAttempted[0] && holdProofs[0].proven;
            const bool holdAtOne = proofAttempted[1] && holdProofs[1].proven;
            if (holdAtZero == holdAtOne) {
                continue;
            }

            FunctionalPatternMatch match;
            match.holdLevel = holdAtZero ? 0 : 1;
            match.activeLevel = 1 - match.holdLevel;
            match.proven = true;
            match.proofs.push_back(
                holdProofs[static_cast<size_t>(match.holdLevel)]);
            match.bindings.push_back({
                FunctionalPatternRole::Target,
                context.targetNetId,
                netlist.getNet(context.targetNetId).name,
                false});
            match.bindings.push_back({
                FunctionalPatternRole::Feedback,
                context.feedbackNetId,
                netlist.getNet(context.feedbackNetId).name,
                false});
            match.bindings.push_back({
                FunctionalPatternRole::Control,
                controlNetId,
                netlist.getNet(controlNetId).name,
                false});

            const int targetDriver =
                netlist.getNet(context.targetNetId).driverGateId;
            if (netlist.isValidGateId(targetDriver) &&
                !netlist.isGateRemoved(targetDriver)) {
                match.evidenceGateIds.push_back(targetDriver);
            }
            const int controlDriver = netlist.getNet(controlNetId).driverGateId;
            if (netlist.isValidGateId(controlDriver) &&
                !netlist.isGateRemoved(controlDriver)) {
                match.evidenceGateIds.push_back(controlDriver);
            }

            if (options.resolveDataNets) {
                match.dataSearchAttempted = true;
                match.dataCandidateCount = dataCandidates.size() -
                    (std::find(dataCandidates.begin(), dataCandidates.end(), controlNetId) !=
                        dataCandidates.end() ? 1u : 0u);
                const size_t dataLimit = std::min(
                    options.maxDataCandidatesPerMatch,
                    match.dataCandidateCount);
                for (int dataNetId : dataCandidates) {
                    if (dataNetId == controlNetId) {
                        continue;
                    }
                    if (match.dataCandidatesExamined >= dataLimit) {
                        match.dataSearchComplete = false;
                        break;
                    }
                    const double remaining =
                        remainingSeconds(start, options.timeLimitSeconds);
                    if (remaining <= 0.0) {
                        markCandidateInconclusive();
                        result.timedOut = true;
                        result.complete = false;
                        match.complete = false;
                        match.dataSearchComplete = false;
                        match.dataSearchTimedOut = true;
                        break;
                    }
                    ++match.dataCandidatesExamined;
                    FunctionalPatternProofResult dataProof =
                        satSession.proveConditionalEquivalence(
                            context.targetNetId,
                            dataNetId,
                            controlNetId,
                            match.activeLevel,
                            remaining);
                    if (dataProof.solverRan) {
                        ++result.satCheckCount;
                    }
                    if (dataProof.timedOut || dataProof.unknown || dataProof.unsupported) {
                        markCandidateInconclusive();
                        result.timedOut = result.timedOut || dataProof.timedOut;
                        result.complete = false;
                        match.complete = false;
                        match.dataSearchComplete = false;
                        match.dataSearchTimedOut = dataProof.timedOut;
                        break;
                    }
                    if (dataProof.proven) {
                        match.dataFunctionResolved = true;
                        match.proofs.push_back(std::move(dataProof));
                        match.bindings.push_back({
                            FunctionalPatternRole::Data,
                            dataNetId,
                            netlist.getNet(dataNetId).name,
                            false});
                        break;
                    }
                }
                if (!match.dataFunctionResolved &&
                    match.dataCandidatesExamined < match.dataCandidateCount) {
                    match.dataSearchComplete = false;
                }
            } else {
                match.dataSearchComplete = false;
            }

            match.message = match.dataFunctionResolved
                ? "MUX hold and load cofactors were proven by SAT."
                : (!match.dataSearchAttempted
                    ? "MUX hold cofactor was proven by SAT; named-data resolution was not requested."
                    : (match.dataSearchComplete
                        ? "MUX hold cofactor was proven by SAT; the active data function has no exact named-net match."
                        : "MUX hold cofactor was proven by SAT; named-data resolution was incomplete."));
            result.matches.push_back(std::move(match));
            if (!options.findAllMatches) {
                // A proven witness completely answers an existence query even
                // if an earlier candidate was inconclusive.
                result.complete = !result.timedOut;
                result.candidateLimitReached = false;
                break;
            }
            if (result.matches.size() >= options.maxMatches) {
                if (index + 1 < candidateLimit) {
                    result.complete = false;
                }
                break;
            }
        }

        result.unexaminedCandidateCount =
            result.searchableCandidateCount > result.candidatesExamined
                ? result.searchableCandidateCount - result.candidatesExamined
                : 0;

        if (!options.findAllMatches && result.matches.empty() &&
            !result.timedOut && candidateLimit < result.searchableCandidateCount &&
            result.candidatesExamined >= candidateLimit) {
            result.candidateLimitReached = true;
            result.complete = false;
        }

        if (options.findAllMatches) {
            result.complete = result.complete &&
                !result.timedOut &&
                result.unexaminedCandidateCount == 0 &&
                result.inconclusiveCandidateCount == 0;
        } else if (!result.matches.empty()) {
            result.complete = !result.timedOut;
            result.candidateLimitReached = false;
        } else {
            result.complete = result.complete &&
                !result.timedOut &&
                result.unexaminedCandidateCount == 0 &&
                result.inconclusiveCandidateCount == 0;
        }

        result.elapsedSeconds = elapsedSeconds(start);
        result.ok = !result.timedOut;
        if (result.timedOut) {
            result.status = "TIMEOUT";
            result.message = "Functional MUX-hold search reached its time limit.";
        } else if (!result.complete) {
            result.status = "PARTIAL";
            result.message = "Functional MUX-hold search has unexamined or inconclusive searchable candidates.";
        } else if (result.matches.empty()) {
            result.status = "NO_MATCH";
            result.message = "No functional MUX-hold decomposition was proven.";
        } else {
            result.status = "OK";
            result.message = "Functional MUX-hold search completed.";
        }
        return result;
    }
};

} // namespace

FunctionalPatternEngine::FunctionalPatternEngine() {
    matchers.push_back(std::make_unique<MuxHoldCofactorMatcher>());
}

FunctionalPatternSearchResult FunctionalPatternEngine::search(
    FunctionalPatternKind kind,
    const Netlist& netlist,
    const FunctionalPatternContext& context,
    const FunctionalPatternSearchOptions& options) const {
    for (const std::unique_ptr<FunctionalPatternMatcher>& matcher : matchers) {
        if (matcher->kind() == kind) {
            return matcher->search(netlist, context, options);
        }
    }
    FunctionalPatternSearchResult result;
    result.status = "UNSUPPORTED_PATTERN";
    result.message = "No matcher is registered for the requested functional pattern.";
    result.complete = false;
    return result;
}

FunctionalPatternContext buildSequentialPatternContext(
    const Netlist& netlist,
    int dffGateId,
    int dNetId,
    int qNetId) {
    FunctionalPatternContext context;
    context.targetGateId = dffGateId;
    context.targetNetId = dNetId;
    context.feedbackNetId = qNetId;
    if (!netlist.isValidNetId(dNetId)) {
        return context;
    }
    const ConeResult cone =
        netlist.getTransitiveFaninCone(netlist.getNet(dNetId).name);
    context.coneNetIds = netlist.getConeNetIds(cone);
    context.coneGateIds = netlist.getConeGateIds(cone);
    return context;
}
