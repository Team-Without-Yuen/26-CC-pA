#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <algorithm>
#include <iostream>
#include <limits>
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

bool loadCircuit(const std::string& path, Netlist& netlist) {
    VerilogReader reader;
    return reader.read(path, netlist);
}

} // namespace

int main(int argc, char** argv) {
    const std::string circuitPath =
        argc > 1 ? argv[1] : "mini test/test31/critical_path_circuit.v";
    TestReport tests;

    Netlist optimized;
    tests.check(loadCircuit(circuitPath, optimized), "critical-path circuit loads");
    const int originalDepth = optimized.findGlobalCriticalPath().depth;

    OptApplyRequest globalRequest;
    globalRequest.passKind = OptPassKind::CriticalPathDepth;
    globalRequest.scope = TargetScope::WHOLE_NETLIST;
    globalRequest.depthObjective = OptDepthObjective::GlobalMaximum;
    globalRequest.timeLimitSeconds = 30.0;
    globalRequest.requireDepthImprovement = true;

    const NetlistEditReport globalReport = optimized.runOptApply(globalRequest);
    tests.check(
        globalReport.success &&
        globalReport.changed &&
        !globalReport.rolledBack &&
        globalReport.depthChange.has_value() &&
        globalReport.depthChange->improved &&
        optimized.findGlobalCriticalPath().depth < originalDepth,
        "improving global-depth candidate is committed");
    tests.check(
        globalReport.validation.equivalenceChecked &&
        globalReport.validation.functionallyEquivalent &&
        globalReport.validation.equivalenceMethod ==
            EquivalenceCheckMethod::WholeDesignSat,
        "committed candidate passes mandatory whole-design SAT");
    tests.check(
        globalReport.depthOptimization.has_value() &&
        globalReport.depthOptimization->candidateAccepted &&
        globalReport.depthOptimization->wholeDesignEquivalent &&
        globalReport.depthOptimization->comparedOutputCount == 2 &&
        globalReport.depthOptimization->comparedDffDCount == 1,
        "depth optimization summary records acceptance and endpoint counts");

    Netlist noOpNetlist;
    const int const0 = noOpNetlist.addNet("1'b0");
    noOpNetlist.setNetConst(const0, true, 0);
    const int const1 = noOpNetlist.addNet("1'b1");
    noOpNetlist.setNetConst(const1, true, 1);
    noOpNetlist.addPrimaryInput("a");
    noOpNetlist.addPrimaryInput("b");
    noOpNetlist.addPrimaryOutput("y");
    const int andGate = noOpNetlist.addGate("opt_g0", GateType::AND);
    noOpNetlist.connectGateInput(andGate, noOpNetlist.getNetId("a"));
    noOpNetlist.connectGateInput(andGate, noOpNetlist.getNetId("b"));
    noOpNetlist.connectGateOutput(andGate, noOpNetlist.getNetId("y"));
    tests.check(noOpNetlist.validateStructure(), "no-op circuit is structurally valid");
    OptApplyRequest noOpRequest = globalRequest;
    noOpRequest.requireDepthImprovement = false;
    const NetlistEditReport noOpReport = noOpNetlist.runOptApply(noOpRequest);
    tests.check(
        noOpReport.success &&
        !noOpReport.changed &&
        !noOpReport.rolledBack &&
        noOpReport.validation.equivalenceChecked &&
        noOpReport.validation.functionallyEquivalent &&
        noOpReport.validation.equivalenceMethod ==
            EquivalenceCheckMethod::StructuralIdentity &&
        noOpReport.depthOptimization.has_value() &&
        !noOpReport.depthOptimization->candidateGenerated &&
        !noOpReport.depthOptimization->candidateAccepted &&
        !noOpReport.depthOptimization->wholeDesignEquivalenceChecked &&
        noOpReport.depthOptimization->wholeDesignEquivalent &&
        !noOpReport.depthOptimization->wholeDesignTimedOut,
        "unchanged optimizer result uses structural identity without whole-design SAT");

    Netlist impossibleTarget;
    tests.check(
        loadCircuit(circuitPath, impossibleTarget),
        "rollback circuit loads");
    const int rollbackDepth = impossibleTarget.findGlobalCriticalPath().depth;
    const size_t rollbackGateCount =
        impossibleTarget.collectNetlistStats().activeGateCount;

    OptApplyRequest impossibleRequest = globalRequest;
    impossibleRequest.targetDepth = 0;
    const NetlistEditReport impossibleReport =
        impossibleTarget.runOptApply(impossibleRequest);
    tests.check(
        !impossibleReport.success &&
        impossibleReport.rolledBack &&
        impossibleTarget.findGlobalCriticalPath().depth == rollbackDepth &&
        impossibleTarget.collectNetlistStats().activeGateCount == rollbackGateCount &&
        impossibleTarget.validateStructure(),
        "unmet targetDepth discards candidate and preserves original netlist");

    OptApplyRequest missingScopeRequest = globalRequest;
    missingScopeRequest.scope = TargetScope::NET_FANIN;
    missingScopeRequest.scopeName = "missing";
    const NetlistEditReport missingScopeReport =
        impossibleTarget.runOptApply(missingScopeRequest);
    tests.check(
        !missingScopeReport.success &&
        !missingScopeReport.changed &&
        !missingScopeReport.rolledBack,
        "missing optimization scope fails before mutation");

    Netlist requestValidation;
    tests.check(
        loadCircuit(circuitPath, requestValidation),
        "request-validation circuit loads");
    const int validationDepth = requestValidation.findGlobalCriticalPath().depth;
    const size_t validationGateCount =
        requestValidation.collectNetlistStats().activeGateCount;

    OptApplyRequest nanTimeRequest = globalRequest;
    nanTimeRequest.timeLimitSeconds =
        std::numeric_limits<double>::quiet_NaN();
    const NetlistEditReport nanTimeReport =
        requestValidation.runOptApply(nanTimeRequest);
    tests.check(
        !nanTimeReport.success &&
        !nanTimeReport.changed &&
        !nanTimeReport.rolledBack &&
        requestValidation.findGlobalCriticalPath().depth == validationDepth &&
        requestValidation.collectNetlistStats().activeGateCount == validationGateCount,
        "non-finite optimization time limit is rejected before mutation");

    OptApplyRequest invalidObjectiveRequest = globalRequest;
    invalidObjectiveRequest.depthObjective =
        static_cast<OptDepthObjective>(999);
    const NetlistEditReport invalidObjectiveReport =
        requestValidation.runOptApply(invalidObjectiveRequest);
    tests.check(
        !invalidObjectiveReport.success &&
        !invalidObjectiveReport.changed &&
        !invalidObjectiveReport.rolledBack,
        "invalid depth objective is rejected before mutation");

    OptApplyRequest invalidGateTypeRequest = globalRequest;
    invalidGateTypeRequest.allowedTypes = {static_cast<GateType>(999)};
    const NetlistEditReport invalidGateTypeReport =
        requestValidation.runOptApply(invalidGateTypeRequest);
    tests.check(
        !invalidGateTypeReport.success &&
        !invalidGateTypeReport.changed &&
        !invalidGateTypeReport.rolledBack,
        "invalid gate-type constraint is rejected before mutation");

    OptApplyRequest exhaustedBudgetRequest = globalRequest;
    exhaustedBudgetRequest.timeLimitSeconds = 1.0e-12;
    const NetlistEditReport exhaustedBudgetReport =
        requestValidation.runOptApply(exhaustedBudgetRequest);
    tests.check(
        !exhaustedBudgetReport.success &&
        !exhaustedBudgetReport.changed &&
        !exhaustedBudgetReport.rolledBack &&
        exhaustedBudgetReport.depthOptimization.has_value() &&
        exhaustedBudgetReport.depthOptimization->coreStatus == "TIMEOUT" &&
        !exhaustedBudgetReport.depthOptimization->candidateGenerated &&
        requestValidation.findGlobalCriticalPath().depth == validationDepth &&
        requestValidation.collectNetlistStats().activeGateCount == validationGateCount &&
        requestValidation.validateStructure(),
        "exhausted pre-core budget does not start an optimizer candidate");

    OptApplyRequest unsupportedLegacyRequest;
    unsupportedLegacyRequest.passKind = OptPassKind::CleanupBufferChain;
    unsupportedLegacyRequest.candidateIds = {0};
    unsupportedLegacyRequest.scope = TargetScope::NET_FANIN;
    unsupportedLegacyRequest.scopeName = "z";
    unsupportedLegacyRequest.allowedTypes = {GateType::AND};
    unsupportedLegacyRequest.targetDepth = 1;
    unsupportedLegacyRequest.validateEquivalence = true;
    const NetlistEditReport unsupportedLegacyReport =
        requestValidation.runOptApply(unsupportedLegacyRequest);
    tests.check(
        !unsupportedLegacyReport.success &&
        !unsupportedLegacyReport.changed &&
        !unsupportedLegacyReport.rolledBack &&
        unsupportedLegacyReport.message.find("candidateIds") != std::string::npos &&
        unsupportedLegacyReport.message.find("scope") != std::string::npos &&
        unsupportedLegacyReport.message.find("allowedTypes") != std::string::npos &&
        unsupportedLegacyReport.message.find("targetDepth") != std::string::npos &&
        unsupportedLegacyReport.message.find("validateEquivalence") != std::string::npos &&
        requestValidation.findGlobalCriticalPath().depth == validationDepth &&
        requestValidation.collectNetlistStats().activeGateCount == validationGateCount,
        "legacy pass rejects unsupported semantic fields before mutation");

    OptApplyRequest verboseLegacyRequest;
    verboseLegacyRequest.passKind = OptPassKind::CleanupBufferChain;
    verboseLegacyRequest.verbose = true;
    const NetlistEditReport verboseLegacyReport =
        requestValidation.runOptApply(verboseLegacyRequest);
    tests.check(
        verboseLegacyReport.success &&
        std::any_of(
            verboseLegacyReport.warnings.begin(),
            verboseLegacyReport.warnings.end(),
            [](const std::string& warning) {
                return warning.find("verbose") != std::string::npos;
            }),
        "legacy pass reports that verbose is unsupported");

    Netlist constrained;
    tests.check(
        loadCircuit(circuitPath, constrained),
        "DFF.Q constrained circuit loads");
    OptApplyRequest constrainedRequest = globalRequest;
    constrainedRequest.scope = TargetScope::NET_FANIN;
    constrainedRequest.scopeName = "q";
    constrainedRequest.allowedTypes = {GateType::NOR, GateType::NOT};
    const NetlistEditReport constrainedReport =
        constrained.runOptApply(constrainedRequest);
    tests.check(
        constrainedReport.success &&
        !constrainedReport.changed &&
        constrainedReport.depthOptimization.has_value() &&
        !constrainedReport.depthOptimization->resolvedThroughDffDataPin &&
        constrainedReport.depthOptimization->resolvedRootNetName == "q" &&
        constrainedReport.depthOptimization->finalConstraintsSatisfied &&
        constrainedReport.depthChange.has_value() &&
        constrainedReport.depthChange->beforeDepth == originalDepth &&
        constrainedReport.depthChange->afterDepth == originalDepth,
        "DFF.Q net-fanin constrained global optimization is a boundary no-op");

    Netlist dffGateConstrained;
    tests.check(
        loadCircuit(circuitPath, dffGateConstrained),
        "DFF gate constrained circuit loads");
    OptApplyRequest dffGateRequest = globalRequest;
    dffGateRequest.scope = TargetScope::GATE_FANIN;
    dffGateRequest.scopeName = "ff0";
    dffGateRequest.allowedTypes = {GateType::NOR, GateType::NOT};
    const NetlistEditReport dffGateReport =
        dffGateConstrained.runOptApply(dffGateRequest);
    tests.check(
        dffGateReport.success &&
        dffGateReport.validation.functionallyEquivalent &&
        dffGateReport.depthOptimization.has_value() &&
        dffGateReport.depthOptimization->resolvedThroughDffDataPin &&
        dffGateReport.depthOptimization->resolvedRootNetName == "d" &&
        dffGateReport.depthOptimization->finalConstraintsSatisfied,
        "explicit DFF gate-fanin constrained optimization resolves D-pin cone");

    Netlist scopedDepth;
    tests.check(
        loadCircuit(circuitPath, scopedDepth),
        "scoped-depth circuit loads");
    OptApplyRequest scopedDepthRequest = globalRequest;
    scopedDepthRequest.scope = TargetScope::NET_FANIN;
    scopedDepthRequest.scopeName = "q";
    scopedDepthRequest.depthObjective = OptDepthObjective::ScopedFaninCone;
    const NetlistEditReport scopedDepthReport =
        scopedDepth.runOptApply(scopedDepthRequest);
    tests.check(
        scopedDepthReport.success &&
        !scopedDepthReport.changed &&
        scopedDepthReport.depthChange.has_value() &&
        scopedDepthReport.depthChange->endpointName == "q" &&
        scopedDepthReport.depthChange->beforeDepth == 0 &&
        scopedDepthReport.depthChange->afterDepth == 0,
        "DFF.Q net-fanin scoped cone depth is already at boundary depth zero");

    std::cout << "Summary: " << tests.passed
              << " passed, " << tests.failed << " failed.\n";
    return tests.failed == 0 ? 0 : 1;
}
