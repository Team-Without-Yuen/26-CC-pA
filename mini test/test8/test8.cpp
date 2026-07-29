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

bool loadCircuit(Netlist& netlist) {
    VerilogReader reader;
    return reader.read("mini test/test8/register_timeout_circuit.v", netlist);
}

void testRegisterPathCountOnly(TestReport& report, const Netlist& netlist) {
    Netlist::RegisterPathQuery query;
    query.mode = Netlist::RegisterPathQueryMode::EnumerateAll;
    query.countOnly = true;
    query.maxEnumeratedPaths = 10;
    query.enumerationTimeLimitSeconds = 55.0;

    const Netlist::RegisterPathReport result = netlist.runRegisterPathQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.pathResult.countOnly &&
                 result.pathResult.pathCount == 3 &&
                 result.pathResult.paths.empty() &&
                 result.pathResult.completeEnumeration,
                 "register_path_query count_only forwards to path_query");
}

void testLegacyRegisterPathLimitDoesNotTruncate(TestReport& report, const Netlist& netlist) {
    Netlist::RegisterPathQuery query;
    query.mode = Netlist::RegisterPathQueryMode::EnumerateAll;
    query.countOnly = true;
    query.maxEnumeratedPaths = 1;
    query.enumerationTimeLimitSeconds = 55.0;

    const Netlist::RegisterPathReport result = netlist.runRegisterPathQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.pathResult.countOnly &&
                 result.pathResult.pathCount == 3 &&
                 result.pathResult.paths.empty() &&
                 result.pathResult.completeEnumeration &&
                 !result.pathResult.enumerationPathLimitReached &&
                 result.pathResult.enumerationStopReason.empty(),
                 "register_path_query legacy max_paths does not truncate enumeration");
}

} // namespace

int main() {
    TestReport report;
    Netlist netlist;

    report.check(loadCircuit(netlist), "test8 load register timeout circuit");
    if (report.failed == 0) {
        testRegisterPathCountOnly(report, netlist);
        testLegacyRegisterPathLimitDoesNotTruncate(report, netlist);
    }

    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
