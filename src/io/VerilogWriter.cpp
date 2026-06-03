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
    // Collect wire names
    std::vector<std::string> internalWires;
    for (size_t i = 0; i < netlist.getNetCount(); ++i) {
        const Net& net = netlist.getNet(i);
        
        // If it is not a PI or PO, and not a constant, it is an internal net
        if (!net.isPI && !net.isPO && !net.isConst) {
            internalWires.push_back(net.name);
        }
    }
    // Print in groups of 8
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
