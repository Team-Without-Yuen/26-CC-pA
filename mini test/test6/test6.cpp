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

void testGateTransitiveFanout(TestReport& report, const Netlist& netlist) {
    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::GateTransitiveFanout;
    query.gateName = "g0";

    const Netlist::ConeReport result = netlist.runConeQuery(query);
    report.check(result.ok && result.exists && result.gateCount == 2 &&
                     containsString(result.gateNames, "g1") &&
                     containsString(result.gateNames, "g2") &&
                     !containsString(result.gateNames, "g0"),
                 "cone_query gate_transitive_fanout");
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

void testRemovedObjectsAndBusRoots(TestReport& report) {
    Netlist netlist;
    const int removedNetId = netlist.addNet("removed_scalar");
    netlist.removeNetIfUnused(removedNetId);

    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::NetTransitiveFanin;
    query.netName = "removed_scalar";
    Netlist::ConeReport result = netlist.runConeQuery(query);
    report.check(!result.ok && !result.exists && result.sourceId == -1,
                 "cone_query rejects removed scalar net");

    const int removedGateId = netlist.addGate("g_removed", GateType::BUF);
    netlist.removeGate(removedGateId);
    query = Netlist::ConeQuery();
    query.type = Netlist::ConeQueryType::GateTransitiveFanin;
    query.gateName = "g_removed";
    result = netlist.runConeQuery(query);
    report.check(!result.ok && !result.exists,
                 "cone_query rejects removed gate");

    Netlist busNetlist;
    busNetlist.addNet("bus[1]");
    busNetlist.addNet("bus[0]");
    busNetlist.addPrimaryOutput("y");
    const int bufferId = busNetlist.addGate("g_bus", GateType::BUF);
    busNetlist.connectGateInput(bufferId, busNetlist.getNetId("bus[0]"));
    busNetlist.connectGateOutput(bufferId, busNetlist.getNetId("y"));
    busNetlist.removeNetIfUnused(busNetlist.getNetId("bus[1]"));

    query = Netlist::ConeQuery();
    query.type = Netlist::ConeQueryType::NetTransitiveFanout;
    query.netName = "bus";
    result = busNetlist.runConeQuery(query);
    report.check(result.ok && result.exists && result.rootNetIds.size() == 1 &&
                     result.rootNetIds.front() == busNetlist.getNetId("bus[0]") &&
                     result.gateCount == 1 &&
                     containsString(result.gateNames, "g_bus"),
                 "cone_query keeps only active bus roots");

    busNetlist.disconnectGateInput("g_bus", "bus[0]");
    busNetlist.removeNetIfUnused(busNetlist.getNetId("bus[0]"));
    result = busNetlist.runConeQuery(query);
    report.check(!result.ok && !result.exists,
                 "cone_query rejects all-removed bus");
}

void testConsistentEdges(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryOutput("y");

    const int oldDriver = netlist.addGate("g_old", GateType::AND);
    netlist.connectGateInput(oldDriver, netlist.getNetId("a"));
    netlist.connectGateInput(oldDriver, netlist.getNetId("b"));
    netlist.connectGateOutput(oldDriver, netlist.getNetId("y"));

    const int currentDriver = netlist.addGate("g_current", GateType::BUF);
    netlist.connectGateInput(currentDriver, netlist.getNetId("a"));
    netlist.connectGateOutput(currentDriver, netlist.getNetId("y"));

    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::NetTransitiveFanin;
    query.netName = "y";
    const Netlist::ConeReport result = netlist.runConeQuery(query);
    report.check(result.ok && result.gateCount == 1 &&
                     containsString(result.gateNames, "g_current") &&
                     !containsString(result.gateNames, "g_old") &&
                     !containsString(result.netNames, "b") &&
                     netlist.getGateTransitiveFaninCone("g_old").rootNetIds.empty(),
                 "cone_query rejects inconsistent stale driver edge");
}

void testSharedFaninReportSemantics(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryOutput("y");
    netlist.addPrimaryOutput("z");

    const int gateY = netlist.addGate("g_y", GateType::BUF);
    netlist.connectGateInput(gateY, netlist.getNetId("a"));
    netlist.connectGateOutput(gateY, netlist.getNetId("y"));
    const int gateZ = netlist.addGate("g_z", GateType::BUF);
    netlist.connectGateInput(gateZ, netlist.getNetId("b"));
    netlist.connectGateOutput(gateZ, netlist.getNetId("z"));

    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::SharedFaninGates;
    query.netName = "y";
    query.secondNetName = "z";
    query.includeIds = false;
    query.includeNames = false;
    const Netlist::ConeReport result = netlist.runConeQuery(query);
    report.check(result.ok && result.exists && result.gateCount == 0 &&
                     result.rootNetIds.empty() && result.rootNetNames.empty() &&
                     result.gateIds.empty() && result.gateNames.empty(),
                 "cone_query shared_fanin empty result and metadata flags");
}

void testLocalPathEngine(TestReport& report) {
    Netlist netlist;
    const ConeResult emptyCone;
    report.check(netlist.findLongestPathInCone(emptyCone).first == -1 &&
                     netlist.findShortestPathInCone(emptyCone).first == -1,
                 "cone local paths reject empty cone");

    ConeResult reconvergent;
    reconvergent.rootNetIds = {0};
    reconvergent.netIds = {0, 1, 2, 3, 4};
    reconvergent.children[0] = {1, 2};
    reconvergent.children[1] = {3};
    reconvergent.children[2] = {4};
    reconvergent.children[4] = {3};

    const auto longest = netlist.findLongestPathInCone(reconvergent);
    const auto shortest = netlist.findShortestPathInCone(reconvergent);
    report.check(longest.first == 3 &&
                     longest.second == std::vector<int>({0, 2, 4, 3}) &&
                     shortest.first == 2 &&
                     shortest.second == std::vector<int>({0, 1, 3}),
                 "cone local paths handle reconvergent DAG");

    ConeResult multiRoot;
    multiRoot.rootNetIds = {10, 20};
    multiRoot.netIds = {10, 11, 20, 21};
    multiRoot.children[10] = {11};
    multiRoot.children[20] = {21};
    const auto multiLongest = netlist.findLongestPathInCone(multiRoot);
    const auto multiShortest = netlist.findShortestPathInCone(multiRoot);
    report.check(multiLongest.first == 1 &&
                     multiLongest.second == std::vector<int>({10, 11}) &&
                     multiShortest.first == 1 &&
                     multiShortest.second == std::vector<int>({10, 11}),
                 "cone local paths preserve multi-root tie order");

    ConeResult cycle;
    cycle.rootNetIds = {0};
    cycle.netIds = {0, 1};
    cycle.children[0] = {1};
    cycle.children[1] = {0};
    report.check(netlist.findLongestPathInCone(cycle).first == -1 &&
                     netlist.findShortestPathInCone(cycle).first == -1,
                 "cone local paths terminate on pure cycle");

    constexpr int kDeepChainDepth = 100000;
    ConeResult deepChain;
    deepChain.rootNetIds = {0};
    deepChain.netIds.reserve(kDeepChainDepth + 1);
    for (int netId = 0; netId <= kDeepChainDepth; ++netId) {
        deepChain.netIds.insert(netId);
        if (netId < kDeepChainDepth) {
            deepChain.children[netId].push_back(netId + 1);
        }
    }
    const auto deepLongest = netlist.findLongestPathInCone(deepChain);
    report.check(deepLongest.first == kDeepChainDepth &&
                     deepLongest.second.size() ==
                         static_cast<size_t>(kDeepChainDepth + 1) &&
                     deepLongest.second.front() == 0 &&
                     deepLongest.second.back() == kDeepChainDepth,
                 "cone iterative longest path handles 100000-level chain");
}

} // namespace

int main() {
    TestReport report;
    Netlist netlist;

    report.check(loadCircuit(netlist), "test6 load cone wrapper circuit");
    if (report.failed == 0) {
        testLargestOutputCone(report, netlist);
        testLargestOutputConeWithoutNames(report, netlist);
        testGateTransitiveFanout(report, netlist);
        testNoPrimaryOutputs(report);
        testRemovedObjectsAndBusRoots(report);
        testConsistentEdges(report);
        testSharedFaninReportSemantics(report);
        testLocalPathEngine(report);
    }

    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
