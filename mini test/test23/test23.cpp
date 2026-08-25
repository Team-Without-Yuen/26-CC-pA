#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

Netlist::FunctionSearchQuery patternQuery(const std::string& target,
                                          GateType patternGateType) {
    Netlist::FunctionSearchQuery query = baseQuery(target);
    query.type = Netlist::FunctionSearchQueryType::FunctionalPatternOperands;
    query.patternGateType = patternGateType;
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
                   (anyMatch.matches.front().proofMethod == "AIG_INCREMENTAL_SAT" ||
                    anyMatch.matches.front().proofMethod == "AIG_LITERAL_EQUALITY") &&
                   anyMatch.matches.front().solverStatus == "UNSAT",
               "every returned pair has an explicit SAT proof");
    test.check(anyMatch.satChecks >= 1 &&
                   anyMatch.simulationEligibleSignalCount >= 2 &&
                   anyMatch.status == "MATCH_FOUND",
               "FindAny report exposes search and solver statistics");

    auto allQuery = baseQuery("y_match");
    allQuery.mode = Netlist::FunctionSearchMode::FindAll;
    allQuery.maxResults = 0;
    const auto allMatches = netlist.runFunctionSearchQuery(allQuery);
    test.check(allMatches.ok && allMatches.complete &&
                   allMatches.allCandidatesExamined && allMatches.matchCount >= 4,
               "FindAll enumerates every functionally duplicated input pair");
    bool allProven = true;
    for (const auto& match : allMatches.matches) {
        allProven = allProven && match.provenEquivalent &&
                    match.solverStatus == "UNSAT";
    }
    test.check(allProven, "FindAll never returns simulation-only matches");

    auto exactLimitQuery = allQuery;
    exactLimitQuery.maxResults = allMatches.matchCount;
    const auto exactLimit = netlist.runFunctionSearchQuery(exactLimitQuery);
    test.check(exactLimit.ok && exactLimit.complete && !exactLimit.truncated &&
                   exactLimit.matchCount == allMatches.matchCount,
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

    const std::vector<std::pair<GateType, std::string>> patternCases = {
        {GateType::BUF, "y_buf"},
        {GateType::NOT, "y_not"},
        {GateType::AND, "y_and"},
        {GateType::NAND, "y_nand"},
        {GateType::OR, "y_or"},
        {GateType::NOR, "y_nor"},
        {GateType::XOR, "y_xor"},
        {GateType::XNOR, "y_xnor"},
    };
    for (const auto& [gateType, target] : patternCases) {
        const auto pattern = netlist.runFunctionSearchQuery(
            patternQuery(target, gateType));
        const size_t expectedArity =
            gateType == GateType::BUF || gateType == GateType::NOT ? 1U : 2U;
        test.check(pattern.ok && pattern.found && pattern.complete &&
                       pattern.patternGateType == gateType &&
                       pattern.operandArity == expectedArity &&
                       pattern.matches.size() == 1 &&
                       pattern.matches.front().operandNetIds.size() == expectedArity &&
                       pattern.matches.front().provenEquivalent,
                   "generic functional search proves " +
                       netlist.gateTypeToString(gateType) + " operands");
    }

    auto allOrQuery = patternQuery("y_or", GateType::OR);
    allOrQuery.mode = Netlist::FunctionSearchMode::FindAll;
    allOrQuery.maxResults = 0;
    const auto allOr = netlist.runFunctionSearchQuery(allOrQuery);
    test.check(allOr.ok && allOr.complete && allOr.found &&
                   allOr.allCandidatesExamined && allOr.matchCount >= 1,
               "generic FindAll exhaustively returns proven OR operands");

    auto scopedAndQuery = patternQuery("y_and", GateType::AND);
    scopedAndQuery.scope = Netlist::FunctionSearchScope::NetFanin;
    scopedAndQuery.scopeName = "y_and";
    const auto scopedAnd = netlist.runFunctionSearchQuery(scopedAndQuery);
    test.check(scopedAnd.ok && scopedAnd.found && scopedAnd.complete &&
                   scopedAnd.matches.front().operandNetIds.size() == 2,
               "functional operand search honors a matching fanin scope");

    auto excludedScopeQuery = patternQuery("y_and", GateType::AND);
    excludedScopeQuery.scope = Netlist::FunctionSearchScope::NetFanin;
    excludedScopeQuery.scopeName = "y_none";
    const auto excludedScope = netlist.runFunctionSearchQuery(excludedScopeQuery);
    test.check(excludedScope.ok && excludedScope.complete && !excludedScope.found,
               "functional operand search excludes candidates outside its scope");

    const auto unary = netlist.runFunctionSearchQuery(
        patternQuery("y_not", GateType::NOT));
    test.check(unary.found && unary.matches.front().netIdB == -1 &&
                   unary.matches.front().netNameB.empty(),
               "unary pattern leaves legacy operand B fields empty");

    auto artifactQuery = patternQuery("y_or", GateType::OR);
    artifactQuery.mode = Netlist::FunctionSearchMode::FindAll;
    artifactQuery.maxResults = 0;
    artifactQuery.writeMatchesToFile = true;
    artifactQuery.outputFilePath = "mini test/test23/pattern_matches.txt";
    const auto artifact = netlist.runFunctionSearchQuery(artifactQuery);
    std::ifstream artifactInput(artifactQuery.outputFilePath);
    std::ostringstream artifactText;
    artifactText << artifactInput.rdbuf();
    test.check(artifact.ok && artifact.complete && artifact.wroteMatchesToFile &&
                   artifact.outputFilePath == artifactQuery.outputFilePath &&
                   artifactText.str().find("pattern: OR") != std::string::npos &&
                   artifactText.str().find("target: y_or") != std::string::npos &&
                   artifactText.str().find("operand_count: 2") != std::string::npos &&
                   artifactText.str().find("net_a:") != std::string::npos,
               "generic FindAll writes a complete self-describing artifact");
    artifactInput.close();
    std::remove(artifactQuery.outputFilePath.c_str());

    auto invalidPattern = patternQuery("y_and", GateType::DFF);
    const auto invalidPatternReport =
        netlist.runFunctionSearchQuery(invalidPattern);
    test.check(!invalidPatternReport.ok && invalidPatternReport.unsupported &&
                   invalidPatternReport.status == "UNSUPPORTED_PATTERN_TYPE",
               "unsupported functional pattern type is explicit");

    std::cout << "Summary: " << test.passed << " passed, "
              << test.failed << " failed.\n";
    return test.failed == 0 ? 0 : 1;
}
