#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

// 簡單測試報告器：每個 API 測完都印 PASS / FAIL，最後用失敗數量當作程式結束碼。
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

// 建立 net 類型的 PathNode，讓 path API 測試比較容易閱讀。
Netlist::PathNode netNode(const std::string& name) {
    return Netlist::PathNode(Netlist::PathNodeType::Net, name);
}

// 建立 gate 類型的 PathNode，讓 path API 測試比較容易閱讀。
Netlist::PathNode gateNode(const std::string& name) {
    return Netlist::PathNode(Netlist::PathNodeType::Gate, name);
}

// 判斷字串陣列是否包含指定字串。
bool containsString(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

// 判斷整數陣列是否包含指定整數。
bool containsInt(const std::vector<int>& values, int target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

// 將 CombinationalPath 內的 net IDs 轉成 net names，方便檢查路徑內容。
std::vector<std::string> pathNetNames(const Netlist& netlist,
                                      const Netlist::CombinationalPath& path) {
    std::vector<std::string> names;
    for (int netId : path.netIds) {
        names.push_back(netlist.getNet(netId).name);
    }
    return names;
}

// 將 CombinationalPath 內的 gate IDs 轉成 gate instance names，方便檢查路徑內容。
std::vector<std::string> pathGateNames(const Netlist& netlist,
                                       const Netlist::CombinationalPath& path) {
    std::vector<std::string> names;
    for (int gateId : path.gateIds) {
        names.push_back(netlist.getGate(gateId).instName);
    }
    return names;
}

// 檢查一條路徑的 net name 序列是否含有指定 net。
bool pathContainsNet(const Netlist& netlist,
                     const Netlist::CombinationalPath& path,
                     const std::string& netName) {
    return containsString(pathNetNames(netlist, path), netName);
}

// 檢查一條路徑的 gate name 序列是否含有指定 gate。
bool pathContainsGate(const Netlist& netlist,
                      const Netlist::CombinationalPath& path,
                      const std::string& gateName) {
    return containsString(pathGateNames(netlist, path), gateName);
}

// 測試 parser 與最基本的 netlist 結構查詢。
void testBasicQueries(TestReport& report, const Netlist& netlist) {
    report.check(netlist.getGateCount() == 38, "getGateCount");
    report.check(netlist.getNetCount() == 49, "getNetCount");
    report.check(netlist.getLogicalWireCount() == 47, "getLogicalWireCount");
    report.check(netlist.getPrimaryInputs().size() == 7, "getPrimaryInputs");
    report.check(netlist.getPrimaryOutputs().size() == 5, "getPrimaryOutputs");

    report.check(netlist.getGateId("g_and") >= 0, "getGateId existing gate");
    report.check(netlist.getGateId("missing_gate") == -1, "getGateId missing gate");
    report.check(netlist.getNetId("n_and") >= 0, "getNetId existing net");
    report.check(netlist.getNetId("missing_net") == -1, "getNetId missing net");
    report.check(netlist.findGate("g_or") != nullptr, "findGate existing gate");
    report.check(netlist.findGate("missing_gate") == nullptr, "findGate missing gate");
    report.check(netlist.findNet("n_or") != nullptr, "findNet existing net");
    report.check(netlist.findNet("missing_net") == nullptr, "findNet missing net");

    report.check(netlist.gateTypeToString(GateType::NAND) == "NAND", "gateTypeToString");
    report.check(netlist.stringToGateType("xnor") == GateType::XNOR, "stringToGateType lower case");
    report.check(netlist.stringToGateType("bad_type") == GateType::UNKNOWN, "stringToGateType unknown");

    const std::vector<int> busBits = netlist.expandNetToBits("bus");
    report.check(busBits.size() == 2, "expandNetToBits bus");
    report.check(netlist.expandNetToBits("a").size() == 1, "expandNetToBits scalar");

    const std::string gateInfo = netlist.getGateInfo("g_nand");
    report.check(gateInfo.find("Type: NAND") != std::string::npos &&
                 gateInfo.find("1'b1") != std::string::npos,
                 "getGateInfo");
}

// 測試 gate type 統計、constant input 查詢、wire loads 與 gate fanout。
void testDirectAnalysis(TestReport& report, const Netlist& netlist) {
    const std::map<GateType, int> counts = netlist.countGatesByType();
    report.check(counts.at(GateType::BUF) == 29, "countGatesByType BUF");
    report.check(counts.at(GateType::XOR) == 2, "countGatesByType XOR");
    report.check(netlist.getGatesByType(GateType::BUF).size() == 29, "getGatesByType");
    report.check(netlist.getGateCountByType(GateType::DFF) == 1, "getGateCountByType");

    const std::vector<int> constOneGates =
        netlist.findGatesWithConstInput(GateType::UNKNOWN, 1);
    report.check(constOneGates.size() == 2, "findGatesWithConstInput const 1");
    report.check(netlist.countGatesWithConstInput(GateType::UNKNOWN, 0) == 1,
                 "countGatesWithConstInput const 0");
    const std::vector<std::string> constOneNames =
        netlist.getGateNamesWithConstInput(GateType::UNKNOWN, 1);
    report.check(containsString(constOneNames, "g_nand") &&
                 containsString(constOneNames, "ff1"),
                 "getGateNamesWithConstInput");

    const std::vector<int> aLoads = netlist.getWireLoads("a");
    report.check(aLoads.size() == 3, "getWireLoads scalar");
    const std::vector<std::string> aLoadNames = netlist.getWireLoadNames("a");
    report.check(containsString(aLoadNames, "g_and") &&
                 containsString(aLoadNames, "g_xnor") &&
                 containsString(aLoadNames, "g_direct"),
                 "getWireLoadNames scalar");
    report.check(netlist.getWireLoadCount("a") == 3, "getWireLoadCount scalar");
    report.check(netlist.getWireLoadCount("bus") == 1, "getWireLoadCount bus");

    const std::vector<int> orFanout = netlist.getGateFanout("g_or");
    report.check(orFanout.size() == 2, "getGateFanout");
    const std::vector<std::string> orFanoutNames = netlist.getGateFanoutNames("g_or");
    report.check(containsString(orFanoutNames, "g_not") &&
                 containsString(orFanoutNames, "g_z"),
                 "getGateFanoutNames");
    report.check(netlist.getGateFanoutCount("g_or") == 2, "getGateFanoutCount");
}

// 測試 fanin / fanout cone 的 set 結果與 gate-name wrapper。
void testConeQueries(TestReport& report, const Netlist& netlist) {
    const ConeResult faninY = netlist.getTransitiveFaninCone("y");
    report.check(!faninY.rootNetIds.empty() &&
                 faninY.rootNetIds.front() == netlist.getNetId("y"),
                 "getTransitiveFaninCone root");
    report.check(containsInt(std::vector<int>(faninY.netIds.begin(), faninY.netIds.end()),
                             netlist.getNetId("n_and")),
                 "getTransitiveFaninCone includes upstream net");

    const ConeResult fanoutNbuf = netlist.getTransitiveFanoutCone("n_buf");
    report.check(containsInt(std::vector<int>(fanoutNbuf.netIds.begin(), fanoutNbuf.netIds.end()),
                             netlist.getNetId("y")) &&
                 !containsInt(std::vector<int>(fanoutNbuf.netIds.begin(), fanoutNbuf.netIds.end()),
                              netlist.getNetId("q")),
                 "getTransitiveFanoutCone stops at DFF");

    report.check(netlist.getGateTransitiveFaninCone("g_y").rootNetIds.front() ==
                 netlist.getNetId("y"),
                 "getGateTransitiveFaninCone");
    report.check(netlist.getGateTransitiveFanoutCone("g_or").rootNetIds.front() ==
                 netlist.getNetId("n_or"),
                 "getGateTransitiveFanoutCone");

    const std::vector<std::string> faninGateNames =
        netlist.getTransitiveFaninConeGateNames("y");
    report.check(containsString(faninGateNames, "g_y") &&
                 containsString(faninGateNames, "g_and"),
                 "getTransitiveFaninConeGateNames");
    report.check(netlist.getTransitiveFaninConeGateCount("y") == faninGateNames.size(),
                 "getTransitiveFaninConeGateCount");

    const std::vector<std::string> fanoutGateNames =
        netlist.getTransitiveFanoutConeGateNames("n_or");
    report.check(containsString(fanoutGateNames, "g_not") &&
                 containsString(fanoutGateNames, "g_z"),
                 "getTransitiveFanoutConeGateNames");
    report.check(netlist.getTransitiveFanoutConeGateCount("n_or") == fanoutGateNames.size(),
                 "getTransitiveFanoutConeGateCount");

    report.check(containsString(netlist.getGateTransitiveFaninConeGateNames("g_y"), "g_buf"),
                 "getGateTransitiveFaninConeGateNames");
    report.check(netlist.getGateTransitiveFaninConeGateCount("g_y") ==
                 netlist.getGateTransitiveFaninConeGateNames("g_y").size(),
                 "getGateTransitiveFaninConeGateCount");
    report.check(containsString(netlist.getGateTransitiveFanoutConeGateNames("g_or"), "g_z"),
                 "getGateTransitiveFanoutConeGateNames");
    report.check(netlist.getGateTransitiveFanoutConeGateCount("g_or") ==
                 netlist.getGateTransitiveFanoutConeGateNames("g_or").size(),
                 "getGateTransitiveFanoutConeGateCount");
}

// 測試 cone 內長短路徑；wrapper 應該回傳 net 名稱序列，而不是把 netId 誤當 gateId。
void testConePathQueries(TestReport& report, const Netlist& netlist) {
    const ConeResult faninY = netlist.getTransitiveFaninCone("y");
    const std::pair<int, std::vector<int>> rawLongest =
        netlist.findLongestPathInCone(faninY);
    report.check(rawLongest.first == 9 &&
                 rawLongest.second.front() == netlist.getNetId("y"),
                 "findLongestPathInCone raw net path");

    const std::pair<int, std::vector<int>> rawShortest =
        netlist.findShortestPathInCone(faninY);
    report.check(rawShortest.first >= 3 &&
                 rawShortest.second.front() == netlist.getNetId("y"),
                 "findShortestPathInCone raw net path");

    const std::pair<int, std::vector<std::string>> faninLongest =
        netlist.getTransitiveFaninConeLongestPath("y");
    report.check(!faninLongest.second.empty() &&
                 faninLongest.second.front() == "y",
                 "getTransitiveFaninConeLongestPath returns net names");

    const std::pair<int, std::vector<std::string>> faninShortest =
        netlist.getTransitiveFaninConeShortestPath("y");
    report.check(!faninShortest.second.empty() &&
                 faninShortest.second.front() == "y",
                 "getTransitiveFaninConeShortestPath returns net names");

    const std::pair<int, std::vector<std::string>> fanoutLongest =
        netlist.getTransitiveFanoutConeLongestPath("n_or");
    report.check(!fanoutLongest.second.empty() &&
                 fanoutLongest.second.front() == "n_or",
                 "getTransitiveFanoutConeLongestPath returns net names");

    const std::pair<int, std::vector<std::string>> fanoutShortest =
        netlist.getTransitiveFanoutConeShortestPath("n_or");
    report.check(!fanoutShortest.second.empty() &&
                 fanoutShortest.second.front() == "n_or",
                 "getTransitiveFanoutConeShortestPath returns net names");

    const std::pair<int, std::vector<std::string>> gateFaninLongest =
        netlist.getGateTransitiveFaninConeLongestPath("g_y");
    report.check(!gateFaninLongest.second.empty() &&
                 gateFaninLongest.second.front() == "y",
                 "getGateTransitiveFaninConeLongestPath returns net names");

    const std::pair<int, std::vector<std::string>> gateFaninShortest =
        netlist.getGateTransitiveFaninConeShortestPath("g_y");
    report.check(!gateFaninShortest.second.empty() &&
                 gateFaninShortest.second.front() == "y",
                 "getGateTransitiveFaninConeShortestPath returns net names");

    const std::pair<int, std::vector<std::string>> gateFanoutLongest =
        netlist.getGateTransitiveFanoutConeLongestPath("g_or");
    report.check(!gateFanoutLongest.second.empty() &&
                 gateFanoutLongest.second.front() == "n_or",
                 "getGateTransitiveFanoutConeLongestPath returns net names");

    const std::pair<int, std::vector<std::string>> gateFanoutShortest =
        netlist.getGateTransitiveFanoutConeShortestPath("g_or");
    report.check(!gateFanoutShortest.second.empty() &&
                 gateFanoutShortest.second.front() == "n_or",
                 "getGateTransitiveFanoutConeShortestPath returns net names");

    ConeResult cyclicCone;
    cyclicCone.rootNetIds.push_back(netlist.getNetId("a"));
    cyclicCone.children[netlist.getNetId("a")].push_back(netlist.getNetId("n_and"));
    cyclicCone.children[netlist.getNetId("n_and")].push_back(netlist.getNetId("a"));
    const std::pair<int, std::vector<int>> cyclicLongest =
        netlist.findLongestPathInCone(cyclicCone);
    report.check(cyclicLongest.first == -1 && cyclicLongest.second.empty(),
                 "findLongestPathInCone rejects cyclic cone");

    ConeResult cyclicConeWithExit = cyclicCone;
    cyclicConeWithExit.children[netlist.getNetId("a")].push_back(netlist.getNetId("b"));
    const std::pair<int, std::vector<int>> cyclicWithExitLongest =
        netlist.findLongestPathInCone(cyclicConeWithExit);
    report.check(cyclicWithExitLongest.first == 1 &&
                 cyclicWithExitLongest.second.size() == 2 &&
                 cyclicWithExitLongest.second.front() == netlist.getNetId("a") &&
                 cyclicWithExitLongest.second.back() == netlist.getNetId("b"),
                 "findLongestPathInCone skips cyclic branch");
}

// 測試 A-D 類 combinational path API。
void testPathQueries(TestReport& report, const Netlist& netlist) {
    report.check(netlist.hasCombinationalPath("a", "y"), "hasCombinationalPath");
    report.check(!netlist.hasCombinationalPathAvoiding("n_and", "y", {netNode("n_or")}),
                 "hasCombinationalPathAvoiding");
    report.check(netlist.hasCombinationalPathThrough("n_and", "y", {gateNode("g_nand"), netNode("n_nor")}),
                 "hasCombinationalPathThrough multiple nodes");
    report.check(netlist.hasCombinationalPathThroughAvoiding(
                     "n_and", "y", {gateNode("g_nand")}, {netNode("z")}),
                 "hasCombinationalPathThroughAvoiding");

    const Netlist::CombinationalPath anyPath =
        netlist.findAnyCombinationalPath("a", "y");
    report.check(anyPath.exists() && anyPath.depth() == 3,
                 "findAnyCombinationalPath");
    report.check(!netlist.findAnyCombinationalPathAvoiding(
                      "n_and", "y", {netNode("n_or")}).exists(),
                 "findAnyCombinationalPathAvoiding");
    report.check(pathContainsGate(netlist,
                                  netlist.findAnyCombinationalPathThrough(
                                      "n_and", "y", {gateNode("g_nand")}),
                                  "g_nand"),
                 "findAnyCombinationalPathThrough");
    report.check(pathContainsGate(netlist,
                                  netlist.findAnyCombinationalPathThroughAvoiding(
                                      "n_and", "y", {gateNode("g_nand")}, {netNode("z")}),
                                  "g_nand"),
                 "findAnyCombinationalPathThroughAvoiding");

    report.check(netlist.enumerateCombinationalPaths("a", "y").size() == 2,
                 "enumerateCombinationalPaths");
    report.check(netlist.enumerateCombinationalPathsAvoiding(
                     "a", "y", {netNode("n_or")}).size() == 1,
                 "enumerateCombinationalPathsAvoiding");
    report.check(netlist.enumerateCombinationalPathsThrough(
                     "a", "y", {gateNode("g_and")}).size() == 1,
                 "enumerateCombinationalPathsThrough");
    report.check(netlist.enumerateCombinationalPathsThroughAvoiding(
                     "a", "y", {gateNode("g_xnor")}, {netNode("n_or")}).size() == 1,
                 "enumerateCombinationalPathsThroughAvoiding");

    report.check(netlist.everyPathPassesThrough("a", "y", {gateNode("g_y")}),
                 "everyPathPassesThrough");
    report.check(!netlist.everyPathPassesThrough("a", "y", {gateNode("g_and")}),
                 "everyPathPassesThrough negative");
    report.check(netlist.everyPathAvoids("a", "y", {netNode("z")}),
                 "everyPathAvoids");
    report.check(!netlist.everyPathAvoids("a", "y", {netNode("n_or")}),
                 "everyPathAvoids negative");
}

// 測試最長組合路徑與 endpoint wrapper。
void testLongestAndEndpointQueries(TestReport& report, const Netlist& netlist) {
    const Netlist::CombinationalPath longest =
        netlist.findLongestCombinationalPath("a", "y");
    report.check(longest.exists() && longest.depth() == 9 &&
                 pathContainsGate(netlist, longest, "g_and"),
                 "findLongestCombinationalPath");
    report.check(netlist.findLongestCombinationalPathAvoiding(
                     "a", "y", {netNode("n_or")}).depth() == 3,
                 "findLongestCombinationalPathAvoiding");
    report.check(pathContainsGate(netlist,
                                  netlist.findLongestCombinationalPathThrough(
                                      "a", "y", {gateNode("g_nand")}),
                                  "g_nand"),
                 "findLongestCombinationalPathThrough");
    report.check(netlist.findLongestCombinationalPathThroughAvoiding(
                     "a", "y", {gateNode("g_xnor")}, {netNode("n_or")}).depth() == 3,
                 "findLongestCombinationalPathThroughAvoiding");

    report.check(netlist.findShortestCombinationalPath("a", "y").depth() == 3,
                 "findShortestCombinationalPath");
    report.check(netlist.findShortestCombinationalPathAvoiding(
                     "a", "y", {netNode("n_or")}).depth() == 3,
                 "findShortestCombinationalPathAvoiding");
    report.check(pathContainsGate(netlist,
                                  netlist.findShortestCombinationalPathThrough(
                                      "a", "y", {gateNode("g_nand")}),
                                  "g_nand"),
                 "findShortestCombinationalPathThrough");
    report.check(netlist.findShortestCombinationalPathThroughAvoiding(
                     "a", "y", {gateNode("g_xnor")}, {netNode("n_or")}).depth() == 3,
                 "findShortestCombinationalPathThroughAvoiding");

    report.check(netlist.isEndpoint(netNode("y")), "isEndpoint net");
    report.check(!netlist.isEndpoint(netNode("n_buf")), "isEndpoint non-endpoint net");
    report.check(netlist.hasCombinationalPathToEndpoint("a", "y"),
                 "hasCombinationalPathToEndpoint");
    report.check(!netlist.hasCombinationalPathToEndpoint("a", "n_buf"),
                 "hasCombinationalPathToEndpoint rejects non-endpoint");
    report.check(netlist.hasCombinationalPathAvoidingToEndpoint(
                     "a", "y", {netNode("z")}),
                 "hasCombinationalPathAvoidingToEndpoint");
    report.check(netlist.hasCombinationalPathThroughToEndpoint(
                     "a", "y", {gateNode("g_y")}),
                 "hasCombinationalPathThroughToEndpoint");
    report.check(netlist.hasCombinationalPathThroughAvoidingToEndpoint(
                     "a", "y", {gateNode("g_xnor")}, {netNode("z")}),
                 "hasCombinationalPathThroughAvoidingToEndpoint");
    report.check(netlist.findAnyCombinationalPathToEndpoint("a", "y").exists(),
                 "findAnyCombinationalPathToEndpoint");
    report.check(netlist.findAnyCombinationalPathAvoidingToEndpoint(
                     "a", "y", {netNode("z")}).exists(),
                 "findAnyCombinationalPathAvoidingToEndpoint");
    report.check(netlist.findAnyCombinationalPathThroughToEndpoint(
                     "a", "y", {gateNode("g_y")}).exists(),
                 "findAnyCombinationalPathThroughToEndpoint");
    report.check(netlist.findAnyCombinationalPathThroughAvoidingToEndpoint(
                     "a", "y", {gateNode("g_xnor")}, {netNode("z")}).exists(),
                 "findAnyCombinationalPathThroughAvoidingToEndpoint");
    report.check(netlist.enumerateCombinationalPathsToEndpoint("a", "y").size() == 2,
                 "enumerateCombinationalPathsToEndpoint");
    report.check(netlist.enumerateCombinationalPathsAvoidingToEndpoint(
                     "a", "y", {netNode("n_or")}).size() == 1,
                 "enumerateCombinationalPathsAvoidingToEndpoint");
    report.check(netlist.enumerateCombinationalPathsThroughToEndpoint(
                     "a", "y", {gateNode("g_y")}).size() == 2,
                 "enumerateCombinationalPathsThroughToEndpoint");
    report.check(netlist.enumerateCombinationalPathsThroughAvoidingToEndpoint(
                     "a", "y", {gateNode("g_xnor")}, {netNode("n_or")}).size() == 1,
                 "enumerateCombinationalPathsThroughAvoidingToEndpoint");
    report.check(netlist.everyPathPassesThroughToEndpoint("a", "y", {gateNode("g_y")}),
                 "everyPathPassesThroughToEndpoint");
    report.check(netlist.everyPathAvoidsToEndpoint("a", "y", {netNode("z")}),
                 "everyPathAvoidsToEndpoint");
    report.check(netlist.findLongestCombinationalPathToEndpoint("a", "y").depth() == 9,
                 "findLongestCombinationalPathToEndpoint");
    report.check(netlist.findLongestCombinationalPathAvoidingToEndpoint(
                     "a", "y", {netNode("n_or")}).depth() == 3,
                 "findLongestCombinationalPathAvoidingToEndpoint");
    report.check(netlist.findLongestCombinationalPathThroughToEndpoint(
                     "a", "y", {gateNode("g_y")}).exists(),
                 "findLongestCombinationalPathThroughToEndpoint");
    report.check(netlist.findLongestCombinationalPathThroughAvoidingToEndpoint(
                     "a", "y", {gateNode("g_xnor")}, {netNode("n_or")}).depth() == 3,
                 "findLongestCombinationalPathThroughAvoidingToEndpoint");
}

// 測試 DFF named pin 查詢，以及所有 PI 到所有 DFF.D 的最大組合邏輯深度。
void testPiToDffDepthQueries(TestReport& report, const Netlist& netlist) {
    const int ff1Id = netlist.getGateId("ff1");
    const int dNetId = netlist.getDffInputNetId(ff1Id, "D");
    report.check(dNetId == netlist.getNetId("n_buf"), "getDffInputNetId D");
    report.check(netlist.getDffInputNetId(ff1Id, "CK") == netlist.getNetId("clk"),
                 "getDffInputNetId CK");
    report.check(netlist.getDffInputNetId(ff1Id, "missing_pin") == -1,
                 "getDffInputNetId missing pin");
    report.check(netlist.getDffInputNetId(netlist.getGateId("g_y"), "D") == -1,
                 "getDffInputNetId rejects non-DFF");

    const std::pair<int, Netlist::CombinationalPath> maxPiToDff =
        netlist.getMaximumLogicDepthFromPiToDffD();
    report.check(maxPiToDff.first == 8 && maxPiToDff.second.exists(),
                 "getMaximumLogicDepthFromPiToDffD depth");
    report.check(pathContainsNet(netlist, maxPiToDff.second, "n_buf") &&
                 pathContainsGate(netlist, maxPiToDff.second, "g_buf"),
                 "getMaximumLogicDepthFromPiToDffD witness path");
}

// 測試 startpoint/endpoint resolver helper，確認抽象端點可轉成正確 net ID。
void testEndpointResolverHelpers(TestReport& report, const Netlist& netlist) {
    const std::vector<int> piNetIds = netlist.getPrimaryInputNetIds();
    report.check(piNetIds.size() == 8 &&
                 containsInt(piNetIds, netlist.getNetId("a")) &&
                 containsInt(piNetIds, netlist.getNetId("bus[0]")) &&
                 containsInt(piNetIds, netlist.getNetId("bus[1]")),
                 "getPrimaryInputNetIds expands scalar and bus ports");

    const std::vector<int> poNetIds = netlist.getPrimaryOutputNetIds();
    report.check(poNetIds.size() == 5 &&
                 containsInt(poNetIds, netlist.getNetId("y")) &&
                 containsInt(poNetIds, netlist.getNetId("bus_out")),
                 "getPrimaryOutputNetIds");

    const int ff1Id = netlist.getGateId("ff1");
    report.check(netlist.getDffOutputNetId(ff1Id) == netlist.getNetId("q"),
                 "getDffOutputNetId");
    report.check(netlist.getDffOutputNetId(netlist.getGateId("g_y")) == -1,
                 "getDffOutputNetId rejects non-DFF");

    report.check(netlist.getGateOutputNetId(netlist.getGateId("g_y")) ==
                 netlist.getNetId("y"),
                 "getGateOutputNetId primitive gate");
    report.check(netlist.getGateOutputNetId(ff1Id) == netlist.getNetId("q"),
                 "getGateOutputNetId DFF");
    report.check(netlist.getGateOutputNetId(-1) == -1,
                 "getGateOutputNetId invalid gate");

    const int gAndId = netlist.getGateId("g_and");
    report.check(netlist.getGateInputNetId(gAndId, 0) == netlist.getNetId("a") &&
                 netlist.getGateInputNetId(gAndId, 1) == netlist.getNetId("b"),
                 "getGateInputNetId by positional index");
    report.check(netlist.getGateInputNetId(gAndId, 2) == -1,
                 "getGateInputNetId rejects invalid index");

    report.check(netlist.getGateInputNetId(ff1Id, "D") == netlist.getNetId("n_buf") &&
                 netlist.getGateInputNetId(ff1Id, "CK") == netlist.getNetId("clk"),
                 "getGateInputNetId by pin name");
    report.check(netlist.getGateInputNetId(gAndId, "D") == -1,
                 "getGateInputNetId rejects missing pin name");

    const std::vector<int> specificNetEndpoint =
        netlist.resolvePathEndpoint(Netlist::PathEndpoint(
            Netlist::PathEndpointType::SpecificNet, "n_buf"));
    report.check(specificNetEndpoint.size() == 1 &&
                 specificNetEndpoint.front() == netlist.getNetId("n_buf"),
                 "resolvePathEndpoint SpecificNet");

    const std::vector<int> allPiEndpoint =
        netlist.resolvePathEndpoint(Netlist::PathEndpoint(
            Netlist::PathEndpointType::PrimaryInput, ""));
    report.check(allPiEndpoint.size() == 8 &&
                 containsInt(allPiEndpoint, netlist.getNetId("bus[0]")),
                 "resolvePathEndpoint all PrimaryInput");

    const std::vector<int> busPiEndpoint =
        netlist.resolvePathEndpoint(Netlist::PathEndpoint(
            Netlist::PathEndpointType::PrimaryInput, "bus"));
    report.check(busPiEndpoint.size() == 2 &&
                 containsInt(busPiEndpoint, netlist.getNetId("bus[0]")) &&
                 containsInt(busPiEndpoint, netlist.getNetId("bus[1]")),
                 "resolvePathEndpoint named PrimaryInput bus");

    const std::vector<int> yPoEndpoint =
        netlist.resolvePathEndpoint(Netlist::PathEndpoint(
            Netlist::PathEndpointType::PrimaryOutput, "y"));
    report.check(yPoEndpoint.size() == 1 &&
                 yPoEndpoint.front() == netlist.getNetId("y"),
                 "resolvePathEndpoint named PrimaryOutput");

    report.check(netlist.resolvePathEndpoint(Netlist::PathEndpoint(
                     Netlist::PathEndpointType::DffQ, "ff1")).front() ==
                 netlist.getNetId("q"),
                 "resolvePathEndpoint DffQ");
    report.check(netlist.resolvePathEndpoint(Netlist::PathEndpoint(
                     Netlist::PathEndpointType::DffD, "ff1")).front() ==
                 netlist.getNetId("n_buf"),
                 "resolvePathEndpoint DffD");
    report.check(netlist.resolvePathEndpoint(Netlist::PathEndpoint(
                     Netlist::PathEndpointType::DffClock, "ff1")).front() ==
                 netlist.getNetId("clk"),
                 "resolvePathEndpoint DffClock");

    const std::vector<int> resetEndpoint =
        netlist.resolvePathEndpoint(Netlist::PathEndpoint(
            Netlist::PathEndpointType::DffReset, "ff1"));
    report.check(resetEndpoint.size() == 2 &&
                 containsInt(resetEndpoint, netlist.getNetId("rst_n")),
                 "resolvePathEndpoint DffReset default RN/SN");

    report.check(netlist.resolvePathEndpoint(Netlist::PathEndpoint(
                     Netlist::PathEndpointType::GateOutput, "g_y")).front() ==
                 netlist.getNetId("y"),
                 "resolvePathEndpoint GateOutput");
    report.check(netlist.resolvePathEndpoint(Netlist::PathEndpoint(
                     Netlist::PathEndpointType::GateInput, "g_and", "", 1)).front() ==
                 netlist.getNetId("b"),
                 "resolvePathEndpoint GateInput positional");
    report.check(netlist.resolvePathEndpoint(Netlist::PathEndpoint(
                     Netlist::PathEndpointType::GateInput, "ff1", "D")).front() ==
                 netlist.getNetId("n_buf"),
                 "resolvePathEndpoint GateInput named");

    const std::vector<int> multiEndpoint =
        netlist.resolvePathEndpoints({
            Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "a"),
            Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "a"),
            Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryOutput, "y")
        });
    report.check(multiEndpoint.size() == 2 &&
                 multiEndpoint[0] == netlist.getNetId("a") &&
                 multiEndpoint[1] == netlist.getNetId("y"),
                 "resolvePathEndpoints removes duplicates");
}

// 測試第一版統一 PathQuery API：目前支援 Exists、FindAny、MaxDepth。
void testRunPathQuery(TestReport& report, const Netlist& netlist) {
    Netlist::PathQuery existsQuery;
    existsQuery.mode = Netlist::PathQueryMode::Exists;
    existsQuery.startpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::PrimaryInput, "a"));
    existsQuery.endpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::PrimaryOutput, "y"));
    const Netlist::PathQueryResult existsResult = netlist.runPathQuery(existsQuery);
    report.check(existsResult.exists, "runPathQuery Exists PI-to-PO");

    Netlist::PathQuery findAnyQuery;
    findAnyQuery.mode = Netlist::PathQueryMode::FindAny;
    findAnyQuery.startpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "a"));
    findAnyQuery.endpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "y"));
    const Netlist::PathQueryResult findAnyResult = netlist.runPathQuery(findAnyQuery);
    report.check(findAnyResult.exists && findAnyResult.path.exists() &&
                 findAnyResult.depth == findAnyResult.path.depth(),
                 "runPathQuery FindAny net-to-net");

    Netlist::PathQuery maxDepthQuery;
    maxDepthQuery.mode = Netlist::PathQueryMode::MaxDepth;
    maxDepthQuery.startpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::PrimaryInput, ""));
    maxDepthQuery.endpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::DffD, "ff1"));
    const Netlist::PathQueryResult maxDepthResult = netlist.runPathQuery(maxDepthQuery);
    report.check(maxDepthResult.exists && maxDepthResult.depth == 8 &&
                 pathContainsNet(netlist, maxDepthResult.path, "n_buf"),
                 "runPathQuery MaxDepth all PI to DFF.D");

    Netlist::PathQuery minDepthQuery;
    minDepthQuery.mode = Netlist::PathQueryMode::MinDepth;
    minDepthQuery.startpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "a"));
    minDepthQuery.endpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "y"));
    const Netlist::PathQueryResult minDepthResult =
        netlist.runPathQuery(minDepthQuery);
    report.check(minDepthResult.exists && minDepthResult.depth == 3,
                 "runPathQuery MinDepth");

    Netlist::PathQuery constrainedQuery;
    constrainedQuery.mode = Netlist::PathQueryMode::FindAny;
    constrainedQuery.startpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "a"));
    constrainedQuery.endpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "y"));
    constrainedQuery.requiredNodes.push_back(gateNode("g_xnor"));
    constrainedQuery.avoidedNodes.push_back(netNode("n_or"));
    const Netlist::PathQueryResult constrainedResult =
        netlist.runPathQuery(constrainedQuery);
    report.check(constrainedResult.exists &&
                 pathContainsGate(netlist, constrainedResult.path, "g_xnor") &&
                 !pathContainsNet(netlist, constrainedResult.path, "n_or"),
                 "runPathQuery required and avoided nodes");

    Netlist::PathQuery unsupportedQuery = existsQuery;
    unsupportedQuery.combinationalOnly = false;
    report.check(!netlist.runPathQuery(unsupportedQuery).exists,
                 "runPathQuery rejects non-combinational query");

    Netlist::PathQuery enumerateQuery;
    enumerateQuery.mode = Netlist::PathQueryMode::EnumerateAll;
    enumerateQuery.startpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "a"));
    enumerateQuery.endpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "y"));
    const Netlist::PathQueryResult enumerateResult =
        netlist.runPathQuery(enumerateQuery);
    report.check(enumerateResult.exists && enumerateResult.paths.size() == 2,
                 "runPathQuery EnumerateAll");

    Netlist::PathQuery everyThroughQuery;
    everyThroughQuery.mode = Netlist::PathQueryMode::EveryPathThrough;
    everyThroughQuery.startpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "a"));
    everyThroughQuery.endpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "y"));
    everyThroughQuery.requiredNodes.push_back(gateNode("g_y"));
    report.check(netlist.runPathQuery(everyThroughQuery).exists,
                 "runPathQuery EveryPathThrough");

    everyThroughQuery.requiredNodes.clear();
    everyThroughQuery.requiredNodes.push_back(gateNode("g_and"));
    report.check(!netlist.runPathQuery(everyThroughQuery).exists,
                 "runPathQuery EveryPathThrough negative");

    Netlist::PathQuery everyAvoidsQuery;
    everyAvoidsQuery.mode = Netlist::PathQueryMode::EveryPathAvoids;
    everyAvoidsQuery.startpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "a"));
    everyAvoidsQuery.endpoints.push_back(Netlist::PathEndpoint(
        Netlist::PathEndpointType::SpecificNet, "y"));
    everyAvoidsQuery.avoidedNodes.push_back(netNode("z"));
    report.check(netlist.runPathQuery(everyAvoidsQuery).exists,
                 "runPathQuery EveryPathAvoids");

    everyAvoidsQuery.avoidedNodes.clear();
    everyAvoidsQuery.avoidedNodes.push_back(netNode("n_or"));
    report.check(!netlist.runPathQuery(everyAvoidsQuery).exists,
                 "runPathQuery EveryPathAvoids negative");
}

// 測試第一階段 DepthAnalysis API：net level、gate level 與單一 net depth。
void testDepthAnalysis(TestReport& report, const Netlist& netlist) {
    const std::vector<int> netLevels = netlist.computeNetLevels();
    report.check(netLevels.size() == netlist.getNetCount(),
                 "computeNetLevels size");
    report.check(netLevels[netlist.getNetId("a")] == 0 &&
                 netLevels[netlist.getNetId("bus[0]")] == 0,
                 "computeNetLevels PI level");
    report.check(netLevels[netlist.getNetId("n_and")] == 1 &&
                 netLevels[netlist.getNetId("n_or")] == 2 &&
                 netLevels[netlist.getNetId("n_buf")] == 8 &&
                 netLevels[netlist.getNetId("y")] == 9,
                 "computeNetLevels combinational chain");
    report.check(netLevels[netlist.getNetId("q")] == 0,
                 "computeNetLevels DFF Q boundary");
    report.check(netLevels[netlist.getNetId("direct_out")] == 1 &&
                 netLevels[netlist.getNetId("bus_out")] == 1,
                 "computeNetLevels shallow outputs");
    report.check(netLevels[netlist.getNetId("d24")] == 25,
                 "computeNetLevels long buffer chain");

    const std::vector<int> gateLevels = netlist.computeGateLevels();
    report.check(gateLevels.size() == netlist.getGateCount(),
                 "computeGateLevels size");
    report.check(gateLevels[netlist.getGateId("g_and")] == 1 &&
                 gateLevels[netlist.getGateId("g_buf")] == 8 &&
                 gateLevels[netlist.getGateId("g_y")] == 9,
                 "computeGateLevels combinational gates");
    report.check(gateLevels[netlist.getGateId("ff1")] == -1,
                 "computeGateLevels DFF boundary");

    report.check(netlist.getMaxDepthToNet("y") == 9,
                 "getMaxDepthToNet y");
    report.check(netlist.getMaxDepthToNet("q") == 0,
                 "getMaxDepthToNet DFF Q");
    report.check(netlist.getMaxDepthToNet("missing_net") == -1,
                 "getMaxDepthToNet missing net");

    const Netlist::CombinationalPath criticalY =
        netlist.findCriticalPathToNet("y");
    report.check(criticalY.exists() && criticalY.depth() == 9 &&
                 criticalY.netIds.front() == netlist.getNetId("a") &&
                 criticalY.netIds.back() == netlist.getNetId("y") &&
                 pathContainsGate(netlist, criticalY, "g_and") &&
                 pathContainsGate(netlist, criticalY, "g_buf") &&
                 pathContainsGate(netlist, criticalY, "g_y"),
                 "findCriticalPathToNet y");

    const Netlist::CombinationalPath criticalQ =
        netlist.findCriticalPathToNet("q");
    report.check(criticalQ.exists() && criticalQ.depth() == 0 &&
                 criticalQ.netIds.front() == netlist.getNetId("q"),
                 "findCriticalPathToNet DFF Q boundary");
    report.check(!netlist.findCriticalPathToNet("missing_net").exists(),
                 "findCriticalPathToNet missing net");

    const Netlist::DepthReport yReport = netlist.analyzeDepthToNet("y");
    report.check(yReport.endpointType == Netlist::DepthEndpointType::SpecificNet &&
                 yReport.endpointName == "y" &&
                 yReport.endpointNetId == netlist.getNetId("y") &&
                 yReport.depth == 9 &&
                 yReport.criticalPath.exists(),
                 "analyzeDepthToNet y");

    const Netlist::DepthReport qReport = netlist.analyzeDepthToNet("q");
    report.check(qReport.endpointType == Netlist::DepthEndpointType::SpecificNet &&
                 qReport.endpointName == "q" &&
                 qReport.endpointNetId == netlist.getNetId("q") &&
                 qReport.depth == 0 &&
                 qReport.criticalPath.exists(),
                 "analyzeDepthToNet DFF Q boundary");

    const Netlist::DepthReport missingReport =
        netlist.analyzeDepthToNet("missing_net");
    report.check(missingReport.endpointNetId == -1 &&
                 missingReport.depth == -1 &&
                 !missingReport.criticalPath.exists(),
                 "analyzeDepthToNet missing net");

    const std::vector<Netlist::DepthReport> outputReports =
        netlist.analyzePrimaryOutputDepths();
    report.check(outputReports.size() == 5,
                 "analyzePrimaryOutputDepths output count");
    bool sawYOutputReport = false;
    bool sawDirectOutputReport = false;
    for (const Netlist::DepthReport& reportItem : outputReports) {
        if (reportItem.endpointType == Netlist::DepthEndpointType::PrimaryOutput &&
            reportItem.endpointName == "y" &&
            reportItem.endpointNetId == netlist.getNetId("y") &&
            reportItem.depth == 9 &&
            reportItem.criticalPath.exists()) {
            sawYOutputReport = true;
        }
        if (reportItem.endpointType == Netlist::DepthEndpointType::PrimaryOutput &&
            reportItem.endpointName == "direct_out" &&
            reportItem.endpointNetId == netlist.getNetId("direct_out") &&
            reportItem.depth == 1) {
            sawDirectOutputReport = true;
        }
    }
    report.check(sawYOutputReport && sawDirectOutputReport,
                 "analyzePrimaryOutputDepths reports expected depths");

    const std::vector<std::string> deepOutputs =
        netlist.getPrimaryOutputsWithDepthGreaterThan(4);
    report.check(containsString(deepOutputs, "y") &&
                 !containsString(deepOutputs, "direct_out") &&
                 !containsString(deepOutputs, "bus_out"),
                 "getPrimaryOutputsWithDepthGreaterThan");

    const std::vector<Netlist::DepthReport> dffDReports =
        netlist.analyzeDffDDepths();
    report.check(dffDReports.size() == 1 &&
                 dffDReports.front().endpointType == Netlist::DepthEndpointType::DffD &&
                 dffDReports.front().endpointName == "ff1.D" &&
                 dffDReports.front().endpointNetId == netlist.getNetId("n_buf") &&
                 dffDReports.front().depth == 8 &&
                 dffDReports.front().criticalPath.exists(),
                 "analyzeDffDDepths");

    const std::vector<std::string> deepDffs =
        netlist.getDffsWithDDepthGreaterThan(4);
    report.check(deepDffs.size() == 1 && deepDffs.front() == "ff1",
                 "getDffsWithDDepthGreaterThan");

    const Netlist::DepthReport globalCritical =
        netlist.findGlobalCriticalPath();
    report.check(globalCritical.endpointType == Netlist::DepthEndpointType::PrimaryOutput &&
                 globalCritical.endpointName == "y" &&
                 globalCritical.endpointNetId == netlist.getNetId("y") &&
                 globalCritical.depth == 9 &&
                 globalCritical.criticalPath.exists(),
                 "findGlobalCriticalPath");

    const std::vector<Netlist::DepthReport> exceededEndpoints =
        netlist.findEndpointsExceedingDepth(7);
    bool sawExceededY = false;
    bool sawExceededDffD = false;
    for (const Netlist::DepthReport& reportItem : exceededEndpoints) {
        if (reportItem.endpointNetId == netlist.getNetId("y") &&
            reportItem.depth == 9) {
            sawExceededY = true;
        }
        if (reportItem.endpointNetId == netlist.getNetId("n_buf") &&
            reportItem.depth == 8) {
            sawExceededDffD = true;
        }
    }
    report.check(exceededEndpoints.size() == 2 &&
                 sawExceededY &&
                 sawExceededDffD,
                 "findEndpointsExceedingDepth");

    const Netlist::OptimizationCandidate yCandidate =
        netlist.buildOptimizationCandidate(yReport, 4);
    report.check(yCandidate.endpoint.endpointName == "y" &&
                 yCandidate.targetDepth == 4 &&
                 yCandidate.criticalPath.exists() &&
                 containsInt(std::vector<int>(yCandidate.faninCone.netIds.begin(),
                                             yCandidate.faninCone.netIds.end()),
                             netlist.getNetId("n_and")),
                 "buildOptimizationCandidate");
    const std::vector<int> yBufferGates =
        netlist.findBufferGatesOnCriticalPath(yCandidate);
    report.check(yBufferGates.size() == 2 &&
                 containsInt(yBufferGates, netlist.getGateId("g_buf")) &&
                 containsInt(yBufferGates, netlist.getGateId("g_y")),
                 "findBufferGatesOnCriticalPath finds y BUF gates");
    const std::vector<int> yRemovableBufferGates =
        netlist.findRemovableBufferGatesOnCriticalPath(yCandidate);
    report.check(yRemovableBufferGates.size() == 1 &&
                 yRemovableBufferGates.front() == netlist.getGateId("g_buf"),
                 "findRemovableBufferGatesOnCriticalPath skips PO BUF");

    const Netlist::DepthReport busReport =
        netlist.analyzeDepthToNet("bus_out");
    const Netlist::OptimizationCandidate busCandidate =
        netlist.buildOptimizationCandidate(busReport, 0);
    report.check(netlist.findBufferGatesOnCriticalPath(busCandidate).empty(),
                 "findBufferGatesOnCriticalPath no BUF");
    report.check(netlist.findRemovableBufferGatesOnCriticalPath(busCandidate).empty(),
                 "findRemovableBufferGatesOnCriticalPath no BUF");

    const std::vector<Netlist::OptimizationCandidate> candidates =
        netlist.findOptimizationCandidatesExceedingDepth(7);
    bool sawYCandidate = false;
    bool sawDffCandidate = false;
    for (const Netlist::OptimizationCandidate& candidate : candidates) {
        if (candidate.endpoint.endpointName == "y" &&
            candidate.endpoint.endpointType == Netlist::DepthEndpointType::PrimaryOutput &&
            candidate.targetDepth == 7 &&
            candidate.criticalPath.exists()) {
            sawYCandidate = true;
        }
        if (candidate.endpoint.endpointName == "ff1.D" &&
            candidate.endpoint.endpointType == Netlist::DepthEndpointType::DffD &&
            candidate.targetDepth == 7 &&
            candidate.criticalPath.exists()) {
            sawDffCandidate = true;
        }
    }
    report.check(candidates.size() == 2 && sawYCandidate && sawDffCandidate,
                 "findOptimizationCandidatesExceedingDepth");
}

// 測試 LEC；需要連結 CaDiCaL library。
void testEquivalence(TestReport& report, const Netlist& netlist) {
    report.check(netlist.checkEquivalence("y", "y"), "checkEquivalence identical");
    report.check(!netlist.checkEquivalence("y", "z"), "checkEquivalence different");
}

// 測試 VerilogWriter 與 ECO transformation API。
void testWriterAndTransformations(TestReport& report, const Netlist& original) {
    VerilogWriter writer;
    report.check(writer.write("mini test/writer_output.v", original), "VerilogWriter::write");

    Netlist netlist = original;
    report.check(netlist.renameGate("g_direct", "g_direct_renamed"), "renameGate");
    report.check(netlist.getGateId("g_direct") == -1 &&
                 netlist.getGateId("g_direct_renamed") >= 0,
                 "renameGate updates lookup");
    report.check(!netlist.renameGate("g_y", "g_buf"), "renameGate rejects duplicate");

    report.check(netlist.renameNet("direct_out", "direct_renamed"), "renameNet");
    report.check(netlist.getNetId("direct_out") == -1 &&
                 netlist.getNetId("direct_renamed") >= 0,
                 "renameNet updates lookup");
    report.check(!netlist.renameNet("y", "z"), "renameNet rejects duplicate");

    report.check(netlist.disconnectGateInput("g_y", "n_buf"), "disconnectGateInput");
    report.check(!netlist.hasCombinationalPath("a", "y"),
                 "disconnectGateInput updates graph");
    report.check(netlist.connectGateInput("g_y", "n_buf"), "connectGateInput by first empty pin");
    report.check(netlist.hasCombinationalPath("a", "y"),
                 "connectGateInput restores graph");

    Netlist dffEditNetlist = original;
    const int ff1Id = dffEditNetlist.getGateId("ff1");
    report.check(dffEditNetlist.disconnectGateInput("ff1", "n_buf"),
                 "disconnectGateInput preserves DFF pin slot");
    report.check(dffEditNetlist.getDffInputNetId(ff1Id, "D") == -1 &&
                 dffEditNetlist.getDffInputNetId(ff1Id, "CK") == dffEditNetlist.getNetId("clk"),
                 "DFF disconnected input does not shift named pins");

    VerilogWriter disconnectedWriter;
    report.check(disconnectedWriter.write("mini test/writer_unconnected_output.v",
                                          dffEditNetlist),
                 "VerilogWriter handles disconnected DFF input");
    report.check(dffEditNetlist.connectGateInput("ff1", "n_buf", 0),
                 "connectGateInput restores DFF pin by index");
    report.check(dffEditNetlist.getDffInputNetId(ff1Id, "D") == dffEditNetlist.getNetId("n_buf") &&
                 dffEditNetlist.getDffInputNetId(ff1Id, "CK") == dffEditNetlist.getNetId("clk"),
                 "DFF reconnect keeps named pin order");
}

} // namespace

// 執行 mini Verilog 的整合測試；可用 argv[1] 指定其他 Verilog 檔。
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

    testBasicQueries(report, netlist);
    testDirectAnalysis(report, netlist);
    testConeQueries(report, netlist);
    testConePathQueries(report, netlist);
    testPathQueries(report, netlist);
    testLongestAndEndpointQueries(report, netlist);
    testPiToDffDepthQueries(report, netlist);
    testEndpointResolverHelpers(report, netlist);
    testRunPathQuery(report, netlist);
    testDepthAnalysis(report, netlist);
    testEquivalence(report, netlist);
    testWriterAndTransformations(report, netlist);

    std::cout << "\nSummary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
