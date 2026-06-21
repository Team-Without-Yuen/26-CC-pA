#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
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

Netlist::PathNode gateNode(const std::string& name) {
    return Netlist::PathNode(Netlist::PathNodeType::Gate, name);
}

bool pathContainsGate(const Netlist& netlist,
                      const Netlist::CombinationalPath& path,
                      const std::string& gateName) {
    for (int gateId : path.gateIds) {
        if (netlist.isValidGateId(gateId) &&
            netlist.getGate(gateId).instName == gateName) {
            return true;
        }
    }
    return false;
}

bool fileContains(const std::string& path, const std::string& pattern) {
    std::ifstream input(path);
    std::string contents((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
    return contents.find(pattern) != std::string::npos;
}

void testRegisterPathCircuit(TestReport& report, const Netlist& netlist) {
    report.check(netlist.getGateId("ff1") >= 0 &&
                 netlist.getGateId("ff2") >= 0 &&
                 netlist.getGateId("ff3") >= 0,
                 "loaded three DFFs");

    report.check(netlist.getDffOutputNetId(netlist.getGateId("ff1")) ==
                 netlist.getNetId("q1"),
                 "ff1 Q resolves to q1");
    report.check(netlist.getDffInputNetId(netlist.getGateId("ff2"), "D") ==
                 netlist.getNetId("d2"),
                 "ff2 D resolves to d2");

    Netlist::RegisterPathQuery query;
    query.mode = Netlist::RegisterPathQueryMode::Exists;
    Netlist::RegisterPathReport result = netlist.runRegisterPathQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.startDffNames.size() == 3 &&
                 result.endDffNames.size() == 3,
                 "register path exists for all DFFs");

    query = Netlist::RegisterPathQuery();
    query.mode = Netlist::RegisterPathQueryMode::MaxDepth;
    query.startDffNames = {"ff1"};
    query.endDffNames = {"ff2"};
    result = netlist.runRegisterPathQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.depth == 2 &&
                 result.startDffName == "ff1" &&
                 result.endDffName == "ff2" &&
                 pathContainsGate(netlist, result.pathResult.path, "g12a") &&
                 pathContainsGate(netlist, result.pathResult.path, "g12b"),
                 "specific ff1.Q to ff2.D max depth path");

    query = Netlist::RegisterPathQuery();
    query.mode = Netlist::RegisterPathQueryMode::Exists;
    query.startDffNames = {"ff1"};
    query.endDffNames = {"ff2"};
    query.avoidedNodes.push_back(gateNode("g12b"));
    result = netlist.runRegisterPathQuery(query);
    report.check(result.ok && !result.exists,
                 "avoid gate g12b blocks ff1 to ff2 path");

    query = Netlist::RegisterPathQuery();
    query.mode = Netlist::RegisterPathQueryMode::EnumerateAll;
    query.outputFilePath = "mini test/register_paths_output.txt";
    result = netlist.runRegisterPathQuery(query);
    report.check(result.ok &&
                 result.exists &&
                 result.pathResult.pathCount == 3 &&
                 result.pathResult.wrotePathsToFile &&
                 result.pathResult.outputFilePath == query.outputFilePath &&
                 fileContains(query.outputFilePath, "Total paths: 3"),
                 "enumerate all register paths writes three paths");

    Netlist::PathQuery pathQuery;
    pathQuery.mode = Netlist::PathQueryMode::MaxDepth;
    pathQuery.startpoints.push_back(
        Netlist::PathEndpoint(Netlist::PathEndpointType::DffQ, ""));
    pathQuery.endpoints.push_back(
        Netlist::PathEndpoint(Netlist::PathEndpointType::DffD, ""));
    Netlist::PathQueryResult pathResult = netlist.runPathQuery(pathQuery);
    report.check(pathResult.exists &&
                 pathResult.depth == 2 &&
                 pathContainsGate(netlist, pathResult.path, "g12a") &&
                 pathContainsGate(netlist, pathResult.path, "g12b"),
                 "PathQuery supports all DFF.Q to all DFF.D max depth");

    pathQuery = Netlist::PathQuery();
    pathQuery.mode = Netlist::PathQueryMode::EnumerateAll;
    pathQuery.startpoints.push_back(
        Netlist::PathEndpoint(Netlist::PathEndpointType::DffQ, "*"));
    pathQuery.endpoints.push_back(
        Netlist::PathEndpoint(Netlist::PathEndpointType::DffD, "*"));
    pathQuery.outputFilePath = "mini test/path_query_all_register_paths.txt";
    pathResult = netlist.runPathQuery(pathQuery);
    report.check(pathResult.exists &&
                 pathResult.pathCount == 3 &&
                 pathResult.wrotePathsToFile &&
                 fileContains(pathQuery.outputFilePath, "Total paths: 3"),
                 "PathQuery enumerates all DFF.Q to all DFF.D paths");

    query = Netlist::RegisterPathQuery();
    query.mode = Netlist::RegisterPathQueryMode::MaxDepth;
    query.startDffNames = {"not_a_ff"};
    result = netlist.runRegisterPathQuery(query);
    report.check(!result.ok && !result.message.empty(),
                 "invalid DFF name is rejected");
}

} // namespace

int main(int argc, char* argv[]) {
    const std::string verilogPath =
        (argc >= 2) ? argv[1] : "mini test/register_path_circuit.v";

    TestReport report;
    Netlist netlist;
    VerilogReader reader;
    report.check(reader.read(verilogPath, netlist), "VerilogReader::read register_path_circuit");
    if (report.failed == 0) {
        testRegisterPathCircuit(report, netlist);
    }

    std::cout << "\nSummary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
