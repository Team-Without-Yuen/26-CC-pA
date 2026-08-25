#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <iostream>
#include <set>
#include <string>

namespace {

struct TestReport {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name) {
        if (condition) {
            ++passed;
            std::cout << "[PASS] " << name << "\n";
        } else {
            ++failed;
            std::cout << "[FAIL] " << name << "\n";
        }
    }
};

Netlist::FunctionSearchQuery baseQuery() {
    Netlist::FunctionSearchQuery query;
    query.type = Netlist::FunctionSearchQueryType::EquivalentGatePairs;
    query.mode = Netlist::FunctionSearchMode::FindAll;
    query.scope = Netlist::FunctionSearchScope::WholeDesign;
    query.maxResults = 64;
    query.simulationPatternCount = 256;
    query.timeLimitSeconds = 5.0;
    return query;
}

std::set<std::string> classMembers(
    const Netlist::FunctionSearchEquivalenceClass& equivalentClass) {
    return std::set<std::string>(equivalentClass.gateNames.begin(),
                                 equivalentClass.gateNames.end());
}

bool containsClass(
    const Netlist::FunctionSearchReport& report,
    const std::set<std::string>& expectedMembers) {
    for (const auto& equivalentClass : report.equivalenceClasses) {
        if (classMembers(equivalentClass) == expectedMembers) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    TestReport test;
    Netlist netlist;
    VerilogReader reader;
    test.check(
        reader.read("mini test/test26/equivalent_gate_pairs_circuit.v", netlist),
        "test26 loads the equivalent-pair circuit");

    const auto all = netlist.runFunctionSearchQuery(baseQuery());
    test.check(all.ok && all.complete && all.allCandidatesExamined &&
                   all.status == "MATCHES_FOUND",
               "whole-design FindAll completes");
    test.check(all.equivalenceClassCount == 2 &&
                   all.equivalentPairCount == 7 && all.matches.size() == 7,
               "two classes expand to seven total pairs");
    test.check(containsClass(
                   all,
                   {"g_and1", "g_and2", "g_buf", "g_not_nand"}),
               "structurally different gates share one SAT-proven class");
    test.check(containsClass(all, {"g_or1", "g_or2"}),
               "a second equivalence class is reported independently");

    bool everyMatchProven = true;
    for (const auto& match : all.matches) {
        everyMatchProven = everyMatchProven && match.provenEquivalent &&
                           match.gateIdA >= 0 && match.gateIdB >= 0 &&
                           match.solverStatus == "UNSAT";
    }
    test.check(everyMatchProven, "every pair includes gate identity and SAT proof");

    auto findAnyQuery = baseQuery();
    findAnyQuery.mode = Netlist::FunctionSearchMode::FindAny;
    const auto any = netlist.runFunctionSearchQuery(findAnyQuery);
    test.check(any.ok && any.found && any.complete &&
                   !any.allCandidatesExamined && any.matches.size() == 1 &&
                   any.status == "MATCH_FOUND",
               "FindAny returns one complete existential witness");

    auto timeoutQuery = baseQuery();
    timeoutQuery.timeLimitSeconds = 1e-12;
    const auto timeout = netlist.runFunctionSearchQuery(timeoutQuery);
    test.check(!timeout.ok && timeout.timedOut && !timeout.complete &&
                   timeout.status == "TIMEOUT",
               "EquivalentGatePairs reports an explicit partial timeout");

    auto andOnlyQuery = baseQuery();
    andOnlyQuery.gateTypeFilter = GateType::AND;
    const auto andOnly = netlist.runFunctionSearchQuery(andOnlyQuery);
    test.check(andOnly.ok && andOnly.equivalenceClassCount == 1 &&
                   andOnly.equivalentPairCount == 1 &&
                   andOnly.equivalenceClasses.front().gateNames.size() == 2,
               "gate-type filtering keeps only the equivalent AND gates");

    auto yConeQuery = baseQuery();
    yConeQuery.scope = Netlist::FunctionSearchScope::NetFanin;
    yConeQuery.scopeName = "y";
    const auto yCone = netlist.runFunctionSearchQuery(yConeQuery);
    test.check(yCone.ok && yCone.equivalenceClassCount == 1 &&
                   yCone.equivalentPairCount == 1 && yCone.matches.size() == 1,
               "net_fanin scope finds only the pair inside y's cone");

    auto zConeQuery = baseQuery();
    zConeQuery.scope = Netlist::FunctionSearchScope::NetFanin;
    zConeQuery.scopeName = "z";
    const auto zCone = netlist.runFunctionSearchQuery(zConeQuery);
    test.check(zCone.ok && zCone.complete && !zCone.found &&
                   zCone.status == "NO_MATCH",
               "a cone without equivalent pairs returns a complete no-match");

    auto limitedQuery = baseQuery();
    limitedQuery.maxResults = 2;
    const auto limited = netlist.runFunctionSearchQuery(limitedQuery);
    test.check(!limited.ok && limited.found && limited.truncated &&
                   !limited.complete && limited.matches.size() == 2 &&
                   limited.equivalenceClassCount == 2 &&
                   limited.equivalentPairCount == 7,
               "maxResults preserves all class statistics while pair output is partial");

    auto missingScopeQuery = baseQuery();
    missingScopeQuery.scope = Netlist::FunctionSearchScope::GateFanin;
    missingScopeQuery.scopeName = "missing_gate";
    const auto missingScope = netlist.runFunctionSearchQuery(missingScopeQuery);
    test.check(!missingScope.ok && missingScope.status == "SCOPE_NOT_FOUND",
               "missing scope names are explicit errors");

    auto invalidFilterQuery = baseQuery();
    invalidFilterQuery.gateTypeFilter = GateType::DFF;
    const auto invalidFilter = netlist.runFunctionSearchQuery(invalidFilterQuery);
    test.check(!invalidFilter.ok && invalidFilter.status == "INVALID_ARGUMENT",
               "sequential gate filters are rejected");

    Netlist mergeNetlist;
    test.check(reader.read(
                   "mini test/test26/equivalent_gate_pairs_circuit.v",
                   mergeNetlist),
               "functional merge reloads a clean circuit");
    const Netlist mergeBaseline = mergeNetlist.cloneForRollback();
    Netlist::EditApplyRequest mergeRequest;
    mergeRequest.kind =
        Netlist::EditCommandKind::MergeFunctionallyEquivalentGates;
    mergeRequest.scope = TargetScope::WHOLE_NETLIST;
    mergeRequest.simulationPatternCount = 256;
    mergeRequest.timeLimitSeconds = 5.0;
    mergeRequest.validateEquivalence = true;
    mergeRequest.rollbackOnFailure = true;

    const auto mergeReport = mergeNetlist.runEditApply(mergeRequest);
    test.check(mergeReport.success && mergeReport.changed &&
                   !mergeReport.rolledBack &&
                   mergeReport.validation.equivalenceChecked &&
                   mergeReport.validation.functionallyEquivalent &&
                   mergeReport.validation.equivalenceMethod ==
                       EquivalenceCheckMethod::CertifiedRewrite &&
                   mergeReport.functionalMerge &&
                   !mergeReport.functionalMerge->wholeDesignEquivalenceChecked,
               "functional merge uses SAT-proven classes without final whole-design SAT");
    test.check(mergeReport.functionalMerge &&
                   mergeReport.functionalMerge->searchComplete &&
                   mergeReport.functionalMerge->equivalenceClassCount == 2 &&
                   mergeReport.functionalMerge->mergedGateCount == 4 &&
                   mergeReport.functionalMerge->records.size() == 4,
               "functional merge report preserves search and per-gate outcomes");

    bool keptSourceMostAnd = false;
    bool removedDependentBuffer = false;
    for (const auto& record : mergeReport.functionalMerge->records) {
        keptSourceMostAnd = keptSourceMostAnd ||
            record.representativeGateName == "g_and1";
        removedDependentBuffer = removedDependentBuffer ||
            (record.removedGateName == "g_buf" &&
             record.representativeGateName == "g_and1");
    }
    test.check(keptSourceMostAnd && removedDependentBuffer,
               "cycle-safe representative selection keeps the source-most AND");
    test.check(mergeReport.diff.activeGateCountDelta == -4 &&
                   mergeReport.changedGateNames.size() >= 6,
               "edit diff and changed-object lists expose the merge effect");

    const auto postMergeEquivalence =
        mergeNetlist.checkWholeDesignEquivalence(mergeBaseline, 5.0);
    test.check(postMergeEquivalence.ok && postMergeEquivalence.equivalent,
               "independent whole-design verification confirms the merged design");

    const auto noChangeReport = mergeNetlist.runEditApply(mergeRequest);
    test.check(noChangeReport.success && !noChangeReport.changed &&
                   noChangeReport.functionalMerge &&
                   noChangeReport.functionalMerge->mergedGateCount == 0,
               "repeating functional merge reports a certified no-change result");

    Netlist scopedMergeNetlist;
    test.check(reader.read(
                   "mini test/test26/equivalent_gate_pairs_circuit.v",
                   scopedMergeNetlist),
               "scoped functional merge reloads a clean circuit");
    auto scopedMergeRequest = mergeRequest;
    scopedMergeRequest.scope = TargetScope::NET_FANIN;
    scopedMergeRequest.scopeName = "y";
    const auto scopedMergeReport =
        scopedMergeNetlist.runEditApply(scopedMergeRequest);
    test.check(scopedMergeReport.success && scopedMergeReport.functionalMerge &&
                   scopedMergeReport.functionalMerge->mergedGateCount == 1 &&
                   scopedMergeReport.functionalMerge->scope == "NET_FANIN",
               "functional merge respects cone scope");

    Netlist timeoutMergeNetlist;
    test.check(reader.read(
                   "mini test/test26/equivalent_gate_pairs_circuit.v",
                   timeoutMergeNetlist),
               "timeout functional merge reloads a clean circuit");
    const size_t timeoutBefore =
        timeoutMergeNetlist.collectNetlistStats().activeGateCount;
    auto timeoutRequest = mergeRequest;
    timeoutRequest.timeLimitSeconds = 1e-12;
    const auto timeoutMergeReport = timeoutMergeNetlist.runEditApply(timeoutRequest);
    test.check(!timeoutMergeReport.success && !timeoutMergeReport.changed &&
                   !timeoutMergeReport.rolledBack &&
                   timeoutMergeNetlist.collectNetlistStats().activeGateCount ==
                       timeoutBefore &&
                   timeoutMergeReport.functionalMerge &&
                   timeoutMergeReport.functionalMerge->searchTimedOut,
               "search timeout is reported before any mutation occurs");

    Netlist endpointFreeMergeNetlist;
    test.check(reader.read(
                   "mini test/test26/functional_merge_rollback_circuit.v",
                   endpointFreeMergeNetlist),
               "functional merge loads a design without endpoints");
    const size_t endpointFreeBefore =
        endpointFreeMergeNetlist.collectNetlistStats().activeGateCount;
    const auto endpointFreeMergeReport =
        endpointFreeMergeNetlist.runEditApply(mergeRequest);
    test.check(endpointFreeMergeReport.success && endpointFreeMergeReport.changed &&
                   !endpointFreeMergeReport.rolledBack &&
                   endpointFreeMergeReport.functionalMerge &&
                   endpointFreeMergeReport.functionalMerge->mergedGateCount == 1 &&
                   !endpointFreeMergeReport.functionalMerge
                       ->wholeDesignEquivalenceChecked &&
                   endpointFreeMergeReport.validation.equivalenceMethod ==
                       EquivalenceCheckMethod::CertifiedRewrite,
               "endpoint-free functional merge uses its rewrite certificate");
    test.check(
        endpointFreeMergeNetlist.collectNetlistStats().activeGateCount ==
            endpointFreeBefore - 1,
        "endpoint-free functional merge commits the duplicate removal");

    std::cout << "Summary: " << test.passed << " passed, "
              << test.failed << " failed.\n";
    return test.failed == 0 ? 0 : 1;
}
