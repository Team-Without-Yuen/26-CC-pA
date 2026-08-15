#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

namespace {

struct TestReport {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name) {
        if (condition) {
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } else {
            ++failed;
            std::cout << "[FAIL] " << name << '\n';
        }
    }
};

std::string readAll(const std::string& path) {
    std::ifstream input(path);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void addNamedGate(Netlist& netlist,
                  const std::string& name,
                  GateType type,
                  const std::vector<std::string>& inputs,
                  const std::string& output) {
    const int outputId = netlist.addNet(output);
    const int gateId = netlist.addGate(name, type);
    for (const std::string& input : inputs) {
        netlist.connectGateInput(gateId, netlist.getNetId(input));
    }
    netlist.connectGateOutput(gateId, outputId);
}

void testSmallFunctionQueryArtifact(TestReport& report) {
    const std::string path = "mini test/test46/small_equations.txt";
    Netlist netlist;
    VerilogReader reader;
    report.check(reader.read("mini test/test4/function_expr_circuit.v", netlist),
                 "load expression fixture");

    FunctionQuery query;
    query.type = FunctionQueryType::BooleanExpression;
    query.netNameA = "y";
    query.writeExpressionToFile = true;
    query.expressionOutputFilePath = path;
    query.timeLimitSeconds = 5.0;
    const FunctionReport result = netlist.runFunctionQuery(query);
    const std::string artifact = readAll(path);

    report.check(result.ok && result.exists &&
                     result.status == "BOOLEAN_EQUATION_ARTIFACT" &&
                     result.wroteExpressionToFile &&
                     result.expressionArtifactComplete &&
                     !result.expressionArtifactTimedOut &&
                     result.expressionEquationCount == 3 &&
                     result.expressionBoundaryCount == 3 &&
                     result.expressionArtifactFormat == "NAMED_DAG_EQUATIONS_V1",
                 "FunctionReport exposes complete artifact metadata");
    report.check(artifact.find("n_and = AND(a, b)") != std::string::npos &&
                     artifact.find("n_not = NOT(c)") != std::string::npos &&
                     artifact.find("y = OR(n_and, n_not)") != std::string::npos &&
                     artifact.find("Complete: yes") != std::string::npos,
                 "small artifact is complete and self-contained");
    std::remove(path.c_str());
}

Netlist makeDeepChain(size_t gateCount) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    std::string input = "a";
    for (size_t index = 0; index < gateCount; ++index) {
        const std::string output = "deep_" + std::to_string(index);
        addNamedGate(netlist, "g_" + output,
                     (index & 1U) ? GateType::BUF : GateType::NOT,
                     {input}, output);
        input = output;
    }
    return netlist;
}

void testDeepChainAndDeadline(TestReport& report) {
    constexpr size_t kGateCount = 5000;
    const std::string completePath = "mini test/test46/deep_equations.txt";
    const std::string timeoutPath = "mini test/test46/timeout_equations.txt";
    Netlist netlist = makeDeepChain(kGateCount);

    const auto startedAt = std::chrono::steady_clock::now();
    const auto complete = netlist.writeBooleanEquationArtifact(
        "deep_4999", completePath, 5.0);
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startedAt).count();
    const std::string artifact = readAll(completePath);
    report.check(complete.ok && complete.complete && !complete.timedOut &&
                     complete.equationCount == kGateCount &&
                     complete.boundaryCount == 1 &&
                     artifact.find("deep_4999 = BUF(deep_4998)") != std::string::npos &&
                     artifact.find("Complete: yes") != std::string::npos,
                 "5000-level chain has no expression depth cap");
    report.check(elapsed < 5.0, "5000-level chain completes within request budget");

    const auto timedOut = netlist.writeBooleanEquationArtifact(
        "deep_4999", timeoutPath, 1.0e-9);
    const std::string timeoutArtifact = readAll(timeoutPath);
    report.check(!timedOut.ok && !timedOut.complete && timedOut.timedOut &&
                     timeoutArtifact.find("Complete: no") != std::string::npos &&
                     timeoutArtifact.find("request deadline exceeded") != std::string::npos,
                 "deadline produces an explicit incomplete artifact");
    std::remove(completePath.c_str());
    std::remove(timeoutPath.c_str());
}

void testReconvergentDag(TestReport& report) {
    constexpr size_t kGateCount = 80;
    const std::string path = "mini test/test46/reconvergent_equations.txt";
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    std::string previous2 = "a";
    std::string previous1 = "b";
    for (size_t index = 0; index < kGateCount; ++index) {
        const std::string output = "fib_" + std::to_string(index);
        addNamedGate(netlist, "g_" + output,
                     (index & 1U) ? GateType::OR : GateType::AND,
                     {previous1, previous2}, output);
        previous2 = previous1;
        previous1 = output;
    }

    const auto result = netlist.writeBooleanEquationArtifact(
        "fib_79", path, 5.0);
    const std::string artifact = readAll(path);
    report.check(result.ok && result.complete &&
                     result.equationCount == kGateCount &&
                     result.boundaryCount == 2 &&
                     artifact.size() < 10000 &&
                     artifact.find("fib_79 = OR(fib_78, fib_77)") != std::string::npos,
                 "reconvergent DAG stays linear and complete");
    std::remove(path.c_str());
}

void testDffBoundary(TestReport& report) {
    const std::string path = "mini test/test46/dff_equations.txt";
    Netlist netlist;
    VerilogReader reader;
    if (!reader.read("mini test/test4/function_expr_circuit.v", netlist)) {
        report.check(false, "reload DFF fixture");
        return;
    }
    const auto result = netlist.writeBooleanEquationArtifact(
        "q_expr", path, 5.0);
    const std::string artifact = readAll(path);
    report.check(result.ok && result.complete && result.equationCount == 1 &&
                     artifact.find("q : DFF_Q") != std::string::npos &&
                     artifact.find("q_expr = OR(q, a)") != std::string::npos,
                 "DFF.Q is an explicit sequential boundary");
    std::remove(path.c_str());
}

} // namespace

int main() {
    TestReport report;
    testSmallFunctionQueryArtifact(report);
    testDeepChainAndDeadline(report);
    testReconvergentDag(report);
    testDffBoundary(report);
    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
