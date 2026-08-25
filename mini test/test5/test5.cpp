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

std::vector<std::string> endpointNames(const Netlist::DepthReportSet& result) {
    std::vector<std::string> names;
    names.reserve(result.reports.size());
    for (const Netlist::DepthReport& item : result.reports) {
        names.push_back(item.endpointName);
    }
    return names;
}

std::vector<std::string> criticalGateNames(
    const Netlist::DepthReportSet& result) {
    std::vector<std::string> names;
    names.reserve(result.criticalGates.size());
    for (const Netlist::CriticalGateReport& item : result.criticalGates) {
        names.push_back(item.gateName);
    }
    return names;
}

size_t criticalGateTypeCount(const Netlist::DepthReportSet& result,
                             GateType type) {
    const auto found = result.criticalGateTypeCounts.find(type);
    return found == result.criticalGateTypeCounts.end() ? 0 : found->second;
}

const Netlist::DepthReport* findEndpointReport(
    const Netlist::DepthReportSet& result,
    const std::string& endpointName) {
    for (const Netlist::DepthReport& item : result.reports) {
        if (item.endpointName == endpointName) {
            return &item;
        }
    }
    return nullptr;
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

void testCriticalGateBatch(TestReport& report, const Netlist& netlist) {
    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::CriticalGateBatch;
    query.includeCriticalPath = false;

    const Netlist::DepthReportSet result = netlist.runDepthQuery(query);
    report.check(result.ok && result.complete && result.exists &&
                 result.criticalGateBatchApplied &&
                 result.checkedGateCount == 5 &&
                 result.analyzableGateCount == 5 &&
                 result.criticalGateCount == 3 && result.count == 3 &&
                 result.noTimingPathGateCount == 0 &&
                 result.graphInconsistentGateCount == 0 &&
                 result.analysisFailureGateCount == 0 &&
                 result.criticalGateTypeCounts.size() == 8 &&
                 criticalGateTypeCount(result, GateType::AND) == 1 &&
                 criticalGateTypeCount(result, GateType::OR) == 1 &&
                 criticalGateTypeCount(result, GateType::NOT) == 1 &&
                 criticalGateTypeCount(result, GateType::NAND) == 0 &&
                 result.worst.endpointName == "y" &&
                 result.worst.depth == 3 &&
                 criticalGateNames(result) ==
                     std::vector<std::string>({"g0", "g1", "g2"}) &&
                 allCriticalPathsEmpty(result),
                 "depth_query critical gate batch exact union");

    bool agreesWithSingleGateQueries = true;
    for (size_t gateIndex = 0; gateIndex < netlist.getGateCount(); ++gateIndex) {
        const Gate& gate = netlist.getGate(static_cast<int>(gateIndex));
        if (gate.type == GateType::UNKNOWN || gate.type == GateType::DFF) {
            continue;
        }
        Netlist::DepthQuery single;
        single.type = Netlist::DepthQueryType::GateOnCriticalPath;
        single.gateName = gate.instName;
        single.includeCriticalPath = false;
        const Netlist::DepthReportSet singleResult =
            netlist.runDepthQuery(single);
        const bool inBatch = std::find_if(
            result.criticalGates.begin(), result.criticalGates.end(),
            [&](const Netlist::CriticalGateReport& item) {
                return item.gateName == gate.instName;
            }) != result.criticalGates.end();
        agreesWithSingleGateQueries = agreesWithSingleGateQueries &&
            singleResult.ok &&
            singleResult.gateOnCriticalPath == inBatch;
    }
    report.check(agreesWithSingleGateQueries,
                 "critical gate batch agrees with per-gate queries");
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

    query = Netlist::DepthQuery();
    query.type = Netlist::DepthQueryType::CriticalGateBatch;
    query.includeCriticalPath = false;
    result = netlist.runDepthQuery(query);
    report.check(result.ok && !result.complete && result.exists &&
                 result.checkedGateCount == 2 &&
                 result.analyzableGateCount == 1 &&
                 result.criticalGateCount == 1 &&
                 result.graphInconsistentGateCount == 1 &&
                 criticalGateNames(result) ==
                     std::vector<std::string>({"current_driver"}),
                 "critical gate batch preserves matches and reports stale graph");

    const int removedGate = netlist.addGate("removed_gate", GateType::BUF);
    netlist.markGateRemoved(removedGate);
    query = Netlist::DepthQuery();
    query.type = Netlist::DepthQueryType::GateOnCriticalPath;
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

void testStaleInputEdgeIsRejected(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("stale_input");
    netlist.addPrimaryOutput("y");
    const int a = netlist.getNetId("a");
    const int staleInput = netlist.getNetId("stale_input");
    const int stage = netlist.addNet("stage");
    const int y = netlist.getNetId("y");

    const int stageGate = netlist.addGate("stage_gate", GateType::BUF);
    netlist.connectGateInput(stageGate, a);
    netlist.connectGateOutput(stageGate, stage);

    const int targetGate = netlist.addGate("stale_input_gate", GateType::AND);
    netlist.connectGateInput(targetGate, stage);
    netlist.connectGateInput(targetGate, staleInput);
    netlist.connectGateOutput(targetGate, y);

    std::vector<int>& staleLoads = netlist.getNetMutable(staleInput).loadGateIds;
    staleLoads.erase(
        std::remove(staleLoads.begin(), staleLoads.end(), targetGate),
        staleLoads.end());

    const std::vector<int> gateLevels = netlist.computeGateLevels();
    const Netlist::CombinationalPath path = netlist.findCriticalPathToNet("y");
    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::SpecificNet;
    query.netName = "y";
    const Netlist::DepthReportSet result = netlist.runDepthQuery(query);

    report.check(netlist.getMaxDepthToNet("y") == -1 &&
                 gateLevels[targetGate] == -1 &&
                 !path.exists() &&
                 result.ok && !result.complete && result.exists &&
                 result.graphInconsistentEndpointCount == 1 &&
                 result.reports.size() == 1 &&
                 result.reports.front().depthStatus ==
                     Netlist::DepthStatus::GraphInconsistent,
                 "depth_query rejects gate with stale input reverse edge");
}

void testTiedInputWithDeduplicatedLoad(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryOutput("y");
    const int a = netlist.getNetId("a");
    const int y = netlist.getNetId("y");

    const int tiedGate = netlist.addGate("tied_gate", GateType::AND);
    netlist.connectGateInput(tiedGate, a);
    netlist.connectGateInput(tiedGate, a);
    netlist.connectGateOutput(tiedGate, y);

    std::vector<int>& loads = netlist.getNetMutable(a).loadGateIds;
    loads.erase(std::unique(loads.begin(), loads.end()), loads.end());

    const std::vector<int> gateLevels = netlist.computeGateLevels();
    const Netlist::CombinationalPath path = netlist.findCriticalPathToNet("y");
    report.check(netlist.getMaxDepthToNet("y") == 1 &&
                 gateLevels[tiedGate] == 1 &&
                 path.depth() == 1 && containsGate(path, tiedGate),
                 "depth_query accepts tied inputs with one reverse load edge");
}

void testReconvergentDepthRemainsStable(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryOutput("y");
    const int a = netlist.getNetId("a");
    const int left = netlist.addNet("left");
    const int right = netlist.addNet("right");
    const int y = netlist.getNetId("y");

    const int leftGate = netlist.addGate("left_gate", GateType::BUF);
    netlist.connectGateInput(leftGate, a);
    netlist.connectGateOutput(leftGate, left);
    const int rightGate = netlist.addGate("right_gate", GateType::NOT);
    netlist.connectGateInput(rightGate, a);
    netlist.connectGateOutput(rightGate, right);
    const int mergeGate = netlist.addGate("merge_gate", GateType::OR);
    netlist.connectGateInput(mergeGate, left);
    netlist.connectGateInput(mergeGate, right);
    netlist.connectGateOutput(mergeGate, y);

    const std::vector<int> gateLevels = netlist.computeGateLevels();
    const Netlist::CombinationalPath path = netlist.findCriticalPathToNet("y");
    report.check(netlist.getMaxDepthToNet("y") == 2 &&
                 gateLevels[leftGate] == 1 &&
                 gateLevels[rightGate] == 1 &&
                 gateLevels[mergeGate] == 2 &&
                 path.depth() == 2 && containsGate(path, mergeGate),
                 "depth_query preserves normal reconvergent depth");

    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::CriticalGateBatch;
    query.includeCriticalPath = false;
    const Netlist::DepthReportSet critical = netlist.runDepthQuery(query);
    report.check(critical.ok && critical.complete &&
                 critical.criticalGateCount == 3 &&
                 criticalGateNames(critical) ==
                     std::vector<std::string>(
                         {"left_gate", "right_gate", "merge_gate"}),
                 "critical gate batch returns the union of tied maximum paths");
}

void testGeneralizedDepthFilter(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("clk");
    netlist.addPrimaryOutput("y1");
    netlist.addPrimaryOutput("y2");
    netlist.addPrimaryOutput("y3");
    const int a = netlist.getNetId("a");
    const int clk = netlist.getNetId("clk");
    const int y1 = netlist.getNetId("y1");
    const int y2 = netlist.getNetId("y2");
    const int y3 = netlist.getNetId("y3");

    const int g1 = netlist.addGate("g1", GateType::BUF);
    netlist.connectGateInput(g1, a);
    netlist.connectGateOutput(g1, y1);
    const int g2 = netlist.addGate("g2", GateType::NOT);
    netlist.connectGateInput(g2, y1);
    netlist.connectGateOutput(g2, y2);
    const int g3 = netlist.addGate("g3", GateType::BUF);
    netlist.connectGateInput(g3, y2);
    netlist.connectGateOutput(g3, y3);

    const int q = netlist.addNet("q");
    const int dff = netlist.addGate("ff0", GateType::DFF);
    netlist.connectGateInput(dff, y2, "D");
    netlist.connectGateInput(dff, clk, "CK");
    netlist.connectGateOutput(dff, q);

    auto runFilter = [&](Netlist::DepthFilterScope scope,
                         Netlist::DepthPredicate predicate,
                         int threshold,
                         int upperThreshold = -1) {
        Netlist::DepthQuery query;
        query.type = Netlist::DepthQueryType::EndpointDepthFilter;
        query.filterScope = scope;
        query.predicate = predicate;
        query.threshold = threshold;
        query.upperThreshold = upperThreshold;
        return netlist.runDepthQuery(query);
    };

    const auto eq = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::Equal, 2);
    report.check(eq.ok && eq.complete && eq.filterApplied && eq.exists &&
                 eq.checkedEndpointCount == 4 &&
                 eq.matchedEndpointCount == 2 && eq.count == 2 &&
                 eq.unavailableEndpointCount == 0 &&
                 endpointNames(eq) == std::vector<std::string>({"y2", "ff0.D"}) &&
                 allCriticalPathsEmpty(eq),
                 "depth filter eq reports PO and DFF.D matches");

    const auto ne = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::NotEqual, 2);
    report.check(ne.ok && ne.count == 2 &&
                 endpointNames(ne) == std::vector<std::string>({"y1", "y3"}),
                 "depth filter ne");

    const auto gt = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::GreaterThan, 1);
    const auto ge = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::GreaterOrEqual, 2);
    report.check(gt.ok && ge.ok &&
                 endpointNames(gt) == std::vector<std::string>({"y2", "y3", "ff0.D"}) &&
                 endpointNames(ge) == endpointNames(gt),
                 "depth filter gt and ge boundaries");

    const auto lt = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::LessThan, 2);
    const auto le = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::LessOrEqual, 2);
    report.check(lt.ok && le.ok &&
                 endpointNames(lt) == std::vector<std::string>({"y1"}) &&
                 endpointNames(le) == std::vector<std::string>({"y1", "y2", "ff0.D"}),
                 "depth filter lt and le boundaries");

    const auto between = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::BetweenInclusive, 1, 2);
    report.check(between.ok && between.threshold == 1 &&
                 between.upperThreshold == 2 && between.count == 3 &&
                 endpointNames(between) ==
                     std::vector<std::string>({"y1", "y2", "ff0.D"}),
                 "depth filter inclusive between");

    const auto po = runFilter(
        Netlist::DepthFilterScope::PrimaryOutputs,
        Netlist::DepthPredicate::Equal, 2);
    const auto dffD = runFilter(
        Netlist::DepthFilterScope::DffD,
        Netlist::DepthPredicate::Equal, 2);
    report.check(po.ok && po.checkedEndpointCount == 3 && po.count == 1 &&
                 endpointNames(po) == std::vector<std::string>({"y2"}) &&
                 dffD.ok && dffD.checkedEndpointCount == 1 && dffD.count == 1 &&
                 endpointNames(dffD) == std::vector<std::string>({"ff0.D"}),
                 "depth filter scopes po and dff_d");

    const auto zero = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::Equal, 99);
    report.check(zero.ok && zero.complete && !zero.exists && zero.count == 0 &&
                 zero.checkedEndpointCount == 4 &&
                 zero.matchedEndpointCount == 0 &&
                 zero.unavailableEndpointCount == 0,
                 "depth filter zero match is complete success");

    Netlist::DepthQuery legacyQuery;
    legacyQuery.type = Netlist::DepthQueryType::EndpointsExceedingDepth;
    legacyQuery.threshold = 1;
    legacyQuery.includeCriticalPath = false;
    const auto legacy = netlist.runDepthQuery(legacyQuery);
    report.check(legacy.ok && !legacy.filterApplied &&
                 legacy.count == gt.count &&
                 endpointNames(legacy) == endpointNames(gt),
                 "legacy exceeding maps to generalized gt semantics");

    legacyQuery.type = Netlist::DepthQueryType::PrimaryOutputsExceedingDepth;
    const auto legacyPo = netlist.runDepthQuery(legacyQuery);
    const auto filteredPoGt = runFilter(
        Netlist::DepthFilterScope::PrimaryOutputs,
        Netlist::DepthPredicate::GreaterThan, 1);
    report.check(legacyPo.ok && legacyPo.count == filteredPoGt.count &&
                 endpointNames(legacyPo) == endpointNames(filteredPoGt),
                 "legacy po_exceeding maps to generalized gt semantics");

    const auto invalidPredicate = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::None, 1);
    const auto invalidThreshold = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::Equal, -1);
    const auto invalidRange = runFilter(
        Netlist::DepthFilterScope::AllTimingEndpoints,
        Netlist::DepthPredicate::BetweenInclusive, 3, 2);
    const auto invalidScope = runFilter(
        static_cast<Netlist::DepthFilterScope>(99),
        Netlist::DepthPredicate::Equal, 1);
    report.check(!invalidPredicate.ok && !invalidPredicate.complete &&
                 !invalidThreshold.ok && !invalidThreshold.complete &&
                 !invalidRange.ok && !invalidRange.complete &&
                 !invalidScope.ok && !invalidScope.complete,
                 "depth filter rejects invalid backend requests");
}

void testDepthFilterUnavailableEndpoint(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("d");
    const int d = netlist.getNetId("d");
    const int q = netlist.addNet("q");
    const int dff = netlist.addGate("broken_ff", GateType::DFF);
    netlist.connectGateInput(dff, d, "D");
    netlist.connectGateOutput(dff, q);

    std::vector<int>& loads = netlist.getNetMutable(d).loadGateIds;
    loads.erase(std::remove(loads.begin(), loads.end(), dff), loads.end());

    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::EndpointDepthFilter;
    query.filterScope = Netlist::DepthFilterScope::DffD;
    query.predicate = Netlist::DepthPredicate::Equal;
    query.threshold = 0;
    const Netlist::DepthReportSet result = netlist.runDepthQuery(query);
    report.check(result.ok && !result.complete && !result.exists &&
                 result.checkedEndpointCount == 1 &&
                 result.matchedEndpointCount == 0 &&
                 result.definedDepthEndpointCount == 0 &&
                 result.noTimingPathEndpointCount == 0 &&
                 result.graphInconsistentEndpointCount == 1 &&
                 result.analysisFailureEndpointCount == 0 &&
                 result.unavailableEndpointCount == 1,
                 "depth filter reports inconsistent DFF.D as incomplete");
}

void testNoTimingPathClassification(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryOutput("defined_out");
    netlist.addPrimaryOutput("undriven_out");
    netlist.addPrimaryOutput("floating_fanout_out");

    const int a = netlist.getNetId("a");
    const int definedOut = netlist.getNetId("defined_out");
    const int floatingRoot = netlist.addNet("floating_root");
    const int floatingFanoutOut = netlist.getNetId("floating_fanout_out");

    const int definedGate = netlist.addGate("defined_gate", GateType::BUF);
    netlist.connectGateInput(definedGate, a);
    netlist.connectGateOutput(definedGate, definedOut);

    const int floatingGate = netlist.addGate("floating_gate", GateType::BUF);
    netlist.connectGateInput(floatingGate, floatingRoot);
    netlist.connectGateOutput(floatingGate, floatingFanoutOut);

    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::EndpointDepthFilter;
    query.filterScope = Netlist::DepthFilterScope::PrimaryOutputs;
    query.predicate = Netlist::DepthPredicate::GreaterOrEqual;
    query.threshold = 0;
    const Netlist::DepthReportSet filtered = netlist.runDepthQuery(query);
    report.check(filtered.ok && filtered.complete && filtered.exists &&
                 filtered.checkedEndpointCount == 3 &&
                 filtered.matchedEndpointCount == 1 &&
                 filtered.definedDepthEndpointCount == 1 &&
                 filtered.noTimingPathEndpointCount == 2 &&
                 filtered.graphInconsistentEndpointCount == 0 &&
                 filtered.analysisFailureEndpointCount == 0 &&
                 filtered.unavailableEndpointCount == 2 &&
                 endpointNames(filtered) ==
                     std::vector<std::string>({"defined_out"}),
                 "depth filter excludes no-timing-path endpoints without partial");

    query = Netlist::DepthQuery();
    query.type = Netlist::DepthQueryType::PrimaryOutputs;
    query.includeCriticalPath = false;
    const Netlist::DepthReportSet allOutputs = netlist.runDepthQuery(query);
    const Netlist::DepthReport* undriven =
        findEndpointReport(allOutputs, "undriven_out");
    const Netlist::DepthReport* propagated =
        findEndpointReport(allOutputs, "floating_fanout_out");
    report.check(allOutputs.ok && allOutputs.complete &&
                 allOutputs.count == 3 &&
                 allOutputs.definedDepthEndpointCount == 1 &&
                 allOutputs.noTimingPathEndpointCount == 2 &&
                 undriven != nullptr && undriven->depth == -1 &&
                 undriven->depthStatus == Netlist::DepthStatus::NoTimingPath &&
                 propagated != nullptr && propagated->depth == -1 &&
                 propagated->depthStatus ==
                     Netlist::DepthStatus::NoTimingPath,
                 "depth all_po preserves explicit no-timing-path status");

    query = Netlist::DepthQuery();
    query.type = Netlist::DepthQueryType::SpecificNet;
    query.netName = "undriven_out";
    query.includeCriticalPath = false;
    const Netlist::DepthReportSet specific = netlist.runDepthQuery(query);
    report.check(specific.ok && specific.complete && specific.exists &&
                 specific.count == 1 &&
                 specific.noTimingPathEndpointCount == 1 &&
                 specific.reports.front().depthStatus ==
                     Netlist::DepthStatus::NoTimingPath,
                 "depth specific net distinguishes no timing path from error");

    query = Netlist::DepthQuery();
    query.type = Netlist::DepthQueryType::CriticalGateBatch;
    query.includeCriticalPath = false;
    const Netlist::DepthReportSet critical = netlist.runDepthQuery(query);
    report.check(critical.ok && critical.complete && critical.exists &&
                 critical.checkedGateCount == 2 &&
                 critical.analyzableGateCount == 1 &&
                 critical.criticalGateCount == 1 &&
                 critical.noTimingPathGateCount == 1 &&
                 critical.graphInconsistentGateCount == 0 &&
                 critical.analysisFailureGateCount == 0 &&
                 criticalGateNames(critical) ==
                     std::vector<std::string>({"defined_gate"}),
                 "critical gate batch classifies legal no-timing-path logic");
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

    query = Netlist::DepthQuery();
    query.type = Netlist::DepthQueryType::CriticalGateBatch;
    query.includeCriticalPath = false;
    const Netlist::DepthReportSet batch = netlist.runDepthQuery(query);
    report.check(batch.ok && batch.complete && batch.exists &&
                 batch.worst.depth == kDepth &&
                 batch.checkedGateCount == kDepth &&
                 batch.analyzableGateCount == kDepth &&
                 batch.criticalGateCount == kDepth &&
                 batch.criticalGates.front().gateName == "deep_g0" &&
                 batch.criticalGates.back().gateName == "deep_g29999" &&
                 allCriticalPathsEmpty(batch),
                 "depth_query deep critical gate batch is iterative");
}

} // namespace

int main() {
    TestReport report;
    Netlist netlist;

    report.check(loadCircuit(netlist), "test5 load depth wrapper circuit");
    if (report.failed == 0) {
        testGateOnCriticalPath(report, netlist);
        testCriticalGateBatch(report, netlist);
        testDeepestOutputCone(report, netlist);
        testInvalidGate(report, netlist);
        testPathSuppression(report, netlist);
    }
    testTombstonesAndStaleEdges(report);
    testStaleInputEdgeIsRejected(report);
    testTiedInputWithDeduplicatedLoad(report);
    testReconvergentDepthRemainsStable(report);
    testGeneralizedDepthFilter(report);
    testDepthFilterUnavailableEndpoint(report);
    testNoTimingPathClassification(report);
    testDeepGateOnQuery(report);

    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
