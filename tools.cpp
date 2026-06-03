#include <iostream>
#include <sstream>
#include <string>
#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

// 輔助函數：解析命令列中的 PathNode 陣列 (支援 -req 與 -avoid 標籤)
// 語法範例：-req gate:U1 net:n2 -avoid gate:U3
void parsePathArgs(std::istringstream& iss, std::vector<Netlist::PathNode>& req, std::vector<Netlist::PathNode>& avoid) {
    std::string token;
    int mode = 1; // 預設讀入到 req (如果是只有 avoiding 的指令，呼叫端可直接傳 avoid 進來)
    
    while (iss >> token) {
        if (token == "-req") { mode = 1; continue; }
        if (token == "-avoid") { mode = 2; continue; }

        Netlist::PathNodeType type = Netlist::PathNodeType::Net;
        std::string name = token;
        
        // 判斷前綴是 gate: 還是 net:
        if (token.find("gate:") == 0) {
            type = Netlist::PathNodeType::Gate;
            name = token.substr(5);
        } else if (token.find("net:") == 0) {
            type = Netlist::PathNodeType::Net;
            name = token.substr(4);
        }

        if (mode == 1) req.push_back(Netlist::PathNode(type, name));
        else avoid.push_back(Netlist::PathNode(type, name));
    }
}

int main() {
    Netlist netlist;
    VerilogReader reader;
    VerilogWriter writer;
    std::string inputLine;
    bool isRunning = true;

    // 核心的 REPL 迴圈
    while (isRunning) {
        std::cout << "eda> "; // 提示符號
        
        // 讀取一整行輸入
        if (!std::getline(std::cin, inputLine)) {
            break; // 遇到 EOF (Ctrl+D / Ctrl+Z) 則結束
        }

        // 使用 stringstream 解析指令與參數
        std::istringstream iss(inputLine);
        std::string command;
        iss >> command;

        if (command.empty()) {
            continue; // 如果只按了 Enter，則繼續等待輸入
        }

        // 指令解析區 (Command Routing)
        if (command == "help") {
            std::cout << "Available commands:\n";
            std::cout << "  read <filepath>\n";
            std::cout << "  getGateCount\n";
            std::cout << "  countGatesByType\n";
            std::cout << "  getGateCountByType <gate_type>\n";
            std::cout << "  getPrimaryInputs\n";
            std::cout << "  getPrimaryOutputs\n";
            std::cout << "  getLogicalWireCount\n";
            std::cout << "  getGateNamesWithConstInput [gate_type] [0/1]\n";
            std::cout << "  countGatesWithConstInput [gate_type] [0/1]\n";
            std::cout << "  getGateInfo <inst_name>\n";
            std::cout << "  getWireLoadNames <wire_name>\n";
            std::cout << "  getWireLoadCount <wire_name>\n";
            std::cout << "  getGateFanoutNames <inst_name>\n";
            std::cout << "  getGateFanoutCount <inst_name>\n";
            std::cout << "  getTransitiveFaninConeGateNames <net_name>\n";
            std::cout << "  getTransitiveFaninConeGateCount <net_name>\n";
            std::cout << "  getTransitiveFanoutConeGateNames <net_name>\n";
            std::cout << "  getTransitiveFanoutConeGateCount <net_name>\n";
            std::cout << "  getGateTransitiveFaninConeGateNames <inst_name>\n";
            std::cout << "  getGateTransitiveFaninConeGateCount <inst_name>\n";
            std::cout << "  getGateTransitiveFanoutConeGateNames <inst_name>\n";
            std::cout << "  getGateTransitiveFanoutConeGateCount <inst_name>\n";
            std::cout << "\n--- Combinational Path Analysis ---\n";
            std::cout << "  * Node Format: use 'gate:<name>' or 'net:<name>'. Default is net.\n";
            std::cout << "  * Example: hasCombinationalPathThrough n1 n2 gate:U1\n";
            std::cout << "  * Dual Args: use '-req ... -avoid ...' for ThroughAvoiding commands.\n\n";
            std::cout << "  [Existence] hasCombinationalPath <start> <end>\n";
            std::cout << "  [Existence] hasCombinationalPathAvoiding <start> <end> [nodes...]\n";
            std::cout << "  [Existence] hasCombinationalPathThrough <start> <end> [nodes...]\n";
            std::cout << "  [Existence] hasCombinationalPathThroughAvoiding <start> <end> -req [nodes...] -avoid [nodes...]\n";
            std::cout << "  [Find Any]  findAnyCombinationalPath <start> <end>\n";
            std::cout << "  [Find Any]  findAnyCombinationalPathAvoiding <start> <end> [nodes...]\n";
            std::cout << "  [Find Any]  findAnyCombinationalPathThrough <start> <end> [nodes...]\n";
            std::cout << "  [Find Any]  findAnyCombinationalPathThroughAvoiding <start> <end> -req [nodes...] -avoid [nodes...]\n";
            std::cout << "  [Enumerate] enumerateCombinationalPaths <start> <end>\n";
            std::cout << "  [Enumerate] enumerateCombinationalPathsAvoiding <start> <end> [nodes...]\n";
            std::cout << "  [Enumerate] enumerateCombinationalPathsThrough <start> <end> [nodes...]\n";
            std::cout << "  [Enumerate] enumerateCombinationalPathsThroughAvoiding <start> <end> -req [nodes...] -avoid [nodes...]\n";
            std::cout << "  [Verify]    everyPathPassesThrough <start> <end> [nodes...]\n";
            std::cout << "  [Verify]    everyPathAvoids <start> <end> [nodes...]\n";
            std::cout << "  [Longest]   findLongestCombinationalPath <start> <end>\n";
            std::cout << "  [Longest]   findLongestCombinationalPathAvoiding <start> <end> [nodes...]\n";
            std::cout << "  [Longest]   findLongestCombinationalPathThrough <start> <end> [nodes...]\n";
            std::cout << "  [Longest]   findLongestCombinationalPathThroughAvoiding <start> <end> -req [nodes...] -avoid [nodes...]\n";
            std::cout << "  * NOTE: Add 'ToEndpoint' to any of the above commands for strict endpoint checking.\n";
            std::cout << "\n--------------------------------\n";
            std::cout << "  checkEquivalence <netA> <netB>\n";
            std::cout << "  getTransitiveFaninConeLongestPath <net_name>\n";
            std::cout << "  getTransitiveFaninConeShortestPath <net_name>\n";
            std::cout << "  getTransitiveFanoutConeLongestPath <net_name>\n";
            std::cout << "  getTransitiveFanoutConeShortestPath <net_name>\n";
            std::cout << "  getGateTransitiveFaninConeLongestPath <gate_name>\n";
            std::cout << "  getGateTransitiveFaninConeShortestPath <gate_name>\n";
            std::cout << "  getGateTransitiveFanoutConeLongestPath <gate_name>\n";
            std::cout << "  getGateTransitiveFanoutConeShortestPath <gate_name>\n";
            std::cout << "  write <filepath> (This will save and exit the tool)\n";
        } 
        else if (command == "read") {
            std::string filepath;
            if (iss >> filepath) {
                if (reader.read(filepath, netlist)) {
                    std::cout << "Successfully read Verilog file: " << filepath << "\n";
                } else {
                    std::cout << "Error: Failed to read file " << filepath << "\n";
                }
            } else {
                std::cout << "Usage: read <filepath>\n";
            }
        } 
        else if (command == "getGateCount") {
            std::cout << "Total Gates: " << netlist.getGateCount() << "\n";
        } 
        else if (command == "countGatesByType") {
            auto counts = netlist.countGatesByType();
            for (const auto& pair : counts) {
                std::cout << netlist.gateTypeToString(pair.first) << " : " << pair.second << "\n";
            }
        } 
        else if (command == "getGateCountByType") {
            std::string typeStr;
            if (iss >> typeStr) {
                GateType type = netlist.stringToGateType(typeStr);
                if (type == GateType::UNKNOWN) {
                    std::cout << "Error: Unknown gate type '" << typeStr << "'\n";
                } else {
                    std::cout << "Total " << typeStr << " count: " 
                              << netlist.getGateCountByType(type) << "\n";
                }
            } else {
                std::cout << "Usage: getGateCountByType <AND|OR|DFF|...>\n";
            }
        } 
        else if (command == "getPrimaryInputs") {
            const auto& pis = netlist.getPrimaryInputs();
            std::cout << "Total Primary Inputs: " << pis.size() << "\n";
        } 
        else if (command == "getPrimaryOutputs") {
            const auto& pos = netlist.getPrimaryOutputs();
            std::cout << "Total Primary Outputs: " << pos.size() << "\n";
        } 
        else if (command == "getLogicalWireCount") {
            std::cout << "Total Logical Wires: " << netlist.getLogicalWireCount() << "\n";
        } 
        else if (command == "getGateNamesWithConstInput") {
            GateType type = GateType::UNKNOWN;
            int constVal = -1;
            std::string typeStr;
            
            // 嘗試讀取選填參數
            if (iss >> typeStr) {
                type = netlist.stringToGateType(typeStr);
                if (type == GateType::UNKNOWN) {
                    std::cout << "Warning: Unknown gate type '" << typeStr << "', searching all types.\n";
                }
                iss >> constVal; // 如果還有數字就讀入常數值限制
            }
            
            auto names = netlist.getGateNamesWithConstInput(type, constVal);
            std::cout << "Gates with constant inputs (" << names.size() << " found):\n";
            for (const auto& name : names) {
                std::cout << "  - " << name << "\n";
            }
        }
        else if (command == "countGatesWithConstInput") {
            GateType type = GateType::UNKNOWN;
            int constVal = -1;
            std::string typeStr;
            
            // 嘗試讀取選填參數
            if (iss >> typeStr) {
                type = netlist.stringToGateType(typeStr);
                if (type == GateType::UNKNOWN) {
                    std::cout << "Warning: Unknown gate type '" << typeStr << "', searching all types.\n";
                }
                iss >> constVal;
            }
            
            std::cout << "Total gates with constant inputs: " 
                      << netlist.countGatesWithConstInput(type, constVal) << "\n";
        }
        else if (command == "getGateInfo") {
            std::string instName;
            // 檢查是否有給定必填的實例名稱
            if (iss >> instName) {
                std::cout << netlist.getGateInfo(instName) << "\n";
            } else {
                std::cout << "Usage: getGateInfo <inst_name>\n";
            }
        }
        else if (command == "getWireLoadNames") {
            std::string wireName;
            if (iss >> wireName) {
                auto names = netlist.getWireLoadNames(wireName);
                std::cout << "Wire '" << wireName << "' directly drives " << names.size() << " gates:\n";
                for (const auto& name : names) {
                    std::cout << "  - " << name << "\n";
                }
            } else {
                std::cout << "Usage: getWireLoadNames <wire_name>\n";
            }
        }
        else if (command == "getWireLoadCount") {
            std::string wireName;
            if (iss >> wireName) {
                std::cout << "Wire '" << wireName << "' directly drives " 
                          << netlist.getWireLoadCount(wireName) << " gates.\n";
            } else {
                std::cout << "Usage: getWireLoadCount <wire_name>\n";
            }
        }
        else if (command == "getGateFanoutNames") {
            std::string instName;
            if (iss >> instName) {
                auto names = netlist.getGateFanoutNames(instName);
                std::cout << "Gate '" << instName << "' directly drives " << names.size() << " gates:\n";
                for (const auto& name : names) {
                    std::cout << "  - " << name << "\n";
                }
            } else {
                std::cout << "Usage: getGateFanoutNames <inst_name>\n";
            }
        }
        else if (command == "getGateFanoutCount") {
            std::string instName;
            if (iss >> instName) {
                std::cout << "Gate '" << instName << "' has an immediate fanout of " 
                          << netlist.getGateFanoutCount(instName) << ".\n";
            } else {
                std::cout << "Usage: getGateFanoutCount <inst_name>\n";
            }
        }
        else if (command == "getTransitiveFaninConeGateNames") {
            std::string netName;
            if (iss >> netName) {
                auto names = netlist.getTransitiveFaninConeGateNames(netName);
                std::cout << "Net '" << netName << "' Fanin Cone contains " << names.size() << " gates:\n";
                for (const auto& name : names) std::cout << "  - " << name << "\n";
            } else {
                std::cout << "Usage: getTransitiveFaninConeGateNames <net_name>\n";
            }
        }
        else if (command == "getTransitiveFaninConeGateCount") {
            std::string netName;
            if (iss >> netName) {
                std::cout << "Net '" << netName << "' Fanin Cone contains " 
                          << netlist.getTransitiveFaninConeGateCount(netName) << " gates.\n";
            } else {
                std::cout << "Usage: getTransitiveFaninConeGateCount <net_name>\n";
            }
        }
        // --- Net-based Fanout Cone ---
        else if (command == "getTransitiveFanoutConeGateNames") {
            std::string netName;
            if (iss >> netName) {
                auto names = netlist.getTransitiveFanoutConeGateNames(netName);
                std::cout << "Net '" << netName << "' Fanout Cone contains " << names.size() << " gates:\n";
                for (const auto& name : names) std::cout << "  - " << name << "\n";
            } else {
                std::cout << "Usage: getTransitiveFanoutConeGateNames <net_name>\n";
            }
        }
        else if (command == "getTransitiveFanoutConeGateCount") {
            std::string netName;
            if (iss >> netName) {
                std::cout << "Net '" << netName << "' Fanout Cone contains " 
                          << netlist.getTransitiveFanoutConeGateCount(netName) << " gates.\n";
            } else {
                std::cout << "Usage: getTransitiveFanoutConeGateCount <net_name>\n";
            }
        }
        // --- Gate-based Fanin Cone ---
        else if (command == "getGateTransitiveFaninConeGateNames") {
            std::string instName;
            if (iss >> instName) {
                auto names = netlist.getGateTransitiveFaninConeGateNames(instName);
                std::cout << "Gate '" << instName << "' Fanin Cone contains " << names.size() << " gates:\n";
                for (const auto& name : names) std::cout << "  - " << name << "\n";
            } else {
                std::cout << "Usage: getGateTransitiveFaninConeGateNames <inst_name>\n";
            }
        }
        else if (command == "getGateTransitiveFaninConeGateCount") {
            std::string instName;
            if (iss >> instName) {
                std::cout << "Gate '" << instName << "' Fanin Cone contains " 
                          << netlist.getGateTransitiveFaninConeGateCount(instName) << " gates.\n";
            } else {
                std::cout << "Usage: getGateTransitiveFaninConeGateCount <inst_name>\n";
            }
        }
        // --- Gate-based Fanout Cone ---
        else if (command == "getGateTransitiveFanoutConeGateNames") {
            std::string instName;
            if (iss >> instName) {
                auto names = netlist.getGateTransitiveFanoutConeGateNames(instName);
                std::cout << "Gate '" << instName << "' Fanout Cone contains " << names.size() << " gates:\n";
                for (const auto& name : names) std::cout << "  - " << name << "\n";
            } else {
                std::cout << "Usage: getGateTransitiveFanoutConeGateNames <inst_name>\n";
            }
        }
        else if (command == "getGateTransitiveFanoutConeGateCount") {
            std::string instName;
            if (iss >> instName) {
                std::cout << "Gate '" << instName << "' Fanout Cone contains " 
                          << netlist.getGateTransitiveFanoutConeGateCount(instName) << " gates.\n";
            } else {
                std::cout << "Usage: getGateTransitiveFanoutConeGateCount <inst_name>\n";
            }
        }
        else if (command.find("hasCombinationalPath") == 0 ||
                 command.find("findAnyCombinationalPath") == 0 ||
                 command.find("enumerateCombinationalPaths") == 0 ||
                 command.find("everyPath") == 0 ||
                 command.find("findLongestCombinationalPath") == 0) {
            
            std::string startNet, endNet;
            if (!(iss >> startNet >> endNet)) {
                std::cout << "Error: Missing startNet or endNet arguments.\n";
                continue;
            }

            // 判斷是否為終點嚴格檢查
            bool strictEndpoint = (command.find("ToEndpoint") != std::string::npos);
            
            // 判斷條件變體
            bool hasAvoid = (command.find("Avoiding") != std::string::npos || command.find("everyPathAvoids") == 0);
            bool hasThrough = (command.find("Through") != std::string::npos && command.find("everyPathAvoids") != 0);

            // 解析後方的條件節點
            std::vector<Netlist::PathNode> reqNodes, avoidNodes;
            if (hasThrough && hasAvoid) {
                parsePathArgs(iss, reqNodes, avoidNodes); // 雙條件
            } else if (hasThrough) {
                parsePathArgs(iss, reqNodes, reqNodes);   // 全塞入 req
            } else if (hasAvoid) {
                parsePathArgs(iss, avoidNodes, avoidNodes); // 全塞入 avoid
            }
            // 存在性檢查 (回傳 bool)
            if (command.find("hasCombinationalPath") == 0) {
                bool result = false;
                if (strictEndpoint && !netlist.isEndpoint(Netlist::PathNode(Netlist::PathNodeType::Net, endNet))) {
                    result = false;
                    std::cout << "Rejected: '" << endNet << "' is not a valid endpoint.\n";
                } else if (hasThrough && hasAvoid) {
                    result = strictEndpoint ? netlist.hasCombinationalPathThroughAvoidingToEndpoint(startNet, endNet, reqNodes, avoidNodes)
                                            : netlist.hasCombinationalPathThroughAvoiding(startNet, endNet, reqNodes, avoidNodes);
                } else if (hasThrough) {
                    result = strictEndpoint ? netlist.hasCombinationalPathThroughToEndpoint(startNet, endNet, reqNodes)
                                            : netlist.hasCombinationalPathThrough(startNet, endNet, reqNodes);
                } else if (hasAvoid) {
                    result = strictEndpoint ? netlist.hasCombinationalPathAvoidingToEndpoint(startNet, endNet, avoidNodes)
                                            : netlist.hasCombinationalPathAvoiding(startNet, endNet, avoidNodes);
                } else {
                    result = strictEndpoint ? netlist.hasCombinationalPathToEndpoint(startNet, endNet)
                                            : netlist.hasCombinationalPath(startNet, endNet);
                }
                std::cout << (result ? "Yes, a path exists.\n" : "No path exists satisfying the conditions.\n");
            }
            // 全局約束驗證 (回傳 bool)
            else if (command.find("everyPath") == 0) {
                bool result = false;
                if (command.find("everyPathPassesThrough") == 0) {
                    result = strictEndpoint ? netlist.everyPathPassesThroughToEndpoint(startNet, endNet, reqNodes)
                                            : netlist.everyPathPassesThrough(startNet, endNet, reqNodes);
                } else {
                    result = strictEndpoint ? netlist.everyPathAvoidsToEndpoint(startNet, endNet, avoidNodes)
                                            : netlist.everyPathAvoids(startNet, endNet, avoidNodes);
                }
                std::cout << (result ? "Verification Passed.\n" : "Verification Failed (or no path exists).\n");
            }
            // 尋找單一路徑 (回傳 Path)
            else if (command.find("findAnyCombinationalPath") == 0 || command.find("findLongestCombinationalPath") == 0) {
                bool isLongest = (command.find("findLongest") == 0);
                Netlist::CombinationalPath path;
                
                if (strictEndpoint && !netlist.isEndpoint(Netlist::PathNode(Netlist::PathNodeType::Net, endNet))) {
                    std::cout << "Rejected: '" << endNet << "' is not a valid endpoint.\n";
                } else if (hasThrough && hasAvoid) {
                    path = isLongest ? (strictEndpoint ? netlist.findLongestCombinationalPathThroughAvoidingToEndpoint(startNet, endNet, reqNodes, avoidNodes) : netlist.findLongestCombinationalPathThroughAvoiding(startNet, endNet, reqNodes, avoidNodes))
                                     : (strictEndpoint ? netlist.findAnyCombinationalPathThroughAvoidingToEndpoint(startNet, endNet, reqNodes, avoidNodes) : netlist.findAnyCombinationalPathThroughAvoiding(startNet, endNet, reqNodes, avoidNodes));
                } else if (hasThrough) {
                    path = isLongest ? (strictEndpoint ? netlist.findLongestCombinationalPathThroughToEndpoint(startNet, endNet, reqNodes) : netlist.findLongestCombinationalPathThrough(startNet, endNet, reqNodes))
                                     : (strictEndpoint ? netlist.findAnyCombinationalPathThroughToEndpoint(startNet, endNet, reqNodes) : netlist.findAnyCombinationalPathThrough(startNet, endNet, reqNodes));
                } else if (hasAvoid) {
                    path = isLongest ? (strictEndpoint ? netlist.findLongestCombinationalPathAvoidingToEndpoint(startNet, endNet, avoidNodes) : netlist.findLongestCombinationalPathAvoiding(startNet, endNet, avoidNodes))
                                     : (strictEndpoint ? netlist.findAnyCombinationalPathAvoidingToEndpoint(startNet, endNet, avoidNodes) : netlist.findAnyCombinationalPathAvoiding(startNet, endNet, avoidNodes));
                } else {
                    path = isLongest ? (strictEndpoint ? netlist.findLongestCombinationalPathToEndpoint(startNet, endNet) : netlist.findLongestCombinationalPath(startNet, endNet))
                                     : (strictEndpoint ? netlist.findAnyCombinationalPathToEndpoint(startNet, endNet) : netlist.findAnyCombinationalPath(startNet, endNet));
                }
                
                if (path.exists()) {
                    std::cout << "Path found! Logic Depth (Gates): " << path.depth() << "\n";
                } else {
                    std::cout << "No path found.\n";
                }
            }
            // 窮舉所有路徑 (回傳 vector<Path>)
            else if (command.find("enumerateCombinationalPaths") == 0) {
                std::vector<Netlist::CombinationalPath> paths;
                
                if (strictEndpoint && !netlist.isEndpoint(Netlist::PathNode(Netlist::PathNodeType::Net, endNet))) {
                    std::cout << "Rejected: '" << endNet << "' is not a valid endpoint.\n";
                } else if (hasThrough && hasAvoid) {
                    paths = strictEndpoint ? netlist.enumerateCombinationalPathsThroughAvoidingToEndpoint(startNet, endNet, reqNodes, avoidNodes)
                                           : netlist.enumerateCombinationalPathsThroughAvoiding(startNet, endNet, reqNodes, avoidNodes);
                } else if (hasThrough) {
                    paths = strictEndpoint ? netlist.enumerateCombinationalPathsThroughToEndpoint(startNet, endNet, reqNodes)
                                           : netlist.enumerateCombinationalPathsThrough(startNet, endNet, reqNodes);
                } else if (hasAvoid) {
                    paths = strictEndpoint ? netlist.enumerateCombinationalPathsAvoidingToEndpoint(startNet, endNet, avoidNodes)
                                           : netlist.enumerateCombinationalPathsAvoiding(startNet, endNet, avoidNodes);
                } else {
                    paths = strictEndpoint ? netlist.enumerateCombinationalPathsToEndpoint(startNet, endNet)
                                           : netlist.enumerateCombinationalPaths(startNet, endNet);
                }
                
                std::cout << "Total paths found: " << paths.size() << "\n";
                if (!paths.empty()) {
                    std::cout << "Max depth among found paths: " << paths.back().depth() << " (Assuming sorted, otherwise index needed)\n";
                }
            }
        }
        else if (command == "checkEquivalence") {
            std::string netA, netB;
            // 確認使用者有輸入兩個要比較的 Net 名稱
            if (iss >> netA >> netB) {
                bool isEquivalent = netlist.checkEquivalence(netA, netB);
                if (isEquivalent) {
                    std::cout << "[LEC PASSED] \033[1;32mIDENTICAL\033[0m: '" << netA << "' and '" << netB << "' have the exact same logic function.\n";
                } else {
                    std::cout << "[LEC FAILED] \033[1;31mDIFFERENT\033[0m: The logic behaviors of '" << netA << "' and '" << netB << "' do not match.\n";
                }
            } else {
                std::cout << "Usage: checkEquivalence <netA> <netB>\n";
            }
        }
        else if (command == "getTransitiveFaninConeLongestPath" || command == "getTransitiveFaninConeShortestPath" || 
                 command == "getTransitiveFanoutConeLongestPath" || command == "getTransitiveFanoutConeShortestPath" ||
                 command == "getGateTransitiveFaninConeLongestPath" || command == "getGateTransitiveFaninConeShortestPath" ||
                 command == "getGateTransitiveFanoutConeLongestPath" || command == "getGateTransitiveFanoutConeShortestPath") {
            
            std::string targetName;
            if (iss >> targetName) {
                std::pair<int, std::vector<std::string>> result;
                
                // 根據指令呼叫對應的 API
                if (command == "getTransitiveFaninConeLongestPath") result = netlist.getTransitiveFaninConeLongestPath(targetName);
                else if (command == "getTransitiveFaninConeShortestPath") result = netlist.getTransitiveFaninConeShortestPath(targetName);
                else if (command == "getTransitiveFanoutConeLongestPath") result = netlist.getTransitiveFanoutConeLongestPath(targetName);
                else if (command == "getTransitiveFanoutConeShortestPath") result = netlist.getTransitiveFanoutConeShortestPath(targetName);
                else if (command == "getGateTransitiveFaninConeLongestPath") result = netlist.getGateTransitiveFaninConeLongestPath(targetName);
                else if (command == "getGateTransitiveFaninConeShortestPath") result = netlist.getGateTransitiveFaninConeShortestPath(targetName);
                else if (command == "getGateTransitiveFanoutConeLongestPath") result = netlist.getGateTransitiveFanoutConeLongestPath(targetName);
                else if (command == "getGateTransitiveFanoutConeShortestPath") result = netlist.getGateTransitiveFanoutConeShortestPath(targetName);
                
                if (result.second.empty()) {
                    std::cout << "No combinational path found in this cone.\n";
                } else {
                    std::cout << "Path Depth: " << result.first << " gates.\n";
                    std::cout << "Path Nodes:\n";
                    for (const auto& nodeName : result.second) {
                        std::cout << "  -> " << nodeName << "\n";
                    }
                }
            } else {
                std::cout << "Usage: " << command << " <target_name>\n";
            }
        }
        else if (command == "write") {
            std::string filepath;
            if (iss >> filepath) {
                if (writer.write(filepath, netlist)) {
                    std::cout << "Successfully wrote to Verilog file: " << filepath << "\n";
                } else {
                    std::cout << "Error: Failed to write to file " << filepath << "\n";
                }
                // 接收到 write 指令後結束程式
                std::cout << "Exiting EDA Tool...\n";
                isRunning = false; 
            } else {
                std::cout << "Usage: write <filepath>\n";
            }
        } 
        else {
            std::cout << "Unknown command: '" << command << "'. Type 'help' for options.\n";
        }
    }

    return 0;
}