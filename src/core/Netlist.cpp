#include "include/core/Netlist.h"
#include <string>

// Adds a new physical net (wire) to the netlist
int Netlist::addNet(const std::string& name) {
    // Determine the net is a constant
    bool isConstant = (name == "1'b0" || name == "1'b1");

    // Check if the net already exists and not a constant
    if (!isConstant && netNameToId.find(name) != netNameToId.end()) {
        return netNameToId[name];
    }
    
    // Create a new net and assign it a unique ID based on the vector size
    int newId = nets.size();
    nets.emplace_back(newId, name);
    if (isConstant) {
        // If it is a constant, mark it
        nets[newId].isConst = true;
    } else {
        netNameToId[name] = newId;
    }

    return newId;
}

// Adds a new logic gate or sequential instance
int Netlist::addGate(const std::string& name, GateType type) {
    int newId = gates.size();
    gates.emplace_back(newId, name, type);
    gateNameToId[name] = newId;
    return newId;
}

// Adds a Primary Input (supports both single-bit and bus)
void Netlist::addPrimaryInput(const std::string& portName, int msb, int lsb) {
    Port newPort(portName, msb, lsb);

    if (newPort.isBus()) {
        // Handle multi-bit bus (e.g., [31:0] or [0:31])
        int step = (msb > lsb) ? -1 : 1;
        
        for (int i = msb; i != lsb + step; i += step) {
            // Generate individual wire names like "data[31]"
            std::string bitName = portName + "[" + std::to_string(i) + "]";
            
            int netId = addNet(bitName); // Create the physical wire
            nets[netId].isPI = true;     // Mark it as a Primary Input
            newPort.netIds.push_back(netId);
        }
    } else {
        // Handle single-bit wire (e.g., "clk")
        int netId = addNet(portName);
        nets[netId].isPI = true;
        newPort.netIds.push_back(netId);
    }

    // Store the port declaration
    primaryInputs.push_back(newPort);
}

// Adds a Primary Output (supports both single-bit and bus)
void Netlist::addPrimaryOutput(const std::string& portName, int msb, int lsb) {
    Port newPort(portName, msb, lsb);

    if (newPort.isBus()) {
        int step = (msb > lsb) ? -1 : 1;
        
        for (int i = msb; i != lsb + step; i += step) {
            std::string bitName = portName + "[" + std::to_string(i) + "]";
            
            int netId = addNet(bitName);
            nets[netId].isPO = true;     // Mark it as a Primary Output
            newPort.netIds.push_back(netId);
        }
    } else {
        int netId = addNet(portName);
        nets[netId].isPO = true;
        newPort.netIds.push_back(netId);
    }

    // Store the port declaration
    primaryOutputs.push_back(newPort);
}

// Connects a net to a gate's input pin
void Netlist::connectGateInput(int gateId, int netId, const std::string& pinName) {
    // Update the Gate: Store the input net ID
    gates[gateId].inputNetIds.push_back(netId);
    
    // If a pin name is provided (e.g., "D" or "CK" for DFF), store it
    if (!pinName.empty()) {
        gates[gateId].inputPinNames.push_back(pinName);
    }
    // Update the Net: Add this gate as one of its loads (receivers)
    nets[netId].loadGateIds.push_back(gateId);
}

// Connects a net to a gate's output pin
void Netlist::connectGateOutput(int gateId, int netId) {
    // Update the Gate: Set its output net ID
    gates[gateId].outputNetId = netId;

    // Update the Net: Set this gate as its driver
    nets[netId].driverGateId = gateId;
}

