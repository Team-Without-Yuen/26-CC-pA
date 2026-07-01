#include "include/io/VerilogWriter.h"
#include "include/core/Netlist.h"
#include <cctype>
#include <iostream>

bool VerilogWriter::write(const std::string& filepath, const Netlist& netlist) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot create file " << filepath << std::endl;
        return false;
    }

    // Write the top module declaration and its port list
    writeTopModule(file, netlist);
    // Write the input and output declarations
    writePorts(file, netlist);
    // Write the wires and gates
    writeWires(file, netlist);
    writeGates(file, netlist);

    // Close the module and the file
    file << "\nendmodule\n"; 
    file.close();

    std::cout << "[Writer] Successfully wrote to " << filepath << "\n";
    return true;
}

void VerilogWriter::writeTopModule(std::ofstream& file, const Netlist& netlist) const {
    file << "module top(";

    std::vector<std::string> allPorts;

    // Collect all Primary Inputs (PI)
    for (const auto& pi : netlist.getPrimaryInputs()) {
        allPorts.push_back(pi.name);
    }

    // Collect all Primary Outputs (PO)
    for (const auto& po : netlist.getPrimaryOutputs()) {
        allPorts.push_back(po.name);
    }

    // Write ports with comma separation
    for (size_t i = 0; i < allPorts.size(); ++i) {
        file << allPorts[i];

        // Add a comma and space if it's not the last port
        if (i != allPorts.size() - 1) {
            file << ", ";
        }
    }

    file << ");\n";
}

void VerilogWriter::writePorts(std::ofstream& file, const Netlist& netlist) const {
    // Visit all Primary Inputs
    for (const auto& pi : netlist.getPrimaryInputs()) {
        file << "  input ";
        
        // If it is a multi-bit bus, print [msb:lsb]
        if (pi.isBus()) {
            file << "[" << pi.msb << ":" << pi.lsb << "] ";
        }
        
        file << pi.name << ";\n";
    }

    // Visit all Primary Outputs
    for (const auto& po : netlist.getPrimaryOutputs()) {
        file << "  output ";
        
        if (po.isBus()) {
            file << "[" << po.msb << ":" << po.lsb << "] ";
        }
        
        file << po.name << ";\n";
    }
}

// Print the internal net segments (wires) in the netlist
void VerilogWriter::writeWires(std::ofstream& file, const Netlist& netlist) const {
    // 收集一般純量線 (Scalar wires)
    std::vector<std::string> internalWires;
    
    // 收集並計算內部匯流排 (Internal buses) 的邊界
    struct BusBounds {
        int msb = -1;
        int lsb = 9999999;
    };
    std::unordered_map<std::string, BusBounds> internalBuses;

    for (size_t i = 0; i < netlist.getNetCount(); ++i) {
        const Net& net = netlist.getNet(i);
        
        // 如果是 PI, PO 或是常數線，不需要在這裡宣告
        if (net.isPI || net.isPO || net.isConst) {
            continue;
        }

        // 活躍狀態檢查 (Liveness Check)
        bool hasActiveDriver = false;
        bool hasActiveLoad = false;

        // 檢查 Driver：有接上，且該 Gate 還活著
        if (net.driverGateId != -1 && netlist.isValidGateId(net.driverGateId)) {
            if (netlist.getGate(net.driverGateId).type != GateType::UNKNOWN) {
                hasActiveDriver = true;
            }
        }

        // 檢查 Load：至少有一個讀取它的 Gate 還活著
        for (int loadId : net.loadGateIds) {
            if (netlist.isValidGateId(loadId) && netlist.getGate(loadId).type != GateType::UNKNOWN) {
                hasActiveLoad = true;
                break;
            }
        }

        // 死線直接拋棄
        if (!hasActiveDriver && !hasActiveLoad) {
            continue;
        }

        // 檢查是否為 Bus Bit (例如 "n5[2]")
        size_t leftBracket = net.name.find('[');
        size_t rightBracket = net.name.find(']');
        
        if (leftBracket != std::string::npos && rightBracket != std::string::npos && rightBracket > leftBracket) {
            std::string baseName = net.name.substr(0, leftBracket);
            std::string idxStr = net.name.substr(leftBracket + 1, rightBracket - leftBracket - 1);
            
            try {
                // 解析出 Index，並更新該 Bus 的 msb 與 lsb
                int idx = std::stoi(idxStr);
                internalBuses[baseName].msb = std::max(internalBuses[baseName].msb, idx);
                internalBuses[baseName].lsb = std::min(internalBuses[baseName].lsb, idx);
            } catch (...) {
                // 萬一括號內不是單純的數字(解析失敗)，退回當作一般線處理
                internalWires.push_back(net.name);
            }
            continue; // Bus Bit 已記錄，跳過下方 scalar wire 收集
        }

        // 通過所有考驗的純量線
        internalWires.push_back(net.name);
    }

    // 1. 先統一輸出 Internal Buses 宣告 (例如: wire [14:2] na;)
    for (const auto& pair : internalBuses) {
        const std::string& baseName = pair.first;
        int msb = pair.second.msb;
        int lsb = pair.second.lsb;
        
        // 防呆確認有抓到合法邊界
        if (msb >= lsb) {
            file << "  wire [" << msb << ":" << lsb << "] " << baseName << ";\n";
        }
    }

    // 2. 接著輸出一般純量線 (Print in groups of 8)
    const size_t WiresPerLine = 8;
    for (size_t i = 0; i < internalWires.size(); i += WiresPerLine) {
        file << "  wire ";
        for (size_t j = 0; j < WiresPerLine && (i + j) < internalWires.size(); ++j) {
            file << internalWires[i + j];
            if (j < WiresPerLine - 1 && (i + j) < internalWires.size() - 1) {
                file << ", ";
            }
        }
        file << ";\n";
    }
}

// Print all logic gates and DFFs
void VerilogWriter::writeGates(std::ofstream& file, const Netlist& netlist) const {
    for (size_t i = 0; i < netlist.getGateCount(); ++i) {
        const Gate& gate = netlist.getGate(i);

        if (gate.type == GateType::UNKNOWN) {
            continue;
        }

        std::string typeStr = netlist.gateTypeToString(gate.type);
        for (char& c : typeStr) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        // Print the gate type and instance name, e.g., " nand g1("
        file << "  " << typeStr << " " << gate.instName << "(";

        if (gate.type == GateType::DFF) {
            // Handle DFFs (Named Mapping: .Q(out), .D(in))
            // print Output (.Q)
            if (gate.outputNetId != -1) {
                file << ".Q(" << netlist.getNet(gate.outputNetId).name << ")";
                if (!gate.inputNetIds.empty()) file << ", ";
            }
            // print Inputs (.D, .CK, .RN)
            for (size_t j = 0; j < gate.inputNetIds.size(); ++j) {
                file << "." << gate.inputPinNames[j] << "(";
                if (gate.inputNetIds[j] >= 0) {
                    file << netlist.getNet(gate.inputNetIds[j]).name;
                }
                file << ")";
                if (j != gate.inputNetIds.size() - 1) {
                    file << ", ";
                }
            }
        } else {
            // Handle basic logic gates (Positional Mapping: out, in1, in2)
            // Output
            if (gate.outputNetId != -1) {
                file << netlist.getNet(gate.outputNetId).name;
                if (!gate.inputNetIds.empty()) file << ", ";
            }
            // Inputs
            for (size_t j = 0; j < gate.inputNetIds.size(); ++j) {
                if (gate.inputNetIds[j] >= 0) {
                    file << netlist.getNet(gate.inputNetIds[j]).name;
                }
                if (j != gate.inputNetIds.size() - 1) {
                    file << ", ";
                }
            }
        }
        // add a ) and a ; for this gate
        file << ");\n";
    }
}
