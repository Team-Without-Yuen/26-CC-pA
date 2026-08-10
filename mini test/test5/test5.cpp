#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <algorithm>
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

bool containsGate(const Netlist::CombinationalPath& path, int gateId) {
    return std::find(path.gateIds.begin(), path.gateIds.end(), gateId) != path.gateIds.end();
}

bool allCriticalPathsEmpty(const Netlist::DepthReportSet& result) {
    if (!result.worst.criticalPath.gateIds.empty() ||
        !result.worst.criticalPath.netIds.empty()) {
        return false;
    }
    for (const Netlist::DepthReport& item : result.reports) {
        if (!item.criticalPath.gateIds.empty() ||
            !item.criticalPath.netIds.empty()) {
            return false;
        }
    }
    return true;
}

bool loadCircuit(Netlist& netlist) {
    VerilogReader reader;
    return reader.read("mini test/test5/depth_wrapper_circuit.v", netlist);
}

void testGateOnCriticalPath(TestReport& report, const Netlist& netlist) {
    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::GateOnCriticalPath;
    query.gateName = "g0";

    Netlist::DepthReportSet result = netlist.runDepthQuery(query);
    const int g0 = netlist.getGateId("g0");
    report.check(result.ok &&
                 result.exists &&
                 result.gateOnCriticalPath &&
                 result.gateId == g0 &&
                 result.worst.endpointName == "y" &&
                 result.worst.depth == 3 &&
                 containsGate(result.worst.criticalPath, g0),
                 "depth_query gate_on_critical_path true");

    query.gateName = "g_short";
    result = netlist.runDepthQuery(query);
    report.check(result.ok &&
                 !result.exists &&
                 !result.gateOnCriticalPath &&
                 result.gateId == netlist.getGateId("g_short") &&
                 result.worst.endpointName == "y" &&
                 result.worst.depth == 3,
                 "depth_query gate_on_critical_path false");
}

void testDeepestOutputCone(TestReport& report, const Netlist& netlist) {
    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::DeepestOutputCone;

    const Netlist::DepthReportSet result = netlist.runDepthQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.count == 3 &&
                 result.worst.endpointType == Netlist::DepthEndpointType::PrimaryOutput &&
                 result.worst.endpointName == "y" &&
                 result.worst.depth == 3,
                 "depth_query deepest_output_cone");
}

void testInvalidGate(TestReport& report, const Netlist& netlist) {
    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::GateOnCriticalPath;
    query.gateName = "missing_gate";

    const Netlist::DepthReportSet result = netlist.runDepthQuery(query);
    report.check(!result.ok &&
                 !result.exists &&
                 result.gateId == -1 &&
                 result.message.find("Gate not found") != std::string::npos,
                 "depth_query gate_on_critical_path missing_gate");
}

void testPathSuppression(TestReport& report, const Netlist& netlist) {
    const Netlist::DepthQueryType queryTypes[] = {
        Netlist::DepthQueryType::SpecificNet,
        Netlist::DepthQueryType::PrimaryOutputs,
        Netlist::DepthQueryType::DffD,
        Netlist::DepthQueryType::GlobalCriticalPath,
        Netlist::DepthQueryType::EndpointsExceedingDepth,
        Netlist::DepthQueryType::PrimaryOutputsExceedingDepth,
        Netlist::DepthQueryType::GateOnCriticalPath,
        Netlist::DepthQueryType::DeepestOutputCone
    };

    bool allPassed = true;
    for (Netlist::DepthQueryType type : queryTypes) {
        Netlist::DepthQuery query;
        query.type = type;
        query.netName = "y";
        query.gateName = "g0";
        query.threshold = 0;
        query.includeCriticalPath = false;
        const Netlist::DepthReportSet result = netlist.runDepthQuery(query);
        allPassed = allPassed && result.ok && allCriticalPathsEmpty(result);
    }
    report.check(allPassed,
                 "depth_query includeCriticalPath=false suppresses every path");
}

void testTombstonesAndStaleEdges(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryOutput("y");
    const int a = netlist.getNetId("a");
    const int b = netlist.getNetId("b");
    const int y = netlist.getNetId("y");

    const int staleGate = netlist.addGate("stale_driver", GateType::AND);
    netlist.connectGateInput(staleGate, a);
    netlist.connectGateInput(staleGate, b);
    netlist.connectGateOutput(staleGate, y);

    const int currentGate = netlist.addGate("current_driver", GateType::BUF);
    netlist.connectGateInput(currentGate, a);
    netlist.connectGateOutput(currentGate, y);

    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::SpecificNet;
    query.netName = "y";
    Netlist::DepthReportSet result = netlist.runDepthQuery(query);
    report.check(result.ok && result.worst.depth == 1 &&
                 containsGate(result.worst.criticalPath, currentGate) &&
                 !containsGate(result.worst.criticalPath, staleGate),
                 "depth_query ignores stale driver edge");

    query = Netlist::DepthQuery();
    query.type = Netlist::DepthQueryType::GateOnCriticalPath;
    query.gateName = "stale_driver";
    result = netlist.runDepthQuery(query);
    report.check(result.ok && !result.gateOnCriticalPath,
                 "depth_query rejects gate with inconsistent output edge");

    const int removedGate = netlist.addGate("removed_gate", GateType::BUF);
    netlist.markGateRemoved(removedGate);
    query.gateName = "removed_gate";
    result = netlist.runDepthQuery(query);
    report.check(!result.ok && !result.exists,
                 "depth_query excludes removed gate tombstone");

    const int removedNet = netlist.addNet("removed_net");
    netlist.removeNetIfUnused(removedNet);
    query = Netlist::DepthQuery();
    query.type = Netlist::DepthQueryType::SpecificNet;
    query.netName = "removed_net";
    result = netlist.runDepthQuery(query);
    report.check(!result.ok && !result.exists,
                 "depth_query excludes removed net tombstone");
}

void testDeepGateOnQuery(TestReport& report) {
    constexpr int kDepth = 30000;
    Netlist netlist;
    netlist.addPrimaryInput("deep_in");
    netlist.addPrimaryOutput("deep_out");

    int currentNet = netlist.getNetId("deep_in");
    int firstGate = -1;
    for (int i = 0; i < kDepth; ++i) {
        const int outputNet = (i + 1 == kDepth)
            ? netlist.getNetId("deep_out")
            : netlist.addNet("deep_n" + std::to_string(i));
        const int gateId = netlist.addGate(
            "deep_g" + std::to_string(i), GateType::BUF);
        if (i == 0) {
            firstGate = gateId;
        }
        netlist.connectGateInput(gateId, currentNet);
        netlist.connectGateOutput(gateId, outputNet);
        currentNet = outputNet;
    }

    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::GateOnCriticalPath;
    query.gateName = "deep_g0";
    query.includeCriticalPath = false;
    const Netlist::DepthReportSet result = netlist.runDepthQuery(query);
    report.check(firstGate >= 0 && result.ok && result.gateOnCriticalPath &&
                 result.worst.depth == kDepth && allCriticalPathsEmpty(result),
                 "depth_query deep GateOnCriticalPath is iterative");
}

} // namespace

int main() {
    TestReport report;
    Netlist netlist;

    report.check(loadCircuit(netlist), "test5 load depth wrapper circuit");
    if (report.failed == 0) {
        testGateOnCriticalPath(report, netlist);
        testDeepestOutputCone(report, netlist);
        testInvalidGate(report, netlist);
        testPathSuppression(report, netlist);
    }
    testTombstonesAndStaleEdges(report);
    testDeepGateOnQuery(report);

    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
