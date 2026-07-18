#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <unordered_set>

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

struct FlowResult {
    bool ok = false;
    NetlistEditReport mapping;
    NetlistEditReport simplification;
    NetlistEditReport collapse;
    WholeDesignEquivalenceReport equivalence;
    SequentialPatternReportSet sequential;
    double mappingSeconds = 0.0;
    double simplificationSeconds = 0.0;
    double collapseSeconds = 0.0;
    double equivalenceSeconds = 0.0;
    double sequentialSeconds = 0.0;
    bool nandNotOnly = false;
};

template <typename Function>
auto timed(Function&& function, double& seconds) {
    const auto start = std::chrono::steady_clock::now();
    auto result = function();
    seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    return result;
}

bool usesOnlyNandNotDff(const Netlist& netlist) {
    for (size_t i = 0; i < netlist.getGateCount(); ++i) {
        const int gateId = static_cast<int>(i);
        if (!netlist.isValidGateId(gateId) || netlist.isGateRemoved(gateId)) {
            continue;
        }
        const GateType type = netlist.getGate(gateId).type;
        if (type != GateType::NAND && type != GateType::NOT && type != GateType::DFF) {
            return false;
        }
    }
    return true;
}

FlowResult runFlow(Netlist& current, const Netlist& original, bool printProgress) {
    FlowResult result;

    EditApplyRequest mappingRequest;
    mappingRequest.kind = EditCommandKind::ConvertToBasis;
    mappingRequest.scope = TargetScope::WHOLE_NETLIST;
    mappingRequest.allowedTypes = {GateType::NAND, GateType::NOT};
    mappingRequest.validateEquivalence = true;
    result.mapping = timed(
        [&]() { return current.runEditApply(mappingRequest); },
        result.mappingSeconds);
    if (printProgress) {
        std::cout << "mapping_success=" << result.mapping.success
                  << " changed=" << result.mapping.changed
                  << " rolled_back=" << result.mapping.rolledBack
                  << " seconds=" << result.mappingSeconds << std::endl;
    }
    if (!result.mapping.success || result.mapping.rolledBack) {
        return result;
    }

    EditApplyRequest simplifyRequest;
    simplifyRequest.kind = EditCommandKind::SimplifyConstants;
    simplifyRequest.gateType = GateType::NAND;
    simplifyRequest.constValue = 1;
    simplifyRequest.inputCount = 2;
    simplifyRequest.validateEquivalence = true;
    result.simplification = timed(
        [&]() { return current.runEditApply(simplifyRequest); },
        result.simplificationSeconds);
    if (printProgress) {
        std::cout << "simplify_success=" << result.simplification.success
                  << " changed=" << result.simplification.changed
                  << " rolled_back=" << result.simplification.rolledBack
                  << " seconds=" << result.simplificationSeconds << std::endl;
    }
    if (!result.simplification.success || result.simplification.rolledBack) {
        return result;
    }

    EditApplyRequest collapseRequest;
    collapseRequest.kind = EditCommandKind::CollapseDoubleInverter;
    collapseRequest.validateEquivalence = true;
    result.collapse = timed(
        [&]() { return current.runEditApply(collapseRequest); },
        result.collapseSeconds);
    if (printProgress) {
        std::cout << "collapse_success=" << result.collapse.success
                  << " changed=" << result.collapse.changed
                  << " rolled_back=" << result.collapse.rolledBack
                  << " seconds=" << result.collapseSeconds << std::endl;
    }
    if (!result.collapse.success || result.collapse.rolledBack) {
        return result;
    }

    result.nandNotOnly = usesOnlyNandNotDff(current);
    result.equivalence = timed(
        [&]() { return current.checkWholeDesignEquivalence(original, 240.0); },
        result.equivalenceSeconds);
    if (printProgress) {
        std::cout << "equivalence_ok=" << result.equivalence.ok
                  << " equivalent=" << result.equivalence.equivalent
                  << " compared_po=" << result.equivalence.comparedOutputCount
                  << " compared_dffd=" << result.equivalence.comparedDffDCount
                  << " seconds=" << result.equivalenceSeconds << std::endl;
    }
    if (!result.equivalence.ok || !result.equivalence.equivalent) {
        return result;
    }

    SequentialPatternQuery sequentialQuery;
    sequentialQuery.type = SequentialPatternQueryType::DffEnableHold;
    result.sequential = timed(
        [&]() { return current.runSequentialPatternQuery(sequentialQuery); },
        result.sequentialSeconds);
    if (printProgress) {
        std::cout << "sequential_status=" << result.sequential.status
                  << " analyzed=" << result.sequential.analyzedDffCount
                  << " matched=" << result.sequential.matchedDffCount
                  << " candidates=" << result.sequential.candidateDffCount
                  << " seconds=" << result.sequentialSeconds << std::endl;
    }

    result.ok = result.nandNotOnly && result.sequential.ok;
    return result;
}

bool containsName(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool hasSatProvenPattern(const Netlist& netlist, const std::string& dffName) {
    SequentialPatternQuery query;
    query.dffName = dffName;
    query.verifyCanonicalMatchesWithSat = true;
    const SequentialPatternReportSet report = netlist.runSequentialPatternQuery(query);
    if (!report.ok || report.matchedDffCount != 1 || report.reports.size() != 1) {
        return false;
    }
    for (const DffInputPattern& pattern : report.reports.front().patterns) {
        if (pattern.confirmed && pattern.holdFunctionallyProven &&
            pattern.loadFunctionallyProven && pattern.solverStatus == "PROVEN") {
            return true;
        }
    }
    return false;
}

void printDriverCone(const Netlist& netlist,
                     int netId,
                     int remainingDepth,
                     std::unordered_set<int>& visited,
                     const std::string& indent = "") {
    if (!netlist.isValidNetId(netId)) {
        std::cout << indent << "invalid net\n";
        return;
    }

    const Net& net = netlist.getNet(netId);
    std::cout << indent << "net " << net.name;
    if (remainingDepth <= 0 || net.driverGateId < 0 ||
        !netlist.isValidGateId(net.driverGateId) ||
        netlist.isGateRemoved(net.driverGateId)) {
        std::cout << "\n";
        return;
    }

    const Gate& driver = netlist.getGate(net.driverGateId);
    std::cout << " <- gate " << driver.instName
              << " type=" << static_cast<int>(driver.type) << "\n";
    if (!visited.insert(driver.id).second) {
        std::cout << indent << "  (already shown)\n";
        return;
    }
    for (int inputNetId : driver.inputNetIds) {
        printDriverCone(netlist, inputNetId, remainingDepth - 1, visited, indent + "  ");
    }
}

void printDffInputCones(const Netlist& netlist) {
    for (size_t i = 0; i < netlist.getGateCount(); ++i) {
        const int gateId = static_cast<int>(i);
        if (!netlist.isValidGateId(gateId) || netlist.isGateRemoved(gateId)) {
            continue;
        }
        const Gate& gate = netlist.getGate(gateId);
        if (gate.type != GateType::DFF || gate.inputNetIds.empty()) {
            continue;
        }
        std::cout << "[DIAG] " << gate.instName << ".D\n";
        std::unordered_set<int> visited;
        printDriverCone(netlist, gate.inputNetIds.front(), 5, visited, "  ");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        Netlist current;
        VerilogReader reader;
        if (!reader.read(argv[1], current)) {
            std::cerr << "Failed to load: " << argv[1] << "\n";
            return 1;
        }
        const Netlist original = current;
        const FlowResult flow = runFlow(current, original, true);
        std::cout << "flow_ok=" << flow.ok << "\n";
        return flow.ok ? 0 : 1;
    }

    TestReport tests;
    VerilogReader reader;
    Netlist original;
    Netlist wrongD;
    tests.check(
        reader.read("mini test/test20/sequential_flow_circuit.v", original),
        "load original sequential flow circuit");
    tests.check(
        reader.read("mini test/test20/sequential_flow_wrong_d.v", wrongD),
        "load DFF.D mismatch circuit");

    const WholeDesignEquivalenceReport same =
        original.checkWholeDesignEquivalence(original, 30.0);
    tests.check(same.ok && same.equivalent && same.comparedOutputCount == 5 &&
                    same.comparedDffDCount == 3,
                "whole-design equivalence compares PO and all DFF.D endpoints");

    const WholeDesignEquivalenceReport mismatch =
        wrongD.checkWholeDesignEquivalence(original, 30.0);
    tests.check(mismatch.ok && !mismatch.equivalent &&
                    containsName(mismatch.mismatchedDffDNames, "ff_high.D") &&
                    mismatch.mismatchedOutputNames.empty(),
                "DFF.D mismatch is detected even when primary outputs match");

    Netlist current = original;
    const SequentialPatternReportSet before = current.runSequentialPatternQuery({});
    const FlowResult flow = runFlow(current, original, false);
    tests.check(before.ok && before.matchedDffCount == 2,
                "two canonical feedback MUXes exist before edits");
    tests.check(flow.mapping.success && !flow.mapping.rolledBack,
                "whole design converts to NAND/NOT");
    tests.check(flow.simplification.success && !flow.simplification.rolledBack,
                "constant-1 NAND simplification succeeds");
    tests.check(flow.collapse.success && !flow.collapse.rolledBack,
                "double-inverter collapse succeeds");
    tests.check(flow.nandNotOnly,
                "current design contains only NAND, NOT and DFF gates");
    tests.check(flow.equivalence.ok && flow.equivalence.equivalent &&
                    flow.equivalence.comparedDffDCount == 3,
                "edited design remains equivalent at PO and DFF.D boundaries");
    if (!flow.sequential.ok || flow.sequential.matchedDffCount != 2) {
        printDffInputCones(current);
    }
    tests.check(flow.sequential.ok && flow.sequential.matchedDffCount == 2,
                "canonical enable/hold patterns remain detectable after edits");
    tests.check(hasSatProvenPattern(current, "ff_high") &&
                    hasSatProvenPattern(current, "ff_low"),
                "mapped enable/hold patterns pass conditional SAT verification");
    tests.check(flow.ok, "complete sequential test40-style flow succeeds");

    std::cout << "Summary: " << tests.passed << " passed, "
              << tests.failed << " failed.\n";
    return tests.failed == 0 ? 0 : 1;
}
