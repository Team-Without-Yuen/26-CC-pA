#include "include/core/Netlist.h"

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// 尋找指定 PI/PO port；找不到時回傳 nullptr。
const Port* findPortByName(const std::vector<Port>& inputs,
                           const std::vector<Port>& outputs,
                           const std::string& portName) {
    for (const Port& port : inputs) {
        if (port.name == portName) {
            return &port;
        }
    }
    for (const Port& port : outputs) {
        if (port.name == portName) {
            return &port;
        }
    }
    return nullptr;
}

// 將 gate ID 陣列轉成 gate instance name 陣列；無效 ID 會被略過。
std::vector<std::string> gateIdsToNames(const Netlist& netlist,
                                        const std::vector<int>& gateIds) {
    std::vector<std::string> names;
    names.reserve(gateIds.size());
    for (int gateId : gateIds) {
        if (netlist.isValidGateId(gateId)) {
            names.push_back(netlist.getGate(gateId).instName);
        }
    }
    return names;
}

} // namespace

// 判斷 gate ID 是否落在 gates vector 的合法範圍內。
bool Netlist::isValidGateId(int gateId) const {
    return gateId >= 0 && gateId < static_cast<int>(gates.size());
}

// 判斷 net ID 是否落在 nets vector 的合法範圍內。
bool Netlist::isValidNetId(int netId) const {
    return netId >= 0 && netId < static_cast<int>(nets.size());
}

// 判斷指定 gate 是否為 DFF；gateId 無效時回傳 false。
bool Netlist::isDffGate(int gateId) const {
    return isValidGateId(gateId) && gates[gateId].type == GateType::DFF;
}

// 判斷指定 gate 是否為 combinational gate；DFF/UNKNOWN/無效 ID 回傳 false。
bool Netlist::isCombinationalGate(int gateId) const {
    return isValidGateId(gateId) &&
           gates[gateId].type != GateType::DFF &&
           gates[gateId].type != GateType::UNKNOWN;
}

// 判斷指定 net 是否為 Primary Input；netId 無效時回傳 false。
bool Netlist::isPrimaryInputNet(int netId) const {
    return isValidNetId(netId) && nets[netId].isPI;
}

// 判斷指定 net 是否為 Primary Output；netId 無效時回傳 false。
bool Netlist::isPrimaryOutputNet(int netId) const {
    return isValidNetId(netId) && nets[netId].isPO;
}

// 判斷指定 net 是否為 constant net；netId 無效時回傳 false。
bool Netlist::isConstantNet(int netId) const {
    return isValidNetId(netId) && nets[netId].isConst;
}

// 依照 gates vector 順序列出所有 gate instance names。
std::vector<std::string> Netlist::getAllGateNames() const {
    std::vector<std::string> names;
    names.reserve(gates.size());
    for (const Gate& gate : gates) {
        names.push_back(gate.instName);
    }
    return names;
}

// 依照 nets vector 順序列出所有 net names。
std::vector<std::string> Netlist::getAllNetNames() const {
    std::vector<std::string> names;
    names.reserve(nets.size());
    for (const Net& net : nets) {
        names.push_back(net.name);
    }
    return names;
}

// 依照 port declaration 順序列出 primary input port names。
std::vector<std::string> Netlist::getPrimaryInputNames() const {
    std::vector<std::string> names;
    names.reserve(primaryInputs.size());
    for (const Port& port : primaryInputs) {
        names.push_back(port.name);
    }
    return names;
}

// 依照 port declaration 順序列出 primary output port names。
std::vector<std::string> Netlist::getPrimaryOutputNames() const {
    std::vector<std::string> names;
    names.reserve(primaryOutputs.size());
    for (const Port& port : primaryOutputs) {
        names.push_back(port.name);
    }
    return names;
}

// 列出所有 DFF instance names。
std::vector<std::string> Netlist::getDffNames() const {
    std::vector<std::string> names;
    for (const Gate& gate : gates) {
        if (gate.type == GateType::DFF) {
            names.push_back(gate.instName);
        }
    }
    return names;
}

// 列出所有 combinational gate instance names；不包含 DFF。
std::vector<std::string> Netlist::getCombinationalGateNames() const {
    std::vector<std::string> names;
    for (const Gate& gate : gates) {
        if (gate.type != GateType::DFF && gate.type != GateType::UNKNOWN) {
            names.push_back(gate.instName);
        }
    }
    return names;
}

// 取得指定 PI/PO port 的 bit width；找不到 port 時回傳 -1。
int Netlist::getPortWidth(const std::string& portName) const {
    const Port* port = findPortByName(primaryInputs, primaryOutputs, portName);
    if (port == nullptr) {
        return -1;
    }
    return static_cast<int>(port->netIds.size());
}

// 判斷指定 PI/PO port 是否為 bus；找不到 port 時回傳 false。
bool Netlist::isBusPort(const std::string& portName) const {
    const Port* port = findPortByName(primaryInputs, primaryOutputs, portName);
    return port != nullptr && port->isBus();
}

// 依照 port bit 順序列出指定 PI/PO port 對應的 net names；找不到時回傳空陣列。
std::vector<std::string> Netlist::getPortBitNames(const std::string& portName) const {
    std::vector<std::string> names;
    const Port* port = findPortByName(primaryInputs, primaryOutputs, portName);
    if (port == nullptr) {
        return names;
    }

    names.reserve(port->netIds.size());
    for (int netId : port->netIds) {
        if (isValidNetId(netId)) {
            names.push_back(nets[netId].name);
        }
    }
    return names;
}

// 列出非 PI、非 constant，且沒有合法 driver gate 的 net names。
std::vector<std::string> Netlist::getUndrivenNetNames() const {
    std::vector<std::string> names;
    for (const Net& net : nets) {
        if (net.isPI || net.isConst) {
            continue;
        }
        if (!isValidGateId(net.driverGateId)) {
            names.push_back(net.name);
        }
    }
    return names;
}

// 列出非 PO、非 constant，且沒有任何 load gate 的 net names。
std::vector<std::string> Netlist::getNoLoadNetNames() const {
    std::vector<std::string> names;
    for (const Net& net : nets) {
        if (net.isPO || net.isConst) {
            continue;
        }
        if (net.loadGateIds.empty()) {
            names.push_back(net.name);
        }
    }
    return names;
}

// 列出有 undriven 或 no-load 結構問題的 net names；會移除重複。
std::vector<std::string> Netlist::getFloatingNetNames() const {
    std::vector<std::string> names = getUndrivenNetNames();
    std::unordered_set<std::string> seen(names.begin(), names.end());

    for (const std::string& netName : getNoLoadNetNames()) {
        if (seen.insert(netName).second) {
            names.push_back(netName);
        }
    }
    return names;
}

// 列出 input 或 output 存在無效 / unconnected net ID 的 gate instance names。
std::vector<std::string> Netlist::getUnconnectedGateNames() const {
    std::vector<std::string> names;
    for (const Gate& gate : gates) {
        bool hasUnconnectedPin = !isValidNetId(gate.outputNetId);
        for (int inputNetId : gate.inputNetIds) {
            if (!isValidNetId(inputNetId)) {
                hasUnconnectedPin = true;
                break;
            }
        }
        if (hasUnconnectedPin) {
            names.push_back(gate.instName);
        }
    }
    return names;
}

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
        // 過濾掉 UNKNOWN，只計算其他明確定義的 GateType
        if (gate.type != GateType::UNKNOWN) {
            ++counts[gate.type];
        }
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

// Count the number of logic gates of specific types
size_t Netlist::getGateCountByType(GateType type) const {
    size_t count = getGatesByType(type).size();
    return count;
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
            if (netId < 0) continue;
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

// 找出 input 接到常數 0/1 的 gate names；可選擇限定 gate type 與常數值。
std::vector<std::string> Netlist::getGateNamesWithConstInput(GateType type, int constValue) const {
    std::vector<std::string> result;
    std::vector<int> gateIds = findGatesWithConstInput(type, constValue);

    result.reserve(gateIds.size());
    for (int id : gateIds) {
        if (isValidGateId(id)) {
            result.push_back(gates[id].instName);
        }
    }

    return result;
}

// 計算 input 接到常數 0/1 的 gate 數量；可選擇限定 gate type 與常數值。
size_t Netlist::countGatesWithConstInput(GateType type, int constValue) const {
    return findGatesWithConstInput(type, constValue).size();
}

// 執行統一 BasicQuery；這層只負責 dispatch 到既有 Basic helper，不做 cone/path/depth。
Netlist::BasicReport Netlist::runBasicQuery(const BasicQuery& query) const {
    BasicReport report;

    switch (query.type) {
    case BasicQueryType::Summary:
        report.ok = true;
        report.message = "Basic design summary";
        report.gateCount = getGateCount();
        report.netCount = getNetCount();
        report.logicalWireCount = getLogicalWireCount();
        report.primaryInputCount = getPrimaryInputs().size();
        report.primaryOutputCount = getPrimaryOutputs().size();
        report.gateTypeCounts = countGatesByType();
        if (query.includeNames) {
            report.gateNames = getAllGateNames();
            report.netNames = getAllNetNames();
            report.portNames = getPrimaryInputNames();
            const std::vector<std::string> outputNames = getPrimaryOutputNames();
            report.portNames.insert(report.portNames.end(), outputNames.begin(), outputNames.end());
        }
        return report;

    case BasicQueryType::ListGates:
        report.ok = true;
        report.message = "List all gates";
        report.gateCount = getGateCount();
        if (query.includeNames) {
            report.gateNames = getAllGateNames();
        }
        if (query.includeIds) {
            for (size_t i = 0; i < getGateCount(); ++i) {
                report.gateIds.push_back(static_cast<int>(i));
            }
        }
        return report;

    case BasicQueryType::ListNets:
        report.ok = true;
        report.message = "List all nets";
        report.netCount = getNetCount();
        if (query.includeNames) {
            report.netNames = getAllNetNames();
        }
        if (query.includeIds) {
            for (size_t i = 0; i < getNetCount(); ++i) {
                report.netIds.push_back(static_cast<int>(i));
            }
        }
        return report;

    case BasicQueryType::ListPrimaryInputs:
        report.ok = true;
        report.message = "List primary inputs";
        report.primaryInputCount = getPrimaryInputs().size();
        if (query.includeNames) {
            report.portNames = getPrimaryInputNames();
        }
        return report;

    case BasicQueryType::ListPrimaryOutputs:
        report.ok = true;
        report.message = "List primary outputs";
        report.primaryOutputCount = getPrimaryOutputs().size();
        if (query.includeNames) {
            report.portNames = getPrimaryOutputNames();
        }
        return report;

    case BasicQueryType::ListDffs:
        report.ok = true;
        report.message = "List DFF gates";
        report.gateNames = query.includeNames ? getDffNames() : std::vector<std::string>();
        report.gateIds = query.includeIds ? getGatesByType(GateType::DFF) : std::vector<int>();
        report.gateCount = getGateCountByType(GateType::DFF);
        return report;

    case BasicQueryType::ListCombinationalGates:
        report.ok = true;
        report.message = "List combinational gates";
        if (query.includeNames) {
            report.gateNames = getCombinationalGateNames();
        }
        if (query.includeIds) {
            for (size_t i = 0; i < getGateCount(); ++i) {
                if (isCombinationalGate(static_cast<int>(i))) {
                    report.gateIds.push_back(static_cast<int>(i));
                }
            }
        }
        report.gateCount = report.gateIds.empty()
            ? getCombinationalGateNames().size()
            : report.gateIds.size();
        return report;

    case BasicQueryType::GateInfo: {
        const int gateId = getGateId(query.name);
        if (!isValidGateId(gateId)) {
            report.message = "Gate not found: " + query.name;
            return report;
        }
        const Gate& gate = getGate(gateId);
        report.ok = true;
        report.exists = true;
        report.message = "Gate info";
        report.objectId = gateId;
        report.objectName = gate.instName;
        report.typeName = gateTypeToString(gate.type);
        report.isDff = isDffGate(gateId);
        report.isCombinational = isCombinationalGate(gateId);
        report.formattedInfo = getGateInfo(query.name);
        if (query.includeIds) {
            report.gateIds.push_back(gateId);
        }
        if (query.includeNames) {
            report.gateNames.push_back(gate.instName);
        }
        return report;
    }

    case BasicQueryType::NetInfo: {
        const int netId = getNetId(query.name);
        if (!isValidNetId(netId)) {
            report.message = "Net not found: " + query.name;
            return report;
        }
        const Net& net = getNet(netId);
        report.ok = true;
        report.exists = true;
        report.message = "Net info";
        report.objectId = netId;
        report.objectName = net.name;
        report.isPrimaryInput = isPrimaryInputNet(netId);
        report.isPrimaryOutput = isPrimaryOutputNet(netId);
        report.isConstant = isConstantNet(netId);
        if (report.isConstant) {
            report.typeName = "CONSTANT";
        } else if (report.isPrimaryInput && report.isPrimaryOutput) {
            report.typeName = "PRIMARY_INPUT_OUTPUT";
        } else if (report.isPrimaryInput) {
            report.typeName = "PRIMARY_INPUT";
        } else if (report.isPrimaryOutput) {
            report.typeName = "PRIMARY_OUTPUT";
        } else {
            report.typeName = "INTERNAL_NET";
        }
        if (query.includeIds) {
            report.netIds.push_back(netId);
        }
        if (query.includeNames) {
            report.netNames.push_back(net.name);
        }
        return report;
    }

    case BasicQueryType::PortInfo:
        report.objectName = query.name;
        report.portWidth = getPortWidth(query.name);
        if (report.portWidth < 0) {
            report.message = "Port not found: " + query.name;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.message = "Port info";
        report.isBus = isBusPort(query.name);
        report.typeName = report.isBus ? "BUS_PORT" : "SCALAR_PORT";
        if (query.includeNames) {
            report.portNames.push_back(query.name);
            report.netNames = getPortBitNames(query.name);
        }
        if (query.includeIds) {
            report.netIds = expandNetToBits(query.name);
        }
        return report;

    case BasicQueryType::CountByGateType:
        report.ok = true;
        report.message = "Count gates by type";
        if (query.gateType == GateType::UNKNOWN) {
            report.gateTypeCounts = countGatesByType();
            report.gateCount = getGateCount();
        } else {
            const int count = static_cast<int>(getGateCountByType(query.gateType));
            report.gateTypeCounts[query.gateType] = count;
            report.gateCount = static_cast<size_t>(count);
            report.typeName = gateTypeToString(query.gateType);
        }
        return report;

    case BasicQueryType::GatesByType:
        if (query.gateType == GateType::UNKNOWN) {
            report.message = "GatesByType requires a concrete gate type";
            return report;
        }
        report.ok = true;
        report.message = "List gates by type";
        report.typeName = gateTypeToString(query.gateType);
        if (query.includeIds) {
            report.gateIds = getGatesByType(query.gateType);
        }
        if (query.includeNames) {
            const std::vector<int> ids = getGatesByType(query.gateType);
            report.gateNames = gateIdsToNames(*this, ids);
        }
        report.gateCount = getGateCountByType(query.gateType);
        return report;

    case BasicQueryType::GatesWithConstantInput:
        report.ok = true;
        report.message = "List gates with constant input";
        if (query.includeIds) {
            report.gateIds = findGatesWithConstInput(query.gateType, query.constValue);
        }
        if (query.includeNames) {
            report.gateNames = getGateNamesWithConstInput(query.gateType, query.constValue);
        }
        report.gateCount = countGatesWithConstInput(query.gateType, query.constValue);
        if (query.gateType != GateType::UNKNOWN) {
            report.typeName = gateTypeToString(query.gateType);
        }
        return report;

    case BasicQueryType::StructuralIssues:
        report.ok = true;
        report.message = "Structural issue summary";
        report.undrivenNets = getUndrivenNetNames();
        report.noLoadNets = getNoLoadNetNames();
        report.floatingNets = getFloatingNetNames();
        report.unconnectedGates = getUnconnectedGateNames();
        return report;
    }

    report.message = "Unsupported BasicQueryType";
    return report;
}
