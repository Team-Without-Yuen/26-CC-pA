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