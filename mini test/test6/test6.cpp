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

void testBoundingDffFaninMembership(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("d0");
    netlist.addPrimaryInput("d1");
    netlist.addPrimaryInput("clk");
    netlist.addNet("q0");
    netlist.addNet("q1");
    netlist.addPrimaryOutput("x");
    netlist.addPrimaryOutput("z");

    const int ff0 = netlist.addGate("ff0", GateType::DFF);
    netlist.connectGateInput(ff0, netlist.getNetId("d0"), "D");
    netlist.connectGateInput(ff0, netlist.getNetId("clk"), "CK");
    netlist.connectGateOutput(ff0, netlist.getNetId("q0"));

    const int ff1 = netlist.addGate("ff1", GateType::DFF);
    netlist.connectGateInput(ff1, netlist.getNetId("d1"), "D");
    netlist.connectGateInput(ff1, netlist.getNetId("clk"), "CK");
    netlist.connectGateOutput(ff1, netlist.getNetId("q1"));

    const int join = netlist.addGate("g_join", GateType::AND);
    netlist.connectGateInput(join, netlist.getNetId("q0"));
    netlist.connectGateInput(join, netlist.getNetId("q1"));
    netlist.connectGateOutput(join, netlist.getNetId("x"));

    const int side = netlist.addGate("g_side", GateType::OR);
    netlist.connectGateInput(side, netlist.getNetId("q0"));
    netlist.connectGateInput(side, netlist.getNetId("d0"));
    netlist.connectGateOutput(side, netlist.getNetId("z"));

    const ConeResult xCone = netlist.getTransitiveFaninCone("x");
    const std::vector<int> structuralGateIds = netlist.getConeGateIds(xCone);
    const std::vector<int> reportedGateIds =
        netlist.getConeGateIdsIncludingBoundaries(xCone);
    report.check(structuralGateIds == std::vector<int>({join}) &&
                     reportedGateIds.size() == 3 &&
                     std::find(reportedGateIds.begin(), reportedGateIds.end(), ff0) !=
                         reportedGateIds.end() &&
                     std::find(reportedGateIds.begin(), reportedGateIds.end(), ff1) !=
                         reportedGateIds.end(),
                 "cone boundary view does not change combinational rewrite scope");

    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::NetTransitiveFanin;
    query.netName = "x";
    query.includeGateDetails = true;
    query.includeLocalPaths = true;
    const Netlist::ConeReport xReport = netlist.runConeQuery(query);
    report.check(xReport.ok && xReport.exists &&
                     xReport.scopeGateCount == 3 && xReport.gateCount == 3 &&
                     xReport.gateTypeCounts.at(GateType::AND) == 1 &&
                     xReport.gateTypeCounts.at(GateType::DFF) == 2 &&
                     containsString(xReport.gateNames, "g_join") &&
                     containsString(xReport.gateNames, "ff0") &&
                     containsString(xReport.gateNames, "ff1") &&
                     xReport.longestDepth == 1 &&
                     !containsString(xReport.netNames, "d0") &&
                     !containsString(xReport.netNames, "d1"),
                 "cone_query counts bounding DFFs without crossing their inputs");

    query.netName = "q0";
    query.includeGateDetails = false;
    query.includeLocalPaths = false;
    const Netlist::ConeReport qReport = netlist.runConeQuery(query);
    report.check(qReport.ok && qReport.scopeGateCount == 1 &&
                     qReport.gateCount == 1 &&
                     qReport.gateTypeCounts.at(GateType::DFF) == 1 &&
                     qReport.gateNames == std::vector<std::string>({"ff0"}) &&
                     qReport.netNames == std::vector<std::string>({"q0"}),
                 "cone_query reports a DFF.Q root as one DFF gate");

    query.gateTypeFilters = {GateType::DFF};
    const Netlist::ConeReport filtered = netlist.runConeQuery(query);
    report.check(filtered.ok && filtered.scopeGateCount == 1 &&
                     filtered.gateCount == 1 &&
                     filtered.gateNames == std::vector<std::string>({"ff0"}),
                 "cone_query DFF filter selects bounding DFFs");

    query = Netlist::ConeQuery();
    query.type = Netlist::ConeQueryType::SharedFaninGates;
    query.netName = "x";
    query.secondNetName = "z";
    const Netlist::ConeReport shared = netlist.runConeQuery(query);
    report.check(shared.ok && shared.gateCount == 1 &&
                     shared.gateNames == std::vector<std::string>({"ff0"}),
                 "cone_query shared fanin includes a shared bounding DFF");

    query = Netlist::ConeQuery();
    query.type = Netlist::ConeQueryType::NetTransitiveFanout;
    query.netName = "q0";
    const Netlist::ConeReport fanout = netlist.runConeQuery(query);
    report.check(fanout.ok && fanout.gateCount == 2 &&
                     containsString(fanout.gateNames, "g_join") &&
                     containsString(fanout.gateNames, "g_side") &&
                     !containsString(fanout.gateNames, "ff0"),
                 "cone_query fanout semantics remain unchanged");
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

    query = Netlist::ConeQuery();
    query.type = Netlist::ConeQueryType::OutputConeRanking;
    const Netlist::ConeReport ranking = netlist.runConeQuery(query);
    report.check(ranking.ok && ranking.exists &&
                     ranking.rankingReport.ok &&
                     ranking.rankingReport.checkedOutputCount == 0 &&
                     ranking.rankingReport.resultOutputCount == 0 &&
                     !ranking.rankingReport.requestedRankExists,
                 "cone_query output ranking empty scope is valid zero");

    query = Netlist::ConeQuery();
    query.type = Netlist::ConeQueryType::OutputConeFilter;
    query.metricPredicate = Netlist::ConeMetricPredicate::GreaterThan;
    const Netlist::ConeReport filter = netlist.runConeQuery(query);
    report.check(filter.ok && filter.exists && filter.filterReport.ok &&
                     filter.filterReport.checkedOutputCount == 0 &&
                     filter.filterReport.matchedOutputCount == 0 &&
                     filter.filterReport.matchedOutputs.empty(),
                 "cone_query output filter empty scope is valid zero");
}

void testOutputConeRanking(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryInput("c");
    netlist.addNet("n_deep");
    netlist.addPrimaryOutput("y_a");
    netlist.addPrimaryOutput("y_b");
    netlist.addPrimaryOutput("y_deep");
    netlist.addPrimaryOutput("y_zero");

    const int gateA = netlist.addGate("g_a", GateType::AND);
    netlist.connectGateInput(gateA, netlist.getNetId("a"));
    netlist.connectGateInput(gateA, netlist.getNetId("b"));
    netlist.connectGateOutput(gateA, netlist.getNetId("y_a"));

    const int gateB = netlist.addGate("g_b", GateType::OR);
    netlist.connectGateInput(gateB, netlist.getNetId("a"));
    netlist.connectGateInput(gateB, netlist.getNetId("c"));
    netlist.connectGateOutput(gateB, netlist.getNetId("y_b"));

    const int deepAnd = netlist.addGate("g_deep_and", GateType::AND);
    netlist.connectGateInput(deepAnd, netlist.getNetId("a"));
    netlist.connectGateInput(deepAnd, netlist.getNetId("b"));
    netlist.connectGateOutput(deepAnd, netlist.getNetId("n_deep"));
    const int deepNot = netlist.addGate("g_deep_not", GateType::NOT);
    netlist.connectGateInput(deepNot, netlist.getNetId("n_deep"));
    netlist.connectGateOutput(deepNot, netlist.getNetId("y_deep"));

    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::OutputConeRanking;
    query.rankMetric = Netlist::ConeRankMetric::ScopeGateCount;
    query.rankMode = Netlist::ConeRankMode::Highest;
    const Netlist::ConeReport highest = netlist.runConeQuery(query);
    report.check(highest.ok && highest.exists &&
                     highest.sourceName == "y_deep" &&
                     highest.rankingReport.ok &&
                     highest.rankingReport.checkedOutputCount == 4 &&
                     highest.rankingReport.distinctMetricLevelCount == 3 &&
                     highest.rankingReport.selectedMetricLevelCount == 1 &&
                     highest.rankingReport.resultOutputCount == 1 &&
                     highest.rankingReport.requestedRankExists &&
                     highest.rankingReport.rankedOutputs.front().metricValue == 2 &&
                     highest.scopeGateCount == 2,
                 "cone_query output ranking highest scope gate count");

    query.rankMode = Netlist::ConeRankMode::NthHighest;
    query.rankValue = 2;
    const Netlist::ConeReport tied = netlist.runConeQuery(query);
    report.check(tied.ok && tied.sourceName == "y_a" &&
                     tied.rankingReport.resultOutputCount == 2 &&
                     tied.rankingReport.rankedOutputs[0].outputNetName == "y_a" &&
                     tied.rankingReport.rankedOutputs[1].outputNetName == "y_b" &&
                     tied.rankingReport.rankedOutputs[0].rank == 2 &&
                     tied.rankingReport.rankedOutputs[1].rank == 2,
                 "cone_query output ranking preserves nth-level ties");

    query.rankMode = Netlist::ConeRankMode::Top;
    query.rankValue = 2;
    const Netlist::ConeReport top = netlist.runConeQuery(query);
    report.check(top.ok && top.rankingReport.selectedMetricLevelCount == 2 &&
                     top.rankingReport.resultOutputCount == 3 &&
                     top.rankingReport.rankedOutputs[0].outputNetName == "y_deep" &&
                     top.rankingReport.rankedOutputs[1].outputNetName == "y_a" &&
                     top.rankingReport.rankedOutputs[2].outputNetName == "y_b",
                 "cone_query output ranking top counts distinct levels");

    query.rankMode = Netlist::ConeRankMode::Lowest;
    query.rankValue = 1;
    const Netlist::ConeReport lowest = netlist.runConeQuery(query);
    report.check(lowest.ok && lowest.sourceName == "y_zero" &&
                     lowest.rankingReport.resultOutputCount == 1 &&
                     lowest.rankingReport.rankedOutputs.front().metricValue == 0 &&
                     lowest.scopeGateCount == 0 && lowest.netCount == 1,
                 "cone_query output ranking lowest includes zero-gate cone");

    query.rankMode = Netlist::ConeRankMode::NthLowest;
    query.rankValue = 2;
    const Netlist::ConeReport nthLowest = netlist.runConeQuery(query);
    report.check(nthLowest.ok && nthLowest.sourceName == "y_a" &&
                     nthLowest.rankingReport.resultOutputCount == 2 &&
                     nthLowest.rankingReport.rankedOutputs[0].outputNetName == "y_a" &&
                     nthLowest.rankingReport.rankedOutputs[1].outputNetName == "y_b" &&
                     nthLowest.rankingReport.rankedOutputs[0].rank == 2,
                 "cone_query output ranking nth-lowest preserves ties");

    query.rankMode = Netlist::ConeRankMode::Bottom;
    query.rankValue = 2;
    const Netlist::ConeReport bottom = netlist.runConeQuery(query);
    report.check(bottom.ok && bottom.sourceName == "y_zero" &&
                     bottom.rankingReport.selectedMetricLevelCount == 2 &&
                     bottom.rankingReport.resultOutputCount == 3 &&
                     bottom.rankingReport.rankedOutputs[0].outputNetName == "y_zero" &&
                     bottom.rankingReport.rankedOutputs[1].outputNetName == "y_a" &&
                     bottom.rankingReport.rankedOutputs[2].outputNetName == "y_b",
                 "cone_query output ranking bottom counts distinct levels");

    query.rankMetric = Netlist::ConeRankMetric::FilteredGateCount;
    query.rankMode = Netlist::ConeRankMode::Highest;
    query.rankValue = 1;
    query.gateTypeFilters = {GateType::OR, GateType::OR};
    const Netlist::ConeReport filtered = netlist.runConeQuery(query);
    report.check(filtered.ok && filtered.sourceName == "y_b" &&
                     filtered.rankingReport.resultOutputCount == 1 &&
                     filtered.rankingReport.rankedOutputs.front().metricValue == 1 &&
                     filtered.rankingReport.rankedOutputs.front().scopeGateCount == 1 &&
                     filtered.rankingReport.rankedOutputs.front().filteredGateCount == 1 &&
                     filtered.gateCount == 1,
                 "cone_query output ranking supports filtered gate metric");

    query.gateTypeFilters.clear();
    query.rankMetric = Netlist::ConeRankMetric::NetCount;
    const Netlist::ConeReport nets = netlist.runConeQuery(query);
    report.check(nets.ok && nets.sourceName == "y_deep" &&
                     nets.rankingReport.rankedOutputs.front().netCount == 4 &&
                     nets.rankingReport.rankedOutputs.front().metricValue == 4,
                 "cone_query output ranking supports net count metric");

    query.rankMetric = Netlist::ConeRankMetric::ScopeGateCount;
    query.rankMode = Netlist::ConeRankMode::NthHighest;
    query.rankValue = 4;
    const Netlist::ConeReport missingRank = netlist.runConeQuery(query);
    report.check(missingRank.ok && missingRank.exists &&
                     missingRank.sourceName.empty() &&
                     missingRank.rankingReport.resultOutputCount == 0 &&
                     !missingRank.rankingReport.requestedRankExists,
                 "cone_query output ranking missing distinct rank is valid zero");

    query.rankValue = 0;
    const Netlist::ConeReport invalid = netlist.runConeQuery(query);
    report.check(!invalid.ok && !invalid.exists &&
                     invalid.message.find("greater than zero") != std::string::npos,
                 "cone_query output ranking rejects zero rank");

    Netlist::ConeQuery filterQuery;
    filterQuery.type = Netlist::ConeQueryType::OutputConeFilter;
    filterQuery.rankMetric = Netlist::ConeRankMetric::ScopeGateCount;
    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::GreaterThan;
    filterQuery.metricValue = 0;
    const Netlist::ConeReport greater = netlist.runConeQuery(filterQuery);
    report.check(greater.ok && greater.exists && greater.filterReport.ok &&
                     greater.filterReport.checkedOutputCount == 4 &&
                     greater.filterReport.matchedOutputCount == 3 &&
                     greater.filterReport.matchedOutputs[0].outputNetName == "y_a" &&
                     greater.filterReport.matchedOutputs[1].outputNetName == "y_b" &&
                     greater.filterReport.matchedOutputs[2].outputNetName == "y_deep",
                 "cone_query output filter greater-than is complete and stable");

    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::Equal;
    filterQuery.metricValue = 1;
    const Netlist::ConeReport equal = netlist.runConeQuery(filterQuery);
    report.check(equal.ok && equal.filterReport.matchedOutputCount == 2 &&
                     equal.filterReport.matchedOutputs[0].metricValue == 1 &&
                     equal.filterReport.matchedOutputs[1].metricValue == 1,
                 "cone_query output filter equal preserves all matches");

    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::NotEqual;
    const Netlist::ConeReport notEqual = netlist.runConeQuery(filterQuery);
    report.check(notEqual.ok && notEqual.filterReport.matchedOutputCount == 2 &&
                     notEqual.filterReport.matchedOutputs[0].outputNetName == "y_deep" &&
                     notEqual.filterReport.matchedOutputs[1].outputNetName == "y_zero",
                 "cone_query output filter not-equal preserves all matches");

    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::LessThan;
    const Netlist::ConeReport less = netlist.runConeQuery(filterQuery);
    report.check(less.ok && less.filterReport.matchedOutputCount == 1 &&
                     less.filterReport.matchedOutputs.front().outputNetName == "y_zero",
                 "cone_query output filter less-than includes zero cone");

    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::LessOrEqual;
    filterQuery.metricValue = 0;
    const Netlist::ConeReport lessOrEqual = netlist.runConeQuery(filterQuery);
    report.check(lessOrEqual.ok &&
                     lessOrEqual.filterReport.matchedOutputCount == 1 &&
                     lessOrEqual.filterReport.matchedOutputs.front().outputNetName == "y_zero",
                 "cone_query output filter less-or-equal includes boundary");

    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::GreaterOrEqual;
    filterQuery.metricValue = 2;
    const Netlist::ConeReport greaterOrEqual = netlist.runConeQuery(filterQuery);
    report.check(greaterOrEqual.ok &&
                     greaterOrEqual.filterReport.matchedOutputCount == 1 &&
                     greaterOrEqual.filterReport.matchedOutputs.front().outputNetName == "y_deep",
                 "cone_query output filter greater-or-equal includes boundary");

    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::BetweenInclusive;
    filterQuery.metricValue = 1;
    filterQuery.metricUpperValue = 2;
    const Netlist::ConeReport between = netlist.runConeQuery(filterQuery);
    report.check(between.ok && between.filterReport.matchedOutputCount == 3,
                 "cone_query output filter between is inclusive");

    filterQuery.rankMetric = Netlist::ConeRankMetric::FilteredGateCount;
    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::GreaterOrEqual;
    filterQuery.metricValue = 1;
    filterQuery.gateTypeFilters = {GateType::OR, GateType::OR};
    const Netlist::ConeReport filteredMatches = netlist.runConeQuery(filterQuery);
    report.check(filteredMatches.ok &&
                     filteredMatches.filterReport.matchedOutputCount == 1 &&
                     filteredMatches.filterReport.matchedOutputs.front().outputNetName == "y_b" &&
                     filteredMatches.filterReport.matchedOutputs.front().filteredGateCount == 1,
                 "cone_query output filter supports filtered gate metric");

    filterQuery.gateTypeFilters.clear();
    filterQuery.rankMetric = Netlist::ConeRankMetric::NetCount;
    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::BetweenInclusive;
    filterQuery.metricValue = 3;
    filterQuery.metricUpperValue = 3;
    const Netlist::ConeReport netMatches = netlist.runConeQuery(filterQuery);
    report.check(netMatches.ok && netMatches.filterReport.matchedOutputCount == 2 &&
                     netMatches.filterReport.matchedOutputs[0].outputNetName == "y_a" &&
                     netMatches.filterReport.matchedOutputs[1].outputNetName == "y_b",
                 "cone_query output filter supports net count metric");

    filterQuery.rankMetric = Netlist::ConeRankMetric::ScopeGateCount;
    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::GreaterThan;
    filterQuery.metricValue = 99;
    const Netlist::ConeReport noMatch = netlist.runConeQuery(filterQuery);
    report.check(noMatch.ok && noMatch.exists && noMatch.filterReport.ok &&
                     noMatch.filterReport.matchedOutputCount == 0 &&
                     noMatch.filterReport.matchedOutputs.empty(),
                 "cone_query output filter zero match is valid zero");

    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::BetweenInclusive;
    filterQuery.metricValue = 3;
    filterQuery.metricUpperValue = 2;
    const Netlist::ConeReport invalidRange = netlist.runConeQuery(filterQuery);
    report.check(!invalidRange.ok && !invalidRange.exists &&
                     invalidRange.message.find("lower bound") != std::string::npos,
                 "cone_query output filter rejects inverted range");

    filterQuery.metricPredicate =
        static_cast<Netlist::ConeMetricPredicate>(999);
    const Netlist::ConeReport invalidPredicate = netlist.runConeQuery(filterQuery);
    report.check(!invalidPredicate.ok && !invalidPredicate.exists &&
                     invalidPredicate.message.find("predicate") != std::string::npos,
                 "cone_query output filter rejects unsupported predicate");

    filterQuery.metricPredicate = Netlist::ConeMetricPredicate::Equal;
    filterQuery.metricValue = 1;
    filterQuery.includeLocalPaths = true;
    const Netlist::ConeReport unsupportedDetails = netlist.runConeQuery(filterQuery);
    report.check(!unsupportedDetails.ok && !unsupportedDetails.exists &&
                     unsupportedDetails.message.find("summary-only") != std::string::npos,
                 "cone_query output filter rejects per-cone detail options");
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
        testBoundingDffFaninMembership(report);
        testNoPrimaryOutputs(report);
        testOutputConeRanking(report);
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
