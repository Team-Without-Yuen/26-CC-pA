#include "include/core/Netlist.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <queue>
#include <unordered_set>

namespace {

FunctionSearchScope toFunctionSearchScope(TargetScope scope) {
    switch (scope) {
    case TargetScope::WHOLE_NETLIST: return FunctionSearchScope::WholeDesign;
    case TargetScope::NET_FANIN: return FunctionSearchScope::NetFanin;
    case TargetScope::NET_FANOUT: return FunctionSearchScope::NetFanout;
    case TargetScope::GATE_FANIN: return FunctionSearchScope::GateFanin;
    case TargetScope::GATE_FANOUT: return FunctionSearchScope::GateFanout;
    }
    return FunctionSearchScope::WholeDesign;
}

std::string targetScopeName(TargetScope scope) {
    switch (scope) {
    case TargetScope::WHOLE_NETLIST: return "WHOLE_NETLIST";
    case TargetScope::NET_FANIN: return "NET_FANIN";
    case TargetScope::NET_FANOUT: return "NET_FANOUT";
    case TargetScope::GATE_FANIN: return "GATE_FANIN";
    case TargetScope::GATE_FANOUT: return "GATE_FANOUT";
    }
    return "UNKNOWN";
}

bool outputDependsOnNet(const Netlist& netlist, int outputNetId, int ancestorNetId) {
    if (!netlist.isValidNetId(outputNetId) || !netlist.isValidNetId(ancestorNetId)) {
        return false;
    }

    std::queue<int> pending;
    std::unordered_set<int> visited;
    pending.push(outputNetId);
    visited.insert(outputNetId);

    while (!pending.empty()) {
        const int netId = pending.front();
        pending.pop();
        if (netId == ancestorNetId) {
            return true;
        }

        const Net& net = netlist.getNet(netId);
        if (!netlist.isValidGateId(net.driverGateId)) {
            continue;
        }
        const Gate& driver = netlist.getGate(net.driverGateId);
        if (driver.type == GateType::DFF || driver.type == GateType::UNKNOWN) {
            continue;
        }
        for (int inputNetId : driver.inputNetIds) {
            if (netlist.isValidNetId(inputNetId) && visited.insert(inputNetId).second) {
                pending.push(inputNetId);
            }
        }
    }
    return false;
}

int chooseCycleSafeRepresentative(
    const Netlist& netlist,
    const FunctionSearchEquivalenceClass& equivalentClass) {
    std::vector<int> gateIds = equivalentClass.gateIds;
    std::sort(gateIds.begin(), gateIds.end());

    for (int candidateGateId : gateIds) {
        if (!netlist.isValidGateId(candidateGateId) ||
            !netlist.isCombinationalGate(candidateGateId)) {
            continue;
        }
        const int candidateOutput = netlist.getGate(candidateGateId).outputNetId;
        if (!netlist.isValidNetId(candidateOutput)) {
            continue;
        }

        bool dependsOnClassMember = false;
        for (int otherGateId : gateIds) {
            if (otherGateId == candidateGateId || !netlist.isValidGateId(otherGateId)) {
                continue;
            }
            const int otherOutput = netlist.getGate(otherGateId).outputNetId;
            if (netlist.isValidNetId(otherOutput) &&
                outputDependsOnNet(netlist, candidateOutput, otherOutput)) {
                dependsOnClassMember = true;
                break;
            }
        }
        if (!dependsOnClassMember) {
            return candidateGateId;
        }
    }
    return -1;
}

void appendChangedObjects(
    NetlistEditReport& report,
    const FunctionalMergeSummary& summary) {
    for (const FunctionalGateMergeRecord& record : summary.records) {
        report.changedGateIds.push_back(record.representativeGateId);
        report.changedGateIds.push_back(record.removedGateId);
        report.changedNetIds.push_back(record.representativeNetId);
        report.changedNetIds.push_back(record.removedNetId);
        report.changedGateNames.push_back(record.representativeGateName);
        report.changedGateNames.push_back(record.removedGateName);
        report.changedNetNames.push_back(record.representativeNetName);
        report.changedNetNames.push_back(record.removedNetName);
    }

    auto uniqueValues = [](auto& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    uniqueValues(report.changedGateIds);
    uniqueValues(report.changedNetIds);
    uniqueValues(report.changedGateNames);
    uniqueValues(report.changedNetNames);
}

NetlistEditReport makeSearchFailureReport(
    const Netlist& netlist,
    FunctionalMergeSummary summary,
    const std::string& message) {
    NetlistEditReport report = Netlist::buildEditReport(
        netlist,
        netlist,
        "mergeFunctionallyEquivalentGates",
        NetlistEditOperationKind::Simplification);
    report.success = false;
    report.changed = false;
    report.message = message;
    report.functionalMerge = std::move(summary);
    report.validation.messages.push_back(message);
    return report;
}

} // namespace

NetlistEditReport Netlist::mergeFunctionallyEquivalentGatesWithReport(
    TargetScope scope,
    const std::string& scopeName,
    GateType gateTypeFilter,
    size_t simulationPatternCount,
    double timeLimitSeconds) {
    const auto startedAt = std::chrono::steady_clock::now();
    auto elapsedSeconds = [&]() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - startedAt).count();
    };

    if (!std::isfinite(timeLimitSeconds) || timeLimitSeconds <= 0.0) {
        FunctionalMergeSummary summary;
        summary.scope = targetScopeName(scope);
        summary.scopeName = scopeName;
        summary.gateTypeFilter = gateTypeFilter;
        summary.searchStatus = "TIMEOUT";
        summary.searchComplete = false;
        summary.searchTimedOut = true;
        summary.totalElapsedSeconds = elapsedSeconds();
        return makeSearchFailureReport(
            *this,
            std::move(summary),
            "Functional gate merge was not applied because the request time budget expired before search.");
    }

    FunctionSearchQuery searchQuery;
    searchQuery.type = FunctionSearchQueryType::EquivalentGatePairs;
    searchQuery.mode = FunctionSearchMode::FindAll;
    searchQuery.scope = toFunctionSearchScope(scope);
    searchQuery.scopeName = scopeName;
    searchQuery.gateTypeFilter = gateTypeFilter;
    searchQuery.simulationPatternCount = simulationPatternCount;
    searchQuery.timeLimitSeconds = timeLimitSeconds;
    searchQuery.expandEquivalentPairs = false;

    const FunctionSearchReport search = runFunctionSearchQuery(searchQuery);
    FunctionalMergeSummary summary;
    summary.scope = targetScopeName(scope);
    summary.scopeName = scopeName;
    summary.gateTypeFilter = gateTypeFilter;
    summary.searchStatus = search.status;
    summary.searchComplete = search.complete;
    summary.searchTimedOut = search.timedOut;
    summary.candidateGateCount = search.candidateGateCount;
    summary.equivalenceClassCount = search.equivalenceClassCount;
    summary.equivalentPairCount = search.equivalentPairCount;
    summary.satChecks = search.satChecks;
    summary.searchElapsedSeconds = search.elapsedSeconds;

    if (!search.ok || !search.complete) {
        summary.totalElapsedSeconds = elapsedSeconds();
        return makeSearchFailureReport(
            *this,
            std::move(summary),
            "Functional gate merge was not applied because equivalent-gate search was incomplete: " +
                search.message);
    }

    if (search.equivalenceClasses.empty()) {
        NetlistEditReport report = buildEditReport(
            *this,
            *this,
            "mergeFunctionallyEquivalentGates",
            NetlistEditOperationKind::Simplification);
        report.message = "No functionally equivalent gate class was found; design is unchanged.";
        certifyEquivalence(
            report,
            EquivalenceCheckMethod::StructuralIdentity,
            "No mutation was applied because the SAT search found no equivalent gate class.");
        summary.totalElapsedSeconds = elapsedSeconds();
        report.functionalMerge = std::move(summary);
        return report;
    }

    if (elapsedSeconds() >= timeLimitSeconds) {
        summary.wholeDesignTimedOut = true;
        summary.totalElapsedSeconds = elapsedSeconds();
        return makeSearchFailureReport(
            *this,
            std::move(summary),
            "Functional gate merge was not applied because no time remained for whole-design equivalence.");
    }

    const Netlist before = cloneForRollback();
    bool mergeFailed = false;

    for (const FunctionSearchEquivalenceClass& equivalentClass : search.equivalenceClasses) {
        const int representativeGateId =
            chooseCycleSafeRepresentative(*this, equivalentClass);
        if (!isValidGateId(representativeGateId)) {
            mergeFailed = true;
            for (const std::string& gateName : equivalentClass.gateNames) {
                summary.skippedGateNames.push_back(gateName);
                ++summary.skippedGateCount;
            }
            break;
        }

        const Gate& representative = getGate(representativeGateId);
        const int representativeNetId = representative.outputNetId;
        const std::string representativeGateName = representative.instName;
        const std::string representativeNetName = getNet(representativeNetId).name;

        for (int removedGateId : equivalentClass.gateIds) {
            if (removedGateId == representativeGateId) {
                continue;
            }
            if (!isValidGateId(removedGateId) || !isCombinationalGate(removedGateId)) {
                mergeFailed = true;
                ++summary.skippedGateCount;
                break;
            }

            const Gate& removedGate = getGate(removedGateId);
            const int removedNetId = removedGate.outputNetId;
            if (!isValidNetId(removedNetId) ||
                outputDependsOnNet(*this, representativeNetId, removedNetId)) {
                mergeFailed = true;
                summary.skippedGateNames.push_back(removedGate.instName);
                ++summary.skippedGateCount;
                break;
            }

            FunctionalGateMergeRecord record;
            record.representativeGateId = representativeGateId;
            record.representativeNetId = representativeNetId;
            record.removedGateId = removedGateId;
            record.removedNetId = removedNetId;
            record.representativeGateName = representativeGateName;
            record.representativeNetName = representativeNetName;
            record.removedGateName = removedGate.instName;
            record.removedNetName = getNet(removedNetId).name;

            if (!mergeNetIntoNet(removedNetId, representativeNetId) ||
                !markGateRemoved(removedGateId)) {
                mergeFailed = true;
                summary.skippedGateNames.push_back(record.removedGateName);
                ++summary.skippedGateCount;
                break;
            }
            removeNetIfUnused(removedNetId);
            summary.records.push_back(std::move(record));
            ++summary.mergedGateCount;
        }
        if (mergeFailed) {
            break;
        }
    }

    NetlistEditReport report = buildEditReport(
        before,
        *this,
        "mergeFunctionallyEquivalentGates",
        NetlistEditOperationKind::Simplification);
    appendChangedObjects(report, summary);

    if (mergeFailed || !report.success) {
        restoreFrom(before);
        report.success = false;
        report.rolledBack = true;
        report.message = mergeFailed
            ? "Functional gate merge could not safely merge every SAT equivalence class and was rolled back."
            : "Functional gate merge failed structural validation and was rolled back.";
        summary.totalElapsedSeconds = elapsedSeconds();
        report.functionalMerge = std::move(summary);
        return report;
    }

    const double remainingTime = timeLimitSeconds - elapsedSeconds();
    if (remainingTime <= 0.0) {
        restoreFrom(before);
        report.success = false;
        report.rolledBack = true;
        report.validation.equivalenceChecked = false;
        report.validation.functionallyEquivalent = false;
        report.validation.equivalenceMethod = EquivalenceCheckMethod::NotChecked;
        report.validation.messages.push_back(
            "No time remained for mandatory whole-design equivalence verification.");
        report.message = "Functional gate merge exceeded its time limit and was rolled back.";
        summary.wholeDesignTimedOut = true;
        summary.totalElapsedSeconds = elapsedSeconds();
        report.functionalMerge = std::move(summary);
        return report;
    }

    const WholeDesignEquivalenceReport equivalence =
        checkWholeDesignEquivalence(before, remainingTime);
    summary.wholeDesignEquivalenceChecked = true;
    summary.wholeDesignEquivalent = equivalence.ok && equivalence.equivalent;
    summary.wholeDesignTimedOut = equivalence.timeBudgetExceeded;
    report.validation.equivalenceChecked = true;
    report.validation.functionallyEquivalent =
        equivalence.ok && equivalence.equivalent;
    report.validation.equivalenceMethod = EquivalenceCheckMethod::WholeDesignSat;
    report.validation.messages.push_back(equivalence.message);
    for (const std::string& warning : equivalence.warnings) {
        report.addWarning(warning);
    }
    for (const std::string& reason : equivalence.unsupportedReasons) {
        report.addWarning(reason);
    }

    if (!equivalence.ok || !equivalence.equivalent) {
        restoreFrom(before);
        report.success = false;
        report.rolledBack = true;
        report.message = equivalence.timeBudgetExceeded
            ? "Whole-design equivalence timed out; functional gate merge was rolled back."
            : "Whole-design equivalence failed; functional gate merge was rolled back.";
    } else {
        report.success = true;
        report.changed = summary.mergedGateCount > 0;
        report.message = "Functionally equivalent gates were merged and whole-design equivalence was proved.";
    }

    summary.totalElapsedSeconds = elapsedSeconds();
    report.functionalMerge = std::move(summary);
    return report;
}
