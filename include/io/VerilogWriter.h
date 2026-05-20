#pragma once
#include <string>
#include <fstream>
#include "include/core/Netlist.h"

class VerilogWriter {
public:
    VerilogWriter() = default;

    // Writes the Netlist model to a specified Verilog file
    bool write(const std::string& filepath, const Netlist& netlist);

private:
    // Writes the top-level module declaration and its port list 
    // (e.g., "module top(n0, n1, n2);")
    void writeTopModule(std::ofstream& file, const Netlist& netlist) const;

    // Writes the direction declarations for all ports
    void writePorts(std::ofstream& file, const Netlist& netlist) const;
    
    // Writes the declarations for all internal nets/wires
    void writeWires(std::ofstream& file, const Netlist& netlist) const;
    // Writes the instantiations of all logic gates
    void writeGates(std::ofstream& file, const Netlist& netlist) const;
};
