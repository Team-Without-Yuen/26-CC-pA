#include "include/core/Netlist.h"
#include <string>

// 將 GateType enum 轉成大寫字串，供報告、debug、查詢結果輸出使用
std::string gateTypeToString(GateType type) {
    switch (type) {
        case GateType::AND:  return "AND";
        case GateType::OR:   return "OR";
        case GateType::NAND: return "NAND";
        case GateType::NOR:  return "NOR";
        case GateType::NOT:  return "NOT";
        case GateType::BUF:  return "BUF";
        case GateType::XOR:  return "XOR";
        case GateType::XNOR: return "XNOR";
        case GateType::DFF:  return "DFF";
        default: return "UNKNOWN";
    }
}

// 新增或取得一條 net；常數 1'b0 / 1'b1 會標記為 constant net
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

// 新增一個 gate / dff instance，並建立 instance name 到 gate ID 的查表
// Adds a new logic gate or sequential instance
int Netlist::addGate(const std::string& name, GateType type) {
    int newId = gates.size();
    gates.emplace_back(newId, name, type);
    gateNameToId[name] = newId;
    return newId;
}

// 新增 primary input；若是 bus，會展開成每一個 bit net，例如 n0[7] ... n0[0]
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

// 新增 primary output；若是 bus，會展開成每一個 bit net，例如 n3[23] ... n3[0]
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

// 將一條 net 接到指定 gate 的 input pin，並同步更新 net 的 load gate 清單
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

// 將一條 net 接到指定 gate 的 output pin，並記錄該 net 的 driver gate
// Connects a net to a gate's output pin
void Netlist::connectGateOutput(int gateId, int netId) {
    // Update the Gate: Set its output net ID
    gates[gateId].outputNetId = netId;

    // Update the Net: Set this gate as its driver
    nets[netId].driverGateId = gateId;
}

// 依 gate instance name 查 gate ID；找不到時回傳 -1
int Netlist::getGateId(const std::string& gateInstName) const {
    auto it = gateNameToId.find(gateInstName);
    if (it == gateNameToId.end()) {
        return -1;
    }
    return it->second;
}

// 依 net name 查 net ID；找不到時回傳 -1
int Netlist::getNetId(const std::string& netName) const {
    auto it = netNameToId.find(netName);
    if (it == netNameToId.end()) {
        return -1;
    }
    return it->second;
}

// 依 gate instance name 取得 Gate 指標；找不到時回傳 nullptr
const Gate* Netlist::findGate(const std::string& gateInstName) const {
    int id = getGateId(gateInstName);
    if (id < 0) {
        return nullptr;
    }
    return &gates[id];
}

// 依 net name 取得 Net 指標；找不到時回傳 nullptr
const Net* Netlist::findNet(const std::string& netName) const {
    int id = getNetId(netName);
    if (id < 0) {
        return nullptr;
    }
    return &nets[id];
}

// 計算指定 wire / bus 被多少個 gate input pins 直接使用
// Calculate how many gate input pins this wire is connected to.
int Netlist::getWireLoadCount(const std::string& wireName) const {
    // Case A: A single-bit wire, or a specific bit is selected (e.g., "clk" or "n32[31]")
    auto it = netNameToId.find(wireName);
    if (it != netNameToId.end()) {
        return nets[it->second].loadGateIds.size();
    }

    // Case B: A multi-bit bus (e.g., "n32", which includes "n32[31]", "n32[30]").
    int totalLoads = 0;
    bool isBusFound = false;
    std::string busPrefix = wireName + "[";

    for (const auto& pair : netNameToId) {
        // If the wire name starts with "n32["
        if (pair.first.find(busPrefix) == 0) {
            totalLoads += nets[pair.second].loadGateIds.size();
            isBusFound = true;
        }
    }

    if (isBusFound) {
        return totalLoads;
    }

    // If the wire not found, return -1 to indicate an error.
    return -1;
}

// 計算指定 gate output net 直接驅動多少個 gate input pins
// Calculate how many gate input pins are connected to this gate's output
int Netlist::getGateFanout(const std::string& gateInstName) const {
    // Find the ID of this gate.
    auto it = gateNameToId.find(gateInstName);
    if (it == gateNameToId.end()) {
        return -1; // Return -1 if the gate not found
    }

    const Gate& gate = gates[it->second];

    // Check whether this gate has an output net. (If outputNetId is -1, the output is unconnected.)
    if (gate.outputNetId == -1) {
        return 0; 
    }

    // Return the number of gates connected to this wire.
    const Net& outNet = nets[gate.outputNetId];
    return outNet.loadGateIds.size();
}
