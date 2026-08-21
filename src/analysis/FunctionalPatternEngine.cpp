#include "include/core/FunctionalPatternEngine.h"

#include "include/core/Netlist.h"
#include "include/core/RequestTimeBudget.h"
#include "include/SATEngine/Primitives.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kOptionalRoleMappingBudgetSeconds = 0.02;

double elapsedSeconds(const Clock::time_point& start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

double remainingSeconds(
    const Clock::time_point& start,
    double localLimitSeconds,
    const request_time_budget::RequestDeadline& deadline) {
    return std::max(
        0.0,
        std::min(
            localLimitSeconds - elapsedSeconds(start),
            deadline.remainingSeconds()));
}

bool isActiveNet(const Netlist& netlist, int netId) {
    return netlist.isValidNetId(netId) && !netlist.getNet(netId).isRemoved;
}

void appendUnique(std::vector<int>& values, int value) {
    if (value >= 0 && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

size_t expectedOperandCount(FunctionalPatternKind kind) {
    switch (kind) {
    case FunctionalPatternKind::Buffer:
    case FunctionalPatternKind::Inverter:
        return 1;
    case FunctionalPatternKind::And:
    case FunctionalPatternKind::Nand:
    case FunctionalPatternKind::Or:
    case FunctionalPatternKind::Nor:
    case FunctionalPatternKind::Xor:
    case FunctionalPatternKind::Xnor:
        return 2;
    case FunctionalPatternKind::Mux:
    case FunctionalPatternKind::MuxHold:
        return 3;
    }
    return 0;
}

std::optional<eqeng::SigRef> buildPatternFunction(
    eqeng::Primitives& primitives,
    FunctionalPatternKind kind,
    const std::vector<eqeng::SigRef>& operands) {
    switch (kind) {
    case FunctionalPatternKind::Buffer:
        return operands[0];
    case FunctionalPatternKind::Inverter:
        return !operands[0];
    case FunctionalPatternKind::And:
        return primitives.make_and(operands[0], operands[1]);
    case FunctionalPatternKind::Nand:
        return !primitives.make_and(operands[0], operands[1]);
    case FunctionalPatternKind::Or:
        return primitives.make_or(operands[0], operands[1]);
    case FunctionalPatternKind::Nor:
        return !primitives.make_or(operands[0], operands[1]);
    case FunctionalPatternKind::Xor:
        return primitives.make_xor(operands[0], operands[1]);
    case FunctionalPatternKind::Xnor:
        return !primitives.make_xor(operands[0], operands[1]);
    case FunctionalPatternKind::Mux:
    case FunctionalPatternKind::MuxHold:
        return primitives.make_mux(operands[0], operands[1], operands[2]);
    }
    return std::nullopt;
}

FunctionalPatternProofStatus proofStatus(eqeng::EquivResult result) {
    switch (result) {
    case eqeng::EquivResult::Equal:
        return FunctionalPatternProofStatus::ProvenMatch;
    case eqeng::EquivResult::NotEqual:
        return FunctionalPatternProofStatus::ProvenNonMatch;
    case eqeng::EquivResult::Unknown:
        return FunctionalPatternProofStatus::Unknown;
    }
    return FunctionalPatternProofStatus::Unknown;
}

FunctionalPatternProofResult makeBooleanProof(
    const std::string& description,
    eqeng::EquivResult result,
    bool solverRan,
    bool timedOut) {
    FunctionalPatternProofResult proof;
    proof.kind = FunctionalPatternProofKind::EquivalentUnderCondition;
    proof.description = description;
    proof.solverRan = solverRan;
    proof.proven = result == eqeng::EquivResult::Equal;
    proof.timedOut = timedOut;
    proof.unknown = result == eqeng::EquivResult::Unknown;
    proof.solverStatus = timedOut
        ? "TIMEOUT"
        : (result == eqeng::EquivResult::Equal
            ? "UNSAT"
            : (result == eqeng::EquivResult::NotEqual ? "SAT" : "UNKNOWN"));
    return proof;
}

struct CheckedEquivalence {
    eqeng::EquivResult result = eqeng::EquivResult::Unknown;
    bool solverRan = false;
    bool timedOut = false;
};

CheckedEquivalence checkEquivalent(
    eqeng::Primitives& primitives,
    eqeng::SigRef lhs,
    eqeng::SigRef rhs,
    double timeLimitSeconds) {
    CheckedEquivalence checked;
    if (timeLimitSeconds <= 0.0) {
        checked.timedOut = true;
        return checked;
    }
    const uint64_t satBefore = primitives.stats().equiv_by_sat;
    checked.result = primitives.equiv_checked(lhs, rhs, timeLimitSeconds);
    checked.solverRan = primitives.stats().equiv_by_sat > satBefore;
    checked.timedOut =
        checked.result == eqeng::EquivResult::Unknown && primitives.last_proof_timed_out();
    return checked;
}

CheckedEquivalence checkConstant(
    eqeng::Primitives& primitives,
    eqeng::SigRef signal,
    bool value,
    double timeLimitSeconds) {
    CheckedEquivalence checked;
    if (timeLimitSeconds <= 0.0) {
        checked.timedOut = true;
        return checked;
    }
    const uint64_t satBefore = primitives.stats().equiv_by_sat;
    checked.result = primitives.is_const_checked(signal, value, timeLimitSeconds);
    checked.solverRan = primitives.stats().equiv_by_sat > satBefore;
    checked.timedOut =
        checked.result == eqeng::EquivResult::Unknown && primitives.last_proof_timed_out();
    return checked;
}

std::vector<int> collectMappingCandidates(
    const Netlist& netlist,
    const FunctionalPatternContext& context,
    const std::vector<int>& preferred) {
    std::vector<int> candidates;
    for (int netId : preferred) {
        if (isActiveNet(netlist, netId) &&
            netId != context.targetNetId && netId != context.feedbackNetId &&
            !netlist.getNet(netId).isConst) {
            appendUnique(candidates, netId);
        }
    }
    for (int netId : context.coneNetIds) {
        if (!isActiveNet(netlist, netId) ||
            netId == context.targetNetId || netId == context.feedbackNetId ||
            netlist.getNet(netId).isConst) {
            continue;
        }
        appendUnique(candidates, netId);
    }
    return candidates;
}

class DffEnableHoldFunctionalMatcher final : public FunctionalPatternMatcher {
public:
    FunctionalPatternKind kind() const override {
        return FunctionalPatternKind::MuxHold;
    }

    FunctionalPatternSearchResult search(
        const Netlist& netlist,
        eqeng::Primitives& primitives,
        const FunctionalPatternContext& context,
        const FunctionalPatternSearchOptions& options,
        const request_time_budget::RequestDeadline& deadline) const override {
        FunctionalPatternSearchResult result;
        const Clock::time_point start = Clock::now();

        auto remaining = [&]() {
            return remainingSeconds(start, options.timeLimitSeconds, deadline);
        };
        auto finish = [&](const std::string& status, const std::string& message) {
            result.status = status;
            result.message = message;
            result.elapsedSeconds = elapsedSeconds(start);
            return result;
        };

        if (!isActiveNet(netlist, context.targetNetId) ||
            !isActiveNet(netlist, context.feedbackNetId) ||
            options.timeLimitSeconds <= 0.0) {
            result.complete = false;
            return finish(
                "INVALID_ARGUMENT",
                "Functional enable/hold analysis requires active D/Q nets and a positive deadline.");
        }

        try {
            const std::string& dName = netlist.getNet(context.targetNetId).name;
            const std::string& qName = netlist.getNet(context.feedbackNetId).name;
            const std::optional<eqeng::SigRef> dSignal = primitives.try_resolve(dName);
            const std::optional<eqeng::SigRef> qSignal = primitives.try_resolve(qName);
            if (!dSignal.has_value() || !qSignal.has_value() ||
                !primitives.is_trustworthy(*dSignal) ||
                !primitives.is_trustworthy(*qSignal)) {
                result.complete = false;
                return finish(
                    "UNSUPPORTED",
                    "D or Q has no trustworthy Boolean signal in the AIG model.");
            }
            if (!primitives.is_free_var(*qSignal)) {
                result.complete = false;
                return finish(
                    "UNSUPPORTED",
                    "The selected DFF.Q is not modeled as a free sequential boundary.");
            }
            if (remaining() <= 0.0) {
                result.timedOut = true;
                result.complete = false;
                return finish("TIMEOUT", "Functional enable/hold analysis reached its deadline.");
            }

            const eqeng::SigRef d0 = primitives.cofactor(*dSignal, *qSignal, false);
            const eqeng::SigRef d1 = primitives.cofactor(*dSignal, *qSignal, true);

            FunctionalPatternMatch match;
            match.kind = FunctionalPatternKind::MuxHold;
            match.detectionMethod = FunctionalPatternDetectionMethod::FunctionalCofactorSat;
            match.bindings.push_back({
                FunctionalPatternRole::Target,
                context.targetNetId,
                dName,
                false});
            match.bindings.push_back({
                FunctionalPatternRole::Feedback,
                context.feedbackNetId,
                qName,
                false});

            auto recordConstCheck = [&](eqeng::SigRef signal,
                                        bool expectedValue,
                                        const std::string& description) {
                const CheckedEquivalence checked = checkConstant(
                    primitives, signal, expectedValue, remaining());
                if (checked.solverRan) {
                    ++result.satCheckCount;
                }
                match.proofs.push_back(makeBooleanProof(
                    description, checked.result, checked.solverRan, checked.timedOut));
                return checked;
            };

            // A valid Q-free enable/data decomposition cannot contain a 1->0
            // transition between the Q=0 and Q=1 cofactors.
            const eqeng::SigRef forbidden = primitives.make_and(d0, !d1);
            const CheckedEquivalence noForbidden = recordConstCheck(
                forbidden,
                false,
                "Prove that D|Q=0 cannot be 1 while D|Q=1 is 0.");
            if (noForbidden.result == eqeng::EquivResult::Unknown) {
                result.complete = false;
                result.timedOut = noForbidden.timedOut;
                ++result.inconclusiveCandidateCount;
                return finish(
                    result.timedOut ? "TIMEOUT" : "UNKNOWN",
                    "The forbidden-transition proof was inconclusive.");
            }
            if (noForbidden.result == eqeng::EquivResult::NotEqual) {
                result.ok = true;
                result.complete = true;
                return finish(
                    "NO_MATCH",
                    "D has a Q-cofactor transition that cannot be represented as enable/hold.");
            }

            // Hold is observable exactly where D0=0 and D1=1.
            const eqeng::SigRef holdFunction = primitives.make_and(!d0, d1);
            const CheckedEquivalence holdAlwaysZero = recordConstCheck(
                holdFunction,
                false,
                "Prove that a same-DFF hold assignment is reachable.");
            if (holdAlwaysZero.result == eqeng::EquivResult::Unknown) {
                result.complete = false;
                result.timedOut = holdAlwaysZero.timedOut;
                ++result.inconclusiveCandidateCount;
                return finish(
                    result.timedOut ? "TIMEOUT" : "UNKNOWN",
                    "The hold-reachability proof was inconclusive.");
            }
            if (holdAlwaysZero.result == eqeng::EquivResult::Equal) {
                result.ok = true;
                result.complete = true;
                return finish(
                    "NO_MATCH",
                    "D has no reachable same-DFF hold mode.");
            }

            // Update is observable where both cofactors agree. Requiring it to
            // be reachable rejects the degenerate permanent-hold function D=Q.
            const eqeng::SigRef enableFunction = !primitives.make_xor(d0, d1);
            const CheckedEquivalence updateAlwaysZero = recordConstCheck(
                enableFunction,
                false,
                "Prove that an update assignment is reachable.");
            if (updateAlwaysZero.result == eqeng::EquivResult::Unknown) {
                result.complete = false;
                result.timedOut = updateAlwaysZero.timedOut;
                ++result.inconclusiveCandidateCount;
                return finish(
                    result.timedOut ? "TIMEOUT" : "UNKNOWN",
                    "The update-reachability proof was inconclusive.");
            }
            if (updateAlwaysZero.result == eqeng::EquivResult::Equal) {
                result.ok = true;
                result.complete = true;
                return finish(
                    "NO_MATCH",
                    "D is permanent hold and has no reachable update mode.");
            }

            match.proven = true;
            match.complete = true;
            match.activeLevel = 1;
            match.holdLevel = 0;

            const Clock::time_point mappingStart = Clock::now();
            auto mappingRemaining = [&]() {
                return std::max(
                    0.0,
                    std::min(
                        remaining(),
                        kOptionalRoleMappingBudgetSeconds - elapsedSeconds(mappingStart)));
            };

            const std::vector<int> controlCandidates = collectMappingCandidates(
                netlist, context, context.preferredControlNetIds);
            result.candidateCount = controlCandidates.size();
            result.searchableCandidateCount = controlCandidates.size();
            const size_t controlLimit = std::min(options.maxCandidates, controlCandidates.size());
            result.candidateLimitReached = controlLimit < controlCandidates.size();

            for (size_t index = 0; index < controlLimit; ++index) {
                if (mappingRemaining() <= 0.0) {
                    break;
                }
                ++result.candidatesExamined;
                const int candidateNetId = controlCandidates[index];
                const std::optional<eqeng::SigRef> candidate =
                    primitives.try_resolve(netlist.getNet(candidateNetId).name);
                if (!candidate.has_value() || !primitives.is_trustworthy(*candidate)) {
                    continue;
                }

                const eqeng::SigRef candidate0 =
                    primitives.cofactor(*candidate, *qSignal, false);
                const eqeng::SigRef candidate1 =
                    primitives.cofactor(*candidate, *qSignal, true);
                const CheckedEquivalence qFree = checkEquivalent(
                    primitives, candidate0, candidate1, mappingRemaining());
                if (qFree.solverRan) {
                    ++result.satCheckCount;
                }
                if (qFree.result == eqeng::EquivResult::Unknown) {
                    ++result.inconclusiveCandidateCount;
                    continue;
                }
                if (qFree.result != eqeng::EquivResult::Equal) {
                    continue;
                }

                const CheckedEquivalence activeHigh = checkEquivalent(
                    primitives, *candidate, enableFunction, mappingRemaining());
                if (activeHigh.solverRan) {
                    ++result.satCheckCount;
                }
                if (activeHigh.result == eqeng::EquivResult::Equal) {
                    match.bindings.push_back({
                        FunctionalPatternRole::Control,
                        candidateNetId,
                        netlist.getNet(candidateNetId).name,
                        false});
                    match.activeLevel = 1;
                    match.holdLevel = 0;
                    break;
                }

                const CheckedEquivalence activeLow = checkEquivalent(
                    primitives, *candidate, !enableFunction, mappingRemaining());
                if (activeLow.solverRan) {
                    ++result.satCheckCount;
                }
                if (activeLow.result == eqeng::EquivResult::Equal) {
                    match.bindings.push_back({
                        FunctionalPatternRole::Control,
                        candidateNetId,
                        netlist.getNet(candidateNetId).name,
                        true});
                    match.activeLevel = 0;
                    match.holdLevel = 1;
                    break;
                }
                if (activeHigh.result == eqeng::EquivResult::Unknown ||
                    activeLow.result == eqeng::EquivResult::Unknown) {
                    ++result.inconclusiveCandidateCount;
                }
            }
            result.unexaminedCandidateCount =
                controlCandidates.size() > result.candidatesExamined
                    ? controlCandidates.size() - result.candidatesExamined
                    : 0;

            match.dataSearchAttempted = options.resolveDataNets;
            if (options.resolveDataNets) {
                const std::vector<int> dataCandidates = collectMappingCandidates(
                    netlist, context, context.preferredDataNetIds);
                match.dataCandidateCount = dataCandidates.size();
                const size_t dataLimit = std::min(
                    options.maxDataCandidatesPerMatch, dataCandidates.size());
                for (size_t index = 0; index < dataLimit; ++index) {
                    if (mappingRemaining() <= 0.0) {
                        match.dataSearchComplete = false;
                        break;
                    }
                    ++match.dataCandidatesExamined;
                    const int candidateNetId = dataCandidates[index];
                    const std::optional<eqeng::SigRef> candidate =
                        primitives.try_resolve(netlist.getNet(candidateNetId).name);
                    if (!candidate.has_value() || !primitives.is_trustworthy(*candidate)) {
                        continue;
                    }

                    const eqeng::SigRef candidate0 =
                        primitives.cofactor(*candidate, *qSignal, false);
                    const eqeng::SigRef candidate1 =
                        primitives.cofactor(*candidate, *qSignal, true);
                    const CheckedEquivalence qFree = checkEquivalent(
                        primitives, candidate0, candidate1, mappingRemaining());
                    if (qFree.solverRan) {
                        ++result.satCheckCount;
                    }
                    if (qFree.result != eqeng::EquivResult::Equal) {
                        if (qFree.result == eqeng::EquivResult::Unknown) {
                            match.dataSearchComplete = false;
                        }
                        continue;
                    }

                    // DATA is a don't-care during hold. Only differences under
                    // the derived update-enable function matter.
                    const eqeng::SigRef difference =
                        primitives.make_xor(*candidate, d0);
                    const eqeng::SigRef activeDifference =
                        primitives.make_and(enableFunction, difference);
                    const CheckedEquivalence dataMatches = checkConstant(
                        primitives, activeDifference, false, mappingRemaining());
                    if (dataMatches.solverRan) {
                        ++result.satCheckCount;
                    }
                    if (dataMatches.result == eqeng::EquivResult::Equal) {
                        match.dataFunctionResolved = true;
                        match.bindings.push_back({
                            FunctionalPatternRole::Data,
                            candidateNetId,
                            netlist.getNet(candidateNetId).name,
                            false});
                        break;
                    }
                    if (dataMatches.result == eqeng::EquivResult::Unknown) {
                        match.dataSearchComplete = false;
                    }
                }
                if (!match.dataFunctionResolved &&
                    match.dataCandidatesExamined < dataCandidates.size()) {
                    match.dataSearchComplete = false;
                }
            } else {
                match.dataSearchComplete = false;
            }

            const int targetDriver = netlist.getNet(context.targetNetId).driverGateId;
            if (netlist.isValidGateId(targetDriver) && !netlist.isGateRemoved(targetDriver)) {
                match.evidenceGateIds.push_back(targetDriver);
            }
            match.message = match.dataFunctionResolved
                ? "Enable/hold and its named data function were proven from D/Q cofactors."
                : "Enable/hold was proven from D/Q cofactors; one or more roles have no resolved named net.";
            result.matches.push_back(std::move(match));
            result.ok = true;
            // Mapping incompleteness does not invalidate the Boolean
            // classification itself.
            result.complete = true;
            return finish("OK", "Functional enable/hold decomposition was proven.");
        } catch (const std::exception& error) {
            result.complete = false;
            result.timedOut = deadline.expired();
            return finish(
                result.timedOut ? "TIMEOUT" : "UNSUPPORTED",
                std::string("Functional enable/hold analysis failed: ") + error.what());
        }
    }
};

} // namespace

FunctionalPatternEngine::FunctionalPatternEngine() {
    matchers.push_back(std::make_unique<DffEnableHoldFunctionalMatcher>());
}

FunctionalPatternEvaluation FunctionalPatternEngine::proveSpecifiedOperands(
    const Netlist& netlist,
    eqeng::Primitives& primitives,
    const FunctionalPatternProofRequest& request,
    const request_time_budget::RequestDeadline& deadline) const {
    FunctionalPatternEvaluation evaluation;
    evaluation.kind = request.kind;

    const size_t arity = expectedOperandCount(request.kind);
    if (!isActiveNet(netlist, request.targetNetId) || arity == 0 ||
        request.operandNetIds.size() != arity ||
        (!request.operandInverted.empty() && request.operandInverted.size() != arity)) {
        evaluation.status = FunctionalPatternProofStatus::Unsupported;
        evaluation.solverStatus = "INVALID_ARGUMENT";
        evaluation.message = "Functional pattern proof received an invalid target or operand list.";
        return evaluation;
    }
    if (deadline.expired()) {
        evaluation.status = FunctionalPatternProofStatus::Unknown;
        evaluation.timedOut = true;
        evaluation.solverStatus = "TIMEOUT";
        evaluation.message = "Functional pattern proof reached its request deadline.";
        return evaluation;
    }

    try {
        const std::optional<eqeng::SigRef> target =
            primitives.try_resolve(netlist.getNet(request.targetNetId).name);
        if (!target.has_value() || !primitives.is_trustworthy(*target)) {
            evaluation.status = FunctionalPatternProofStatus::Unsupported;
            evaluation.solverStatus = "UNTRUSTED_TARGET";
            evaluation.message = "Target has no trustworthy Boolean signal.";
            return evaluation;
        }

        std::vector<eqeng::SigRef> operands;
        operands.reserve(arity);
        for (size_t index = 0; index < arity; ++index) {
            const int netId = request.operandNetIds[index];
            if (!isActiveNet(netlist, netId)) {
                evaluation.status = FunctionalPatternProofStatus::Unsupported;
                evaluation.solverStatus = "INVALID_OPERAND";
                evaluation.message = "A functional pattern operand is not an active net.";
                return evaluation;
            }
            std::optional<eqeng::SigRef> operand =
                primitives.try_resolve(netlist.getNet(netId).name);
            if (!operand.has_value() || !primitives.is_trustworthy(*operand)) {
                evaluation.status = FunctionalPatternProofStatus::Unsupported;
                evaluation.solverStatus = "UNTRUSTED_OPERAND";
                evaluation.message = "A functional pattern operand has no trustworthy Boolean signal.";
                return evaluation;
            }
            if (!request.operandInverted.empty() && request.operandInverted[index]) {
                *operand = !*operand;
            }
            operands.push_back(*operand);
        }

        const std::optional<eqeng::SigRef> pattern =
            buildPatternFunction(primitives, request.kind, operands);
        if (!pattern.has_value()) {
            evaluation.status = FunctionalPatternProofStatus::Unsupported;
            evaluation.solverStatus = "UNSUPPORTED_PATTERN";
            evaluation.message = "No Boolean builder is registered for this pattern.";
            return evaluation;
        }

        const CheckedEquivalence checked = checkEquivalent(
            primitives, *target, *pattern, deadline.remainingSeconds());
        evaluation.status = proofStatus(checked.result);
        evaluation.complete = checked.result != eqeng::EquivResult::Unknown;
        evaluation.solverRan = checked.solverRan;
        evaluation.timedOut = checked.timedOut;
        evaluation.solverStatus = checked.timedOut
            ? "TIMEOUT"
            : (checked.result == eqeng::EquivResult::Equal
                ? "UNSAT"
                : (checked.result == eqeng::EquivResult::NotEqual ? "SAT" : "UNKNOWN"));
        evaluation.message = checked.result == eqeng::EquivResult::Equal
            ? "Target is functionally equivalent to the requested Boolean pattern."
            : (checked.result == eqeng::EquivResult::NotEqual
                ? "Target is not functionally equivalent to the requested Boolean pattern."
                : "Functional pattern proof was inconclusive.");
        return evaluation;
    } catch (const std::exception& error) {
        evaluation.status = deadline.expired()
            ? FunctionalPatternProofStatus::Unknown
            : FunctionalPatternProofStatus::Unsupported;
        evaluation.timedOut = deadline.expired();
        evaluation.solverStatus = evaluation.timedOut ? "TIMEOUT" : "UNSUPPORTED";
        evaluation.message = std::string("Functional pattern proof failed: ") + error.what();
        return evaluation;
    }
}

FunctionalPatternSearchResult FunctionalPatternEngine::search(
    FunctionalPatternKind kind,
    const Netlist& netlist,
    eqeng::Primitives& primitives,
    const FunctionalPatternContext& context,
    const FunctionalPatternSearchOptions& options,
    const request_time_budget::RequestDeadline& deadline) const {
    for (const std::unique_ptr<FunctionalPatternMatcher>& matcher : matchers) {
        if (matcher->kind() == kind) {
            return matcher->search(netlist, primitives, context, options, deadline);
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
