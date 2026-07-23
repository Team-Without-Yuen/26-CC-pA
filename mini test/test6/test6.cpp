#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <algorithm>
#include <iostream>
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

bool containsString(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

bool loadCircuit(Netlist& netlist) {
    VerilogReader reader;
    return reader.read("mini test/test6/cone_wrapper_circuit.v", netlist);
}

void testLargestOutputCone(TestReport& report, const Netlist& netlist) {
    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::LargestOutputCone;
    query.includeIds = true;
    query.includeNames = true;

    const Netlist::ConeReport result = netlist.runConeQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.sourceName == "y" &&
                 result.sourceId == netlist.getNetId("y") &&
                 result.checkedOutputCount == 3 &&
                 result.gateCount == 3 &&
                 containsString(result.gateNames, "g0") &&
                 containsString(result.gateNames, "g1") &&
                 containsString(result.gateNames, "g2") &&
                 containsString(result.rootNetNames, "y"),
                 "cone_query largest_output_cone");
}

void testLargestOutputConeWithoutNames(TestReport& report, const Netlist& netlist) {
    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::LargestOutputCone;
    query.includeIds = false;
    query.includeNames = false;

    const Netlist::ConeReport result = netlist.runConeQuery(query);
    report.check(result.ok &&
                 result.sourceName == "y" &&
                 result.gateCount == 3 &&
                 result.gateNames.empty() &&
                 result.netNames.empty() &&
                 result.gateIds.empty() &&
                 result.netIds.empty(),
                 "cone_query largest_output_cone metadata_flags");
}

void testNoPrimaryOutputs(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");

    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::LargestOutputCone;

    const Netlist::ConeReport result = netlist.runConeQuery(query);
    report.check(!result.ok &&
                 !result.exists &&
                 result.checkedOutputCount == 0 &&
                 result.message.find("No primary outputs") != std::string::npos,
                 "cone_query largest_output_cone no_outputs");
}

} // namespace

int main() {
    TestReport report;
    Netlist netlist;

    report.check(loadCircuit(netlist), "test6 load cone wrapper circuit");
    if (report.failed == 0) {
        testLargestOutputCone(report, netlist);
        testLargestOutputConeWithoutNames(report, netlist);
        testNoPrimaryOutputs(report);
    }

    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
