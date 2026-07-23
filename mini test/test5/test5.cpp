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

} // namespace

int main() {
    TestReport report;
    Netlist netlist;

    report.check(loadCircuit(netlist), "test5 load depth wrapper circuit");
    if (report.failed == 0) {
        testGateOnCriticalPath(report, netlist);
        testDeepestOutputCone(report, netlist);
        testInvalidGate(report, netlist);
    }

    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
