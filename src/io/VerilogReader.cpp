#include "include/io/VerilogReader.h"
#include "include/core/Netlist.h"
#include <fstream>
#include <iostream>

bool VerilogReader::read(const std::string& filepath, Netlist& netlist) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file " << filepath << std::endl;
        return false;
    }

    std::string line;
    std::vector<std::string> modulePorts; 

    // Read the file line by line
    while (std::getline(file, line)) {
        line = trim(line); // Remove leading and trailing spaces
        if (line.empty()) continue; 
        // Detect and parse the top module declaration
        if (line.find("module") == 0) {
            modulePorts = parseTopModule(line);
        }
        // Detect input declaration
        else if (line.find("input ") == 0) {
            parseInputDeclaration(line, netlist);
        }
        // Detect output declaration
        else if (line.find("output ") == 0) {
            parseOutputDeclaration(line, netlist);
        }
        else if (line.find("wire ") == 0) {
            continue; // Ignore wire declarations.
        } else {
            // Extract the first token of the line (e.g., "nand" or "dff").
            std::string firstWord = line.substr(0, line.find_first_of(" \t("));
            if (stringToGateType(firstWord) != GateType::UNKNOWN) {
                parseGateInstance(line, netlist);
            }
        }

    }

    file.close();
    return true;
}

std::vector<std::string> VerilogReader::parseTopModule(const std::string& line) {
    std::vector<std::string> ports;

    // Locate the parentheses enclosing the port list
    size_t startPos = line.find('(');
    size_t endPos = line.find(')');

    if (startPos != std::string::npos && endPos != std::string::npos && endPos > startPos) {
        // Extract the substring inside the parentheses
        std::string portsStr = line.substr(startPos + 1, endPos - startPos - 1);

        // Split the substring by commas
        std::vector<std::string> rawPorts = split(portsStr, ',');

        // Remove extra spaces from each port name and store it
        for (const auto& p : rawPorts) {
            ports.push_back(trim(p));
        }
    }
    return ports;
}

void VerilogReader::parseInputDeclaration(const std::string& line, Netlist& netlist) {
    int msb = -1, lsb = -1;
    
    // Remove "input " (6 characters)
    std::string portStr = line.substr(6); 

    // Check for bit-width (e.g., "[31:0]")
    size_t bracketStart = portStr.find('[');
    size_t bracketEnd = portStr.find(']');
    
    if (bracketStart != std::string::npos && bracketEnd != std::string::npos) {
        // Extract "31:0"
        std::string rangeStr = portStr.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
        size_t colonPos = rangeStr.find(':');
        
        if (colonPos != std::string::npos) {
            // Convert string to integer
            msb = std::stoi(rangeStr.substr(0, colonPos));
            lsb = std::stoi(rangeStr.substr(colonPos + 1));
        }
        // Keep the rest of the string after "]"
        portStr = portStr.substr(bracketEnd + 1); 
    }

    // Remove the trailing semicolon ";"
    size_t semiPos = portStr.find(';');
    if (semiPos != std::string::npos) {
        portStr = portStr.substr(0, semiPos);
    }

    // Split port names by comma and add to Netlist
    std::vector<std::string> ports = split(portStr, ',');
    for (const auto& p : ports) {
        std::string name = trim(p);
        if (!name.empty()) {
            netlist.addPrimaryInput(name, msb, lsb);
        }
    }
}

void VerilogReader::parseOutputDeclaration(const std::string& line, Netlist& netlist) {
    int msb = -1, lsb = -1;
    
    // Remove "output " (7 characters)
    std::string portStr = line.substr(7); 

    // Check for bit-width (e.g., "[31:0]")
    size_t bracketStart = portStr.find('[');
    size_t bracketEnd = portStr.find(']');
    
    if (bracketStart != std::string::npos && bracketEnd != std::string::npos) {
        std::string rangeStr = portStr.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
        size_t colonPos = rangeStr.find(':');
        
        if (colonPos != std::string::npos) {
            msb = std::stoi(rangeStr.substr(0, colonPos));
            lsb = std::stoi(rangeStr.substr(colonPos + 1));
        }
        portStr = portStr.substr(bracketEnd + 1); 
    }

    // Remove the trailing semicolon ";"
    size_t semiPos = portStr.find(';');
    if (semiPos != std::string::npos) {
        portStr = portStr.substr(0, semiPos);
    }

    // Split port names by comma and add to Netlist
    std::vector<std::string> ports = split(portStr, ',');
    for (const auto& p : ports) {
        std::string name = trim(p);
        if (!name.empty()) {
            netlist.addPrimaryOutput(name, msb, lsb);
        }
    }
}

// Convert the string to a GateType enum
GateType VerilogReader::stringToGateType(const std::string& typeStr) const {
    if (typeStr == "and")  return GateType::AND;
    if (typeStr == "or")   return GateType::OR;
    if (typeStr == "nand") return GateType::NAND;
    if (typeStr == "nor")  return GateType::NOR;
    if (typeStr == "xor")  return GateType::XOR;
    if (typeStr == "xnor") return GateType::XNOR;
    if (typeStr == "not")  return GateType::NOT;
    if (typeStr == "buf")  return GateType::BUF;
    if (typeStr == "dff")  return GateType::DFF;
    return GateType::UNKNOWN;
}

void VerilogReader::parseGateInstance(const std::string& line, Netlist& netlist) {
    // Extract the gate type and instance name
    size_t firstSpace = line.find_first_of(" \t");
    std::string typeStr = line.substr(0, firstSpace);
    GateType type = stringToGateType(typeStr);

    size_t parenStart = line.find('(');
    std::string instName = trim(line.substr(firstSpace + 1, parenStart - firstSpace - 1));

    // Extract the connection string inside the parentheses (e.g., ".D(n1), .Q(n2)").
    size_t parenEnd = line.find_last_of(')');
    std::string portsStr = line.substr(parenStart + 1, parenEnd - parenStart - 1);

    // create this gate in the netlist
    int gateId = netlist.addGate(instName, type);
    
    // Split each pin entry by commas.
    std::vector<std::string> ports = split(portsStr, ',');

    if (type == GateType::DFF) {
        // Handle DFFs (named mapping)
        for (const auto& p : ports) {
            std::string trimmedP = trim(p); 
            size_t dotPos = trimmedP.find('.');
            size_t openP = trimmedP.find('(');
            size_t closeP = trimmedP.find(')');

            if (dotPos != std::string::npos && openP != std::string::npos && closeP != std::string::npos) {
                // Extract the pin name (CK) and net name (n0).
                std::string pinName = trim(trimmedP.substr(dotPos + 1, openP - dotPos - 1));
                std::string netName = trim(trimmedP.substr(openP + 1, closeP - openP - 1));

                int netId = netlist.addNet(netName);
                if (pinName == "Q") {
                    netlist.connectGateOutput(gateId, netId);
                } else {
                    netlist.connectGateInput(gateId, netId, pinName);
                }
            }
        }
    } else {
        // Handle basic logic gates (positional mapping)
        for (size_t i = 0; i < ports.size(); ++i) {
            std::string netName = trim(ports[i]);
            int netId = netlist.addNet(netName);
            
            // The first argument is the output; the remaining arguments are inputs.
            if (i == 0) {
                netlist.connectGateOutput(gateId, netId);
            } else {
                netlist.connectGateInput(gateId, netId);
            }
        }
    }
}

// Removes leading and trailing whitespaces
std::string VerilogReader::trim(const std::string& str) const {
    const std::string whitespace = " \t\n\r";
    size_t start = str.find_first_not_of(whitespace);
    if (start == std::string::npos) return ""; // Return empty string if all spaces
    size_t end = str.find_last_not_of(whitespace);
    return str.substr(start, end - start + 1);
}

// Splits a string by a specific delimiter character
std::vector<std::string> VerilogReader::split(const std::string& str, char delimiter) const {
    std::vector<std::string> tokens;
    size_t start = 0;
    size_t end = str.find(delimiter);

    while (end != std::string::npos) {
        tokens.push_back(str.substr(start, end - start));
        start = end + 1;
        end = str.find(delimiter, start);
    }
    tokens.push_back(str.substr(start)); // Add the last token
    return tokens;
}