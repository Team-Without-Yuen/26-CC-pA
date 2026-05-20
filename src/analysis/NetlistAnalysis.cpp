#include "include/core/Netlist.h"

// 統計每一種 gate type 的數量，供「gate count breakdown」類 prompt 使用
std::map<GateType, int> Netlist::countGatesByType() const {
    std::map<GateType, int> counts;
    counts[GateType::AND] = 0;
    counts[GateType::OR] = 0;
    counts[GateType::NOT] = 0;
    counts[GateType::NAND] = 0;
    counts[GateType::NOR] = 0;
    counts[GateType::XOR] = 0;
    counts[GateType::XNOR] = 0;
    counts[GateType::BUF] = 0;
    counts[GateType::DFF] = 0;

    for (size_t i = 0; i < getGateCount(); ++i) {
        const Gate& gate = getGate(static_cast<int>(i));
        ++counts[gate.type];
    }
    return counts;
}

// 回傳指定 gate type 的所有 gate ID，供「list all XOR gates」等 prompt 使用
std::vector<int> Netlist::getGatesByType(GateType type) const {
    std::vector<int> result;
    for (size_t i = 0; i < getGateCount(); ++i) {
        const Gate& gate = getGate(static_cast<int>(i));
        if (gate.type == type) {
            result.push_back(gate.id);
        }
    }
    return result;
}

// 找出 input 接到常數 0/1 的 gates；可選擇限定 gate type 與常數值
std::vector<int> Netlist::findGatesWithConstInput(GateType type, int constValue) const {
    std::vector<int> result;
    const std::string wantedConst =
        (constValue == 0) ? "1'b0" :
        (constValue == 1) ? "1'b1" : "";

    for (size_t i = 0; i < getGateCount(); ++i) {
        const Gate& gate = getGate(static_cast<int>(i));
        if (type != GateType::UNKNOWN && gate.type != type) {
            continue;
        }

        bool matched = false;
        for (int netId : gate.inputNetIds) {
            const Net& net = getNet(netId);
            if (!net.isConst) {
                continue;
            }
            if (wantedConst.empty() || net.name == wantedConst) {
                matched = true;
                break;
            }
        }
        if (matched) {
            result.push_back(gate.id);
        }
    }
    return result;
}

// 回傳直接使用指定 net 作為 input 的 gate IDs；若 net 不存在則回傳空 vector
std::vector<int> Netlist::getDirectFanoutGatesOfNet(const std::string& netName) const {
    std::vector<int> result;
    int netId = getNetId(netName);
    if (netId < 0) {
        return result;
    }

    const Net& net = getNet(netId);
    return net.loadGateIds;
}

// 回傳指定 gate output net 直接驅動的 gate IDs；若 gate 不存在或無 output 則回傳空 vector
std::vector<int> Netlist::getDirectFanoutGatesOfGate(const std::string& gateInstName) const {
    std::vector<int> result;
    const Gate* gate = findGate(gateInstName);
    if (!gate || gate->outputNetId < 0) {
        return result;
    }

    const Net& outputNet = getNet(gate->outputNetId);
    return outputNet.loadGateIds;
}

// 回傳指定 gate 的 immediate successor gates；目前定義為該 gate output net 的 direct fanout gates
std::vector<int> Netlist::getImmediateSuccessors(const std::string& gateInstName) const {
    return getDirectFanoutGatesOfGate(gateInstName);
}
