#include "include/core/Netlist.h"
#include <string>
#include <algorithm>
#include <sstream>

void Netlist::setNetConst(int netId, bool isConst, int val) {
    if (netId >= 0 && netId < (int)nets.size()) {
        nets[netId].isConst = isConst;
        nets[netId].constVal = val;
        markDirty();
    }
}

// 將 GateType enum 轉成大寫字串，供報告、debug、查詢結果輸出使用
std::string Netlist::gateTypeToString(GateType type) const {
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

GateType Netlist::stringToGateType(std::string str) const {
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);
    if (str == "AND") return GateType::AND;
    if (str == "OR") return GateType::OR;
    if (str == "NAND") return GateType::NAND;
    if (str == "NOR") return GateType::NOR;
    if (str == "NOT") return GateType::NOT;
    if (str == "BUF") return GateType::BUF;
    if (str == "XOR") return GateType::XOR;
    if (str == "XNOR") return GateType::XNOR;
    if (str == "DFF") return GateType::DFF;
    
    return GateType::UNKNOWN;
}

// 新增或取得一條 net；常數 1'b0 / 1'b1 會標記為 constant net
// Adds a new physical net (wire) to the netlist
int Netlist::addNet(const std::string& name) {
    // Determine the net is a constant
    bool isConstant = (name == "1'b0" || name == "1'b1");

    // Check if the net already exists. Constant nets must be deduplicated by
    // name exactly like every other net: callers such as TechMapper's rule
    // application look the constant up with getNetId(name) first and only
    // call addNet(name) as a fallback when it is missing. Previously constant
    // nets were never registered in netNameToId, so that lookup always missed
    // and every rule application that needed a tied-off constant leaf created
    // a brand-new "1'b0"/"1'b1" net. Those leaked nets can never be swept by
    // removeUnusedNets() (it explicitly protects isConst nets), so nets.size()
    // grew without bound on any workload that repeatedly technology-maps a
    // large design, which in turn slows down every O(net count) pass.
    auto it = netNameToId.find(name);
    if (it != netNameToId.end()) {
        int existingId = it->second;
        if (existingId >= 0 && existingId < (int)nets.size() && nets[existingId].isRemoved) {
            nets[existingId] = Net(existingId, name);
            if (isConstant) {
                nets[existingId].isConst = true;
                nets[existingId].constVal = (name == "1'b1") ? 1 : 0;
            }
            markDirty();
        }
        return existingId;
    }

    // Create a new net and assign it a unique ID based on the vector size
    int newId = nets.size();
    nets.emplace_back(newId, name);
    if (isConstant) {
        // If it is a constant, mark it
        nets[newId].isConst = true;
        nets[newId].constVal = (name == "1'b1") ? 1 : 0;
    }
    netNameToId[name] = newId;

    markDirty();
    return newId;
}

// 新增一個 gate / dff instance，並建立 instance name 到 gate ID 的查表
// Adds a new logic gate or sequential instance
int Netlist::addGate(const std::string& name, GateType type) {
    int newId = gates.size();
    gates.emplace_back(newId, name, type);
    gateNameToId[name] = newId;
    markDirty();
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
    markDirty();
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
    markDirty();
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
    // 只有當 netId 是有效的 ID 時，才去更新這條線的 load 清單
    if (netId >= 0 && netId < (int)nets.size()) {
        nets[netId].loadGateIds.push_back(gateId);
    }
    markDirty();
}

// 將一條 net 接到指定 gate 的 output pin，並記錄該 net 的 driver gate
// Connects a net to a gate's output pin
void Netlist::connectGateOutput(int gateId, int netId) {
    // Update the Gate: Set its output net ID
    gates[gateId].outputNetId = netId;

    // 只有當 netId 是有效的 ID 時，才將這個 Gate 註冊為該線的 driver
    if (netId >= 0 && netId < (int)nets.size()) {
        nets[netId].driverGateId = gateId;
    }
    markDirty();
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

size_t Netlist::getLogicalWireCount() const {
    std::unordered_set<std::string> uniqueWireNames;
    
    for (const auto& net : nets) {
        if (net.isRemoved) {
            continue;
        }
        std::string baseName = net.name;
        
        // 尋找陣列/匯流排後綴的起始位置，例如 "data[3]" 找到 '['
        size_t pos = baseName.find_last_of('[');
        
        // 確保是合法的後綴且以 ']' 結尾
        if (pos != std::string::npos && baseName.back() == ']') {
            baseName = baseName.substr(0, pos);
        }
        
        // 利用 set 的特性剔除重複的 baseName
        uniqueWireNames.insert(baseName);
    }
    
    return uniqueWireNames.size();
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

// 展開 Bus 或單一位元線路 (依照 index 小到大排序)
std::vector<int> Netlist::expandNetToBits(const std::string& name) const {
    std::vector<int> bits;
    
    // 如果它是單一位元 (如 "clk" 或 "n32[5]")
    auto it = netNameToId.find(name);
    if (it != netNameToId.end()) {
        bits.push_back(it->second);
        return bits;
    }

    // 如果它是多位寬 Bus (如 "n32")，找出所有 "n32[" 開頭的線
    std::string prefix = name + "[";
    std::vector<std::pair<int, int>> bit_indices; // <index, net_id>
    
    for(const auto& pair : netNameToId) {
        if (pair.first.find(prefix) == 0) {
            size_t start = prefix.size();
            size_t end = pair.first.find(']', start);
            if(end != std::string::npos) {
                int idx = std::stoi(pair.first.substr(start, end - start));
                bit_indices.push_back({idx, pair.second});
            }
        }
    }
    
    // 確保對齊順序 (例如 bit 0 對 bit 0)
    std::sort(bit_indices.begin(), bit_indices.end()); 
    for(const auto& p : bit_indices) bits.push_back(p.second);
    
    return bits;
}

std::string Netlist::getGateInfo(const std::string& instName) const {
    const Gate* gate = findGate(instName);
    if (!gate) {
        return "Error: Gate instance '" + instName + "' not found.";
    }

    std::ostringstream oss;
    oss << "Gate: " << gate->instName << "\n";
    oss << "Type: " << gateTypeToString(gate->type) << "\n";
    
    // --- 處理 Inputs ---
    oss << "Inputs:\n";
    if (gate->inputNetIds.empty()) {
        oss << "  (None)\n";
    } else {
        for (size_t i = 0; i < gate->inputNetIds.size(); ++i) {
            int netId = gate->inputNetIds[i];
            
            // 嘗試取得腳位名稱 (支援 DFF 特殊腳位名稱，否則預設為 IN1, IN2...)
            std::string pinName = (i < gate->inputPinNames.size()) ? 
                                  gate->inputPinNames[i] : 
                                  "IN" + std::to_string(i + 1);

            oss << "  - " << pinName << " connected to net ";
            
            if (netId != -1) {
                const Net& net = nets[netId];
                oss << "'" << net.name << "'";
                // 附加額外屬性標示
                if (net.isConst) oss << " (Constant)";
                if (net.isPI)    oss << " (Primary Input)";
            } else {
                oss << "(Unconnected)";
            }
            oss << "\n";
        }
    }

    // --- 處理 Output ---
    oss << "Outputs:\n";
    if (gate->outputNetId != -1) {
        const Net& net = nets[gate->outputNetId];
        oss << "  - OUT connected to net '" << net.name << "'";
        if (net.isPO) oss << " (Primary Output)";
        oss << "\n";
    } else {
        oss << "  - OUT (Unconnected)\n";
    }

    return oss.str();
}
