#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

namespace {

using Clock = std::chrono::steady_clock;

struct QueryCase {
    std::string testcase;
    std::string verilogPath;
    int promptNumber = 0;
    FunctionQuery query;
    std::string prompt;
    std::string expectedStatus;
};

std::string yesNo(bool value) {
    return value ? "yes" : "no";
}

FunctionQuery dependenceQuery(const std::string& target,
                              const std::string& input) {
    FunctionQuery query;
    query.type = FunctionQueryType::FunctionalDependence;
    query.netNameA = target;
    query.netNameB = input;
    query.timeLimitSeconds = 30.0;
    return query;
}

FunctionQuery symmetryQuery(const std::string& target,
                            const std::string& inputA,
                            const std::string& inputB) {
    FunctionQuery query;
    query.type = FunctionQueryType::Symmetry;
    query.netNameA = target;
    query.symmetryInputNameA = inputA;
    query.symmetryInputNameB = inputB;
    query.timeLimitSeconds = 30.0;
    return query;
}

FunctionQuery withTimeLimit(FunctionQuery query, double seconds) {
    query.timeLimitSeconds = seconds;
    return query;
}

std::vector<QueryCase> makeCases() {
    return {
        {"test33", "NewTestCase/test33/test33.v", 12,
         dependenceQuery("n8", "n1"),
         "Does output n8 depend on input n1? Report yes or no.",
         "FUNCTIONALLY_INDEPENDENT"},
        {"test36", "NewTestCase/test36/test36.v", 10,
         symmetryQuery("n11", "n3", "n9[0]"),
         "Check whether the function at n11 is symmetric with respect to inputs n3 and n9[0].",
         "SYMMETRIC"},
        {"test37", "NewTestCase/test37/test37.v", 16,
         symmetryQuery("n8", "n3", "n4[0]"),
         "Check whether the function at n8 is symmetric with respect to inputs n3 and n4[0].",
         "SYMMETRIC"},
        {"dependence_sat", "mini test/test17/functional_dependence_circuit.v", 0,
         dependenceQuery("dependent", "a"),
         "SAT-active positive dependence case.",
         "FUNCTIONALLY_DEPENDENT"},
        {"dependence_unsat", "mini test/test17/functional_dependence_circuit.v", 0,
         dependenceQuery("cancelled", "a"),
         "UNSAT-active functional cancellation case.",
         "FUNCTIONALLY_INDEPENDENT"},
        {"symmetry_unsat", "mini test/test22/symmetry_circuit.v", 0,
         symmetryQuery("sym_and", "a", "b"),
         "UNSAT-active symmetric case.",
         "SYMMETRIC"},
        {"symmetry_sat", "mini test/test22/symmetry_circuit.v", 0,
         symmetryQuery("asym", "a", "b"),
         "SAT-active asymmetric case.",
         "NOT_SYMMETRIC"},
        {"dependence_invalid_limit", "mini test/test17/functional_dependence_circuit.v", 0,
         withTimeLimit(dependenceQuery("dependent", "a"), 0.0),
         "FunctionalDependence rejects a non-positive SAT budget.",
         "INVALID_ARGUMENT"},
        {"symmetry_invalid_limit", "mini test/test22/symmetry_circuit.v", 0,
         withTimeLimit(symmetryQuery("sym_and", "a", "b"), 0.0),
         "Symmetry rejects a non-positive SAT budget.",
         "INVALID_ARGUMENT"}
    };
}

template<typename Fn>
FunctionReport timedRun(Fn&& function, double& seconds) {
    const auto startedAt = Clock::now();
    FunctionReport report = function();
    seconds = std::chrono::duration<double>(Clock::now() - startedAt).count();
    return report;
}

} // namespace

int main() {
    const std::filesystem::path outputPath =
        "mini test/test39/dependence_symmetry_results.txt";
    std::ofstream output(outputPath);
    if (!output) {
        std::cerr << "Failed to open result file: " << outputPath.string() << '\n';
        return 2;
    }

    const std::vector<QueryCase> cases = makeCases();
    size_t expectedStatusMatches = 0;
    size_t timeoutCount = 0;
    double elapsedSecondsTotal = 0.0;
    bool passed = true;

    for (const QueryCase& item : cases) {
        VerilogReader reader;
        Netlist netlist;
        if (!reader.read(item.verilogPath, netlist)) {
            output << "TESTCASE " << item.testcase << " LOAD_FAILED\n\n";
            passed = false;
            continue;
        }

        double elapsedSeconds = 0.0;
        const FunctionReport report = timedRun(
            [&]() { return netlist.runFunctionQuery(item.query); },
            elapsedSeconds);

        const bool expectedStatus = report.status == item.expectedStatus;
        expectedStatusMatches += expectedStatus ? 1u : 0u;
        timeoutCount += report.solverTimedOut ? 1u : 0u;
        elapsedSecondsTotal += elapsedSeconds;
        passed = passed && expectedStatus;

        output << "TESTCASE " << item.testcase
               << " PROMPT #" << item.promptNumber << '\n'
               << "  text: " << item.prompt << '\n'
               << "  query_type: "
               << (item.query.type == FunctionQueryType::FunctionalDependence
                       ? "FunctionalDependence"
                       : "Symmetry") << '\n'
               << "  status: " << report.status << '\n'
               << "  expected_status: " << item.expectedStatus << '\n'
               << "  expected_status_matches: " << yesNo(expectedStatus) << '\n'
               << "  solver_status: " << report.solverStatus << '\n'
               << "  answer: " << yesNo(report.exists) << '\n'
               << "  depends_on_input: " << yesNo(report.dependsOnInput) << '\n'
               << "  symmetric: " << yesNo(report.symmetric) << '\n'
               << "  input_in_structural_support: "
               << yesNo(report.inputInStructuralSupport) << '\n'
               << "  symmetry_input_a_in_structural_support: "
               << yesNo(report.symmetryInputAInStructuralSupport) << '\n'
               << "  symmetry_input_b_in_structural_support: "
               << yesNo(report.symmetryInputBInStructuralSupport) << '\n'
               << "  timeout: " << yesNo(report.solverTimedOut) << '\n'
               << "  elapsed_seconds: " << std::fixed << std::setprecision(6)
               << elapsedSeconds << "\n\n";
    }

    output << "SUMMARY\n"
           << "  queries=" << cases.size() << '\n'
           << "  expected_status_matches=" << expectedStatusMatches << '\n'
           << "  timeouts=" << timeoutCount << '\n'
           << "  elapsed_seconds=" << std::fixed << std::setprecision(6)
           << elapsedSecondsTotal << '\n'
           << "  result=" << (passed ? "PASS" : "FAIL") << '\n';
    output.close();

    std::cout << "Dependence/symmetry cases: " << expectedStatusMatches
              << '/' << cases.size() << " expected statuses, result="
              << (passed ? "PASS" : "FAIL") << '\n'
              << "Details: " << outputPath.string() << '\n';
    return passed ? 0 : 1;
}
