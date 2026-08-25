#pragma once
#include <string>
#include <vector>
#include "include/core/Netlist.h"

class VerilogReader {
public:
    VerilogReader() = default;

    // Reads the Verilog file and populates the provided Netlist object
    bool read(const std::string& filepath, Netlist& netlist);

    size_t multiDriverCount() const { return multiDriverCount_; }

private:
    size_t multiDriverCount_ = 0;

    // 從 "module top(a, b, out);" 取出 "top"。
    std::string parseModuleName(const std::string& line) const;
    // Parses the top-level module declaration (e.g., "module top(n0, n1, n2);")
    std::vector<std::string> parseTopModule(const std::string& line);
    // Parses input port declarations 
    void parseInputDeclaration(const std::string& line, Netlist& netlist);
    // Parses output port declarations
    void parseOutputDeclaration(const std::string& line, Netlist& netlist);

    // Parses logic gate or DFF instantiations (e.g., "and U1 (out, in1, in2);")
    void parseGateInstance(const std::string& line, Netlist& netlist);
    // Converts a string representation of a gate (e.g., "and") to the GateType enum
    GateType stringToGateType(const std::string& typeStr) const;

    // Removes leading and trailing whitespaces, tabs, and newline characters from a string
    std::string trim(const std::string& str) const;

    // Splits a string into multiple substrings based on a specified delimiter (e.g., ',')
    std::vector<std::string> split(const std::string& str, char delimiter) const;
};