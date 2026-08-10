#include "include/core/AigFunctionQueryAdapter.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "include/SATEngine/Primitives.h"
#include "include/core/Netlist.h"

namespace {

using eqeng::EquivResult;
using eqeng::ModelHealth;
using eqeng::Primitives;
using eqeng::SigRef;

FunctionReport makeBaseReport(const FunctionQuery& query) {
    FunctionReport report;
    report.netNameA = query.netNameA;
    report.netNameB = query.netNameB;
    report.conditionNetName = query.conditionNetName;
    report.symmetryInputNameA = query.symmetryInputNameA;
    report.symmetryInputNameB = query.symmetryInputNameB;
    report.constValue = query.constValue;
    report.conditionValue = query.conditionValue;
    report.maxExpressionDepth = query.maxExpressionDepth;
    return report;
}

void markUnknown(FunctionReport& report,
                 Primitives& primitives,
                 const std::string& context,
                 bool timedOut) {
    report.ok = false;
    report.exists = false;
    report.solverTimedOut = timedOut;
    report.solverUnknown = !timedOut;
    report.unsupported = primitives.model_health() == ModelHealth::Invalid;
    if (report.unsupported) {
        report.status = "UNSUPPORTED";
        report.solverStatus = "UNSUPPORTED";
        report.message = context + " is unsupported: " +
                         primitives.model_health_message();
    } else if (timedOut) {
        report.status = "SOLVER_TIMEOUT";
        report.solverStatus = "TIMEOUT";
        report.message = context +
                         " completed after the requested Phase-A time budget.";
    } else {
        report.status = "SOLVER_UNKNOWN";
        report.solverStatus = "UNKNOWN";
        report.message = context + " returned an inconclusive AIG proof.";
    }
}

std::optional<SigRef> resolveNetId(const Netlist& netlist,
                                   Primitives& primitives,
                                   int netId) {
    if (!netlist.isValidNetId(netId)) return std::nullopt;
    const Net& net = netlist.getNet(netId);
    if (net.isRemoved || net.name.empty()) return std::nullopt;
    return primitives.try_resolve(net.name);
}

bool resolveBits(const Netlist& netlist,
                 Primitives& primitives,
                 const std::vector<int>& netIds,
                 std::vector<SigRef>& signals) {
    signals.clear();
    signals.reserve(netIds.size());
    for (int netId : netIds) {
        const std::optional<SigRef> signal =
            resolveNetId(netlist, primitives, netId);
        if (!signal) return false;
        signals.push_back(*signal);
    }
    return true;
}

bool timeBudgetExceeded(const std::chrono::steady_clock::time_point& startedAt,
                        double limitSeconds) {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - startedAt).count() >
           limitSeconds;
}

double remainingBudget(const std::chrono::steady_clock::time_point& startedAt,
                       double limitSeconds) {
    return limitSeconds - std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startedAt).count();
}

void updateSolverUsage(FunctionReport& report,
                       const Primitives::Stats& before,
                       const Primitives::Stats& after) {
    report.solverRan = after.miters_built > before.miters_built ||
                       after.equiv_by_sat > before.equiv_by_sat;
}

EquivResult compareBitVectors(Primitives& primitives,
                              const std::vector<SigRef>& bitsA,
                              const std::vector<SigRef>& bitsB,
                              const std::chrono::steady_clock::time_point& startedAt,
                              double limitSeconds,
                              bool& timedOut) {
    if (bitsA.size() != bitsB.size()) return EquivResult::NotEqual;

    bool sawUnknown = false;
    for (size_t i = 0; i < bitsA.size(); ++i) {
        const double remaining = remainingBudget(startedAt, limitSeconds);
        if (remaining <= 0.0) {
            timedOut = true;
            return EquivResult::Unknown;
        }
        const EquivResult result = primitives.equiv_checked(
            bitsA[i], bitsB[i], remaining);
        timedOut = timedOut || primitives.last_proof_timed_out();
        if (result == EquivResult::NotEqual) return result;
        if (result == EquivResult::Unknown) sawUnknown = true;
    }
    return sawUnknown ? EquivResult::Unknown : EquivResult::Equal;
}

bool analyzeScalarValues(Primitives& primitives,
                         SigRef signal,
                         FunctionReport& report,
                         const std::chrono::steady_clock::time_point& startedAt,
                         double limitSeconds,
                         bool& timedOut) {
    double remaining = remainingBudget(startedAt, limitSeconds);
    if (remaining <= 0.0) {
        timedOut = true;
        return false;
    }
    const EquivResult constantZero =
        primitives.is_const_checked(signal, false, remaining);
    timedOut = timedOut || primitives.last_proof_timed_out();
    if (constantZero == EquivResult::Unknown) return false;

    remaining = remainingBudget(startedAt, limitSeconds);
    if (remaining <= 0.0) {
        timedOut = true;
        return false;
    }
    const EquivResult constantOne =
        primitives.is_const_checked(signal, true, remaining);
    timedOut = timedOut || primitives.last_proof_timed_out();
    if (constantZero == EquivResult::Unknown ||
        constantOne == EquivResult::Unknown) {
        return false;
    }

    report.canBeZero = constantOne == EquivResult::NotEqual;
    report.canBeOne = constantZero == EquivResult::NotEqual;
    return true;
}

} // namespace

bool isAigFunctionQuerySupported(FunctionQueryType type) {
    switch (type) {
    case FunctionQueryType::Equivalence:
    case FunctionQueryType::ConditionalEquivalence:
    case FunctionQueryType::CanBeValue:
    case FunctionQueryType::ConstantFunction:
    case FunctionQueryType::AlwaysZero:
    case FunctionQueryType::AlwaysOne:
    case FunctionQueryType::TruthStatus:
        return true;
    default:
        return false;
    }
}

FunctionReport runAigFunctionQuery(
    const Netlist& netlist,
    Primitives& primitives,
    const FunctionQuery& query) {
    FunctionReport report = makeBaseReport(query);

    if (!isAigFunctionQuerySupported(query.type)) {
        report.status = "AIG_BACKEND_UNSUPPORTED_QUERY";
        report.unsupported = true;
        report.message = "This FunctionQuery mode has not been migrated to the AIG backend.";
        return report;
    }
    if (query.timeLimitSeconds <= 0.0) {
        report.status = "INVALID_ARGUMENT";
        report.message = "SAT timeLimitSeconds must be positive.";
        return report;
    }

    const auto startedAt = std::chrono::steady_clock::now();
    const Primitives::Stats statsBefore = primitives.stats();
    bool proofTimedOut = false;

    auto finishSolverUsage = [&]() {
        updateSolverUsage(report, statsBefore, primitives.stats());
    };
    auto finishUnknown = [&](const std::string& context) {
        finishSolverUsage();
        markUnknown(report, primitives, context,
                    proofTimedOut ||
                        timeBudgetExceeded(startedAt, query.timeLimitSeconds));
        return report;
    };

    if (query.type == FunctionQueryType::Equivalence ||
        query.type == FunctionQueryType::ConditionalEquivalence) {
        if (query.netNameA.empty() || query.netNameB.empty()) {
            report.status = "INVALID_ARGUMENT";
            report.message = query.type == FunctionQueryType::Equivalence
                ? "Equivalence requires netNameA and netNameB"
                : "ConditionalEquivalence requires netNameA, netNameB, "
                  "conditionNetName, and conditionValue 0 or 1";
            return report;
        }

        const std::vector<int> netIdsA = netlist.expandNetToBits(query.netNameA);
        const std::vector<int> netIdsB = netlist.expandNetToBits(query.netNameB);
        if (netIdsA.empty()) {
            report.status = "NET_A_NOT_FOUND";
            report.message = "Net not found: " + query.netNameA;
            return report;
        }
        if (netIdsB.empty()) {
            report.status = "NET_B_NOT_FOUND";
            report.message = "Net not found: " + query.netNameB;
            return report;
        }
        if (netIdsA.size() != netIdsB.size()) {
            report.status = "UNSUPPORTED";
            report.unsupported = true;
            report.message = "Equivalence requires equal bit widths.";
            return report;
        }

        std::vector<SigRef> bitsA;
        std::vector<SigRef> bitsB;
        if (!resolveBits(netlist, primitives, netIdsA, bitsA) ||
            !resolveBits(netlist, primitives, netIdsB, bitsB)) {
            return finishUnknown("Equivalence query");
        }

        EquivResult result = EquivResult::Unknown;
        if (query.type == FunctionQueryType::Equivalence) {
            result = compareBitVectors(
                primitives, bitsA, bitsB, startedAt,
                query.timeLimitSeconds, proofTimedOut);
        } else {
            if (query.conditionNetName.empty() ||
                (query.conditionValue != 0 && query.conditionValue != 1)) {
                report.status = "INVALID_ARGUMENT";
                report.message = "ConditionalEquivalence requires netNameA, netNameB, "
                                 "conditionNetName, and conditionValue 0 or 1";
                return report;
            }
            const std::vector<int> conditionIds =
                netlist.expandNetToBits(query.conditionNetName);
            if (conditionIds.size() != 1) {
                report.status = "SCALAR_CONDITION_REQUIRED";
                report.message =
                    "ConditionalEquivalence requires an existing scalar condition net";
                return report;
            }
            const std::optional<SigRef> condition =
                resolveNetId(netlist, primitives, conditionIds.front());
            if (!condition) return finishUnknown("ConditionalEquivalence query");

            SigRef mismatch = primitives.make_xor(bitsA.front(), bitsB.front());
            for (size_t i = 1; i < bitsA.size(); ++i) {
                mismatch = primitives.make_or(
                    mismatch, primitives.make_xor(bitsA[i], bitsB[i]));
            }
            const SigRef activeCondition =
                query.conditionValue == 1 ? *condition : !*condition;
            const SigRef violation =
                primitives.make_and(mismatch, activeCondition);
            const double remaining =
                remainingBudget(startedAt, query.timeLimitSeconds);
            if (remaining <= 0.0) {
                proofTimedOut = true;
                result = EquivResult::Unknown;
            } else {
                result = primitives.is_const_checked(
                    violation, false, remaining);
                proofTimedOut = primitives.last_proof_timed_out();
            }
            report.conditionNetId = conditionIds.front();
        }

        finishSolverUsage();
        if (result == EquivResult::Unknown ||
            timeBudgetExceeded(startedAt, query.timeLimitSeconds)) {
            return finishUnknown(query.type == FunctionQueryType::Equivalence
                                     ? "Equivalence query"
                                     : "ConditionalEquivalence query");
        }

        report.ok = true;
        report.equivalent = result == EquivResult::Equal;
        report.exists = report.equivalent;
        report.netIdA = netIdsA.size() == 1 ? netIdsA.front() : -1;
        report.netIdB = netIdsB.size() == 1 ? netIdsB.front() : -1;
        report.solverStatus = report.equivalent ? "UNSAT" : "SAT";
        if (query.type == FunctionQueryType::Equivalence) {
            report.status = report.equivalent ? "EQUIVALENT" : "NOT_EQUIVALENT";
            report.message = report.equivalent ? "Functions are equivalent"
                                               : "Functions are not equivalent";
        } else {
            report.status = report.equivalent
                ? "CONDITIONALLY_EQUIVALENT"
                : "NOT_CONDITIONALLY_EQUIVALENT";
            report.message = report.equivalent
                ? "Functions are equivalent under the requested condition"
                : "Functions are not equivalent under the requested condition";
        }
        return report;
    }

    if (query.netNameA.empty() ||
        netlist.expandNetToBits(query.netNameA).size() != 1) {
        report.status = "SCALAR_NET_REQUIRED";
        switch (query.type) {
        case FunctionQueryType::CanBeValue:
            report.message = "CanBeValue requires an existing scalar net";
            break;
        case FunctionQueryType::ConstantFunction:
            report.message = "ConstantFunction requires an existing scalar net";
            break;
        case FunctionQueryType::AlwaysZero:
            report.message = "AlwaysZero requires an existing scalar net";
            break;
        case FunctionQueryType::AlwaysOne:
            report.message = "AlwaysOne requires an existing scalar net";
            break;
        default:
            report.message = "TruthStatus requires an existing scalar net";
            break;
        }
        return report;
    }
    if ((query.type == FunctionQueryType::CanBeValue ||
         query.type == FunctionQueryType::ConstantFunction) &&
        query.constValue != 0 && query.constValue != 1) {
        report.status = "INVALID_ARGUMENT";
        report.message = query.type == FunctionQueryType::CanBeValue
            ? "CanBeValue requires netNameA and constValue 0 or 1"
            : "ConstantFunction requires netNameA and constValue 0 or 1";
        return report;
    }

    const int netId = netlist.expandNetToBits(query.netNameA).front();
    const std::optional<SigRef> signal = resolveNetId(netlist, primitives, netId);
    if (!signal || !analyzeScalarValues(
            primitives, *signal, report, startedAt,
            query.timeLimitSeconds, proofTimedOut)) {
        return finishUnknown("Constant/function query");
    }
    finishSolverUsage();
    if (timeBudgetExceeded(startedAt, query.timeLimitSeconds)) {
        return finishUnknown("Constant/function query");
    }

    report.ok = true;
    report.netIdA = netId;
    report.solverStatus = "CONCLUSIVE";

    if (query.type == FunctionQueryType::CanBeValue) {
        report.exists = query.constValue == 0 ? report.canBeZero : report.canBeOne;
        report.status = report.exists ? "SATISFIABLE_VALUE" : "UNSATISFIABLE_VALUE";
        report.solverStatus = report.exists ? "SAT" : "UNSAT";
        report.message = report.exists ? "Target value is satisfiable"
                                       : "Target value is not satisfiable";
        return report;
    }

    if (query.type == FunctionQueryType::ConstantFunction) {
        report.exists = query.constValue == 0
            ? (report.canBeZero && !report.canBeOne)
            : (!report.canBeZero && report.canBeOne);
        report.isConstant = report.exists;
        report.status = report.exists
            ? (query.constValue == 0 ? "ALWAYS_ZERO" : "ALWAYS_ONE")
            : "NOT_REQUESTED_CONSTANT";
        report.message = report.exists ? "Net is the requested constant function"
                                       : "Net is not the requested constant function";
        return report;
    }

    const bool alwaysZero = report.canBeZero && !report.canBeOne;
    const bool alwaysOne = !report.canBeZero && report.canBeOne;
    if (query.type == FunctionQueryType::AlwaysZero) {
        report.constValue = 0;
        report.exists = alwaysZero;
        report.isConstant = report.exists;
        report.status = report.exists ? "ALWAYS_ZERO" : "NOT_ALWAYS_ZERO";
        report.message = report.exists ? "Net is always 0" : "Net is not always 0";
        return report;
    }
    if (query.type == FunctionQueryType::AlwaysOne) {
        report.constValue = 1;
        report.exists = alwaysOne;
        report.isConstant = report.exists;
        report.status = report.exists ? "ALWAYS_ONE" : "NOT_ALWAYS_ONE";
        report.message = report.exists ? "Net is always 1" : "Net is not always 1";
        return report;
    }

    report.exists = true;
    if (alwaysZero) {
        report.isConstant = true;
        report.constValue = 0;
        report.status = "ALWAYS_ZERO";
        report.message = "Net is always 0";
    } else if (alwaysOne) {
        report.isConstant = true;
        report.constValue = 1;
        report.status = "ALWAYS_ONE";
        report.message = "Net is always 1";
    } else {
        report.isConstant = false;
        report.constValue = -1;
        report.status = "NON_CONSTANT";
        report.message = "Net can be both 0 and 1";
    }
    return report;
}
