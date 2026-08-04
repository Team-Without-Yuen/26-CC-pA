#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
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
            std::cout << "[PASS] " << name << "\n" << std::flush;
        } else {
            ++failed;
            std::cout << "[FAIL] " << name << "\n" << std::flush;
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

std::vector<std::string> netIdsToNames(const Netlist& netlist,
                                       const std::vector<int>& netIds) {
    std::vector<std::string> names;
    names.reserve(netIds.size());
    for (int netId : netIds) {
        if (!netlist.isValidNetId(netId)) {
            return {};
        }
        names.push_back(netlist.getNet(netId).name);
    }
    return names;
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

    const std::vector<std::string> expectedBusBits = {"bus[1]", "bus[0]"};
    Netlist::BasicQuery portInfoQuery;
    portInfoQuery.type = Netlist::BasicQueryType::PortInfo;
    portInfoQuery.name = "bus";
    const Netlist::BasicReport portInfo = netlist.runBasicQuery(portInfoQuery);
    report.check(portInfo.ok && portInfo.exists && portInfo.isBus &&
                     portInfo.isPrimaryInput && !portInfo.isPrimaryOutput &&
                     portInfo.portWidth == 2 &&
                     portInfo.netNames == expectedBusBits &&
                     netIdsToNames(netlist, portInfo.netIds) == expectedBusBits,
                 "runBasicQuery PortInfo preserves declaration-order name/id alignment");

    portInfoQuery.includeIds = false;
    const Netlist::BasicReport portNamesOnly = netlist.runBasicQuery(portInfoQuery);
    report.check(portNamesOnly.netIds.empty() &&
                     portNamesOnly.netNames == expectedBusBits,
                 "runBasicQuery PortInfo names-only ordering");

    portInfoQuery.includeIds = true;
    portInfoQuery.includeNames = false;
    const Netlist::BasicReport portIdsOnly = netlist.runBasicQuery(portInfoQuery);
    report.check(portIdsOnly.netNames.empty() && portIdsOnly.portNames.empty() &&
                     netIdsToNames(netlist, portIdsOnly.netIds) == expectedBusBits,
                 "runBasicQuery PortInfo ids-only ordering");

    portInfoQuery.includeIds = false;
    const Netlist::BasicReport portMetadataOnly = netlist.runBasicQuery(portInfoQuery);
    report.check(portMetadataOnly.ok && portMetadataOnly.netIds.empty() &&
                     portMetadataOnly.netNames.empty() &&
                     portMetadataOnly.portNames.empty() &&
                     portMetadataOnly.isPrimaryInput,
                 "runBasicQuery PortInfo metadata-only query");

    portInfoQuery.name = "missing_port";
    const Netlist::BasicReport missingPort = netlist.runBasicQuery(portInfoQuery);
    report.check(!missingPort.ok && !missingPort.exists &&
                     missingPort.portWidth == -1,
                 "runBasicQuery PortInfo missing port");

    Netlist::BasicQuery listPortsQuery;
    listPortsQuery.type = Netlist::BasicQueryType::ListPrimaryInputs;
    listPortsQuery.includeNames = false;
    const Netlist::BasicReport inputMetadata = netlist.runBasicQuery(listPortsQuery);
    const auto busSummary = std::find_if(
        inputMetadata.ports.begin(), inputMetadata.ports.end(),
        [](const PortSummary& port) { return port.name == "bus"; });
    report.check(inputMetadata.ok && inputMetadata.portNames.empty() &&
                     inputMetadata.ports.size() == inputMetadata.primaryInputCount &&
                     busSummary != inputMetadata.ports.end() && busSummary->width == 2 &&
                     busSummary->msb == 1 && busSummary->lsb == 0 &&
                     busSummary->isBus && busSummary->isInput && !busSummary->isOutput,
                 "runBasicQuery ListPrimaryInputs preserves structured metadata");

    listPortsQuery.type = Netlist::BasicQueryType::ListPrimaryOutputs;
    const Netlist::BasicReport outputMetadata = netlist.runBasicQuery(listPortsQuery);
    report.check(outputMetadata.ok && outputMetadata.portNames.empty() &&
                     outputMetadata.ports.size() == outputMetadata.primaryOutputCount &&
                     std::all_of(outputMetadata.ports.begin(), outputMetadata.ports.end(),
                         [](const PortSummary& port) {
                             return !port.isInput && port.isOutput;
                         }),
                 "runBasicQuery ListPrimaryOutputs preserves structured metadata");

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

    constInputQuery.inputCount = 2;
    const Netlist::BasicReport constInputByArity =
        netlist.runBasicQuery(constInputQuery);
    report.check(constInputByArity.ok && constInputByArity.gateCount == 1 &&
                     containsString(constInputByArity.gateNames, "g_nand"),
                 "runBasicQuery GatesWithConstantInput input-count filter");

    constInputQuery.constValue = 2;
    const Netlist::BasicReport invalidConstValue =
        netlist.runBasicQuery(constInputQuery);
    report.check(!invalidConstValue.ok && invalidConstValue.gateCount == 0,
                 "runBasicQuery rejects invalid constant-input value");

    constInputQuery.constValue = 1;
    constInputQuery.inputCount = -2;
    const Netlist::BasicReport invalidInputCount =
        netlist.runBasicQuery(constInputQuery);
    report.check(!invalidInputCount.ok && invalidInputCount.gateCount == 0,
                 "runBasicQuery rejects invalid constant-input arity");
}

// 測試 Direct Connectivity Query：driver、loads、gate input/output、fanin/fanout。
void testDirectConnectivityQuery(TestReport& report, const Netlist& netlist) {
    Netlist::DirectConnectivityQuery query;
    query.type = Netlist::DirectConnectivityQueryType::NetDriverGates;
    query.netName = "n_or";
    Netlist::DirectConnectivityReport direct = netlist.runDirectConnectivityQuery(query);
    report.check(direct.ok &&
                 direct.count == 1 &&
                 containsString(direct.gateNames, "g_or"),
                 "runDirectConnectivityQuery NetDriverGates");

    query = Netlist::DirectConnectivityQuery();
    query.type = Netlist::DirectConnectivityQueryType::NetLoadGates;
    query.netName = "n_or";
    direct = netlist.runDirectConnectivityQuery(query);
    report.check(direct.ok &&
                 direct.count == 3 &&
                 containsString(direct.gateNames, "g_nand") &&
                 containsString(direct.gateNames, "g_not") &&
                 containsString(direct.gateNames, "g_z"),
                 "runDirectConnectivityQuery NetLoadGates");

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

    const Netlist::FanoutLoadReport nBufFanout = netlist.getFanoutLoadReport("n_buf");
    report.check(nBufFanout.ok &&
                 nBufFanout.totalLoadCount == 2 &&
                 nBufFanout.combinationalGateLoads.size() == 1 &&
                 nBufFanout.dffDataLoads.size() == 1 &&
                 containsInt(nBufFanout.combinationalGateLoads, netlist.getGateId("g_y")) &&
                 containsInt(nBufFanout.dffDataLoads, netlist.getGateId("ff1")),
                 "fanout load report classifies gate input and DFF D");

    const Netlist::FanoutLoadReport clkFanout = netlist.getFanoutLoadReport("clk");
    report.check(clkFanout.ok &&
                 clkFanout.totalLoadCount == 1 &&
                 clkFanout.dffClockLoads.size() == 1 &&
                 containsInt(clkFanout.dffClockLoads, netlist.getGateId("ff1")),
                 "fanout load report classifies DFF CK");

    const Netlist::FanoutLoadReport rstFanout = netlist.getFanoutLoadReport("rst_n");
    report.check(rstFanout.ok &&
                 rstFanout.totalLoadCount == 2 &&
                 rstFanout.dffResetSetLoads.size() == 2,
                 "fanout load report counts DFF RN and SN as separate loads");

    const Netlist::FanoutLoadReport yFanout = netlist.getFanoutLoadReport("y");
    report.check(yFanout.ok &&
                 yFanout.drivesPrimaryOutput &&
                 yFanout.primaryOutputLoadCount == 1 &&
                 yFanout.totalLoadCount == 1 &&
                 netlist.getGateFanoutCount("g_y") == 0,
                 "fanout load report includes primary output load");

    query = Netlist::DirectConnectivityQuery();
    query.type = Netlist::DirectConnectivityQueryType::FanoutLoadReport;
    query.netName = "rst_n";
    direct = netlist.runDirectConnectivityQuery(query);
    report.check(direct.ok &&
                 direct.count == 2 &&
                 direct.fanoutLoadReport.dffResetSetLoads.size() == 2,
                 "runDirectConnectivityQuery FanoutLoadReport");

    const Netlist::GlobalFanoutReport globalFanout =
        netlist.getGlobalFanoutReport(/*maxFanoutLimit=*/2);
    report.check(globalFanout.ok &&
                 globalFanout.maxFanout == 3 &&
                 !globalFanout.satisfiesLimit &&
                 !globalFanout.violatingReports.empty(),
                 "global fanout report finds max and violations");

    const Netlist::GlobalFanoutReport piFanout =
        netlist.getGlobalFanoutReport(/*maxFanoutLimit=*/-1,
                                      /*primaryInputsOnly=*/true);
    bool hasA = false;
    bool hasC = false;
    for (const Netlist::FanoutLoadReport& fanout : piFanout.maxFanoutReports) {
        hasA = hasA || fanout.netName == "a";
        hasC = hasC || fanout.netName == "c";
    }
    report.check(piFanout.ok &&
                 piFanout.maxFanout == 3 &&
                 hasA &&
                 hasC,
                 "primary-input global fanout report finds highest PI fanout");

    query = Netlist::DirectConnectivityQuery();
    query.type = Netlist::DirectConnectivityQueryType::GlobalFanoutReport;
    query.fanoutLimit = 2;
    direct = netlist.runDirectConnectivityQuery(query);
    report.check(direct.ok &&
                 direct.globalFanoutReport.maxFanout == 3 &&
                 !direct.globalFanoutReport.satisfiesLimit,
                 "runDirectConnectivityQuery GlobalFanoutReport");
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

    query = Netlist::PathQuery();
    query.mode = Netlist::PathQueryMode::EnumerateAll;
    query.startpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "a"));
    query.endpoints.push_back(Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "y"));
    query.outputFilePath = "mini test/path_enum_output.txt";
    path = netlist.runPathQuery(query);

    std::ifstream pathFile(query.outputFilePath);
    std::string pathFileContents((std::istreambuf_iterator<char>(pathFile)),
                                 std::istreambuf_iterator<char>());
    report.check(path.exists &&
                 path.pathCount == path.paths.size() &&
                 path.wrotePathsToFile &&
                 path.outputFilePath == query.outputFilePath &&
                 pathFileContents.find("Total paths:") != std::string::npos &&
                 pathFileContents.find("Path 0") != std::string::npos,
                 "runPathQuery EnumerateAll writes complete paths to file");
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

// 測試 RegisterPathQuery：自動把 DFF.Q 展開成起點、DFF.D 展開成終點。
void testRegisterPathQuery(TestReport& report) {
    Netlist regNetlist;
    const int d1 = regNetlist.addNet("d1");
    const int q1 = regNetlist.addNet("q1");
    const int a = regNetlist.addNet("a");
    const int mid = regNetlist.addNet("mid");
    const int d2 = regNetlist.addNet("d2");
    const int q2 = regNetlist.addNet("q2");

    const int ff1 = regNetlist.addGate("ff1", GateType::DFF);
    regNetlist.connectGateInput(ff1, d1, "D");
    regNetlist.connectGateOutput(ff1, q1);

    const int g1 = regNetlist.addGate("g1", GateType::AND);
    regNetlist.connectGateInput(g1, q1);
    regNetlist.connectGateInput(g1, a);
    regNetlist.connectGateOutput(g1, mid);

    const int g2 = regNetlist.addGate("g2", GateType::BUF);
    regNetlist.connectGateInput(g2, mid);
    regNetlist.connectGateOutput(g2, d2);

    const int ff2 = regNetlist.addGate("ff2", GateType::DFF);
    regNetlist.connectGateInput(ff2, d2, "D");
    regNetlist.connectGateOutput(ff2, q2);

    Netlist::RegisterPathQuery query;
    query.mode = Netlist::RegisterPathQueryMode::MaxDepth;
    Netlist::RegisterPathReport regPath = regNetlist.runRegisterPathQuery(query);
    report.check(regPath.ok &&
                 regPath.exists &&
                 regPath.depth == 2 &&
                 regPath.startDffName == "ff1" &&
                 regPath.endDffName == "ff2",
                 "runRegisterPathQuery MaxDepth all DFFs");

    query.mode = Netlist::RegisterPathQueryMode::Exists;
    query.startDffNames = {"ff1"};
    query.endDffNames = {"ff2"};
    regPath = regNetlist.runRegisterPathQuery(query);
    report.check(regPath.ok && regPath.exists,
                 "runRegisterPathQuery Exists specific DFFs");

    query.mode = Netlist::RegisterPathQueryMode::Exists;
    query.avoidedNodes.push_back(gateNode("g1"));
    regPath = regNetlist.runRegisterPathQuery(query);
    report.check(regPath.ok && !regPath.exists,
                 "runRegisterPathQuery avoided gate blocks path");

    query = Netlist::RegisterPathQuery();
    query.mode = Netlist::RegisterPathQueryMode::EnumerateAll;
    query.outputFilePath = "mini test/reg_path_enum_output.txt";
    regPath = regNetlist.runRegisterPathQuery(query);
    std::ifstream regPathFile(query.outputFilePath);
    std::string regPathFileContents((std::istreambuf_iterator<char>(regPathFile)),
                                    std::istreambuf_iterator<char>());
    report.check(regPath.ok &&
                 regPath.exists &&
                 regPath.pathResult.pathCount == 1 &&
                 regPath.pathResult.wrotePathsToFile &&
                 regPath.pathResult.outputFilePath == query.outputFilePath &&
                 regPathFileContents.find("Path 0") != std::string::npos,
                 "runRegisterPathQuery EnumerateAll writes file");

    query = Netlist::RegisterPathQuery();
    query.mode = Netlist::RegisterPathQueryMode::MaxDepth;
    query.startDffNames = {"missing_ff"};
    regPath = regNetlist.runRegisterPathQuery(query);
    report.check(!regPath.ok && !regPath.message.empty(),
                 "runRegisterPathQuery rejects invalid DFF");
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

    Netlist renameGateNetlist = original.cloneForRollback();
    const Netlist::NetlistEditReport renameGateReport =
        renameGateNetlist.renameGateWithReport("g_direct", "g_direct_report");
    report.check(renameGateReport.success &&
                 renameGateReport.changed &&
                 !renameGateReport.rolledBack &&
                 renameGateReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 renameGateReport.validation.equivalenceChecked &&
                 renameGateReport.validation.functionallyEquivalent &&
                 renameGateReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 renameGateNetlist.getGateId("g_direct_report") >= 0,
                 "renameGateWithReport");

    Netlist renameNetNetlist = original.cloneForRollback();
    const Netlist::NetlistEditReport renameNetReport =
        renameNetNetlist.renameNetWithReport("n_buf", "n_buf_report");
    report.check(renameNetReport.success &&
                 renameNetReport.changed &&
                 !renameNetReport.rolledBack &&
                 renameNetReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 renameNetReport.validation.equivalenceChecked &&
                 renameNetReport.validation.functionallyEquivalent &&
                 renameNetReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 renameNetNetlist.getNetId("n_buf_report") >= 0,
                 "renameNetWithReport");

    Netlist reconnectNetlist = original.cloneForRollback();
    const Netlist::NetlistEditReport disconnectReport =
        reconnectNetlist.disconnectGateInputWithReport("g_y", "n_buf");
    const Netlist::NetlistEditReport connectReport =
        reconnectNetlist.connectGateInputWithReport("g_y", "n_buf");
    report.check(disconnectReport.success &&
                 connectReport.success &&
                 disconnectReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 connectReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 !disconnectReport.validation.equivalenceChecked &&
                 !connectReport.validation.equivalenceChecked &&
                 reconnectNetlist.hasCombinationalPath("a", "y"),
                 "disconnect/connectGateInputWithReport");

    Netlist replaceLoadsNetlist = original.cloneForRollback();
    const Netlist::NetlistEditReport replaceLoadsReport =
        replaceLoadsNetlist.replaceAllLoadsOfNetWithReport(
            replaceLoadsNetlist.getNetId("n_buf"),
            replaceLoadsNetlist.getNetId("a"));
    report.check(replaceLoadsReport.success &&
                 replaceLoadsReport.changed &&
                 !replaceLoadsReport.rolledBack &&
                 replaceLoadsReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 replaceLoadsNetlist.validateAfterMutation(),
                 "replaceAllLoadsOfNetWithReport");

    Netlist removeGateNetlist = original.cloneForRollback();
    const int directGateId = removeGateNetlist.getGateId("g_short");
    const Netlist::NetlistEditReport removeGateReport =
        removeGateNetlist.removeGateWithReport(directGateId);
    report.check(removeGateReport.success &&
                 removeGateReport.changed &&
                 !removeGateReport.rolledBack &&
                 removeGateReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 removeGateNetlist.isGateRemoved(directGateId),
                 "removeGateWithReport");

    Netlist replaceGateNetlist = original.cloneForRollback();
    const Netlist::NetlistEditReport replaceGateReport =
        replaceGateNetlist.replaceGateWithNetWithReport(
            replaceGateNetlist.getGateId("g_short"),
            replaceGateNetlist.getNetId("a"));
    report.check(replaceGateReport.success &&
                 replaceGateReport.changed &&
                 !replaceGateReport.rolledBack &&
                 replaceGateReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 !replaceGateReport.validation.equivalenceChecked &&
                 replaceGateNetlist.validateAfterMutation(),
                 "replaceGateWithNetWithReport");

    Netlist replaceConstNetlist = original.cloneForRollback();
    const Netlist::NetlistEditReport replaceConstReport =
        replaceConstNetlist.replaceGateWithConstantWithReport(
            replaceConstNetlist.getGateId("g_short"),
            replaceConstNetlist.getConst0NetId());
    report.check(replaceConstReport.success &&
                 replaceConstReport.changed &&
                 !replaceConstReport.rolledBack &&
                 replaceConstReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 replaceConstNetlist.validateAfterMutation(),
                 "replaceGateWithConstantWithReport");

    Netlist replaceNotNetlist = original.cloneForRollback();
    const Netlist::NetlistEditReport replaceNotReport =
        replaceNotNetlist.replaceGateWithNotOfNetWithReport(
            replaceNotNetlist.getGateId("g_short"),
            replaceNotNetlist.getNetId("a"));
    report.check(replaceNotReport.success &&
                 replaceNotReport.changed &&
                 !replaceNotReport.rolledBack &&
                 replaceNotReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 replaceNotNetlist.validateAfterMutation(),
                 "replaceGateWithNotOfNetWithReport");

    auto makeDriverRewriteNetlist = []() {
        Netlist n;
        n.addPrimaryInput("a");
        n.addPrimaryInput("b");
        n.addPrimaryOutput("y");
        const int aNet = n.getNetId("a");
        const int bNet = n.getNetId("b");
        const int yNet = n.getNetId("y");
        const int oldDriver = n.addGate("old_driver", GateType::AND);
        n.connectGateInput(oldDriver, aNet);
        n.connectGateInput(oldDriver, bNet);
        n.connectGateOutput(oldDriver, yNet);
        const int newDriver = n.addGate("new_driver", GateType::BUF);
        n.connectGateInput(newDriver, aNet);
        return n;
    };

    Netlist replaceDriverNetlist = makeDriverRewriteNetlist();
    const int replaceDriverTargetNet = replaceDriverNetlist.getNetId("y");
    const int replacementDriverGate = replaceDriverNetlist.getGateId("new_driver");
    const Netlist::NetlistEditReport replaceDriverReport =
        replaceDriverNetlist.replaceDriverOfNetWithReport(
            replaceDriverTargetNet,
            replacementDriverGate);
    report.check(replaceDriverReport.success &&
                 replaceDriverReport.changed &&
                 !replaceDriverReport.rolledBack &&
                 replaceDriverReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 replaceDriverNetlist.getNetDriverGateId("y") == replacementDriverGate &&
                 replaceDriverNetlist.getGate(replacementDriverGate).outputNetId == replaceDriverTargetNet &&
                 replaceDriverNetlist.validateAfterMutation(),
                 "replaceDriverOfNetWithReport");

    Netlist rewireOutputNetlist = makeDriverRewriteNetlist();
    const int rewireTargetNet = rewireOutputNetlist.getNetId("y");
    const int rewireDriverGate = rewireOutputNetlist.getGateId("new_driver");
    const Netlist::NetlistEditReport rewireOutputReport =
        rewireOutputNetlist.rewireGateOutputToExistingNetWithReport(
            rewireDriverGate,
            rewireTargetNet);
    report.check(rewireOutputReport.success &&
                 rewireOutputReport.changed &&
                 !rewireOutputReport.rolledBack &&
                 rewireOutputReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 rewireOutputNetlist.getNetDriverGateId("y") == rewireDriverGate &&
                 rewireOutputNetlist.getGate(rewireDriverGate).outputNetId == rewireTargetNet &&
                 rewireOutputNetlist.validateAfterMutation(),
                 "rewireGateOutputToExistingNetWithReport");

    Netlist replaceNetFunctionNetlist = makeDriverRewriteNetlist();
    const int poNet = replaceNetFunctionNetlist.getNetId("y");
    const int sourceNet = replaceNetFunctionNetlist.getNetId("a");
    const Netlist::NetlistEditReport replaceNetFunctionReport =
        replaceNetFunctionNetlist.replaceNetFunctionWithNetKeepingNameWithReport(
            poNet,
            sourceNet);
    const int newPoDriver = replaceNetFunctionNetlist.getNetDriverGateId("y");
    report.check(replaceNetFunctionReport.success &&
                 replaceNetFunctionReport.changed &&
                 !replaceNetFunctionReport.rolledBack &&
                 replaceNetFunctionReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 replaceNetFunctionNetlist.isPrimaryOutputNet(poNet) &&
                 newPoDriver >= 0 &&
                 replaceNetFunctionNetlist.getGate(newPoDriver).type == GateType::BUF &&
                 replaceNetFunctionNetlist.getGate(newPoDriver).inputNetIds.size() == 1 &&
                 replaceNetFunctionNetlist.getGate(newPoDriver).inputNetIds[0] == sourceNet &&
                 replaceNetFunctionNetlist.validateAfterMutation(),
                 "replaceNetFunctionWithNetKeepingNameWithReport");

    auto makeNetMergeNetlist = []() {
        Netlist n;
        n.addPrimaryInput("a");
        n.addPrimaryInput("b");
        n.addPrimaryOutput("y");
        const int aNet = n.getNetId("a");
        const int bNet = n.getNetId("b");
        const int yNet = n.getNetId("y");
        const int fromNet = n.addNet("from_net");
        const int fromDriver = n.addGate("from_driver", GateType::BUF);
        n.connectGateInput(fromDriver, aNet);
        n.connectGateOutput(fromDriver, fromNet);
        const int loadGate = n.addGate("load_gate", GateType::AND);
        n.connectGateInput(loadGate, fromNet);
        n.connectGateInput(loadGate, bNet);
        n.connectGateOutput(loadGate, yNet);
        return n;
    };

    Netlist mergeNetlist = makeNetMergeNetlist();
    const int mergeFromNet = mergeNetlist.getNetId("from_net");
    const int mergeToNet = mergeNetlist.getNetId("b");
    const int mergeLoadGate = mergeNetlist.getGateId("load_gate");
    const Netlist::NetlistEditReport mergeNetReport =
        mergeNetlist.mergeNetIntoNetWithReport(mergeFromNet, mergeToNet);
    report.check(mergeNetReport.success &&
                 mergeNetReport.changed &&
                 !mergeNetReport.rolledBack &&
                 mergeNetReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 mergeNetlist.getGate(mergeLoadGate).inputNetIds[0] == mergeToNet &&
                 mergeNetlist.validateAfterMutation(),
                 "mergeNetIntoNetWithReport");

    Netlist redirectLoadsNetlist = makeNetMergeNetlist();
    const int redirectOldNet = redirectLoadsNetlist.getNetId("from_net");
    const int redirectNewNet = redirectLoadsNetlist.getNetId("b");
    const int redirectLoadGate = redirectLoadsNetlist.getGateId("load_gate");
    const Netlist::NetlistEditReport redirectLoadsReport =
        redirectLoadsNetlist.redirectAllLoadsWithReport(redirectOldNet, redirectNewNet);
    report.check(redirectLoadsReport.success &&
                 redirectLoadsReport.changed &&
                 !redirectLoadsReport.rolledBack &&
                 redirectLoadsReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 redirectLoadsNetlist.getGate(redirectLoadGate).inputNetIds[0] == redirectNewNet &&
                 redirectLoadsNetlist.validateAfterMutation(),
                 "redirectAllLoadsWithReport");

    Netlist bypassNetlist;
    bypassNetlist.addPrimaryInput("a");
    bypassNetlist.addPrimaryInput("b");
    bypassNetlist.addPrimaryOutput("y");
    const int bypassA = bypassNetlist.getNetId("a");
    const int bypassB = bypassNetlist.getNetId("b");
    const int bypassY = bypassNetlist.getNetId("y");
    const int replacementNet = bypassNetlist.addNet("replacement_net");
    const int replacementDriver = bypassNetlist.addGate("replacement_driver", GateType::BUF);
    bypassNetlist.connectGateInput(replacementDriver, bypassA);
    bypassNetlist.connectGateOutput(replacementDriver, replacementNet);
    const int originalPoDriver = bypassNetlist.addGate("original_po_driver", GateType::BUF);
    bypassNetlist.connectGateInput(originalPoDriver, bypassB);
    bypassNetlist.connectGateOutput(originalPoDriver, bypassY);
    const Netlist::NetlistEditReport bypassReport =
        bypassNetlist.bypassNetKeepingPortSemanticsWithReport(bypassY, replacementNet);
    report.check(bypassReport.success &&
                 bypassReport.changed &&
                 !bypassReport.rolledBack &&
                 bypassReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 !bypassNetlist.isPrimaryOutputNet(bypassY) &&
                 bypassNetlist.isPrimaryOutputNet(replacementNet) &&
                 bypassNetlist.validateAfterMutation(),
                 "bypassNetKeepingPortSemanticsWithReport");

    Netlist removeUnusedNetlist = makeNetMergeNetlist();
    const int unusedNet = removeUnusedNetlist.addNet("unused_internal");
    const Netlist::NetlistEditReport removeUnusedReport =
        removeUnusedNetlist.removeNetIfUnusedWithReport(unusedNet);
    report.check(removeUnusedReport.success &&
                 removeUnusedReport.changed &&
                 !removeUnusedReport.rolledBack &&
                 removeUnusedReport.operationKind == Netlist::NetlistEditOperationKind::PrimitiveMutation &&
                 removeUnusedReport.validation.equivalenceChecked &&
                 removeUnusedReport.validation.functionallyEquivalent &&
                 removeUnusedReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 removeUnusedNetlist.validateAfterMutation(),
                 "removeNetIfUnusedWithReport");

    Netlist removeUnusedAllNetlist = makeNetMergeNetlist();
    const int unusedA = removeUnusedAllNetlist.addNet("unused_a");
    const int unusedB = removeUnusedAllNetlist.addNet("unused_b");
    const Netlist::NetlistEditReport removeUnusedAllReport =
        removeUnusedAllNetlist.removeUnusedNetsWithReport();
    report.check(removeUnusedAllReport.success &&
                 removeUnusedAllReport.changed &&
                 !removeUnusedAllReport.rolledBack &&
                 removeUnusedAllReport.operationKind == Netlist::NetlistEditOperationKind::Cleanup &&
                 containsInt(removeUnusedAllReport.changedNetIds, unusedA) &&
                 containsInt(removeUnusedAllReport.changedNetIds, unusedB) &&
                 removeUnusedAllNetlist.validateAfterMutation(),
                 "removeUnusedNetsWithReport");

    Netlist failedRenameNetlist = original.cloneForRollback();
    const Netlist::NetlistEditReport failedRenameReport =
        failedRenameNetlist.renameGateWithReport("missing_gate", "still_missing");
    report.check(!failedRenameReport.success &&
                 !failedRenameReport.changed &&
                 !failedRenameReport.rolledBack &&
                 failedRenameNetlist.getGateCount() == original.getGateCount(),
                 "failed primitive report preserves netlist");
}

// 測試 fanout buffer transformation 使用 QA fanout load 定義：
// primary output 要算 load，同一 DFF 的 RN/SN 也要算成兩個 sink pins。
void testFanoutBufferTransformation(TestReport& report) {
    Netlist poNetlist;
    poNetlist.addPrimaryInput("a");
    poNetlist.addPrimaryInput("b");
    poNetlist.addPrimaryOutput("y");

    const int a = poNetlist.getNetId("a");
    const int b = poNetlist.getNetId("b");
    const int y = poNetlist.getNetId("y");
    const int n1 = poNetlist.addNet("n1");
    const int n2 = poNetlist.addNet("n2");

    const int g0 = poNetlist.addGate("g0", GateType::AND);
    poNetlist.connectGateInput(g0, a);
    poNetlist.connectGateInput(g0, b);
    poNetlist.connectGateOutput(g0, y);

    const int g1 = poNetlist.addGate("g1", GateType::NOT);
    poNetlist.connectGateInput(g1, y);
    poNetlist.connectGateOutput(g1, n1);

    const int g2 = poNetlist.addGate("g2", GateType::BUF);
    poNetlist.connectGateInput(g2, y);
    poNetlist.connectGateOutput(g2, n2);
    const Netlist basePoNetlist = poNetlist.cloneForRollback();

    report.check(poNetlist.getFanoutLoadReport("y").totalLoadCount == 3,
                 "fanout buffer setup counts primary output load");
    Netlist poReportNetlist = poNetlist.cloneForRollback();
    const Netlist::NetlistEditReport poFanoutReport =
        poReportNetlist.insertBuffersForFanoutWithReport(2);
    report.check(poFanoutReport.success &&
                 poFanoutReport.changed &&
                 !poFanoutReport.rolledBack &&
                 poFanoutReport.operationKind == Netlist::NetlistEditOperationKind::BufferInsertion &&
                 poFanoutReport.validation.equivalenceChecked &&
                 poFanoutReport.validation.functionallyEquivalent &&
                 poFanoutReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 poFanoutReport.fanoutChange.has_value() &&
                 poFanoutReport.fanoutChange->targetFanout == 2 &&
                 poFanoutReport.fanoutChange->meetsConstraint &&
                 !poFanoutReport.changedGateNames.empty() &&
                 poReportNetlist.satisfiesFanoutLimit(2),
                 "insertBuffersForFanoutWithReport");
    poNetlist.insertBuffersForSpecificNet("y", 2);
    report.check(poNetlist.satisfiesFanoutLimit(2),
                 "fanout buffer insertion respects primary output load");

    Netlist dffNetlist;
    dffNetlist.addPrimaryInput("d");
    dffNetlist.addPrimaryInput("clk");
    dffNetlist.addPrimaryInput("rst_n");
    dffNetlist.addPrimaryOutput("q1");

    const int d = dffNetlist.getNetId("d");
    const int clk = dffNetlist.getNetId("clk");
    const int rst = dffNetlist.getNetId("rst_n");
    const int q1 = dffNetlist.getNetId("q1");
    const int q2 = dffNetlist.addNet("q2");

    const int ff1 = dffNetlist.addGate("ff1", GateType::DFF);
    dffNetlist.connectGateInput(ff1, d, "D");
    dffNetlist.connectGateInput(ff1, clk, "CK");
    dffNetlist.connectGateInput(ff1, rst, "RN");
    dffNetlist.connectGateInput(ff1, rst, "SN");
    dffNetlist.connectGateOutput(ff1, q1);

    const int ff2 = dffNetlist.addGate("ff2", GateType::DFF);
    dffNetlist.connectGateInput(ff2, d, "D");
    dffNetlist.connectGateInput(ff2, clk, "CK");
    dffNetlist.connectGateInput(ff2, rst, "RN");
    dffNetlist.connectGateInput(ff2, rst, "SN");
    dffNetlist.connectGateOutput(ff2, q2);

    report.check(dffNetlist.getFanoutLoadReport("rst_n").totalLoadCount == 4,
                 "fanout buffer setup counts DFF RN/SN sink pins");
    Netlist dffReportNetlist = dffNetlist.cloneForRollback();
    const Netlist::NetlistEditReport dffFanoutReport =
        dffReportNetlist.insertBuffersForSpecificNetWithReport("rst_n", 2);
    report.check(dffFanoutReport.success &&
                 dffFanoutReport.changed &&
                 !dffFanoutReport.rolledBack &&
                 dffFanoutReport.operationKind == Netlist::NetlistEditOperationKind::BufferInsertion &&
                 dffFanoutReport.fanoutChange.has_value() &&
                 dffFanoutReport.fanoutChange->targetFanout == 2 &&
                 dffFanoutReport.fanoutChange->meetsConstraint &&
                 !dffFanoutReport.changedGateNames.empty() &&
                 dffReportNetlist.satisfiesFanoutLimit(2),
                 "insertBuffersForSpecificNetWithReport");
    Netlist dffControlReportNetlist = dffNetlist.cloneForRollback();
    const Netlist::NetlistEditReport dffControlReport =
        dffControlReportNetlist.insertBuffersForDffControlWithReport(2, true, true);
    report.check(dffControlReport.success &&
                 dffControlReport.changed &&
                 !dffControlReport.rolledBack &&
                 dffControlReport.operationKind == Netlist::NetlistEditOperationKind::BufferInsertion &&
                 !dffControlReport.changedGateNames.empty() &&
                 dffControlReportNetlist.validateAfterMutation(),
                 "insertBuffersForDffControlWithReport");
    dffNetlist.insertBuffersForSpecificNet("rst_n", 2);
    report.check(dffNetlist.satisfiesFanoutLimit(2),
                 "fanout buffer insertion respects DFF RN/SN sink pins");

    Netlist eachLoadNetlist = basePoNetlist.cloneForRollback();
    const Netlist::NetlistEditReport eachLoadReport =
        eachLoadNetlist.insertBuffersOnEachLoadWithReport("y");
    report.check(eachLoadReport.success &&
                 eachLoadReport.changed &&
                 !eachLoadReport.rolledBack &&
                 eachLoadReport.operationKind == Netlist::NetlistEditOperationKind::BufferInsertion &&
                 !eachLoadReport.changedGateNames.empty() &&
                 eachLoadNetlist.validateAfterMutation(),
                 "insertBuffersOnEachLoadWithReport");

    Netlist driverBufferNetlist = basePoNetlist.cloneForRollback();
    const Netlist::NetlistEditReport driverBufferReport =
        driverBufferNetlist.insertBufferAtDriverWithReport("y");
    report.check(driverBufferReport.success &&
                 driverBufferReport.changed &&
                 !driverBufferReport.rolledBack &&
                 driverBufferReport.operationKind == Netlist::NetlistEditOperationKind::BufferInsertion &&
                 !driverBufferReport.changedGateNames.empty() &&
                 driverBufferNetlist.validateAfterMutation(),
                 "insertBufferAtDriverWithReport");

    Netlist beforeGateNetlist = basePoNetlist.cloneForRollback();
    const Netlist::NetlistEditReport beforeGateReport =
        beforeGateNetlist.insertBufferBeforeGateWithReport("y", "g1");
    report.check(beforeGateReport.success &&
                 beforeGateReport.changed &&
                 !beforeGateReport.rolledBack &&
                 beforeGateReport.operationKind == Netlist::NetlistEditOperationKind::BufferInsertion &&
                 !beforeGateReport.changedGateNames.empty() &&
                 beforeGateNetlist.validateAfterMutation(),
                 "insertBufferBeforeGateWithReport");

    Netlist gateTypeBufferNetlist = basePoNetlist.cloneForRollback();
    const Netlist::NetlistEditReport gateTypeBufferReport =
        gateTypeBufferNetlist.insertBuffersByGateTypeWithReport(GateType::AND, true, false);
    report.check(gateTypeBufferReport.success &&
                 gateTypeBufferReport.changed &&
                 !gateTypeBufferReport.rolledBack &&
                 gateTypeBufferReport.operationKind == Netlist::NetlistEditOperationKind::BufferInsertion &&
                 !gateTypeBufferReport.changedGateNames.empty() &&
                 gateTypeBufferNetlist.validateAfterMutation(),
                 "insertBuffersByGateTypeWithReport");
}

void testNetlistEditReportHelpers(TestReport& report, const Netlist& netlist) {
    const Netlist::NetlistStats beforeStats = netlist.collectNetlistStats();
    report.check(beforeStats.gateCount == 16 &&
                 beforeStats.activeGateCount == 16 &&
                 beforeStats.removedGateCount == 0 &&
                 beforeStats.netCount == 25 &&
                 beforeStats.activeNetCount == 25 &&
                 beforeStats.primaryInputCount == 6 &&
                 beforeStats.primaryOutputCount == 5 &&
                 beforeStats.dffCount == 1 &&
                 beforeStats.combinationalGateCount == 15,
                 "collectNetlistStats baseline");

    Netlist edited = netlist.cloneForRollback();
    edited.addGate("report_extra_buf", GateType::BUF);
    const Netlist::NetlistStats afterStats = edited.collectNetlistStats();
    const Netlist::NetlistDiff diff = Netlist::diffStats(beforeStats, afterStats);
    const auto bufDelta = diff.gateTypeCountDelta.find(GateType::BUF);

    report.check(diff.gateCountDelta == 1 &&
                 diff.activeGateCountDelta == 1 &&
                 diff.combinationalGateCountDelta == 1 &&
                 bufDelta != diff.gateTypeCountDelta.end() &&
                 bufDelta->second == 1,
                 "diffStats gate delta");

    const Netlist::NetlistEditReport editReport = Netlist::buildEditReport(
        netlist,
        edited,
        "report helper smoke test",
        Netlist::NetlistEditOperationKind::CustomRewrite);

    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationKind == Netlist::NetlistEditOperationKind::CustomRewrite &&
                 editReport.validation.structureChecked &&
                 editReport.validation.structureValid &&
                 editReport.validation.problemAConstraintsChecked &&
                 editReport.validation.problemAConstraintsValid &&
                 !editReport.validation.equivalenceChecked,
                 "buildEditReport validation summary");

    Netlist cleanupNetlist = netlist.cloneForRollback();
    const Netlist::NetlistEditReport cleanupReport =
        cleanupNetlist.runLocalSimplificationFixpointWithReport();
    report.check(cleanupReport.success &&
                 !cleanupReport.rolledBack &&
                 cleanupReport.operationKind == Netlist::NetlistEditOperationKind::Simplification &&
                 cleanupReport.depthChange.has_value() &&
                 cleanupReport.validation.structureValid &&
                 cleanupReport.validation.problemAConstraintsValid &&
                 cleanupReport.validation.equivalenceChecked &&
                 cleanupReport.validation.functionallyEquivalent &&
                 cleanupReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 cleanupNetlist.validateAfterMutation(),
                 "runLocalSimplificationFixpointWithReport");

    Netlist bufferCleanupNetlist = netlist.cloneForRollback();
    const Netlist::NetlistEditReport bufferCleanupReport =
        bufferCleanupNetlist.cleanupAllRemovableBuffersWithReport();
    report.check(bufferCleanupReport.success &&
                 !bufferCleanupReport.rolledBack &&
                 bufferCleanupReport.operationKind == Netlist::NetlistEditOperationKind::Cleanup &&
                 bufferCleanupReport.depthChange.has_value() &&
                 bufferCleanupReport.validation.structureValid &&
                 bufferCleanupReport.validation.problemAConstraintsValid &&
                 bufferCleanupReport.validation.equivalenceChecked &&
                 bufferCleanupReport.validation.functionallyEquivalent &&
                 bufferCleanupReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 bufferCleanupNetlist.validateAfterMutation(),
                 "cleanupAllRemovableBuffersWithReport");

    auto checkEditWrapper = [&](const Netlist::NetlistEditReport& wrapperReport,
                                const Netlist& editedNetlist,
                                const std::string& name) {
        report.check(wrapperReport.success &&
                     !wrapperReport.rolledBack &&
                     wrapperReport.validation.structureValid &&
                     wrapperReport.validation.problemAConstraintsValid &&
                     editedNetlist.validateAfterMutation(),
                     name);
    };

    Netlist trimNetlist = netlist.cloneForRollback();
    checkEditWrapper(trimNetlist.trimDeadLogicWithReport(), trimNetlist,
                     "trimDeadLogicWithReport");

    Netlist inverterNetlist = netlist.cloneForRollback();
    checkEditWrapper(inverterNetlist.collapseBackToBackInvertersWithReport(), inverterNetlist,
                     "collapseBackToBackInvertersWithReport");

    Netlist mergeEquivalentNetlist = netlist.cloneForRollback();
    checkEditWrapper(mergeEquivalentNetlist.mergeEquivalentGatesWithReport(), mergeEquivalentNetlist,
                     "mergeEquivalentGatesWithReport");

    Netlist danglingNetlist = netlist.cloneForRollback();
    checkEditWrapper(danglingNetlist.removeDanglingLogicWithReport(), danglingNetlist,
                     "removeDanglingLogicWithReport");

    Netlist structuralNetlist = netlist.cloneForRollback();
    checkEditWrapper(structuralNetlist.mergeStructurallyEquivalentGatesWithReport(), structuralNetlist,
                     "mergeStructurallyEquivalentGatesWithReport");

    Netlist constantNetlist = netlist.cloneForRollback();
    checkEditWrapper(constantNetlist.simplifyAllGatesWithConstantsWithReport(), constantNetlist,
                     "simplifyAllGatesWithConstantsWithReport");

    Netlist sameInputNetlist = netlist.cloneForRollback();
    checkEditWrapper(sameInputNetlist.simplifyAllSameInputGatesWithReport(), sameInputNetlist,
                     "simplifyAllSameInputGatesWithReport");

    TechMapper mapper;
    Netlist mappingNetlist = netlist.cloneForRollback();
    const Netlist::NetlistStats mappingBefore = mappingNetlist.collectNetlistStats();
    const Netlist::NetlistEditReport mappingReport =
        mapper.customMapTechnologyWithReport(
            mappingNetlist,
            {{GateType::AND, -1}},
            {{GateType::NAND, -1}},
            TargetScope::WHOLE_NETLIST,
            "",
            false);
    report.check(!mappingReport.success &&
                 mappingReport.rolledBack &&
                 mappingReport.operationKind == Netlist::NetlistEditOperationKind::TechnologyMapping &&
                 mappingReport.mappingDelta.has_value() &&
                 mappingReport.validation.structureValid &&
                 mappingReport.validation.problemAConstraintsValid &&
                 mappingNetlist.collectNetlistStats().gateCount == mappingBefore.gateCount,
                 "customMapTechnologyWithReport invalid constraints rollback");
}

void testOptimizationFlow(TestReport& report) {
    Netlist bufferNetlist;
    bufferNetlist.addPrimaryInput("a");
    bufferNetlist.addPrimaryOutput("y");
    const int bufferA = bufferNetlist.getNetId("a");
    const int bufferY = bufferNetlist.getNetId("y");
    const int mid = bufferNetlist.addNet("mid");
    const int inner = bufferNetlist.addNet("inner");
    const int b0 = bufferNetlist.addGate("buf0", GateType::BUF);
    bufferNetlist.connectGateInput(b0, bufferA);
    bufferNetlist.connectGateOutput(b0, mid);
    const int b1 = bufferNetlist.addGate("buf1", GateType::BUF);
    bufferNetlist.connectGateInput(b1, mid);
    bufferNetlist.connectGateOutput(b1, inner);
    const int outBuf = bufferNetlist.addGate("out_buf", GateType::BUF);
    bufferNetlist.connectGateInput(outBuf, inner);
    bufferNetlist.connectGateOutput(outBuf, bufferY);

    Netlist::OptQueryRequest bufferQuery;
    bufferQuery.passKind = Netlist::OptPassKind::CleanupBufferChain;
    const Netlist::OptQueryReport bufferQueryReport = bufferNetlist.runOptQuery(bufferQuery);
    report.check(bufferQueryReport.ok &&
                 !bufferQueryReport.candidates.empty() &&
                 bufferQueryReport.candidates[0].passKind == Netlist::OptPassKind::CleanupBufferChain &&
                 !bufferQueryReport.candidates[0].gateNames.empty(),
                 "runOptQuery cleanup_buffer_chain");

    Netlist::OptApplyRequest bufferApply;
    bufferApply.passKind = Netlist::OptPassKind::CleanupBufferChain;
    const Netlist::NetlistEditReport bufferApplyReport = bufferNetlist.runOptApply(bufferApply);
    report.check(bufferApplyReport.success &&
                 bufferApplyReport.changed &&
                 !bufferApplyReport.rolledBack &&
                 bufferApplyReport.operationName == "opt_apply:cleanup_buffer_chain" &&
                 bufferApplyReport.depthChange.has_value() &&
                 bufferApplyReport.depthChange->beforeDepth > bufferApplyReport.depthChange->afterDepth &&
                 bufferApplyReport.depthChange->improved &&
                 bufferApplyReport.validation.equivalenceChecked &&
                 bufferApplyReport.validation.functionallyEquivalent &&
                 bufferApplyReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 bufferNetlist.validateAfterMutation(),
                 "runOptApply cleanup_buffer_chain");

    Netlist inverterNetlist;
    inverterNetlist.addPrimaryInput("a");
    inverterNetlist.addPrimaryOutput("y");
    const int invA = inverterNetlist.getNetId("a");
    const int invY = inverterNetlist.getNetId("y");
    const int n1 = inverterNetlist.addNet("n1");
    const int n2 = inverterNetlist.addNet("n2");
    const int g1 = inverterNetlist.addGate("not0", GateType::NOT);
    inverterNetlist.connectGateInput(g1, invA);
    inverterNetlist.connectGateOutput(g1, n1);
    const int g2 = inverterNetlist.addGate("not1", GateType::NOT);
    inverterNetlist.connectGateInput(g2, n1);
    inverterNetlist.connectGateOutput(g2, n2);
    const int g3 = inverterNetlist.addGate("out_buf", GateType::BUF);
    inverterNetlist.connectGateInput(g3, n2);
    inverterNetlist.connectGateOutput(g3, invY);

    Netlist::OptQueryRequest inverterQuery;
    inverterQuery.passKind = Netlist::OptPassKind::CollapseDoubleInverter;
    const Netlist::OptQueryReport inverterQueryReport = inverterNetlist.runOptQuery(inverterQuery);
    report.check(inverterQueryReport.ok &&
                 inverterQueryReport.candidates.size() == 1 &&
                 inverterQueryReport.candidates[0].gateIds.size() == 2 &&
                 inverterQueryReport.candidates[0].passKind == Netlist::OptPassKind::CollapseDoubleInverter,
                 "runOptQuery collapse_double_inverter");

    Netlist::OptApplyRequest inverterApply;
    inverterApply.passKind = Netlist::OptPassKind::CollapseDoubleInverter;
    const Netlist::NetlistEditReport inverterApplyReport = inverterNetlist.runOptApply(inverterApply);
    report.check(inverterApplyReport.success &&
                 inverterApplyReport.changed &&
                 !inverterApplyReport.rolledBack &&
                 inverterApplyReport.operationName == "opt_apply:collapse_double_inverter" &&
                 inverterApplyReport.depthChange.has_value() &&
                 inverterApplyReport.depthChange->beforeDepth > inverterApplyReport.depthChange->afterDepth &&
                 inverterApplyReport.depthChange->improved &&
                 inverterApplyReport.validation.equivalenceChecked &&
                 inverterApplyReport.validation.functionallyEquivalent &&
                 inverterApplyReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 inverterNetlist.validateAfterMutation(),
                 "runOptApply collapse_double_inverter");

    Netlist simplifyNetlist = inverterNetlist.cloneForRollback();
    Netlist::OptQueryRequest simplifyQuery;
    simplifyQuery.passKind = Netlist::OptPassKind::LocalSimplificationFixpoint;
    const Netlist::OptQueryReport simplifyQueryReport = simplifyNetlist.runOptQuery(simplifyQuery);
    report.check(simplifyQueryReport.ok &&
                 simplifyQueryReport.candidates.size() == 1 &&
                 !simplifyQueryReport.warnings.empty(),
                 "runOptQuery local_simplification_fixpoint");

    Netlist::OptApplyRequest simplifyApply;
    simplifyApply.passKind = Netlist::OptPassKind::LocalSimplificationFixpoint;
    const Netlist::NetlistEditReport simplifyApplyReport = simplifyNetlist.runOptApply(simplifyApply);
    report.check(simplifyApplyReport.success &&
                 !simplifyApplyReport.rolledBack &&
                 simplifyApplyReport.operationName == "opt_apply:local_simplification_fixpoint" &&
                 simplifyApplyReport.depthChange.has_value() &&
                 simplifyApplyReport.validation.equivalenceChecked &&
                 simplifyApplyReport.validation.functionallyEquivalent &&
                 simplifyApplyReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 simplifyNetlist.validateAfterMutation(),
                 "runOptApply local_simplification_fixpoint");
}

void testEditApplyFlow(TestReport& report, const Netlist& original) {
    Netlist renameNetlist = original.cloneForRollback();
    Netlist::EditApplyRequest renameRequest;
    renameRequest.kind = Netlist::EditCommandKind::RenameGate;
    renameRequest.oldName = "g_direct";
    renameRequest.newName = "g_direct_edit_apply";
    const Netlist::NetlistEditReport renameReport = renameNetlist.runEditApply(renameRequest);
    report.check(renameReport.success &&
                 renameReport.changed &&
                 !renameReport.rolledBack &&
                 renameReport.operationName == "edit_apply:rename_gate" &&
                 renameReport.validation.equivalenceChecked &&
                 renameReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 renameNetlist.getGateId("g_direct_edit_apply") >= 0,
                 "runEditApply rename_gate");

    Netlist cleanupNetlist;
    cleanupNetlist.addPrimaryInput("a");
    cleanupNetlist.addPrimaryOutput("y");
    const int a = cleanupNetlist.getNetId("a");
    const int y = cleanupNetlist.getNetId("y");
    const int mid = cleanupNetlist.addNet("mid");
    const int buf0 = cleanupNetlist.addGate("buf0", GateType::BUF);
    cleanupNetlist.connectGateInput(buf0, a);
    cleanupNetlist.connectGateOutput(buf0, mid);
    const int buf1 = cleanupNetlist.addGate("buf1", GateType::BUF);
    cleanupNetlist.connectGateInput(buf1, mid);
    cleanupNetlist.connectGateOutput(buf1, y);

    Netlist::EditApplyRequest cleanupRequest;
    cleanupRequest.kind = Netlist::EditCommandKind::CleanupBuffers;
    cleanupRequest.validateEquivalence = true;
    const Netlist::NetlistEditReport cleanupReport = cleanupNetlist.runEditApply(cleanupRequest);
    report.check(cleanupReport.success &&
                 cleanupReport.changed &&
                 !cleanupReport.rolledBack &&
                 cleanupReport.operationName == "edit_apply:cleanup_buffers" &&
                 cleanupReport.validation.equivalenceChecked &&
                 cleanupReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 cleanupNetlist.validateAfterMutation(),
                 "runEditApply cleanup_buffers");

    Netlist fanoutNetlist;
    fanoutNetlist.addPrimaryInput("a");
    fanoutNetlist.addPrimaryOutput("y");
    const int fanoutA = fanoutNetlist.getNetId("a");
    const int fanoutY = fanoutNetlist.getNetId("y");
    const int outBuf = fanoutNetlist.addGate("fanout_out", GateType::BUF);
    fanoutNetlist.connectGateInput(outBuf, fanoutA);
    fanoutNetlist.connectGateOutput(outBuf, fanoutY);
    for (int i = 0; i < 4; ++i) {
        const int out = fanoutNetlist.addNet("sink_" + std::to_string(i));
        const int gate = fanoutNetlist.addGate("sink_gate_" + std::to_string(i), GateType::BUF);
        fanoutNetlist.connectGateInput(gate, fanoutA);
        fanoutNetlist.connectGateOutput(gate, out);
    }

    Netlist::EditApplyRequest fanoutRequest;
    fanoutRequest.kind = Netlist::EditCommandKind::InsertBuffersForSpecificNet;
    fanoutRequest.netName = "a";
    fanoutRequest.maxFanout = 2;
    const Netlist::NetlistEditReport fanoutReport = fanoutNetlist.runEditApply(fanoutRequest);
    report.check(fanoutReport.success &&
                 fanoutReport.changed &&
                 !fanoutReport.rolledBack &&
                 fanoutReport.operationName == "edit_apply:insert_buffers_for_specific_net" &&
                 fanoutReport.fanoutChange.has_value() &&
                 fanoutReport.fanoutChange->targetFanout == 2 &&
                 fanoutReport.validation.equivalenceChecked &&
                 fanoutReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 fanoutNetlist.satisfiesFanoutLimit(2),
                 "runEditApply insert_buffers_for_specific_net");

    Netlist primitiveNetlist = original.cloneForRollback();
    Netlist::EditApplyRequest primitiveRequest;
    primitiveRequest.kind = Netlist::EditCommandKind::ReplaceGateWithNet;
    primitiveRequest.gateName = "g_short";
    primitiveRequest.netName = "a";
    const Netlist::NetlistEditReport primitiveReport = primitiveNetlist.runEditApply(primitiveRequest);
    report.check(!primitiveReport.success &&
                 !primitiveReport.changed &&
                 !primitiveReport.rolledBack &&
                 primitiveReport.operationName == "edit_apply:replace_gate_with_net" &&
                 primitiveReport.message == "Command is an internal low-level primitive and is not exposed through EditApply because functional equivalence is not guaranteed." &&
                 primitiveNetlist.validateAfterMutation(),
                 "runEditApply blocks unchecked low-level primitive");

    Netlist uncheckedEquivNetlist = original.cloneForRollback();
    Netlist::EditApplyRequest uncheckedEquivRequest;
    uncheckedEquivRequest.kind = Netlist::EditCommandKind::RenameNet;
    uncheckedEquivRequest.oldName = "n_buf";
    uncheckedEquivRequest.newName = "n_buf_checked";
    uncheckedEquivRequest.validateEquivalence = true;
    const Netlist::NetlistEditReport uncheckedEquivReport =
        uncheckedEquivNetlist.runEditApply(uncheckedEquivRequest);
    report.check(uncheckedEquivReport.success &&
                 uncheckedEquivReport.validation.equivalenceChecked &&
                 uncheckedEquivReport.warnings.empty(),
                 "runEditApply validateEquivalence has certificate for public edit");

    Netlist missingRenameNetlist = original.cloneForRollback();
    Netlist::EditApplyRequest missingRenameRequest;
    missingRenameRequest.kind = Netlist::EditCommandKind::RenameNet;
    missingRenameRequest.oldName = "n_buf";
    const Netlist::NetlistEditReport missingRenameReport =
        missingRenameNetlist.runEditApply(missingRenameRequest);
    report.check(!missingRenameReport.success &&
                 !missingRenameReport.changed &&
                 !missingRenameReport.rolledBack &&
                 missingRenameReport.operationName == "edit_apply:rename_net" &&
                 missingRenameReport.message == "Missing required argument: newName." &&
                 missingRenameNetlist.getNetId("n_buf") >= 0,
                 "runEditApply missing required rename argument");

    Netlist missingFanoutNetlist = original.cloneForRollback();
    Netlist::EditApplyRequest missingFanoutRequest;
    missingFanoutRequest.kind = Netlist::EditCommandKind::InsertBuffersForSpecificNet;
    missingFanoutRequest.netName = "missing_net";
    missingFanoutRequest.maxFanout = 4;
    const Netlist::NetlistEditReport missingFanoutReport =
        missingFanoutNetlist.runEditApply(missingFanoutRequest);
    report.check(!missingFanoutReport.success &&
                 !missingFanoutReport.changed &&
                 !missingFanoutReport.rolledBack &&
                 missingFanoutReport.operationName == "edit_apply:insert_buffers_for_specific_net" &&
                 missingFanoutReport.message == "Net not found or already removed: missing_net.",
                 "runEditApply missing fanout net");

    Netlist invalidFanoutNetlist = original.cloneForRollback();
    Netlist::EditApplyRequest invalidFanoutRequest;
    invalidFanoutRequest.kind = Netlist::EditCommandKind::InsertBuffersForFanout;
    invalidFanoutRequest.maxFanout = 1;
    const Netlist::NetlistEditReport invalidFanoutReport =
        invalidFanoutNetlist.runEditApply(invalidFanoutRequest);
    report.check(!invalidFanoutReport.success &&
                 !invalidFanoutReport.changed &&
                 !invalidFanoutReport.rolledBack &&
                 invalidFanoutReport.operationName == "edit_apply:insert_buffers_for_fanout" &&
                 invalidFanoutReport.message == "Invalid maxFanout: must be at least 2.",
                 "runEditApply invalid fanout limit");

    Netlist unsupportedNetlist = original.cloneForRollback();
    Netlist::EditApplyRequest unsupportedRequest;
    unsupportedRequest.kind = Netlist::EditCommandKind::Unknown;
    const Netlist::NetlistEditReport unsupportedReport = unsupportedNetlist.runEditApply(unsupportedRequest);
    report.check(!unsupportedReport.success &&
                 !unsupportedReport.changed &&
                 !unsupportedReport.rolledBack &&
                 unsupportedReport.operationName == "edit_apply:unknown",
                 "runEditApply unsupported command");
}

} // namespace

// 執行 mini Verilog 整合測試；argv[1] 可指定 Verilog，argv[2] 可用 --basic-only。
int main(int argc, char* argv[]) {
    const std::string verilogPath =
        (argc >= 2) ? argv[1] : "mini test/mini_circuit.v";
    const bool basicOnly = argc >= 3 && std::string(argv[2]) == "--basic-only";

    TestReport report;
    Netlist netlist;
    VerilogReader reader;
    report.check(reader.read(verilogPath, netlist), "VerilogReader::read");
    if (report.failed != 0) {
        std::cout << "\nCannot continue because the test Verilog could not be read.\n";
        return report.failed;
    }

    testBasicQuery(report, netlist);
    if (basicOnly) {
        std::cout << "\nSummary: " << report.passed << " passed, "
                  << report.failed << " failed.\n";
        return report.failed == 0 ? 0 : 1;
    }

    testDirectConnectivityQuery(report, netlist);
    testFunctionQuery(report, netlist);
    testConeQuery(report, netlist);
    testPathQuery(report, netlist);
    testDepthQuery(report, netlist);
    testRegisterPathQuery(report);
    testWriterAndSmallMutation(report, netlist);
    testFanoutBufferTransformation(report);
    testNetlistEditReportHelpers(report, netlist);
    testOptimizationFlow(report);
    testEditApplyFlow(report, netlist);

    std::cout << "\nSummary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
