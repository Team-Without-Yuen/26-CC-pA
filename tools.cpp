#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

namespace {

// 將字串轉小寫，讓 CLI mode/type 可以接受大小寫混用。
std::string toLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// 判斷 token 是否為整數，用於 gate_in:<gate>:<index>。
bool isIntegerToken(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    size_t start = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (start == text.size()) {
        return false;
    }
    for (size_t i = start; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }
    return true;
}

// 依 delimiter 切字串；這裡只用於解析 CLI endpoint/node token。
std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream iss(text);
    while (std::getline(iss, current, delimiter)) {
        parts.push_back(current);
    }
    if (!text.empty() && text.back() == delimiter) {
        parts.emplace_back();
    }
    return parts;
}

// 清掉 CLI 參數前後空白。
std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

// read/write 檔名可能包含空白；吃掉整行剩餘內容並移除外層引號。
std::string readRestPath(std::istringstream& iss) {
    std::string path;
    std::getline(iss >> std::ws, path);
    path = trim(path);
    if (path.size() >= 2 &&
        ((path.front() == '"' && path.back() == '"') ||
         (path.front() == '\'' && path.back() == '\''))) {
        path = path.substr(1, path.size() - 2);
    }
    return path;
}

// 印出 string 陣列；所有高階 query report 的 name list 都共用這個輸出。
void printStringList(const std::string& title,
                     const std::vector<std::string>& values) {
    std::cout << title << " (" << values.size() << "):\n";
    for (const std::string& value : values) {
        std::cout << "  " << value << "\n";
    }
}

// 印出 path 的 net/gate 序列，供 path/depth 類 query 共用。
void printPath(const Netlist& netlist, const Netlist::CombinationalPath& path) {
    if (!path.exists()) {
        std::cout << "  (no path)\n";
        return;
    }

    std::cout << "  Depth: " << path.depth() << "\n";
    std::cout << "  Nets:\n";
    for (int netId : path.netIds) {
        std::cout << "    " << netlist.getNet(netId).name << "\n";
    }
    std::cout << "  Gates:\n";
    for (int gateId : path.gateIds) {
        std::cout << "    " << netlist.getGate(gateId).instName << "\n";
    }
}

// 將 CLI node token 轉成 PathNode。
// 語法：gate:<name>、net:<name>，沒有前綴時預設視為 net。
Netlist::PathNode parsePathNode(const std::string& token) {
    if (token.rfind("gate:", 0) == 0) {
        return Netlist::PathNode(Netlist::PathNodeType::Gate, token.substr(5));
    }
    if (token.rfind("net:", 0) == 0) {
        return Netlist::PathNode(Netlist::PathNodeType::Net, token.substr(4));
    }
    return Netlist::PathNode(Netlist::PathNodeType::Net, token);
}

// 將 CLI endpoint token 轉成 PathEndpoint。
// 支援 net/pi/po/dff_q/dff_d/dff_clk/dff_reset/gate_out/gate_in。
Netlist::PathEndpoint parseEndpoint(const std::string& token) {
    std::vector<std::string> parts = split(token, ':');
    if (parts.size() < 2) {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, token);
    }

    const std::string prefix = toLower(parts[0]);
    const std::string name = parts[1];
    const std::string pin = parts.size() >= 3 ? parts[2] : "";

    if (prefix == "net") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, name);
    }
    if (prefix == "pi") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryInput, name);
    }
    if (prefix == "po") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryOutput, name);
    }
    if (prefix == "dff_q") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::DffQ, name);
    }
    if (prefix == "dff_d") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::DffD, name);
    }
    if (prefix == "dff_clk" || prefix == "dff_clock") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::DffClock, name, pin);
    }
    if (prefix == "dff_reset" || prefix == "dff_rst") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::DffReset, name, pin);
    }
    if (prefix == "gate_out") {
        return Netlist::PathEndpoint(Netlist::PathEndpointType::GateOutput, name);
    }
    if (prefix == "gate_in") {
        if (isIntegerToken(pin)) {
            return Netlist::PathEndpoint(Netlist::PathEndpointType::GateInput,
                                         name, "", std::stoi(pin));
        }
        return Netlist::PathEndpoint(Netlist::PathEndpointType::GateInput,
                                     name, pin, pin.empty() ? 0 : -1);
    }

    return Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, token);
}

// 印出 BasicQuery 的統一 report。
void printBasicReport(const Netlist& netlist, const Netlist::BasicReport& report) {
    if (!report.ok) {
        std::cout << "Error: " << report.message << "\n";
        return;
    }

    std::cout << "OK: " << report.message << "\n";
    if (report.gateCount || report.netCount || report.logicalWireCount ||
        report.primaryInputCount || report.primaryOutputCount) {
        std::cout << "  gates: " << report.gateCount << "\n";
        std::cout << "  nets: " << report.netCount << "\n";
        std::cout << "  logical wires: " << report.logicalWireCount << "\n";
        std::cout << "  primary inputs: " << report.primaryInputCount << "\n";
        std::cout << "  primary outputs: " << report.primaryOutputCount << "\n";
    }
    if (!report.objectName.empty()) {
        std::cout << "  object: " << report.objectName << "\n";
        std::cout << "  id: " << report.objectId << "\n";
        if (!report.typeName.empty()) {
            std::cout << "  type: " << report.typeName << "\n";
        }
        if (report.portWidth >= 0) {
            std::cout << "  width: " << report.portWidth << "\n";
        }
        if (!report.formattedInfo.empty()) {
            std::cout << report.formattedInfo << "\n";
        }
    }
    if (!report.gateTypeCounts.empty()) {
        std::cout << "Gate type counts:\n";
        for (const auto& item : report.gateTypeCounts) {
            std::cout << "  " << netlist.gateTypeToString(item.first)
                      << " : " << item.second << "\n";
        }
    }
    if (!report.gateNames.empty()) {
        printStringList("Gate names", report.gateNames);
    }
    if (!report.netNames.empty()) {
        printStringList("Net names", report.netNames);
    }
    if (!report.portNames.empty()) {
        printStringList("Port names", report.portNames);
    }
    if (!report.undrivenNets.empty()) {
        printStringList("Undriven nets", report.undrivenNets);
    }
    if (!report.noLoadNets.empty()) {
        printStringList("No-load nets", report.noLoadNets);
    }
    if (!report.floatingNets.empty()) {
        printStringList("Floating nets", report.floatingNets);
    }
    if (!report.unconnectedGates.empty()) {
        printStringList("Unconnected gates", report.unconnectedGates);
    }
}

// 印出 DirectConnectivityQuery 的統一 report。
void printConnectivityReport(const Netlist::DirectConnectivityReport& report) {
    if (!report.ok) {
        std::cout << "Error: " << report.message << "\n";
        return;
    }

    std::cout << "OK: " << report.message << "\n";
    if (!report.gateName.empty()) {
        std::cout << "  gate: " << report.gateName << "\n";
    }
    if (!report.netName.empty()) {
        std::cout << "  net: " << report.netName << "\n";
    }
    std::cout << "  count: " << report.count << "\n";
    if (report.connected) {
        std::cout << "  connected: yes\n";
    }
    if (!report.gateNames.empty()) {
        printStringList("Gate names", report.gateNames);
    }
    if (!report.netNames.empty()) {
        printStringList("Net names", report.netNames);
    }
}

// 印出 ConeQuery 的統一 report。
void printConeReport(const Netlist& netlist, const Netlist::ConeReport& report) {
    if (!report.ok) {
        std::cout << "Error: " << report.message << "\n";
        return;
    }

    std::cout << "OK: " << report.message << "\n";
    std::cout << "  source: " << report.sourceName << "\n";
    std::cout << "  gates: " << report.gateCount << "\n";
    std::cout << "  nets: " << report.netCount << "\n";
    if (!report.rootNetNames.empty()) {
        printStringList("Root nets", report.rootNetNames);
    }
    if (!report.gateNames.empty()) {
        printStringList("Cone gates", report.gateNames);
    }
    if (!report.netNames.empty()) {
        printStringList("Cone nets", report.netNames);
    }
    if (report.longestDepth >= 0) {
        std::cout << "  longest local path depth: " << report.longestDepth << "\n";
        for (int netId : report.longestPathNetIds) {
            std::cout << "    " << netlist.getNet(netId).name << "\n";
        }
    }
    if (report.shortestDepth >= 0) {
        std::cout << "  shortest local path depth: " << report.shortestDepth << "\n";
    }
}

// 印出 PathQuery 的統一 result。
void printPathResult(const Netlist& netlist,
                     Netlist::PathQueryMode mode,
                     const Netlist::PathQueryResult& result) {
    if (mode == Netlist::PathQueryMode::Exists ||
        mode == Netlist::PathQueryMode::EveryPathThrough ||
        mode == Netlist::PathQueryMode::EveryPathAvoids) {
        std::cout << (result.exists ? "Yes\n" : "No\n");
        return;
    }

    if (mode == Netlist::PathQueryMode::EnumerateAll) {
        std::cout << "Total paths: " << result.paths.size() << "\n";
        for (size_t i = 0; i < result.paths.size(); ++i) {
            std::cout << "Path " << (i + 1) << ":\n";
            printPath(netlist, result.paths[i]);
        }
        return;
    }

    if (!result.path.exists()) {
        std::cout << "No path found.\n";
        return;
    }
    printPath(netlist, result.path);
}

// 印出 DepthQuery 的統一 report。
void printDepthReportSet(const Netlist& netlist, const Netlist::DepthReportSet& report) {
    if (!report.ok) {
        std::cout << "Error: " << report.message << "\n";
        return;
    }

    std::cout << "OK: " << report.message << "\n";
    std::cout << "Report count: " << report.count << "\n";
    for (const DepthReport& item : report.reports) {
        std::cout << "  " << item.endpointName << " depth=" << item.depth << "\n";
    }
    if (report.worst.depth >= 0) {
        std::cout << "Worst endpoint: " << report.worst.endpointName
                  << " depth=" << report.worst.depth << "\n";
        printPath(netlist, report.worst.criticalPath);
    }
}

// 印出 FunctionQuery 的統一 report。
void printFunctionReport(const Netlist::FunctionReport& report) {
    if (!report.ok) {
        std::cout << "Error: " << report.message << "\n";
        return;
    }

    std::cout << "OK: " << report.message << "\n";
    if (!report.netNameA.empty()) {
        std::cout << "  net A: " << report.netNameA << "\n";
    }
    if (!report.netNameB.empty()) {
        std::cout << "  net B: " << report.netNameB << "\n";
    }
    if (!report.status.empty()) {
        std::cout << "  status: " << report.status << "\n";
    }
    std::cout << "  answer: " << (report.exists ? "yes" : "no") << "\n";
    std::cout << "  equivalent: " << (report.equivalent ? "yes" : "no") << "\n";
    std::cout << "  can be 0: " << (report.canBeZero ? "yes" : "no") << "\n";
    std::cout << "  can be 1: " << (report.canBeOne ? "yes" : "no") << "\n";
    std::cout << "  is constant: " << (report.isConstant ? "yes" : "no") << "\n";
}

// 將 basic_query 的 mode 轉成 BasicQuery。
bool buildBasicQuery(const Netlist& netlist,
                     std::istringstream& iss,
                     const std::string& mode,
                     Netlist::BasicQuery& query) {
    const std::string m = toLower(mode);
    if (m == "summary") {
        query.type = Netlist::BasicQueryType::Summary;
    } else if (m == "list_gates") {
        query.type = Netlist::BasicQueryType::ListGates;
    } else if (m == "list_nets") {
        query.type = Netlist::BasicQueryType::ListNets;
    } else if (m == "list_pi") {
        query.type = Netlist::BasicQueryType::ListPrimaryInputs;
    } else if (m == "list_po") {
        query.type = Netlist::BasicQueryType::ListPrimaryOutputs;
    } else if (m == "list_dffs") {
        query.type = Netlist::BasicQueryType::ListDffs;
    } else if (m == "list_comb" || m == "list_comb_gates") {
        query.type = Netlist::BasicQueryType::ListCombinationalGates;
    } else if (m == "gate_info") {
        query.type = Netlist::BasicQueryType::GateInfo;
        iss >> query.name;
    } else if (m == "net_info") {
        query.type = Netlist::BasicQueryType::NetInfo;
        iss >> query.name;
    } else if (m == "port_info") {
        query.type = Netlist::BasicQueryType::PortInfo;
        iss >> query.name;
    } else if (m == "count_by_type") {
        query.type = Netlist::BasicQueryType::CountByGateType;
        std::string gateType;
        if (iss >> gateType) {
            query.gateType = netlist.stringToGateType(gateType);
        }
    } else if (m == "gates_by_type") {
        query.type = Netlist::BasicQueryType::GatesByType;
        std::string gateType;
        iss >> gateType;
        query.gateType = netlist.stringToGateType(gateType);
    } else if (m == "const_input_gates") {
        query.type = Netlist::BasicQueryType::GatesWithConstantInput;
        std::string gateType;
        if (iss >> gateType) {
            query.gateType = netlist.stringToGateType(gateType);
            iss >> query.constValue;
        }
    } else if (m == "structural_issues") {
        query.type = Netlist::BasicQueryType::StructuralIssues;
    } else {
        return false;
    }
    return true;
}

// 將 conn_query 的 mode 轉成 DirectConnectivityQuery。
bool buildConnectivityQuery(std::istringstream& iss,
                            const std::string& mode,
                            Netlist::DirectConnectivityQuery& query) {
    const std::string m = toLower(mode);
    if (m == "net_driver") {
        query.type = Netlist::DirectConnectivityQueryType::NetDriver;
        iss >> query.netName;
    } else if (m == "net_loads") {
        query.type = Netlist::DirectConnectivityQueryType::NetLoads;
        iss >> query.netName;
    } else if (m == "gate_inputs") {
        query.type = Netlist::DirectConnectivityQueryType::GateInputs;
        iss >> query.gateName;
    } else if (m == "gate_output") {
        query.type = Netlist::DirectConnectivityQueryType::GateOutput;
        iss >> query.gateName;
    } else if (m == "gate_fanin") {
        query.type = Netlist::DirectConnectivityQueryType::GateFanin;
        iss >> query.gateName;
    } else if (m == "gate_fanout") {
        query.type = Netlist::DirectConnectivityQueryType::GateFanout;
        iss >> query.gateName;
    } else if (m == "is_connected") {
        query.type = Netlist::DirectConnectivityQueryType::DirectlyConnected;
        iss >> query.gateName >> query.netName;
    } else {
        return false;
    }
    return true;
}

// 將 cone_query 的 mode 轉成 ConeQuery。
bool buildConeQuery(std::istringstream& iss,
                    const std::string& mode,
                    Netlist::ConeQuery& query) {
    const std::string m = toLower(mode);
    if (m == "net_fanin") {
        query.type = Netlist::ConeQueryType::NetTransitiveFanin;
        iss >> query.netName;
    } else if (m == "net_fanout") {
        query.type = Netlist::ConeQueryType::NetTransitiveFanout;
        iss >> query.netName;
    } else if (m == "gate_fanin") {
        query.type = Netlist::ConeQueryType::GateTransitiveFanin;
        iss >> query.gateName;
    } else if (m == "gate_fanout") {
        query.type = Netlist::ConeQueryType::GateTransitiveFanout;
        iss >> query.gateName;
    } else {
        return false;
    }

    std::string option;
    while (iss >> option) {
        if (toLower(option) == "with_paths") {
            query.includeLocalPaths = true;
        }
    }
    return true;
}

// 將 depth_query 的 mode 轉成 DepthQuery。
bool buildDepthQuery(std::istringstream& iss,
                     const std::string& mode,
                     Netlist::DepthQuery& query) {
    const std::string m = toLower(mode);
    if (m == "net") {
        query.type = Netlist::DepthQueryType::SpecificNet;
        iss >> query.netName;
    } else if (m == "all_po") {
        query.type = Netlist::DepthQueryType::PrimaryOutputs;
    } else if (m == "all_dff_d") {
        query.type = Netlist::DepthQueryType::DffD;
    } else if (m == "global_critical") {
        query.type = Netlist::DepthQueryType::GlobalCriticalPath;
    } else if (m == "exceeding") {
        query.type = Netlist::DepthQueryType::EndpointsExceedingDepth;
        iss >> query.threshold;
    } else {
        return false;
    }
    return true;
}

// 將 func_query 的 mode 轉成 FunctionQuery。
bool buildFunctionQuery(std::istringstream& iss,
                        const std::string& mode,
                        Netlist::FunctionQuery& query) {
    const std::string m = toLower(mode);
    if (m == "equivalence") {
        query.type = Netlist::FunctionQueryType::Equivalence;
        iss >> query.netNameA >> query.netNameB;
    } else if (m == "can_be_value") {
        query.type = Netlist::FunctionQueryType::CanBeValue;
        iss >> query.netNameA >> query.constValue;
    } else if (m == "constant") {
        query.type = Netlist::FunctionQueryType::ConstantFunction;
        iss >> query.netNameA >> query.constValue;
    } else if (m == "always_zero") {
        query.type = Netlist::FunctionQueryType::AlwaysZero;
        iss >> query.netNameA;
    } else if (m == "always_one") {
        query.type = Netlist::FunctionQueryType::AlwaysOne;
        iss >> query.netNameA;
    } else if (m == "truth_status") {
        query.type = Netlist::FunctionQueryType::TruthStatus;
        iss >> query.netNameA;
    } else {
        return false;
    }
    return true;
}

// 將 path_query 的 mode 轉成 PathQueryMode。
bool parsePathMode(const std::string& mode, Netlist::PathQueryMode& outMode) {
    const std::string m = toLower(mode);
    if (m == "exists") {
        outMode = Netlist::PathQueryMode::Exists;
    } else if (m == "find_any") {
        outMode = Netlist::PathQueryMode::FindAny;
    } else if (m == "enumerate") {
        outMode = Netlist::PathQueryMode::EnumerateAll;
    } else if (m == "min_depth") {
        outMode = Netlist::PathQueryMode::MinDepth;
    } else if (m == "max_depth") {
        outMode = Netlist::PathQueryMode::MaxDepth;
    } else if (m == "every_through") {
        outMode = Netlist::PathQueryMode::EveryPathThrough;
    } else if (m == "every_avoids") {
        outMode = Netlist::PathQueryMode::EveryPathAvoids;
    } else {
        return false;
    }
    return true;
}

// 印出統一 CLI 的 help；只保留分類式高階入口，避免 LLM 選到低階散裝 API。
void printHelp() {
    std::cout
        << "Unified EDA query CLI\n"
        << "\nI/O\n"
        << "  read <verilog_file>\n"
        << "  write <verilog_file>\n"
        << "  quit\n"
        << "\nBasic netlist query\n"
        << "  basic_query <mode> [args]\n"
        << "  mode: summary | list_gates | list_nets | list_pi | list_po\n"
        << "        list_dffs | list_comb | gate_info <gate> | net_info <net>\n"
        << "        port_info <port> | count_by_type [type] | gates_by_type <type>\n"
        << "        const_input_gates [type] [0|1] | structural_issues\n"
        << "\nDirect connectivity query\n"
        << "  conn_query <mode> [args]\n"
        << "  mode: net_driver <net> | net_loads <net> | gate_inputs <gate>\n"
        << "        gate_output <gate> | gate_fanin <gate> | gate_fanout <gate>\n"
        << "        is_connected <gate> <net>\n"
        << "\nCone query\n"
        << "  cone_query <mode> <name> [with_paths]\n"
        << "  mode: net_fanin | net_fanout | gate_fanin | gate_fanout\n"
        << "\nPath query\n"
        << "  path_query <mode> <start_endpoint> <end_endpoint> [-req node...] [-avoid node...]\n"
        << "  mode: exists | find_any | enumerate | min_depth | max_depth\n"
        << "        every_through | every_avoids\n"
        << "  endpoint: net:<n> | pi:<p> | po:<p> | dff_q:<ff> | dff_d:<ff>\n"
        << "            dff_clk:<ff>[:pin] | dff_reset:<ff>[:pin]\n"
        << "            gate_out:<g> | gate_in:<g>:<index_or_pin> | bare_net\n"
        << "  node: gate:<g> | net:<n> | bare_net\n"
        << "\nDepth query\n"
        << "  depth_query <mode> [args]\n"
        << "  mode: net <net> | all_po | all_dff_d | global_critical | exceeding <depth>\n"
        << "\nFunction query\n"
        << "  func_query <mode> [args]\n"
        << "  mode: equivalence <net_a> <net_b> | can_be_value <net> <0|1>\n"
        << "        constant <net> <0|1> | always_zero <net> | always_one <net>\n"
        << "        truth_status <net>\n";
}

} // namespace

int main() {
    Netlist netlist;
    VerilogReader reader;
    VerilogWriter writer;
    std::string inputLine;

    while (true) {
        std::cout << "eda> ";
        if (!std::getline(std::cin, inputLine)) {
            break;
        }

        std::istringstream iss(inputLine);
        std::string command;
        iss >> command;
        command = toLower(command);
        if (command.empty()) {
            continue;
        }

        if (command == "quit" || command == "exit") {
            break;
        }

        if (command == "help") {
            printHelp();
            continue;
        }

        if (command == "read") {
            const std::string filepath = readRestPath(iss);
            if (filepath.empty()) {
                std::cout << "Usage: read <verilog_file>\n";
                continue;
            }
            std::cout << (reader.read(filepath, netlist) ? "OK: loaded " : "Error: failed to read ")
                      << filepath << "\n";
            continue;
        }

        if (command == "write") {
            const std::string filepath = readRestPath(iss);
            if (filepath.empty()) {
                std::cout << "Usage: write <verilog_file>\n";
                continue;
            }
            std::cout << (writer.write(filepath, netlist) ? "OK: wrote " : "Error: failed to write ")
                      << filepath << "\n";
            continue;
        }

        if (command == "basic_query") {
            std::string mode;
            if (!(iss >> mode)) {
                std::cout << "Usage: basic_query <mode> [args]\n";
                continue;
            }
            Netlist::BasicQuery query;
            if (!buildBasicQuery(netlist, iss, mode, query)) {
                std::cout << "Unknown basic_query mode: " << mode << "\n";
                continue;
            }
            printBasicReport(netlist, netlist.runBasicQuery(query));
            continue;
        }

        if (command == "conn_query") {
            std::string mode;
            if (!(iss >> mode)) {
                std::cout << "Usage: conn_query <mode> [args]\n";
                continue;
            }
            Netlist::DirectConnectivityQuery query;
            if (!buildConnectivityQuery(iss, mode, query)) {
                std::cout << "Unknown conn_query mode: " << mode << "\n";
                continue;
            }
            printConnectivityReport(netlist.runDirectConnectivityQuery(query));
            continue;
        }

        if (command == "cone_query") {
            std::string mode;
            if (!(iss >> mode)) {
                std::cout << "Usage: cone_query <mode> <name> [with_paths]\n";
                continue;
            }
            Netlist::ConeQuery query;
            if (!buildConeQuery(iss, mode, query)) {
                std::cout << "Unknown cone_query mode: " << mode << "\n";
                continue;
            }
            printConeReport(netlist, netlist.runConeQuery(query));
            continue;
        }

        if (command == "path_query") {
            std::string modeText;
            std::string startToken;
            std::string endToken;
            if (!(iss >> modeText >> startToken >> endToken)) {
                std::cout << "Usage: path_query <mode> <start> <end> [-req node...] [-avoid node...]\n";
                continue;
            }

            Netlist::PathQuery query;
            if (!parsePathMode(modeText, query.mode)) {
                std::cout << "Unknown path_query mode: " << modeText << "\n";
                continue;
            }
            query.startpoints.push_back(parseEndpoint(startToken));
            query.endpoints.push_back(parseEndpoint(endToken));

            int listMode = 0; // 0=ignore, 1=required, 2=avoided
            std::string token;
            while (iss >> token) {
                if (token == "-req") {
                    listMode = 1;
                    continue;
                }
                if (token == "-avoid") {
                    listMode = 2;
                    continue;
                }
                if (listMode == 1) {
                    query.requiredNodes.push_back(parsePathNode(token));
                } else if (listMode == 2) {
                    query.avoidedNodes.push_back(parsePathNode(token));
                }
            }

            printPathResult(netlist, query.mode, netlist.runPathQuery(query));
            continue;
        }

        if (command == "depth_query") {
            std::string mode;
            if (!(iss >> mode)) {
                std::cout << "Usage: depth_query <mode> [args]\n";
                continue;
            }
            Netlist::DepthQuery query;
            if (!buildDepthQuery(iss, mode, query)) {
                std::cout << "Unknown depth_query mode: " << mode << "\n";
                continue;
            }
            printDepthReportSet(netlist, netlist.runDepthQuery(query));
            continue;
        }

        if (command == "func_query") {
            std::string mode;
            if (!(iss >> mode)) {
                std::cout << "Usage: func_query <mode> [args]\n";
                continue;
            }
            Netlist::FunctionQuery query;
            if (!buildFunctionQuery(iss, mode, query)) {
                std::cout << "Unknown func_query mode: " << mode << "\n";
                continue;
            }
            printFunctionReport(netlist.runFunctionQuery(query));
            continue;
        }

        std::cout << "Unknown command: " << command << ". Type help for unified commands.\n";
    }

    return 0;
}
