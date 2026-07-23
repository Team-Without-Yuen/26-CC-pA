#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <iostream>
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

bool loadCircuit(Netlist& netlist) {
    VerilogReader reader;
    return reader.read("mini test/test23/function_search_circuit.v", netlist);
}

Netlist::FunctionSearchQuery baseQuery(const std::string& target) {
    Netlist::FunctionSearchQuery query;
    query.type = Netlist::FunctionSearchQueryType::NandEquivalentInputPairs;
    query.targetNetName = target;
    query.mode = Netlist::FunctionSearchMode::FindAny;
    query.internalSignalsOnly = true;
    query.allowSameSignalPair = false;
    query.maxResults = 32;
    query.simulationPatternCount = 256;
    query.timeLimitSeconds = 5.0;
    return query;
}

} // namespace

int main() {
    TestReport test;
    Netlist netlist;
    test.check(loadCircuit(netlist), "test23 load function search circuit");

    const auto anyMatch = netlist.runFunctionSearchQuery(baseQuery("y_match"));
    test.check(anyMatch.ok && anyMatch.found && anyMatch.complete &&
                   !anyMatch.matches.empty(),
               "FindAny returns a complete existential answer");
    test.check(anyMatch.matches.size() == 1 &&
                   anyMatch.matches.front().provenEquivalent &&
                   anyMatch.matches.front().proofMethod == "SAT_UNSAT_MITER" &&
                   anyMatch.matches.front().solverStatus == "UNSAT",
               "every returned pair has an explicit SAT proof");
    test.check(anyMatch.satChecks >= 1 &&
                   anyMatch.simulationEligibleSignalCount >= 2 &&
                   anyMatch.status == "MATCH_FOUND",
               "FindAny report exposes search and solver statistics");

    auto allQuery = baseQuery("y_match");
    allQuery.mode = Netlist::FunctionSearchMode::FindAll;
    const auto allMatches = netlist.runFunctionSearchQuery(allQuery);
    test.check(allMatches.ok && allMatches.complete &&
                   allMatches.allCandidatesExamined && allMatches.matches.size() == 4,
               "FindAll enumerates all four functionally duplicated input pairs");
    bool allProven = true;
    for (const auto& match : allMatches.matches) {
        allProven = allProven && match.provenEquivalent &&
                    match.solverStatus == "UNSAT";
    }
    test.check(allProven, "FindAll never returns simulation-only matches");

    auto exactLimitQuery = allQuery;
    exactLimitQuery.maxResults = 4;
    const auto exactLimit = netlist.runFunctionSearchQuery(exactLimitQuery);
    test.check(exactLimit.ok && exactLimit.complete && !exactLimit.truncated &&
                   exactLimit.matches.size() == 4,
               "maxResults equal to the full result count remains complete");

    auto limitedQuery = allQuery;
    limitedQuery.maxResults = 2;
    const auto limited = netlist.runFunctionSearchQuery(limitedQuery);
    test.check(!limited.ok && limited.found && limited.truncated &&
                   !limited.complete && limited.matches.size() == 2 &&
                   limited.status == "RESULT_LIMIT_REACHED",
               "FindAll reports a partial result when maxResults is reached");

    const auto noMatch = netlist.runFunctionSearchQuery(baseQuery("y_none"));
    test.check(noMatch.ok && !noMatch.found && noMatch.complete &&
                   noMatch.allCandidatesExamined && noMatch.status == "NO_MATCH",
               "exhaustive search distinguishes no match from unknown");

    const auto missing = netlist.runFunctionSearchQuery(baseQuery("missing_net"));
    test.check(!missing.ok && !missing.complete &&
                   missing.status == "TARGET_NOT_FOUND",
               "missing target is an explicit request error");

    auto invalid = baseQuery("y_match");
    invalid.simulationPatternCount = 0;
    const auto invalidReport = netlist.runFunctionSearchQuery(invalid);
    test.check(!invalidReport.ok && invalidReport.status == "INVALID_ARGUMENT",
               "invalid search limits are rejected");

    auto timeout = baseQuery("y_match");
    timeout.timeLimitSeconds = 1e-12;
    const auto timeoutReport = netlist.runFunctionSearchQuery(timeout);
    test.check(!timeoutReport.ok && timeoutReport.timedOut &&
                   timeoutReport.status == "TIMEOUT",
               "global search time limit returns TIMEOUT instead of no match");

    std::cout << "Summary: " << test.passed << " passed, "
              << test.failed << " failed.\n";
    return test.failed == 0 ? 0 : 1;
}
