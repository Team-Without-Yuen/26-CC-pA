#include "include/core/Netlist.h"

#include <map>
#include <set>
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

size_t countPortBits(const std::vector<Port>& ports) {
    size_t count = 0;
    for (const Port& port : ports) {
        count += port.netIds.size();
    }
    return count;
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

// Active-object helpers keep public reports independent from tombstone slots.
bool isActiveGate(const Netlist& netlist, int gateId) {
    return netlist.isValidGateId(gateId) &&
           netlist.getGate(gateId).type != GateType::UNKNOWN;
}

bool isActiveNet(const Netlist& netlist, int netId) {
    return netlist.isValidNetId(netId) && !netlist.getNet(netId).isRemoved;
}

const std::vector<GateType>& supportedGateTypes() {
    static const std::vector<GateType> types = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
        GateType::NOT, GateType::BUF, GateType::XOR, GateType::XNOR,
        GateType::DFF
    };
    return types;
}

bool prepareBasicGateTypeFilters(const BasicQuery& query,
                                 BasicReport& report,
                                 std::set<GateType>& included,
                                 std::set<GateType>& excluded) {
    if (query.gateType != GateType::UNKNOWN && !query.gateTypeFilters.empty()) {
        report.message = "gateType and gateTypeFilters cannot both be specified";
        return false;
    }

    if (query.gateType != GateType::UNKNOWN) {
        included.insert(query.gateType);
    }
    for (GateType type : query.gateTypeFilters) {
        if (type == GateType::UNKNOWN) {
            report.message = "gateTypeFilters cannot contain UNKNOWN";
            return false;
        }
        included.insert(type);
    }
    for (GateType type : query.excludedGateTypeFilters) {
        if (type == GateType::UNKNOWN) {
            report.message = "excludedGateTypeFilters cannot contain UNKNOWN";
            return false;
        }
        excluded.insert(type);
    }

    report.gateTypeFilterApplied = !included.empty();
    report.gateTypeExclusionApplied = !excluded.empty();
    report.appliedGateTypeFilters.assign(included.begin(), included.end());
    report.appliedExcludedGateTypeFilters.assign(excluded.begin(), excluded.end());
    return true;
}

bool matchesBasicGateType(GateType type,
                          const std::set<GateType>& included,
                          const std::set<GateType>& excluded) {
    return type != GateType::UNKNOWN &&
           (included.empty() || included.count(type) != 0) &&
           excluded.count(type) == 0;
}

bool hasConsistentActiveDriver(const Netlist& netlist, const Net& net) {
    if (!isActiveGate(netlist, net.driverGateId)) {
        return false;
    }
    return netlist.getGate(net.driverGateId).outputNetId == net.id;
}

bool hasConsistentActiveLoad(const Netlist& netlist, const Net& net) {
    for (int loadGateId : net.loadGateIds) {
        if (!isActiveGate(netlist, loadGateId)) {
            continue;
        }
        const Gate& loadGate = netlist.getGate(loadGateId);
        for (int inputNetId : loadGate.inputNetIds) {
            if (inputNetId == net.id) {
                return true;
            }
        }
    }
    return false;
}

size_t getActiveGateCount(const Netlist& netlist) {
    size_t count = 0;
    for (size_t i = 0; i < netlist.getGateCount(); ++i) {
        if (isActiveGate(netlist, static_cast<int>(i))) {
            ++count;
        }
    }
    return count;
}

size_t getActiveNetCount(const Netlist& netlist) {
    size_t count = 0;
    for (size_t i = 0; i < netlist.getNetCount(); ++i) {
        if (isActiveNet(netlist, static_cast<int>(i))) {
            ++count;
        }
    }
    return count;
}

PinConnectionSummary makePinConnectionSummary(
    const Netlist& netlist,
    const std::string& pinName,
    int netId) {
    PinConnectionSummary summary;
    summary.pinName = pinName;
    summary.netId = netId;
    if (!isActiveNet(netlist, netId)) {
        return summary;
    }

    const Net& net = netlist.getNet(netId);
    summary.connected = true;
    summary.netName = net.name;
    summary.isConstant = net.isConst;
    summary.constantValue = net.constVal;
    summary.isPrimaryInput = net.isPI;
    summary.isPrimaryOutput = net.isPO;
    return summary;
}

bool classifyUnconnectedPin(const Netlist& netlist,
                            int netId,
                            UnconnectedPinReason& reason) {
    if (netId < 0) {
        reason = UnconnectedPinReason::Unconnected;
        return true;
    }
    if (!netlist.isValidNetId(netId)) {
        reason = UnconnectedPinReason::InvalidNetId;
        return true;
    }
    if (netlist.getNet(netId).isRemoved) {
        reason = UnconnectedPinReason::RemovedNet;
        return true;
    }
    return false;
}

template <typename Visitor>
void visitUnconnectedPins(const Netlist& netlist, Visitor&& visitor) {
    for (size_t gateIndex = 0; gateIndex < netlist.getGateCount(); ++gateIndex) {
        const int gateId = static_cast<int>(gateIndex);
        if (!isActiveGate(netlist, gateId)) {
            continue;
        }

        const Gate& gate = netlist.getGate(gateId);
        for (size_t pinIndex = 0; pinIndex < gate.inputNetIds.size(); ++pinIndex) {
            UnconnectedPinReason reason;
            const int netId = gate.inputNetIds[pinIndex];
            if (classifyUnconnectedPin(netlist, netId, reason)) {
                visitor(UnconnectedPinSummary{
                    gateId, static_cast<int>(pinIndex), netId,
                    PinDirection::Input, reason});
            }
        }

        UnconnectedPinReason reason;
        if (classifyUnconnectedPin(netlist, gate.outputNetId, reason)) {
            visitor(UnconnectedPinSummary{
                gateId, 0, gate.outputNetId, PinDirection::Output, reason});
        }
    }
}

std::vector<UnconnectedPinSummary> collectUnconnectedPins(
    const Netlist& netlist) {
    std::vector<UnconnectedPinSummary> records;
    visitUnconnectedPins(netlist, [&](const UnconnectedPinSummary& pin) {
        records.push_back(pin);
    });
    return records;
}

std::vector<std::string> unconnectedGateNamesFromPins(
    const Netlist& netlist,
    const std::vector<UnconnectedPinSummary>& pins) {
    std::vector<std::string> names;
    int previousGateId = -1;
    for (const UnconnectedPinSummary& pin : pins) {
        if (pin.gateId == previousGateId || !isActiveGate(netlist, pin.gateId)) {
            continue;
        }
        names.push_back(netlist.getGate(pin.gateId).instName);
        previousGateId = pin.gateId;
    }
    return names;
}

} // namespace

GateConnectionSummary Netlist::buildGateConnectionSummary(int gateId) const {
    GateConnectionSummary summary;
    if (!isActiveGate(*this, gateId)) {
        return summary;
    }

    const Gate& gate = getGate(gateId);
    summary.gateId = gateId;
    summary.gateName = gate.instName;
    summary.typeName = gateTypeToString(gate.type);
    summary.inputs.reserve(gate.inputNetIds.size());
    for (size_t i = 0; i < gate.inputNetIds.size(); ++i) {
        const std::string pinName =
            (i < gate.inputPinNames.size() && !gate.inputPinNames[i].empty())
                ? gate.inputPinNames[i]
                : "IN" + std::to_string(i + 1);
        summary.inputs.push_back(
            makePinConnectionSummary(*this, pinName, gate.inputNetIds[i]));
    }
    summary.output = makePinConnectionSummary(
        *this, gate.type == GateType::DFF ? "Q" : "OUT", gate.outputNetId);
    return summary;
}

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

// 依照 gates vector 順序列出所有未移除的 gate instance names。
std::vector<std::string> Netlist::getAllGateNames() const {
    std::vector<std::string> names;
    names.reserve(gates.size());
    for (const Gate& gate : gates) {
        if (gate.type != GateType::UNKNOWN) {
            names.push_back(gate.instName);
        }
    }
    return names;
}

// 依照 nets vector 順序列出所有未移除的 net names。
std::vector<std::string> Netlist::getAllNetNames() const {
    std::vector<std::string> names;
    names.reserve(nets.size());
    for (const Net& net : nets) {
        if (!net.isRemoved) {
            names.push_back(net.name);
        }
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
        if (isActiveNet(*this, netId)) {
            names.push_back(nets[netId].name);
        }
    }
    return names;
}

// 列出非 PI、非 constant，且沒有合法 driver gate 的 net names。
std::vector<std::string> Netlist::getUndrivenNetNames() const {
    std::vector<std::string> names;
    for (const Net& net : nets) {
        if (net.isRemoved || net.isPI || net.isConst) {
            continue;
        }
        if (!hasConsistentActiveDriver(*this, net)) {
            names.push_back(net.name);
        }
    }
    return names;
}

// 列出非 PO、非 constant，且沒有任何 load gate 的 net names。
std::vector<std::string> Netlist::getNoLoadNetNames() const {
    std::vector<std::string> names;
    for (const Net& net : nets) {
        if (net.isRemoved || net.isPO || net.isConst) {
            continue;
        }
        if (!hasConsistentActiveLoad(*this, net)) {
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
    int previousGateId = -1;
    visitUnconnectedPins(*this, [&](const UnconnectedPinSummary& pin) {
        if (pin.gateId != previousGateId) {
            names.push_back(getGate(pin.gateId).instName);
            previousGateId = pin.gateId;
        }
    });
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
    if (type == GateType::UNKNOWN) {
        return result;
    }
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
std::vector<int> Netlist::findGatesWithConstInput(
    GateType type,
    int constValue,
    int inputCount) const {
    std::vector<int> result;

    for (size_t i = 0; i < getGateCount(); ++i) {
        const Gate& gate = getGate(static_cast<int>(i));
        if (gate.type == GateType::UNKNOWN) {
            continue;
        }
        if (type != GateType::UNKNOWN && gate.type != type) {
            continue;
        }
        if (inputCount >= 0 && static_cast<int>(gate.inputNetIds.size()) != inputCount) {
            continue;
        }

        bool matched = false;
        for (int netId : gate.inputNetIds) {
            if (!isActiveNet(*this, netId)) {
                continue;
            }
            const Net& net = getNet(netId);
            if (!net.isConst) {
                continue;
            }
            if (constValue < 0 || net.constVal == constValue) {
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
std::vector<std::string> Netlist::getGateNamesWithConstInput(
    GateType type,
    int constValue,
    int inputCount) const {
    std::vector<std::string> result;
    std::vector<int> gateIds = findGatesWithConstInput(type, constValue, inputCount);

    result.reserve(gateIds.size());
    for (int id : gateIds) {
        if (isValidGateId(id)) {
            result.push_back(gates[id].instName);
        }
    }

    return result;
}

// 計算 input 接到常數 0/1 的 gate 數量；可選擇限定 gate type 與常數值。
size_t Netlist::countGatesWithConstInput(
    GateType type,
    int constValue,
    int inputCount) const {
    return findGatesWithConstInput(type, constValue, inputCount).size();
}

// 執行統一 BasicQuery；這層只負責 dispatch 到既有 Basic helper，不做 cone/path/depth。
Netlist::BasicReport Netlist::runBasicQuery(const BasicQuery& query) const {
    BasicReport report;

    switch (query.type) {
    case BasicQueryType::Summary: {
        report.ok = true;
        report.message = "Basic design summary";
        report.hasGateCount = true;
        report.hasNetCount = true;
        report.hasLogicalWireCount = true;
        report.hasPrimaryInputCount = true;
        report.hasPrimaryOutputCount = true;
        report.hasPrimaryInputBitCount = true;
        report.hasPrimaryOutputBitCount = true;
        // [Perf #4] Merged two separate gate scans into one pass.
        // Before: getActiveGateCount() + countGatesByType() each iterated gates once.
        size_t activeGates = 0;
        std::map<GateType, int> typeCounts = {
            {GateType::AND,0},{GateType::OR,0},{GateType::NAND,0},{GateType::NOR,0},
            {GateType::NOT,0},{GateType::BUF,0},{GateType::XOR,0},{GateType::XNOR,0},
            {GateType::DFF,0}
        };
        for (size_t i = 0; i < getGateCount(); ++i) {
            const Gate& gate = getGate(static_cast<int>(i));
            if (gate.type != GateType::UNKNOWN) {
                ++activeGates;
                ++typeCounts[gate.type];
            }
        }
        report.gateCount = activeGates;
        report.gateTypeCounts = typeCounts;
        report.netCount = getActiveNetCount(*this);
        report.logicalWireCount = getLogicalWireCount();
        report.primaryInputCount = getPrimaryInputs().size();
        report.primaryOutputCount = getPrimaryOutputs().size();
        report.primaryInputBitCount = countPortBits(getPrimaryInputs());
        report.primaryOutputBitCount = countPortBits(getPrimaryOutputs());
        if (query.includeNames) {
            report.gateNames = getAllGateNames();
            report.netNames = getAllNetNames();
            report.portNames = getPrimaryInputNames();
            const std::vector<std::string> outputNames = getPrimaryOutputNames();
            report.portNames.insert(report.portNames.end(), outputNames.begin(), outputNames.end());
        }
        return report;
    }

    case BasicQueryType::ListGates:
        report.ok = true;
        report.message = "List all gates";
        report.hasGateCount = true;
        report.gateCount = getActiveGateCount(*this);
        if (query.includeNames) {
            report.gateNames = getAllGateNames();
        }
        if (query.includeIds) {
            for (size_t i = 0; i < getGateCount(); ++i) {
                if (isActiveGate(*this, static_cast<int>(i))) {
                    report.gateIds.push_back(static_cast<int>(i));
                }
            }
        }
        return report;

    case BasicQueryType::ListNets:
        report.ok = true;
        report.message = "List all nets";
        report.hasNetCount = true;
        report.netCount = getActiveNetCount(*this);
        if (query.includeNames) {
            report.netNames = getAllNetNames();
        }
        if (query.includeIds) {
            for (size_t i = 0; i < getNetCount(); ++i) {
                if (isActiveNet(*this, static_cast<int>(i))) {
                    report.netIds.push_back(static_cast<int>(i));
                }
            }
        }
        return report;

    case BasicQueryType::NetClassification: {
        report.ok = true;
        report.message = "Net classification summary";
        report.hasNetCount = true;
        NetClassificationSummary& summary = report.netClassification;
        summary.valid = true;

        const auto addNet = [&](int netId,
                                std::vector<int>& ids,
                                std::vector<std::string>& names) {
            if (query.includeIds) {
                ids.push_back(netId);
            }
            if (query.includeNames) {
                names.push_back(nets[netId].name);
            }
        };

        for (size_t i = 0; i < getNetCount(); ++i) {
            const int netId = static_cast<int>(i);
            if (!isActiveNet(*this, netId)) {
                continue;
            }

            const Net& net = nets[netId];
            ++summary.activeNetCount;

            if (net.isPI) {
                ++summary.primaryInputNetCount;
                addNet(netId, summary.primaryInputNetIds,
                       summary.primaryInputNetNames);
            }
            if (net.isPO) {
                ++summary.primaryOutputNetCount;
                addNet(netId, summary.primaryOutputNetIds,
                       summary.primaryOutputNetNames);
            }
            if (net.isPI && net.isPO) {
                ++summary.primaryInputOutputNetCount;
                addNet(netId, summary.primaryInputOutputNetIds,
                       summary.primaryInputOutputNetNames);
            }
            if (net.isConst) {
                ++summary.constantNetCount;
                addNet(netId, summary.constantNetIds,
                       summary.constantNetNames);
            }
            if (!net.isPI && !net.isPO && !net.isConst) {
                ++summary.internalNetCount;
                addNet(netId, summary.internalNetIds,
                       summary.internalNetNames);
            }
        }

        report.netCount = summary.activeNetCount;
        return report;
    }

    case BasicQueryType::ListPrimaryInputs:
        report.ok = true;
        report.message = "List primary inputs";
        report.hasPrimaryInputCount = true;
        report.hasPrimaryInputBitCount = true;
        report.primaryInputCount = getPrimaryInputs().size();
        report.primaryInputBitCount = countPortBits(getPrimaryInputs());
        if (query.includeNames) {
            report.portNames = getPrimaryInputNames();
        }
        for (const Port& port : primaryInputs) {
            PortSummary summary;
            summary.name = port.name;
            summary.width = static_cast<int>(port.netIds.size());
            summary.msb = port.msb;
            summary.lsb = port.lsb;
            summary.isBus = port.isBus();
            summary.isInput = true;
            report.ports.push_back(summary);
        }
        return report;

    case BasicQueryType::ListPrimaryOutputs:
        report.ok = true;
        report.message = "List primary outputs";
        report.hasPrimaryOutputCount = true;
        report.hasPrimaryOutputBitCount = true;
        report.primaryOutputCount = getPrimaryOutputs().size();
        report.primaryOutputBitCount = countPortBits(getPrimaryOutputs());
        if (query.includeNames) {
            report.portNames = getPrimaryOutputNames();
        }
        for (const Port& port : primaryOutputs) {
            PortSummary summary;
            summary.name = port.name;
            summary.width = static_cast<int>(port.netIds.size());
            summary.msb = port.msb;
            summary.lsb = port.lsb;
            summary.isBus = port.isBus();
            summary.isOutput = true;
            report.ports.push_back(summary);
        }
        return report;

    case BasicQueryType::ListDffs: {
        report.ok = true;
        report.message = "List DFF gates";
        report.hasGateCount = true;
        // [Perf #2] Compute DFF id list once; derive names + count from it.
        // Before: getDffNames(), getGatesByType(DFF), getGateCountByType(DFF) each scanned gates.
        const std::vector<int> dffIds = getGatesByType(GateType::DFF);
        if (query.includeIds)   report.gateIds   = dffIds;
        if (query.includeNames) report.gateNames = gateIdsToNames(*this, dffIds);
        report.gateCount = dffIds.size();
        return report;
    }

    case BasicQueryType::ListCombinationalGates:
        report.ok = true;
        report.message = "List combinational gates";
        report.hasGateCount = true;
        if (query.includeIds) {
            for (size_t i = 0; i < getGateCount(); ++i) {
                if (isCombinationalGate(static_cast<int>(i))) {
                    report.gateIds.push_back(static_cast<int>(i));
                }
            }
        }
        if (query.includeNames) {
            report.gateNames = getCombinationalGateNames();
        }
        // [Perf #5] Derive count from already-computed data; avoid re-calling
        // getCombinationalGateNames() just to get its .size().
        // Before: when includeIds=false + includeNames=true, getCombinationalGateNames()
        //         was called twice (once for names, once for count).
        if (!report.gateIds.empty())
            report.gateCount = report.gateIds.size();
        else if (!report.gateNames.empty())
            report.gateCount = report.gateNames.size();
        else
            report.gateCount = getCombinationalGateNames().size();
        return report;

    case BasicQueryType::GateInfo: {
        const int gateId = getGateId(query.name);
        if (!isActiveGate(*this, gateId)) {
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
        report.gateDetailsIncluded = true;
        report.gateConnections.push_back(buildGateConnectionSummary(gateId));
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
        if (!isActiveNet(*this, netId)) {
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

    case BasicQueryType::PortInfo: {
        report.objectName = query.name;
        const Port* port = nullptr;
        for (const Port& input : primaryInputs) {
            if (input.name == query.name) {
                port = &input;
                report.isPrimaryInput = true;
                break;
            }
        }
        for (const Port& output : primaryOutputs) {
            if (output.name == query.name) {
                if (port == nullptr) {
                    port = &output;
                }
                report.isPrimaryOutput = true;
                break;
            }
        }
        if (port == nullptr) {
            report.message = "Port not found: " + query.name;
            return report;
        }

        report.ok = true;
        report.exists = true;
        report.message = "Port info";
        report.portWidth = static_cast<int>(port->netIds.size());
        report.isBus = port->isBus();
        report.typeName = report.isBus ? "BUS_PORT" : "SCALAR_PORT";
        PortSummary summary;
        summary.name = port->name;
        summary.width = report.portWidth;
        summary.msb = port->msb;
        summary.lsb = port->lsb;
        summary.isBus = report.isBus;
        summary.isInput = report.isPrimaryInput;
        summary.isOutput = report.isPrimaryOutput;
        report.ports.push_back(summary);
        if (query.includeNames) {
            report.portNames.push_back(query.name);
        }
        for (int netId : port->netIds) {
            if (!isActiveNet(*this, netId)) {
                continue;
            }
            if (query.includeIds) {
                report.netIds.push_back(netId);
            }
            if (query.includeNames) {
                report.netNames.push_back(nets[netId].name);
            }
        }
        return report;
    }

    case BasicQueryType::CountByGateType: {
        std::set<GateType> included;
        std::set<GateType> excluded;
        if (!prepareBasicGateTypeFilters(query, report, included, excluded)) {
            return report;
        }
        report.ok = true;
        report.message = "Count gates by type";
        report.hasGateCount = true;
        report.scopeGateCount = getActiveGateCount(*this);
        if (included.empty() && excluded.empty()) {
            report.gateTypeCounts = countGatesByType();
            report.gateCount = report.scopeGateCount;
        } else {
            for (GateType type : supportedGateTypes()) {
                if (matchesBasicGateType(type, included, excluded)) {
                    report.gateTypeCounts[type] = 0;
                }
            }
            for (size_t gateId = 0; gateId < getGateCount(); ++gateId) {
                const Gate& gate = getGate(static_cast<int>(gateId));
                if (matchesBasicGateType(gate.type, included, excluded)) {
                    ++report.gateTypeCounts[gate.type];
                    ++report.gateCount;
                }
            }
            if (included.size() == 1 && excluded.empty()) {
                report.typeName = gateTypeToString(*included.begin());
            }
        }
        return report;
    }

    case BasicQueryType::GatesByType: {
        std::set<GateType> included;
        std::set<GateType> excluded;
        if (!prepareBasicGateTypeFilters(query, report, included, excluded)) {
            return report;
        }
        report.ok = true;
        report.message = "List gates by type";
        report.hasGateCount = true;
        report.gateDetailsIncluded = query.includeConnectionDetails;
        report.scopeGateCount = getActiveGateCount(*this);
        if (included.size() == 1 && excluded.empty()) {
            report.typeName = gateTypeToString(*included.begin());
        }
        std::vector<int> ids;
        ids.reserve(report.scopeGateCount);
        for (size_t gateId = 0; gateId < getGateCount(); ++gateId) {
            const Gate& gate = getGate(static_cast<int>(gateId));
            if (matchesBasicGateType(gate.type, included, excluded)) {
                ids.push_back(static_cast<int>(gateId));
                ++report.gateTypeCounts[gate.type];
            }
        }
        if (query.includeIds)   report.gateIds   = ids;
        if (query.includeNames) report.gateNames = gateIdsToNames(*this, ids);
        if (query.includeConnectionDetails) {
            report.gateConnections.reserve(ids.size());
            for (int gateId : ids) {
                report.gateConnections.push_back(
                    buildGateConnectionSummary(gateId));
            }
        }
        report.gateCount = ids.size();
        return report;
    }

    case BasicQueryType::GatesWithConstantInput: {
        if (query.constValue < -1 || query.constValue > 1) {
            report.message = "GatesWithConstantInput constValue must be -1, 0, or 1";
            return report;
        }
        if (query.inputCount < -1) {
            report.message = "GatesWithConstantInput inputCount must be -1 or non-negative";
            return report;
        }
        std::set<GateType> included;
        std::set<GateType> excluded;
        if (!prepareBasicGateTypeFilters(query, report, included, excluded)) {
            return report;
        }
        report.ok = true;
        report.message = "List gates with constant input";
        report.hasGateCount = true;
        report.gateDetailsIncluded = query.includeConnectionDetails;
        // [Perf #3] Compute id list once; derive names + count from it.
        // Before: findGatesWithConstInput() invoked 3× (directly for ids,
        //         inside getGateNamesWithConstInput, inside countGatesWithConstInput).
        const std::vector<int> unfilteredConstIds = findGatesWithConstInput(
            GateType::UNKNOWN, query.constValue, query.inputCount);
        report.scopeGateCount = unfilteredConstIds.size();
        std::vector<int> constIds;
        constIds.reserve(unfilteredConstIds.size());
        for (int gateId : unfilteredConstIds) {
            const Gate& gate = getGate(gateId);
            if (matchesBasicGateType(gate.type, included, excluded)) {
                constIds.push_back(gateId);
                ++report.gateTypeCounts[gate.type];
            }
        }
        if (query.includeIds)   report.gateIds   = constIds;
        if (query.includeNames) report.gateNames = gateIdsToNames(*this, constIds);
        if (query.includeConnectionDetails) {
            report.gateConnections.reserve(constIds.size());
            for (int gateId : constIds) {
                report.gateConnections.push_back(
                    buildGateConnectionSummary(gateId));
            }
        }
        report.gateCount = constIds.size();
        if (included.size() == 1 && excluded.empty()) {
            report.typeName = gateTypeToString(*included.begin());
        }
        return report;
    }

    case BasicQueryType::StructuralIssues:
        report.ok = true;
        report.message = "Structural issue summary";
        report.undrivenNets = getUndrivenNetNames();
        report.noLoadNets = getNoLoadNetNames();
        report.floatingNets = getFloatingNetNames();
        report.hasUnconnectedPinCounts = true;
        report.unconnectedPins = collectUnconnectedPins(*this);
        report.unconnectedGates =
            unconnectedGateNamesFromPins(*this, report.unconnectedPins);
        for (const UnconnectedPinSummary& pin : report.unconnectedPins) {
            if (pin.direction == PinDirection::Input) {
                ++report.unconnectedInputPinCount;
            } else {
                ++report.unconnectedOutputPinCount;
            }
        }
        for (const std::string& netName : report.noLoadNets) {
            const int netId = getNetId(netName);
            if (isActiveNet(*this, netId) && isPrimaryInputNet(netId)) {
                report.floatingPrimaryInputNets.push_back(netName);
            }
        }
        for (const std::string& netName : report.undrivenNets) {
            const int netId = getNetId(netName);
            if (isActiveNet(*this, netId) && isPrimaryOutputNet(netId)) {
                report.unconnectedPrimaryOutputNets.push_back(netName);
            }
        }
        return report;
    }

    report.message = "Unsupported BasicQueryType";
    return report;
}
