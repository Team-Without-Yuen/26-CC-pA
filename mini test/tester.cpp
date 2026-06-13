#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

// 簡單測試報告器：每個檢查印出 PASS / FAIL，最後用 failed 數量作為 exit code。
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

// 判斷字串陣列是否包含指定字串。
bool containsString(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

// 判斷整數陣列是否包含指定整數。
bool containsInt(const std::vector<int>& values, int target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

// 建立 net 類型的 path condition node。
Netlist::PathNode netNode(const std::string& name) {
    return Netlist::PathNode(Netlist::PathNodeType::Net, name);
}

// 建立 gate 類型的 path condition node。
Netlist::PathNode gateNode(const std::string& name) {
    return Netlist::PathNode(Netlist::PathNodeType::Gate, name);
}

// 檢查路徑中是否經過指定 gate。
bool pathContainsGate(const Netlist& netlist,
                      const Netlist::CombinationalPath& path,
                      const std::string& gateName) {
    for (int gateId : path.gateIds) {
        if (netlist.isValidGateId(gateId) && netlist.getGate(gateId).instName == gateName) {
            return true;
        }
    }
    return false;
}

// 檢查路徑中是否經過指定 net。
bool pathContainsNet(const Netlist& netlist,
                     const Netlist::CombinationalPath& path,
                     const std::string& netName) {
    for (int netId : path.netIds) {
        if (netlist.isValidNetId(netId) && netlist.getNet(netId).name == netName) {
            return true;
        }
    }
    return false;
}

// 測試 Basic Query：規模、型別、port、gate/net 基本資訊。
void testBasicQuery(TestReport& report, const Netlist& netlist) {
    report.check(netlist.getGateCount() == 16, "basic getGateCount");
    report.check(netlist.getNetCount() == 25, "basic getNetCount");
    report.check(netlist.getLogicalWireCount() == 24, "basic getLogicalWireCount");
    report.check(netlist.getPrimaryInputs().size() == 6, "basic PI port count");
    report.check(netlist.getPrimaryOutputs().size() == 5, "basic PO port count");

    report.check(netlist.getGateId("g_and") >= 0 &&
                 netlist.getGateId("missing_gate") == -1,
                 "basic gate id lookup");
    report.check(netlist.getNetId("n_buf") >= 0 &&
                 netlist.getNetId("missing_net") == -1,
                 "basic net id lookup");

    report.check(netlist.isDffGate(netlist.getGateId("ff1")) &&
                 netlist.isCombinationalGate(netlist.getGateId("g_and")),
                 "basic DFF/combinational classification");
    report.check(netlist.isPrimaryInputNet(netlist.getNetId("a")) &&
                 netlist.isPrimaryOutputNet(netlist.getNetId("y")),
                 "basic PI/PO net classification");

    report.check(netlist.getPortWidth("bus") == 2 &&
                 netlist.isBusPort("bus") &&
                 containsString(netlist.getPortBitNames("bus"), "bus[0]"),
                 "basic bus port helpers");

    Netlist::BasicQuery summaryQuery;
    summaryQuery.type = Netlist::BasicQueryType::Summary;
    const Netlist::BasicReport summary = netlist.runBasicQuery(summaryQuery);
    report.check(summary.ok &&
                 summary.gateCount == 16 &&
                 summary.netCount == 25 &&
                 summary.gateTypeCounts.at(GateType::BUF) == 6 &&
                 summary.gateTypeCounts.at(GateType::XOR) == 2,
                 "runBasicQuery Summary");

    Netlist::BasicQuery gateInfoQuery;
    gateInfoQuery.type = Netlist::BasicQueryType::GateInfo;
    gateInfoQuery.name = "g_buf";
    const Netlist::BasicReport gateInfo = netlist.runBasicQuery(gateInfoQuery);
    report.check(gateInfo.ok &&
                 gateInfo.exists &&
                 gateInfo.objectId == netlist.getGateId("g_buf") &&
                 gateInfo.typeName == "BUF",
                 "runBasicQuery GateInfo");

    Netlist::BasicQuery netInfoQuery;
    netInfoQuery.type = Netlist::BasicQueryType::NetInfo;
    netInfoQuery.name = "a";
    const Netlist::BasicReport netInfo = netlist.runBasicQuery(netInfoQuery);
    report.check(netInfo.ok &&
                 netInfo.exists &&
                 netInfo.isPrimaryInput &&
                 !netInfo.isPrimaryOutput,
                 "runBasicQuery NetInfo");

    Netlist::BasicQuery constInputQuery;
    constInputQuery.type = Netlist::BasicQueryType::GatesWithConstantInput;
    constInputQuery.constValue = 1;
    const Netlist::BasicReport constInput = netlist.runBasicQuery(constInputQuery);
    report.check(constInput.ok &&
                 constInput.gateCount == 1 &&
                 containsString(constInput.gateNames, "g_nand"),
                 "runBasicQuery GatesWithConstantInput");
}

// 測試 Direct Connectivity Query：driver、loads、gate input/output、fanin/fanout。
void testDirectConnectivityQuery(TestReport& report, const Netlist& netlist) {
    Netlist::DirectConnectivityQuery query;
    query.type = Netlist::DirectConnectivityQueryType::NetDriver;
    query.netName = "n_or";
    Netlist::DirectConnectivityReport direct = netlist.runDirectConnectivityQuery(query);
    report.check(direct.ok &&
                 direct.count == 1 &&
                 containsString(direct.gateNames, "g_or"),
                 "runDirectConnectivityQuery NetDriver");

    query = Netlist::DirectConnectivityQuery();
    query.type = Netlist::DirectConnectivityQueryType::NetLoads;
    query.netName = "n_or";
    direct = netlist.runDirectConnectivityQuery(query);
    report.check(direct.ok &&
                 direct.count == 3 &&
                 containsString(direct.gateNames, "g_nand") &&
                 containsString(direct.gateNames, "g_not") &&
                 containsString(direct.gateNames, "g_z"),
                 "runDirectConnectivityQuery NetLoads");

    query = Netlist::DirectConnectivityQuery();
    query.type = Netlist::DirectConnectivityQueryType::GateInputs;
    query.gateName = "g_or";
    direct = netlist.runDirectConnectivityQuery(query);
    report.check(direct.ok &&
                 direct.count == 2 &&
                 containsString(direct.netNames, "n_and") &&
                 containsString(direct.netNames, "c"),
                 "runDirectConnectivityQuery GateInputs");

    query = Netlist::DirectConnectivityQuery();
    query.type = Netlist::DirectConnectivityQueryType::GateFanout;
    query.gateName = "g_or";
    direct = netlist.runDirectConnectivityQuery(query);
    report.check(direct.ok &&
                 direct.count == 3 &&
                 containsString(direct.gateNames, "g_nand") &&
                 containsString(direct.gateNames, "g_z"),
                 "runDirectConnectivityQuery GateFanout");

    query = Netlist::DirectConnectivityQuery();
    query.type = Netlist::DirectConnectivityQueryType::DirectlyConnected;
    query.gateName = "g_or";
    query.netName = "n_and";
    direct = netlist.runDirectConnectivityQuery(query);
    report.check(direct.ok && direct.connected, "runDirectConnectivityQuery DirectlyConnected");
}

// 測試 Function Query：SAT-based equivalence、可能值、常數函數判斷。
void testFunctionQuery(TestReport& report, const Netlist& netlist) {
    Netlist::FunctionQuery query;
    query.type = Netlist::FunctionQueryType::Equivalence;
    query.netNameA = "y";
    query.netNameB = "y";
    Netlist::FunctionReport function = netlist.runFunctionQuery(query);
    report.check(function.ok && function.equivalent, "runFunctionQuery Equivalence true");

    query.netNameB = "z";
    function = netlist.runFunctionQuery(query);
    report.check(function.ok && !function.equivalent, "runFunctionQuery Equivalence false");

    query = Netlist::FunctionQuery();
    query.type = Netlist::FunctionQueryType::CanBeValue;
    query.netNameA = "a";
    query.constValue = 0;
    function = netlist.runFunctionQuery(query);
    report.check(function.ok && function.exists && function.canBeZero,
                 "runFunctionQuery CanBeValue zero");

    Netlist constantNetlist = netlist;
    const int constZero = constantNetlist.addNet("1'b0");
    const int zeroNet = constantNetlist.addNet("const_zero_net");
    const int zeroBuf = constantNetlist.addGate("g_const_zero", GateType::BUF);
    constantNetlist.connectGateInput(zeroBuf, constZero);
    constantNetlist.connectGateOutput(zeroBuf, zeroNet);

    query = Netlist::FunctionQuery();
    query.type = Netlist::FunctionQueryType::AlwaysZero;
    query.netNameA = "const_zero_net";
    function = constantNetlist.runFunctionQuery(query);
    report.check(function.ok &&
                 function.exists &&
                 function.isConstant &&
                 function.constValue == 0,
                 "runFunctionQuery AlwaysZero");
}

// 測試 Cone Query：fanin/fanout cone 與 cone-local path。
void testConeQuery(TestReport& report, const Netlist& netlist) {
    Netlist::ConeQuery query;
    query.type = Netlist::ConeQueryType::NetTransitiveFanin;
    query.netName = "y";
    query.includeLocalPaths = true;
    Netlist::ConeReport cone = netlist.runConeQuery(query);
    report.check(cone.ok &&
                 cone.exists &&
                 containsString(cone.netNames, "n_and") &&
                 containsString(cone.gateNames, "g_buf") &&
                 cone.longestDepth == 9,
                 "runConeQuery NetTransitiveFanin");

    query = Netlist::ConeQuery();
    query.type = Netlist::ConeQueryType::NetTransitiveFanout;
    query.netName = "n_or";
    cone = netlist.runConeQuery(query);
    report.check(cone.ok &&
                 cone.exists &&
                 containsString(cone.netNames, "z") &&
                 containsString(cone.gateNames, "g_z"),
                 "runConeQuery NetTransitiveFanout");

    query = Netlist::ConeQuery();
    query.type = Netlist::ConeQueryType::GateTransitiveFanin;
    query.gateName = "g_y";
    cone = netlist.runConeQuery(query);
    report.check(cone.ok &&
                 containsString(cone.gateNames, "g_and") &&
                 containsString(cone.netNames, "n_buf"),
                 "runConeQuery GateTransitiveFanin");
}

// 測試 Path Query：endpoint resolver、exist/find/min/max/every constraints。
void testPathQuery(TestReport& report, const Netlist& netlist) {
    report.check(netlist.getDffInputNetId(netlist.getGateId("ff1"), "D") ==
                 netlist.getNetId("n_buf"),
                 "path helper getDffInputNetId D");
    report.check(netlist.getDffOutputNetId(netlist.getGateId("ff1")) ==
                 netlist.getNetId("q"),
                 "path helper getDffOutputNetId Q");

    Netlist::PathQuery query;
    query.mode = Netlist::PathQueryMode::Exists;
    query.startpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryInput, "a"));
    query.endpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryOutput, "y"));
    Netlist::PathQueryResult path = netlist.runPathQuery(query);
    report.check(path.exists, "runPathQuery Exists");

    query = Netlist::PathQuery();
    query.mode = Netlist::PathQueryMode::MaxDepth;
    query.startpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "a"));
    query.endpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "y"));
    path = netlist.runPathQuery(query);
    report.check(path.exists &&
                 path.depth == 9 &&
                 pathContainsGate(netlist, path.path, "g_buf"),
                 "runPathQuery MaxDepth");

    query.mode = Netlist::PathQueryMode::MinDepth;
    path = netlist.runPathQuery(query);
    report.check(path.exists &&
                 path.depth == 9,
                 "runPathQuery MinDepth");

    query = Netlist::PathQuery();
    query.mode = Netlist::PathQueryMode::FindAny;
    query.startpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "a"));
    query.endpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "y"));
    query.requiredNodes.push_back(gateNode("g_nand"));
    query.avoidedNodes.push_back(netNode("z"));
    path = netlist.runPathQuery(query);
    report.check(path.exists &&
                 pathContainsGate(netlist, path.path, "g_nand") &&
                 !pathContainsNet(netlist, path.path, "z"),
                 "runPathQuery required/avoided nodes");

    query.mode = Netlist::PathQueryMode::EveryPathThrough;
    query.requiredNodes.clear();
    query.requiredNodes.push_back(gateNode("g_y"));
    path = netlist.runPathQuery(query);
    report.check(path.exists, "runPathQuery EveryPathThrough");

    query.mode = Netlist::PathQueryMode::EveryPathAvoids;
    query.requiredNodes.clear();
    query.avoidedNodes.push_back(netNode("z"));
    path = netlist.runPathQuery(query);
    report.check(path.exists, "runPathQuery EveryPathAvoids");
}

// 測試 Depth Query：level、critical path、PO/DFF.D endpoint report。
void testDepthQuery(TestReport& report, const Netlist& netlist) {
    const std::vector<int> netLevels = netlist.computeNetLevels();
    report.check(netLevels[netlist.getNetId("a")] == 0 &&
                 netLevels[netlist.getNetId("n_buf")] == 8 &&
                 netLevels[netlist.getNetId("y")] == 9,
                 "computeNetLevels");

    const std::vector<int> gateLevels = netlist.computeGateLevels();
    report.check(gateLevels[netlist.getGateId("g_and")] == 1 &&
                 gateLevels[netlist.getGateId("g_buf")] == 8 &&
                 gateLevels[netlist.getGateId("ff1")] == -1,
                 "computeGateLevels");

    const Netlist::CombinationalPath criticalY = netlist.findCriticalPathToNet("y");
    report.check(criticalY.exists() &&
                 criticalY.depth() == 9 &&
                 pathContainsGate(netlist, criticalY, "g_buf"),
                 "findCriticalPathToNet");

    Netlist::DepthQuery query;
    query.type = Netlist::DepthQueryType::SpecificNet;
    query.netName = "y";
    Netlist::DepthReportSet depth = netlist.runDepthQuery(query);
    report.check(depth.ok &&
                 depth.count == 1 &&
                 depth.worst.depth == 9,
                 "runDepthQuery SpecificNet");

    query = Netlist::DepthQuery();
    query.type = Netlist::DepthQueryType::DffD;
    depth = netlist.runDepthQuery(query);
    report.check(depth.ok &&
                 depth.count == 1 &&
                 depth.reports.front().endpointName == "ff1.D" &&
                 depth.reports.front().depth == 8,
                 "runDepthQuery DffD");

    query = Netlist::DepthQuery();
    query.type = Netlist::DepthQueryType::GlobalCriticalPath;
    depth = netlist.runDepthQuery(query);
    report.check(depth.ok &&
                 depth.worst.endpointName == "y" &&
                 depth.worst.depth == 9,
                 "runDepthQuery GlobalCriticalPath");
}

// 測試 writer 與少量 transformation primitive，確保 tester 仍覆蓋基本修改流程。
void testWriterAndSmallMutation(TestReport& report, const Netlist& original) {
    VerilogWriter writer;
    report.check(writer.write("mini test/writer_output.v", original), "VerilogWriter::write");

    Netlist netlist = original;
    report.check(netlist.renameGate("g_direct", "g_direct_renamed") &&
                 netlist.getGateId("g_direct") == -1 &&
                 netlist.getGateId("g_direct_renamed") >= 0,
                 "renameGate");
    report.check(netlist.disconnectGateInput("g_y", "n_buf") &&
                 !netlist.hasCombinationalPath("a", "y"),
                 "disconnectGateInput updates path");
    report.check(netlist.connectGateInput("g_y", "n_buf") &&
                 netlist.hasCombinationalPath("a", "y"),
                 "connectGateInput restores path");
}

} // namespace

// 執行 mini Verilog 整合測試；可用 argv[1] 指定其他 Verilog 檔。
int main(int argc, char* argv[]) {
    const std::string verilogPath =
        (argc >= 2) ? argv[1] : "mini test/mini_circuit.v";

    TestReport report;
    Netlist netlist;
    VerilogReader reader;
    report.check(reader.read(verilogPath, netlist), "VerilogReader::read");
    if (report.failed != 0) {
        std::cout << "\nCannot continue because the test Verilog could not be read.\n";
        return report.failed;
    }

    testBasicQuery(report, netlist);
    testDirectConnectivityQuery(report, netlist);
    testFunctionQuery(report, netlist);
    testConeQuery(report, netlist);
    testPathQuery(report, netlist);
    testDepthQuery(report, netlist);
    testWriterAndSmallMutation(report, netlist);

    std::cout << "\nSummary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
