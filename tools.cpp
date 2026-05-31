#include <iostream>
#include <sstream>
#include <string>
#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

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