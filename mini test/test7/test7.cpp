#include "include/core/Netlist.h"
#include "include/core/SatTime.h"
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

bool loadCircuit(Netlist& netlist) {
    VerilogReader reader;
    return reader.read("mini test/test7/timeout_guard_circuit.v", netlist);
}

void testPathEnumerationLimit(TestReport& report, const Netlist& netlist) {
    Netlist::PathQuery query;
    query.mode = Netlist::PathQueryMode::EnumerateAll;
    query.startpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "a"));
    query.endpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "y"));
    query.writePathsToFile = false;
    query.maxEnumeratedPaths = 1;
    query.enumerationTimeLimitSeconds = 55.0;

    const Netlist::PathQueryResult result = netlist.runPathQuery(query);
    report.check(result.exists &&
                 result.pathCount == 1 &&
                 result.paths.size() == 1 &&
                 !result.completeEnumeration &&
                 result.enumerationPathLimitReached &&
                 !result.enumerationTimedOut &&
                 result.enumerationStopReason.find("maxEnumeratedPaths") != std::string::npos,
                 "path_query enumerate max_paths guard");
}

void testPathEnumerationCountOnly(TestReport& report, const Netlist& netlist) {
    Netlist::PathQuery query;
    query.mode = Netlist::PathQueryMode::EnumerateAll;
    query.startpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "a"));
    query.endpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "y"));
    query.writePathsToFile = false;
    query.maxEnumeratedPaths = 10;
    query.countOnly = true;

    const Netlist::PathQueryResult result = netlist.runPathQuery(query);
    report.check(result.exists &&
                 result.pathCount == 4 &&
                 result.paths.empty() &&
                 result.completeEnumeration &&
                 result.countOnly,
                 "path_query enumerate count_only");
}

void testWholeDesignBudgetField(TestReport& report, const Netlist& original) {
    Netlist current = original.cloneForRollback();
    const Netlist::WholeDesignEquivalenceReport result =
        current.checkWholeDesignEquivalence(original, 5.0);

    report.check(result.ok &&
                 result.equivalent &&
                 result.timeBudgetSeconds == 5.0 &&
                 result.comparedOutputCount == 2 &&
                 result.skippedOutputCount == 0 &&
                 !result.timeBudgetExceeded,
                 "whole_design_equivalence budget field");
}

void testTerminatorInitialState(TestReport& report) {
    TimeLimitTerminator terminator(100.0);
    report.check(!terminator.terminate(),
                 "sat_time terminator initial call");
}

} // namespace

int main() {
    TestReport report;
    Netlist netlist;

    report.check(loadCircuit(netlist), "test7 load timeout guard circuit");
    if (report.failed == 0) {
        testPathEnumerationLimit(report, netlist);
        testPathEnumerationCountOnly(report, netlist);
        testWholeDesignBudgetField(report, netlist);
        testTerminatorInitialState(report);
    }

    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
