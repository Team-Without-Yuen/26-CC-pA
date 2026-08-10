#include <chrono>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "include/SATEngine/Primitives.h"
#include "include/core/DesignAnalysisContext.h"

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

eqeng::Primitives::Config quietConfig() {
    eqeng::Primitives::Config config;
    config.verbose_rebuild = false;
    return config;
}

int addUnary(Netlist& netlist,
             const std::string& gateName,
             GateType type,
             const std::string& input,
             const std::string& output) {
    const int outputId = netlist.addNet(output);
    const int gateId = netlist.addGate(gateName, type);
    netlist.connectGateInput(gateId, netlist.getNetId(input));
    netlist.connectGateOutput(gateId, outputId);
    return outputId;
}

int addBinary(Netlist& netlist,
              const std::string& gateName,
              GateType type,
              const std::string& inputA,
              const std::string& inputB,
              const std::string& output) {
    const int outputId = netlist.addNet(output);
    const int gateId = netlist.addGate(gateName, type);
    netlist.connectGateInput(gateId, netlist.getNetId(inputA));
    netlist.connectGateInput(gateId, netlist.getNetId(inputB));
    netlist.connectGateOutput(gateId, outputId);
    return outputId;
}

Netlist makeFunctionCircuit() {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryInput("d");

    addBinary(netlist, "g_and", GateType::AND, "a", "b", "n_and");
    addUnary(netlist, "g_not_a", GateType::NOT, "a", "n_not_a");
    addUnary(netlist, "g_not_b", GateType::NOT, "b", "n_not_b");
    addBinary(netlist, "g_demorgan", GateType::NOR,
              "n_not_a", "n_not_b", "n_demorgan");
    addBinary(netlist, "g_zero", GateType::XOR, "a", "a", "n_zero");
    addBinary(netlist, "g_one", GateType::XNOR, "a", "a", "n_one");
    addBinary(netlist, "g_condition", GateType::OR, "a", "b", "n_condition");

    addUnary(netlist, "g_bus_a0", GateType::BUF, "a", "bus_a[0]");
    addUnary(netlist, "g_bus_a1", GateType::BUF, "n_and", "bus_a[1]");
    addUnary(netlist, "g_bus_b0", GateType::BUF, "a", "bus_b[0]");
    addUnary(netlist, "g_bus_b1", GateType::BUF, "n_demorgan", "bus_b[1]");

    const int qId = netlist.addNet("q");
    const int dffId = netlist.addGate("ff0", GateType::DFF);
    netlist.connectGateInput(dffId, netlist.getNetId("d"), "D");
    netlist.connectGateOutput(dffId, qId);
    return netlist;
}

Netlist makeMixedGateCircuit(std::vector<std::string>& signalNames) {
    Netlist netlist;
    for (int i = 0; i < 5; ++i) {
        const std::string name = "p" + std::to_string(i);
        netlist.addPrimaryInput(name);
        signalNames.push_back(name);
    }

    const std::vector<GateType> binaryTypes = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
        GateType::XOR, GateType::XNOR};
    uint32_t state = 0x5a17u;
    auto nextIndex = [&](size_t limit) {
        state = state * 1664525u + 1013904223u;
        return static_cast<size_t>(state % static_cast<uint32_t>(limit));
    };

    for (int i = 0; i < 40; ++i) {
        const std::string output = "m" + std::to_string(i);
        if (i % 7 == 0) {
            addUnary(netlist, "mu" + std::to_string(i), GateType::NOT,
                     signalNames[nextIndex(signalNames.size())], output);
        } else if (i % 11 == 0) {
            addUnary(netlist, "mu" + std::to_string(i), GateType::BUF,
                     signalNames[nextIndex(signalNames.size())], output);
        } else {
            const std::string inputA =
                signalNames[nextIndex(signalNames.size())];
            const std::string inputB =
                signalNames[nextIndex(signalNames.size())];
            addBinary(netlist, "mb" + std::to_string(i),
                      binaryTypes[static_cast<size_t>(i) % binaryTypes.size()],
                      inputA, inputB, output);
        }
        signalNames.push_back(output);
    }
    return netlist;
}

bool sameCoreResult(const FunctionReport& oldReport,
                    const FunctionReport& aigReport) {
    return oldReport.ok == aigReport.ok &&
           oldReport.exists == aigReport.exists &&
           oldReport.equivalent == aigReport.equivalent &&
           oldReport.canBeZero == aigReport.canBeZero &&
           oldReport.canBeOne == aigReport.canBeOne &&
           oldReport.isConstant == aigReport.isConstant &&
           oldReport.constValue == aigReport.constValue &&
           oldReport.status == aigReport.status;
}

FunctionQuery makeQuery(FunctionQueryType type,
                        const std::string& netA,
                        const std::string& netB = "") {
    FunctionQuery query;
    query.type = type;
    query.netNameA = netA;
    query.netNameB = netB;
    query.timeLimitSeconds = 10.0;
    return query;
}

void checkDifferential(TestReport& report,
                       DesignAnalysisContext& context,
                       const FunctionQuery& query,
                       const std::string& name) {
    const FunctionReport oldReport = context.netlist().runFunctionQuery(query);
    const FunctionReport aigReport = context.runFunctionQuery(query);
    report.check(sameCoreResult(oldReport, aigReport), name);
}

void testFirstBatchDifferential(TestReport& report) {
    DesignAnalysisContext context(
        makeFunctionCircuit(), eqeng::AigModel::Options{}, quietConfig());

    checkDifferential(report, context,
                      makeQuery(FunctionQueryType::Equivalence,
                                "n_and", "n_demorgan"),
                      "equivalent scalar nets match legacy report");
    checkDifferential(report, context,
                      makeQuery(FunctionQueryType::Equivalence, "a", "b"),
                      "non-equivalent scalar nets match legacy report");
    checkDifferential(report, context,
                      makeQuery(FunctionQueryType::Equivalence,
                                "bus_a", "bus_b"),
                      "bus equivalence matches legacy report");

    FunctionQuery canBeOne =
        makeQuery(FunctionQueryType::CanBeValue, "n_zero");
    canBeOne.constValue = 1;
    checkDifferential(report, context, canBeOne,
                      "CanBeValue matches legacy report");

    FunctionQuery constantOne =
        makeQuery(FunctionQueryType::ConstantFunction, "n_one");
    constantOne.constValue = 1;
    checkDifferential(report, context, constantOne,
                      "ConstantFunction matches legacy report");

    checkDifferential(report, context,
                      makeQuery(FunctionQueryType::AlwaysZero, "n_zero"),
                      "AlwaysZero matches legacy report");
    checkDifferential(report, context,
                      makeQuery(FunctionQueryType::AlwaysOne, "n_one"),
                      "AlwaysOne matches legacy report");
    checkDifferential(report, context,
                      makeQuery(FunctionQueryType::TruthStatus, "n_and"),
                      "non-constant TruthStatus matches legacy report");
    checkDifferential(report, context,
                      makeQuery(FunctionQueryType::TruthStatus, "q"),
                      "DFF.Q boundary TruthStatus matches legacy report");

    FunctionQuery conditional = makeQuery(
        FunctionQueryType::ConditionalEquivalence,
        "n_condition", "n_one");
    conditional.conditionNetName = "n_condition";
    conditional.conditionValue = 1;
    checkDifferential(report, context, conditional,
                      "internal-net ConditionalEquivalence matches legacy report");
}

void testMixedGateDifferential(TestReport& report) {
    std::vector<std::string> signalNames;
    DesignAnalysisContext context(
        makeMixedGateCircuit(signalNames),
        eqeng::AigModel::Options{}, quietConfig());

    bool pairReportsMatch = true;
    for (size_t i = 0; i < 64; ++i) {
        const std::string& a = signalNames[(i * 7 + 3) % signalNames.size()];
        const std::string& b = signalNames[(i * 13 + 5) % signalNames.size()];
        const FunctionQuery query =
            makeQuery(FunctionQueryType::Equivalence, a, b);
        pairReportsMatch = pairReportsMatch && sameCoreResult(
            context.netlist().runFunctionQuery(query),
            context.runFunctionQuery(query));
    }
    report.check(pairReportsMatch,
                 "mixed-gate equivalence reports match legacy backend");

    bool truthReportsMatch = true;
    for (const std::string& signalName : signalNames) {
        const FunctionQuery query =
            makeQuery(FunctionQueryType::TruthStatus, signalName);
        truthReportsMatch = truthReportsMatch && sameCoreResult(
            context.netlist().runFunctionQuery(query),
            context.runFunctionQuery(query));
    }
    report.check(truthReportsMatch,
                 "mixed-gate truth-status reports match legacy backend");
}

void testSharedLifecycle(TestReport& report) {
    DesignAnalysisContext context(
        makeFunctionCircuit(), eqeng::AigModel::Options{}, quietConfig());
    FunctionQuery initial =
        makeQuery(FunctionQueryType::Equivalence, "n_and", "n_demorgan");
    const FunctionReport initialReport = context.runFunctionQuery(initial);
    const uint32_t generationBefore = context.primitives().generation();
    const uint64_t rebuildsBefore = context.primitives().stats().rebuilds;
    const eqeng::SigRef staleA = context.primitives().resolve("a");

    addUnary(context.netlist(), "g_copy", GateType::BUF, "a", "n_copy");
    FunctionQuery afterEdit =
        makeQuery(FunctionQueryType::Equivalence, "a", "n_copy");
    const FunctionReport afterEditReport = context.runFunctionQuery(afterEdit);

    report.check(initialReport.ok && initialReport.equivalent &&
                     afterEditReport.ok && afterEditReport.equivalent,
                 "one context answers before and after an edit");
    report.check(context.primitives().generation() > generationBefore &&
                     context.primitives().stats().rebuilds == rebuildsBefore + 1,
                 "dirty edit triggers exactly one lazy AIG rebuild");

    bool staleRejected = false;
    try {
        const eqeng::SigRef currentA = context.primitives().resolve("a");
        (void)context.primitives().equiv_checked(staleA, currentA);
    } catch (const eqeng::StaleSignal&) {
        staleRejected = true;
    }
    report.check(staleRejected, "old SigRef is rejected after rebuild");
}

void testInvalidModel(TestReport& report) {
    Netlist invalid;
    invalid.addPrimaryInput("a");
    invalid.addPrimaryOutput("y");
    const int gateId = invalid.addGate("broken_and", GateType::AND);
    invalid.connectGateInput(gateId, invalid.getNetId("a"));
    invalid.connectGateOutput(gateId, invalid.getNetId("y"));

    DesignAnalysisContext context(
        std::move(invalid), eqeng::AigModel::Options{}, quietConfig());
    const FunctionReport result = context.runFunctionQuery(
        makeQuery(FunctionQueryType::AlwaysZero, "y"));
    report.check(!result.ok && result.unsupported &&
                     result.status == "UNSUPPORTED" &&
                     result.solverStatus == "UNSUPPORTED",
                 "invalid AIG model cannot produce a Boolean proof");
}

void testInterruptibleTimeout(TestReport& report) {
    DesignAnalysisContext context(
        makeFunctionCircuit(), eqeng::AigModel::Options{}, quietConfig());
    (void)context.primitives().model_health();
    const eqeng::SigRef a = context.primitives().resolve("a");
    const eqeng::SigRef b = context.primitives().resolve("b");

    const auto startedAt = std::chrono::steady_clock::now();
    const eqeng::EquivResult timed =
        context.primitives().equiv_checked(a, b, 1e-12);
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startedAt).count();
    report.check(timed == eqeng::EquivResult::Unknown &&
                     context.primitives().last_proof_timed_out() &&
                     elapsed < 0.5,
                 "timed Phase-A proof returns Unknown promptly at deadline");

    const eqeng::EquivResult conclusive =
        context.primitives().equiv_checked(a, b, 10.0);
    report.check(conclusive == eqeng::EquivResult::NotEqual &&
                     !context.primitives().last_proof_timed_out(),
                 "a later proof remains conclusive after a timeout");

    FunctionQuery query =
        makeQuery(FunctionQueryType::Equivalence, "a", "b");
    query.timeLimitSeconds = 1e-12;
    const FunctionReport adapterReport = context.runFunctionQuery(query);
    report.check(!adapterReport.ok && adapterReport.solverTimedOut &&
                     adapterReport.status == "SOLVER_TIMEOUT",
                 "Function adapter preserves timed proof as SOLVER_TIMEOUT");
}

void testLegacyFallback(TestReport& report) {
    DesignAnalysisContext context(
        makeFunctionCircuit(), eqeng::AigModel::Options{}, quietConfig());
    FunctionQuery query =
        makeQuery(FunctionQueryType::PrimaryInputsOfNet, "n_and");
    const FunctionReport direct = context.netlist().runFunctionQuery(query);
    const FunctionReport throughContext = context.runFunctionQuery(query);
    report.check(direct.ok == throughContext.ok &&
                     direct.status == throughContext.status &&
                     direct.supportPrimaryInputs ==
                         throughContext.supportPrimaryInputs,
                 "unmigrated Function Query mode keeps legacy behavior");
}

} // namespace

int main() {
    TestReport report;
    testFirstBatchDifferential(report);
    testMixedGateDifferential(report);
    testSharedLifecycle(report);
    testInvalidModel(report);
    testInterruptibleTimeout(report);
    testLegacyFallback(report);

    std::cout << "AIG Function adapter regression: " << report.passed
              << " passed, " << report.failed << " failed\n";
    return report.failed == 0 ? 0 : 1;
}
