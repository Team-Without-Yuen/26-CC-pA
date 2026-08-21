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

const GateConnectionSummary* findGateConnection(
    const std::vector<GateConnectionSummary>& connections,
    const std::string& gateName) {
    for (const GateConnectionSummary& connection : connections) {
        if (connection.gateName == gateName) {
            return &connection;
        }
    }
    return nullptr;
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

void testGateTypeFiltersAndDetails(TestReport& report, const Netlist& netlist) {
    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::NetTransitiveFanin;
    query.netName = "y";
    query.gateTypeFilters = {GateType::OR, GateType::AND, GateType::OR};
    query.includeIds = false;
    query.includeNames = true;
    query.includeGateDetails = true;

    const Netlist::ConeReport filtered = netlist.runConeQuery(query);
    const GateConnectionSummary* g0 =
        findGateConnection(filtered.gateConnections, "g0");
    const GateConnectionSummary* g1 =
        findGateConnection(filtered.gateConnections, "g1");
    report.check(filtered.ok && filtered.exists &&
                     filtered.gateTypeFilterApplied &&
                     filtered.gateDetailsIncluded &&
                     filtered.scopeGateCount == 3 &&
                     filtered.gateCount == 2 &&
                     filtered.appliedGateTypeFilters.size() == 2 &&
                     filtered.gateIds.empty() &&
                     containsString(filtered.gateNames, "g0") &&
                     containsString(filtered.gateNames, "g1") &&
                     !containsString(filtered.gateNames, "g2") &&
                     filtered.gateTypeCounts.at(GateType::AND) == 1 &&
                     filtered.gateTypeCounts.at(GateType::OR) == 1 &&
                     filtered.gateConnections.size() == 2 &&
                     g0 != nullptr && g0->gateId == netlist.getGateId("g0") &&
                     g0->inputs.size() == 2 &&
                     g0->inputs[0].netName == "a" &&
                     g0->inputs[1].netName == "b" &&
                     g0->output.netName == "n_ab" &&
                     g1 != nullptr && g1->inputs.size() == 2 &&
                     g1->inputs[0].netName == "n_ab" &&
                     g1->inputs[1].netName == "c" &&
                     g1->output.netName == "n_or",
                 "cone_query multi-type filter and structured gate details");

    query.gateTypeFilters = {GateType::NAND};
    const Netlist::ConeReport zero = netlist.runConeQuery(query);
    report.check(zero.ok && zero.exists && zero.scopeGateCount == 3 &&
                     zero.gateCount == 0 && zero.gateIds.empty() &&
                     zero.gateNames.empty() && zero.gateConnections.empty() &&
                     zero.gateTypeCounts.empty(),
                 "cone_query gate-type filter returns valid zero");

    query.gateTypeFilters = {GateType::UNKNOWN};
    const Netlist::ConeReport invalid = netlist.runConeQuery(query);
    report.check(!invalid.ok && !invalid.exists &&
                     invalid.message.find("UNKNOWN") != std::string::npos,
                 "cone_query rejects UNKNOWN gate-type filter");

    query.gateTypeFilters.clear();
    const Netlist::ConeReport unfiltered = netlist.runConeQuery(query);
    report.check(unfiltered.ok && !unfiltered.gateTypeFilterApplied &&
                     unfiltered.scopeGateCount == 3 &&
                     unfiltered.gateCount == 3 &&
                     unfiltered.gateConnections.size() == 3,
                 "cone_query empty gate-type filter preserves full scope");
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
    query.includeGateDetails = true;
    result = busNetlist.runConeQuery(query);
    const GateConnectionSummary* busGate =
        findGateConnection(result.gateConnections, "g_bus");
    report.check(result.ok && result.exists && result.rootNetIds.size() == 1 &&
                     result.rootNetIds.front() == busNetlist.getNetId("bus[0]") &&
                     result.gateCount == 1 &&
                     containsString(result.gateNames, "g_bus") &&
                     busGate != nullptr && busGate->inputs.size() == 1 &&
                     busGate->inputs.front().netName == "bus[0]" &&
                     busGate->output.netName == "y",
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
    query.includeGateDetails = true;
    const Netlist::ConeReport result = netlist.runConeQuery(query);
    report.check(result.ok && result.gateCount == 1 &&
                     result.scopeGateCount == 1 &&
                     result.gateConnections.size() == 1 &&
                     result.gateConnections.front().gateName == "g_current" &&
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

void testSharedFaninGateTypeFilter(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addNet("shared");
    netlist.addPrimaryOutput("y");
    netlist.addPrimaryOutput("z");

    const int sharedGate = netlist.addGate("g_shared", GateType::AND);
    netlist.connectGateInput(sharedGate, netlist.getNetId("a"));
    netlist.connectGateInput(sharedGate, netlist.getNetId("b"));
    netlist.connectGateOutput(sharedGate, netlist.getNetId("shared"));
    const int gateY = netlist.addGate("g_y", GateType::OR);
    netlist.connectGateInput(gateY, netlist.getNetId("shared"));
    netlist.connectGateInput(gateY, netlist.getNetId("a"));
    netlist.connectGateOutput(gateY, netlist.getNetId("y"));
    const int gateZ = netlist.addGate("g_z", GateType::XOR);
    netlist.connectGateInput(gateZ, netlist.getNetId("shared"));
    netlist.connectGateInput(gateZ, netlist.getNetId("b"));
    netlist.connectGateOutput(gateZ, netlist.getNetId("z"));

    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::SharedFaninGates;
    query.netName = "y";
    query.secondNetName = "z";
    query.gateTypeFilters = {GateType::OR};
    query.includeGateDetails = true;
    const Netlist::ConeReport filtered = netlist.runConeQuery(query);
    report.check(filtered.ok && filtered.exists &&
                     filtered.scopeGateCount == 1 && filtered.gateCount == 0 &&
                     filtered.gateConnections.empty(),
                 "cone_query shared fanin applies gate-type filter");

    query.gateTypeFilters = {GateType::AND};
    const Netlist::ConeReport matched = netlist.runConeQuery(query);
    report.check(matched.ok && matched.scopeGateCount == 1 &&
                     matched.gateCount == 1 &&
                     matched.gateNames == std::vector<std::string>({"g_shared"}) &&
                     matched.gateConnections.size() == 1 &&
                     matched.gateConnections.front().output.netName == "shared",
                 "cone_query shared fanin returns filtered gate detail");
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

void testOfficialLargeConeFilter(TestReport& report) {
    Netlist netlist;
    VerilogReader reader;
    report.check(
        reader.read("NewTestCase/test70/test70.v", netlist),
        "cone_query load official NewTestCase test70");
    if (netlist.getPrimaryOutputNetIds().empty()) {
        report.check(false, "cone_query official test70 has primary outputs");
        return;
    }

    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::LargestOutputCone;
    const Netlist::ConeReport full = netlist.runConeQuery(query);

    query.gateTypeFilters = {GateType::NAND, GateType::NOR, GateType::NAND};
    query.includeGateDetails = true;
    const Netlist::ConeReport filtered = netlist.runConeQuery(query);

    size_t breakdownCount = 0;
    for (const auto& item : filtered.gateTypeCounts) {
        breakdownCount += static_cast<size_t>(item.second);
    }
    bool detailTypesValid = true;
    for (const GateConnectionSummary& gate : filtered.gateConnections) {
        if (gate.typeName != "NAND" && gate.typeName != "NOR") {
            detailTypesValid = false;
            break;
        }
    }

    report.check(full.ok && filtered.ok &&
                     filtered.sourceName == full.sourceName &&
                     filtered.sourceId == full.sourceId &&
                     filtered.scopeGateCount == full.gateCount &&
                     filtered.gateCount <= filtered.scopeGateCount &&
                     filtered.gateIds.size() == filtered.gateCount &&
                     filtered.gateNames.size() == filtered.gateCount &&
                     filtered.gateConnections.size() == filtered.gateCount &&
                     breakdownCount == filtered.gateCount && detailTypesValid,
                 "cone_query official test70 filter/detail consistency");
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
        testGateTypeFiltersAndDetails(report, netlist);
        testNoPrimaryOutputs(report);
        testRemovedObjectsAndBusRoots(report);
        testConsistentEdges(report);
        testSharedFaninReportSemantics(report);
        testSharedFaninGateTypeFilter(report);
        testLocalPathEngine(report);
        testOfficialLargeConeFilter(report);
    }

    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
