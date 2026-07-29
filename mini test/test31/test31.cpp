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
