#include "include/core/Netlist.h"
#include "include/lib/cadical.hpp"
#include <algorithm>
#include <unordered_set>
#include <functional>
#include <queue>
#include <chrono>
#include <string>
#include <vector>

// 客製化 CaDiCaL 終止器：用來設定 Wall-clock Time 限制
class TimeLimitTerminator : public CaDiCaL::Terminator {
private:
    std::chrono::time_point<std::chrono::steady_clock> start_time;
    double time_limit_seconds;
    int call_counter; // 新增一個計數器

public:
    // 傳入想設定的秒數，並記錄當下時間
    TimeLimitTerminator(double limit) : time_limit_seconds(limit) {
        start_time = std::chrono::steady_clock::now();
    }

    // CaDiCaL 內部會在解題過程中頻繁呼叫這個函式
    // 如果回傳 true，CaDiCaL 就會立刻中斷並回傳 UNKNOWN (0)
    bool terminate() override {
        // 每被呼叫 1000 次，才真正去讀取一次系統時間，以減少頻繁讀取時間帶來的效能影響
        if (++call_counter < 1000) {
            return false; 
        }
        
        call_counter = 0; // 重置計數器

        // 真正檢查時間
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - start_time;
        return elapsed.count() >= time_limit_seconds; 
    }
};

// 將一個 combinational gate 的布林功能轉成 CNF clause，加入 SAT solver。
// 回傳 false 代表 gate 腳位不完整或 gate type 不支援，呼叫端應保守視為分析失敗。
static bool addGateCnfClauses(CaDiCaL::Solver& solver, const Gate& gate) {
    if (gate.outputNetId < 0) {
        return false;
    }

    const int outLit = gate.outputNetId + 1;
    const auto& in = gate.inputNetIds;

    if (gate.type == GateType::AND) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        for (int id : in) { solver.add(id + 1); solver.add(-outLit); solver.add(0); }
        for (int id : in) { solver.add(-(id + 1)); } solver.add(outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::OR) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        for (int id : in) { solver.add(-(id + 1)); solver.add(outLit); solver.add(0); }
        for (int id : in) { solver.add(id + 1); } solver.add(-outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::NAND) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        for (int id : in) { solver.add(id + 1); solver.add(outLit); solver.add(0); }
        for (int id : in) { solver.add(-(id + 1)); } solver.add(-outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::NOR) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        for (int id : in) { solver.add(-(id + 1)); solver.add(-outLit); solver.add(0); }
        for (int id : in) { solver.add(id + 1); } solver.add(outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::NOT) {
        if (in.size() != 1 || in[0] < 0) return false;
        const int inLit = in[0] + 1;
        solver.add(inLit); solver.add(outLit); solver.add(0);
        solver.add(-inLit); solver.add(-outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::BUF) {
        if (in.size() != 1 || in[0] < 0) return false;
        const int inLit = in[0] + 1;
        solver.add(-inLit); solver.add(outLit); solver.add(0);
        solver.add(inLit); solver.add(-outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::XOR) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        const int a = in[0] + 1;
        const int b = in[1] + 1;
        solver.add(-a); solver.add(-b); solver.add(-outLit); solver.add(0);
        solver.add(a); solver.add(b); solver.add(-outLit); solver.add(0);
        solver.add(a); solver.add(-b); solver.add(outLit); solver.add(0);
        solver.add(-a); solver.add(b); solver.add(outLit); solver.add(0);
        return true;
    }

    if (gate.type == GateType::XNOR) {
        if (in.size() != 2 || in[0] < 0 || in[1] < 0) return false;
        const int a = in[0] + 1;
        const int b = in[1] + 1;
        solver.add(-a); solver.add(-b); solver.add(outLit); solver.add(0);
        solver.add(a); solver.add(b); solver.add(outLit); solver.add(0);
        solver.add(a); solver.add(-b); solver.add(-outLit); solver.add(0);
        solver.add(-a); solver.add(b); solver.add(-outLit); solver.add(0);
        return true;
    }

    return false;
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

// 尋找指定 PI/PO port；找不到時回傳 nullptr。
static const Port* findPortByName(const std::vector<Port>& inputs,
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

std::vector<std::string> Netlist::getGateNamesWithConstInput(GateType type, int constValue) const {
    std::vector<std::string> result;
    // 重用底層 API 取得 Gate IDs
    std::vector<int> gateIds = findGatesWithConstInput(type, constValue);
    
    // 將 ID 轉換為 Instance Name
    result.reserve(gateIds.size());
    for (int id : gateIds) {
        result.push_back(gates[id].instName);
    }
    
    return result;
}

size_t Netlist::countGatesWithConstInput(GateType type, int constValue) const {
    return findGatesWithConstInput(type, constValue).size();
}

// 將 gate ID 陣列轉成 gate instance name 陣列；無效 ID 會被略過。
static std::vector<std::string> gateIdsToNames(const Netlist& netlist,
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

// 將 gate ID 依首次出現順序去重，並略過無效 gate ID。
static std::vector<int> uniqueValidGateIds(const Netlist& netlist,
                                           const std::vector<int>& gateIds) {
    std::vector<int> uniqueIds;
    std::unordered_set<int> seen;
    for (int gateId : gateIds) {
        if (netlist.isValidGateId(gateId) && seen.insert(gateId).second) {
            uniqueIds.push_back(gateId);
        }
    }
    return uniqueIds;
}

// 取得指定 net 的唯一 driver gate ID；bus 或多 driver 情況請使用 getNetDriverGateIds。
int Netlist::getNetDriverGateId(const std::string& netName) const {
    const int netId = getNetId(netName);
    if (!isValidNetId(netId)) {
        return -1;
    }

    const int driverGateId = nets[netId].driverGateId;
    if (!isValidGateId(driverGateId)) {
        return -1;
    }
    return driverGateId;
}

// 取得指定 net / bus 的所有直接 driver gate IDs；bus 會展開每個 bit 並去重。
std::vector<int> Netlist::getNetDriverGateIds(const std::string& netName) const {
    std::vector<int> driverIds;
    const std::vector<int> netIds = expandNetToBits(netName);
    driverIds.reserve(netIds.size());

    for (int netId : netIds) {
        if (!isValidNetId(netId)) {
            continue;
        }
        const int driverGateId = nets[netId].driverGateId;
        if (isValidGateId(driverGateId)) {
            driverIds.push_back(driverGateId);
        }
    }
    return uniqueValidGateIds(*this, driverIds);
}

// 取得指定 net / bus 的所有直接 driver gate names。
std::vector<std::string> Netlist::getNetDriverGateNames(const std::string& netName) const {
    return gateIdsToNames(*this, getNetDriverGateIds(netName));
}

// 取得指定 scalar net 的唯一 driver gate name；找不到 driver 時回傳空字串。
std::string Netlist::getNetDriverGateName(const std::string& netName) const {
    const int driverGateId = getNetDriverGateId(netName);
    if (!isValidGateId(driverGateId)) {
        return "";
    }
    return gates[driverGateId].instName;
}

// 取得指定 net / bus 直接 load 到的 gate IDs；保留 getWireLoads 的 bus 展開語意。
std::vector<int> Netlist::getNetLoadGateIds(const std::string& netName) const {
    return getWireLoads(netName);
}

// 取得指定 net / bus 直接 load 到的 gate names。
std::vector<std::string> Netlist::getNetLoadGateNames(const std::string& netName) const {
    return getWireLoadNames(netName);
}

// 取得指定 net / bus 直接 load 到的 gate 數量。
size_t Netlist::getNetLoadGateCount(const std::string& netName) const {
    return getWireLoadCount(netName);
}

// 取得指定 gate 的所有有效 input net IDs；未連接 pin 以 -1 表示時會被略過。
std::vector<int> Netlist::getGateInputNetIds(const std::string& gateInstName) const {
    std::vector<int> inputNetIds;
    const int gateId = getGateId(gateInstName);
    if (!isValidGateId(gateId)) {
        return inputNetIds;
    }

    const Gate& gate = gates[gateId];
    inputNetIds.reserve(gate.inputNetIds.size());
    for (int netId : gate.inputNetIds) {
        if (isValidNetId(netId)) {
            inputNetIds.push_back(netId);
        }
    }
    return inputNetIds;
}

// 取得指定 gate 的所有有效 input net names。
std::vector<std::string> Netlist::getGateInputNetNames(const std::string& gateInstName) const {
    std::vector<std::string> inputNetNames;
    const std::vector<int> inputNetIds = getGateInputNetIds(gateInstName);
    inputNetNames.reserve(inputNetIds.size());
    for (int netId : inputNetIds) {
        inputNetNames.push_back(nets[netId].name);
    }
    return inputNetNames;
}

// 取得指定 gate 的 output net name；gate 不存在或 output 未連接時回傳空字串。
std::string Netlist::getGateOutputNetName(const std::string& gateInstName) const {
    const int outputNetId = getGateOutputNetId(getGateId(gateInstName));
    if (!isValidNetId(outputNetId)) {
        return "";
    }
    return nets[outputNetId].name;
}

// 取得直接驅動指定 gate inputs 的上一層 gate IDs；PI/constant input 沒有 driver 會被略過。
std::vector<int> Netlist::getGateFaninGateIds(const std::string& gateInstName) const {
    std::vector<int> faninGateIds;
    const std::vector<int> inputNetIds = getGateInputNetIds(gateInstName);
    faninGateIds.reserve(inputNetIds.size());

    for (int netId : inputNetIds) {
        const int driverGateId = nets[netId].driverGateId;
        if (isValidGateId(driverGateId)) {
            faninGateIds.push_back(driverGateId);
        }
    }
    return uniqueValidGateIds(*this, faninGateIds);
}

// 取得直接驅動指定 gate inputs 的上一層 gate names。
std::vector<std::string> Netlist::getGateFaninGateNames(const std::string& gateInstName) const {
    return gateIdsToNames(*this, getGateFaninGateIds(gateInstName));
}

// 取得直接驅動指定 gate inputs 的上一層 gate 數量。
size_t Netlist::getGateFaninGateCount(const std::string& gateInstName) const {
    return getGateFaninGateIds(gateInstName).size();
}

// 判斷指定 gate 的 input 或 output 是否直接連到指定 net。
bool Netlist::isGateDirectlyConnectedToNet(const std::string& gateInstName,
                                           const std::string& netName) const {
    const int gateId = getGateId(gateInstName);
    const int netId = getNetId(netName);
    if (!isValidGateId(gateId) || !isValidNetId(netId)) {
        return false;
    }

    const Gate& gate = gates[gateId];
    if (gate.outputNetId == netId) {
        return true;
    }

    return std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), netId) !=
           gate.inputNetIds.end();
}

// 判斷指定 net 是否直接連到指定 gate；與 isGateDirectlyConnectedToNet 同語意。
bool Netlist::isNetDirectlyConnectedToGate(const std::string& netName,
                                           const std::string& gateInstName) const {
    return isGateDirectlyConnectedToNet(gateInstName, netName);
}

// 執行統一 DirectConnectivityQuery；這層只負責 dispatch 到直接連線 helper。
Netlist::DirectConnectivityReport Netlist::runDirectConnectivityQuery(
    const DirectConnectivityQuery& query) const {
    DirectConnectivityReport report;
    report.gateName = query.gateName;
    report.netName = query.netName;

    switch (query.type) {
    case DirectConnectivityQueryType::NetDriver:
        if (query.netName.empty()) {
            report.message = "NetDriver requires netName";
            return report;
        }
        report.netId = getNetId(query.netName);
        if (!isValidNetId(report.netId) && expandNetToBits(query.netName).empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.message = "Net driver";
        if (query.includeIds) {
            report.gateIds = getNetDriverGateIds(query.netName);
            if (report.gateIds.size() == 1) {
                report.gateId = report.gateIds.front();
            }
        }
        if (query.includeNames) {
            report.gateNames = getNetDriverGateNames(query.netName);
            if (report.gateNames.size() == 1) {
                report.gateName = report.gateNames.front();
            }
        }
        report.count = query.includeIds ? report.gateIds.size() : report.gateNames.size();
        return report;

    case DirectConnectivityQueryType::NetLoads:
        if (query.netName.empty()) {
            report.message = "NetLoads requires netName";
            return report;
        }
        report.netId = getNetId(query.netName);
        if (!isValidNetId(report.netId) && expandNetToBits(query.netName).empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.message = "Net loads";
        if (query.includeIds) {
            report.gateIds = getNetLoadGateIds(query.netName);
        }
        if (query.includeNames) {
            report.gateNames = getNetLoadGateNames(query.netName);
        }
        report.count = getNetLoadGateCount(query.netName);
        return report;

    case DirectConnectivityQueryType::GateInputs:
        if (query.gateName.empty()) {
            report.message = "GateInputs requires gateName";
            return report;
        }
        report.gateId = getGateId(query.gateName);
        if (!isValidGateId(report.gateId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.message = "Gate input nets";
        if (query.includeIds) {
            report.netIds = getGateInputNetIds(query.gateName);
        }
        if (query.includeNames) {
            report.netNames = getGateInputNetNames(query.gateName);
        }
        report.count = query.includeIds ? report.netIds.size() : report.netNames.size();
        return report;

    case DirectConnectivityQueryType::GateOutput:
        if (query.gateName.empty()) {
            report.message = "GateOutput requires gateName";
            return report;
        }
        report.gateId = getGateId(query.gateName);
        if (!isValidGateId(report.gateId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.message = "Gate output net";
        report.netId = getGateOutputNetId(report.gateId);
        if (isValidNetId(report.netId)) {
            report.netName = nets[report.netId].name;
            report.count = 1;
            if (query.includeIds) {
                report.netIds.push_back(report.netId);
            }
            if (query.includeNames) {
                report.netNames.push_back(report.netName);
            }
        }
        return report;

    case DirectConnectivityQueryType::GateFanin:
        if (query.gateName.empty()) {
            report.message = "GateFanin requires gateName";
            return report;
        }
        report.gateId = getGateId(query.gateName);
        if (!isValidGateId(report.gateId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.message = "Gate immediate fanin gates";
        if (query.includeIds) {
            report.gateIds = getGateFaninGateIds(query.gateName);
        }
        if (query.includeNames) {
            report.gateNames = getGateFaninGateNames(query.gateName);
        }
        report.count = getGateFaninGateCount(query.gateName);
        return report;

    case DirectConnectivityQueryType::GateFanout:
        if (query.gateName.empty()) {
            report.message = "GateFanout requires gateName";
            return report;
        }
        report.gateId = getGateId(query.gateName);
        if (!isValidGateId(report.gateId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.message = "Gate immediate fanout gates";
        if (query.includeIds) {
            report.gateIds = getGateFanout(query.gateName);
        }
        if (query.includeNames) {
            report.gateNames = getGateFanoutNames(query.gateName);
        }
        report.count = getGateFanoutCount(query.gateName);
        return report;

    case DirectConnectivityQueryType::DirectlyConnected:
        if (query.gateName.empty() || query.netName.empty()) {
            report.message = "DirectlyConnected requires gateName and netName";
            return report;
        }
        report.gateId = getGateId(query.gateName);
        report.netId = getNetId(query.netName);
        if (!isValidGateId(report.gateId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        if (!isValidNetId(report.netId)) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.connected = isGateDirectlyConnectedToNet(query.gateName, query.netName);
        report.message = report.connected ? "Gate and net are directly connected"
                                           : "Gate and net are not directly connected";
        report.count = report.connected ? 1 : 0;
        if (report.connected) {
            if (query.includeIds) {
                report.gateIds.push_back(report.gateId);
                report.netIds.push_back(report.netId);
            }
            if (query.includeNames) {
                report.gateNames.push_back(query.gateName);
                report.netNames.push_back(query.netName);
            }
        }
        return report;
    }

    report.message = "Unsupported DirectConnectivityQueryType";
    return report;
}

// 回傳指定 wire / bus 被多少個 gate input pins 直接使用
// return which gate input pins this wire is connected to.
std::vector<int> Netlist::getWireLoads(const std::string& wireName) const {
    // Case A: A single-bit wire, or a specific bit is selected (e.g., "clk" or "n32[31]")
    auto it = netNameToId.find(wireName);
    if (it != netNameToId.end()) {
        return nets[it->second].loadGateIds;
    }

    // Case B: A multi-bit bus (e.g., "n32", which includes "n32[31]", "n32[30]").
    std::vector<int> allLoadGates;
    bool isBusFound = false;
    std::string busPrefix = wireName + "[";

    for (const auto& pair : netNameToId) {
        // If the wire name starts with "n32["
        if (pair.first.find(busPrefix) == 0) {
            const std::vector<int>& loads = nets[pair.second].loadGateIds;
            
            // 把這條 bit 線的 loads 全部塞進 allLoadGates 陣列的尾端
            allLoadGates.insert(allLoadGates.end(), loads.begin(), loads.end());
            isBusFound = true;
        }
    }

    if (isBusFound) {
        // 【重要】去除重複的 Gate ID
        // 因為 Bus 的多個 bit 很有可能接到同一個模組/Gate，不去除的話 ID 會重複
        std::sort(allLoadGates.begin(), allLoadGates.end());
        auto last = std::unique(allLoadGates.begin(), allLoadGates.end());
        allLoadGates.erase(last, allLoadGates.end());

        return allLoadGates;
    }

    // If the wire not found, return an empty vector to indicate an error or no loads.
    return {};
}

std::vector<std::string> Netlist::getWireLoadNames(const std::string& wireName) const {
    std::vector<std::string> result;
    // 呼叫你原有的底層 API 取得下游 Gate IDs
    std::vector<int> loadIds = getWireLoads(wireName);
    
    // 將 ID 轉換為 Instance Name
    result.reserve(loadIds.size());
    for (int id : loadIds) {
        result.push_back(gates[id].instName);
    }
    
    return result;
}

size_t Netlist::getWireLoadCount(const std::string& wireName) const {
    // 直接計算底層 API 回傳的陣列大小
    return getWireLoads(wireName).size();
}

// 回傳指定 gate output net 直接驅動多少個 gate input pins
// return which gate input pins are connected to this gate's output
std::vector<int> Netlist::getGateFanout(const std::string& gateInstName) const {
    // 1. Find the ID of this gate.
    auto it = gateNameToId.find(gateInstName);
    if (it == gateNameToId.end()) {
        return {}; // Return an empty vector if the gate not found
    }

    const Gate& gate = gates[it->second];

    // 2. Check whether this gate has an output net. 
    // (If outputNetId is -1, the output is unconnected.)
    if (gate.outputNetId == -1) {
        return {}; // Return an empty vector if unconnected
    }

    // 3. Return the vector of gate IDs connected to this wire.
    const Net& outNet = nets[gate.outputNetId];
    return outNet.loadGateIds;
}

std::vector<std::string> Netlist::getGateFanoutNames(const std::string& gateInstName) const {
    std::vector<std::string> result;
    // 呼叫你原有的底層 API 取得 Gate IDs
    std::vector<int> fanoutIds = getGateFanout(gateInstName);
    
    // 將 ID 轉換為 Instance Name
    result.reserve(fanoutIds.size());
    for (int id : fanoutIds) {
        result.push_back(gates[id].instName);
    }
    
    return result;
}

size_t Netlist::getGateFanoutCount(const std::string& gateInstName) const {
    // 直接計算底層 API 回傳的陣列大小
    return getGateFanout(gateInstName).size();
}

// Count the number of logic gates of specific types
size_t Netlist::getGateCountByType(GateType type) const {
    size_t count = getGatesByType(type).size();
    return count;
}

// 將 cone path 回傳的 net ID 序列轉成 net name 序列。
// findLongestPathInCone / findShortestPathInCone 的 raw path 存的是 net ID，不是 gate ID。
static std::vector<std::string> coneNetPathToNames(
    const Netlist& netlist,
    const std::vector<int>& netPath) {
    std::vector<std::string> pathNames;
    pathNames.reserve(netPath.size());
    for (int netId : netPath) {
        if (netId >= 0 && netId < static_cast<int>(netlist.getNetCount())) {
            pathNames.push_back(netlist.getNet(netId).name);
        }
    }
    return pathNames;
}

// 邏輯等價性檢查 (LEC) using CaDiCaL SAT solver
bool Netlist::checkEquivalence(const std::string& nameA, const std::string& nameB) const {
    std::vector<int> netsA = expandNetToBits(nameA);
    std::vector<int> netsB = expandNetToBits(nameB);

    // 找不到線，或是兩邊 bit 數量不一樣，絕對不等價
    if (netsA.empty() || netsB.empty() || netsA.size() != netsB.size()) {
        return false; 
    }

    // 向後 BFS 展開邏輯錐 (Logic Cone)
    std::unordered_set<int> visitedNets;
    std::unordered_set<int> gatesToEncode;
    std::queue<int> q;

    for (int n : netsA) { q.push(n); visitedNets.insert(n); }
    for (int n : netsB) { if (visitedNets.insert(n).second) q.push(n); }

    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        const Net& net = nets[currNetId];
        if (net.driverGateId != -1) {
            const Gate& driverGate = gates[net.driverGateId];
            
            // DFF 的輸出視為邏輯錐的起點 (Pseudo-PI)，不穿透
            if (driverGate.type != GateType::DFF) {
                // 如果這個 Gate 還沒被加入過，就加入並展開它的 Input
                if (gatesToEncode.insert(driverGate.id).second) {
                    for (int inNetId : driverGate.inputNetIds) {
                        if (inNetId < 0) continue; 
                        if (visitedNets.insert(inNetId).second) {
                            q.push(inNetId);
                        }
                    }
                }
            }
        }
    }

    // 初始化 CaDiCaL 引擎
    CaDiCaL::Solver solver;
    // 關閉 factor 演算法
    solver.set("factor", 0); 
    // 提前宣告最大的變數 ID
    int maxSolverVar = nets.size() + netsA.size();
    solver.resize(maxSolverVar);

    // 處理常數線 (1'b0, 1'b1)
    for (int netId : visitedNets) {
        const Net& net = nets[netId];
        if (net.isConst) {
            int lit = netId + 1; // SAT Solver 的變數不能是 0，所以全部 +1
            if (net.name == "1'b0") solver.add(-lit);
            else if (net.name == "1'b1") solver.add(lit);
            solver.add(0); // 0 代表 Clause 結束
        }
    }

    // Tseitin Transformation (將 Gate 轉為 CNF)
    for (int gateId : gatesToEncode) {
        const Gate& gate = gates[gateId];
        int outLit = gate.outputNetId + 1;
        const auto& in = gate.inputNetIds;

        if (gate.type == GateType::AND) {
            for (int id : in) { solver.add(id + 1); solver.add(-outLit); solver.add(0); }
            for (int id : in) { solver.add(-(id + 1)); } solver.add(outLit); solver.add(0);
        } 
        else if (gate.type == GateType::OR) {
            for (int id : in) { solver.add(-(id + 1)); solver.add(outLit); solver.add(0); }
            for (int id : in) { solver.add(id + 1); } solver.add(-outLit); solver.add(0);
        } 
        else if (gate.type == GateType::NAND) {
            for (int id : in) { solver.add(id + 1); solver.add(outLit); solver.add(0); }
            for (int id : in) { solver.add(-(id + 1)); } solver.add(-outLit); solver.add(0);
        } 
        else if (gate.type == GateType::NOR) {
            for (int id : in) { solver.add(-(id + 1)); solver.add(-outLit); solver.add(0); }
            for (int id : in) { solver.add(id + 1); } solver.add(outLit); solver.add(0);
        } 
        else if (gate.type == GateType::NOT) {
            int inLit = in[0] + 1;
            solver.add(inLit); solver.add(outLit); solver.add(0);
            solver.add(-inLit); solver.add(-outLit); solver.add(0);
        } 
        else if (gate.type == GateType::BUF) {
            int inLit = in[0] + 1;
            solver.add(-inLit); solver.add(outLit); solver.add(0);
            solver.add(inLit); solver.add(-outLit); solver.add(0);
        } 
        else if (gate.type == GateType::XOR) { // 假設 Gate-Level XOR 為 2-input
            int a = in[0] + 1, b = in[1] + 1;
            solver.add(-a); solver.add(-b); solver.add(-outLit); solver.add(0);
            solver.add(a); solver.add(b); solver.add(-outLit); solver.add(0);
            solver.add(a); solver.add(-b); solver.add(outLit); solver.add(0);
            solver.add(-a); solver.add(b); solver.add(outLit); solver.add(0);
        } 
        else if (gate.type == GateType::XNOR) {
            int a = in[0] + 1, b = in[1] + 1;
            solver.add(-a); solver.add(-b); solver.add(outLit); solver.add(0);
            solver.add(a); solver.add(b); solver.add(outLit); solver.add(0);
            solver.add(a); solver.add(-b); solver.add(-outLit); solver.add(0);
            solver.add(-a); solver.add(b); solver.add(-outLit); solver.add(0);
        }
    }

    // 建立 Miter 電路 (比對 Diff)
    int maxVar = nets.size(); 
    std::vector<int> diffLits;

    for (size_t i = 0; i < netsA.size(); ++i) {
        int aLit = netsA[i] + 1;
        int bLit = netsB[i] + 1;
        
        // 幫每一對 bit 建立一個新的 Diff 變數 (dLit = aLit XOR bLit)
        int dLit = ++maxVar; 
        diffLits.push_back(dLit);

        solver.add(-aLit); solver.add(-bLit); solver.add(-dLit); solver.add(0);
        solver.add(aLit); solver.add(bLit); solver.add(-dLit); solver.add(0);
        solver.add(aLit); solver.add(-bLit); solver.add(dLit); solver.add(0);
        solver.add(-aLit); solver.add(bLit); solver.add(dLit); solver.add(0);
    }

    // 找出「是否有任何情況」會讓其中一個 Diff 為 1
    // (dLit0 OR dLit1 OR dLit2 ...)
    for (int dLit : diffLits) {
        solver.add(dLit);
    }
    solver.add(0);

    TimeLimitTerminator terminator(30.0); 
    solver.connect_terminator(&terminator); // 把計時器接上 Solver

    // 呼叫求解工具，查看是否存在讓 Diff 為 1 的輸入組合
    int res = solver.solve();

    solver.disconnect_terminator();

    const int SAT = 10;
    const int UNSAT = 20;
    const int UNKNOWN = 0;

    if (res == UNSAT) {
        return true;  // 找不到反例，代表永遠等價
    } else if (res == SAT) {
        return false; // 找到反例，代表不等價
    } else {
        // res == 0 的情況，通常是 solver 被手動中斷，或設定了時間/資源上限
        // 這裡保守起見回傳 false
        return false; 
    }
}

// 語意化 wrapper：目前直接重用既有 checkEquivalence()。
// 保留這個名字是為了讓高階 Function Analysis API 讀起來更直覺。
bool Netlist::areNetsEquivalent(const std::string& netA, const std::string& netB) const {
    return checkEquivalence(netA, netB);
}

// 使用 SAT 判斷一條 scalar net 是否「可能」等於指定 value。
// 做法：
// 1. 先把 netName 解析成 net ID；第一版只接受 scalar，所以 bus 會回傳 false。
// 2. 從 target net 往 fanin 做 BFS，收集需要編碼的 combinational gates。
// 3. 遇到 DFF driver 時停止，因為 DFF.Q 在組合分析裡視為 pseudo PI。
// 4. 把 cone 內常數 net 與 primitive gate 轉成 CNF。
// 5. 額外加上一個 target=value 的限制，讓 SAT solver 判斷是否存在可行輸入。
bool Netlist::canNetBeValue(const std::string& netName, int value) const {
    if (value != 0 && value != 1) {
        return false;
    }

    const std::vector<int> targetBits = expandNetToBits(netName);
    if (targetBits.size() != 1) {
        return false;
    }

    const int targetNetId = targetBits.front();
    if (!isValidNetId(targetNetId)) {
        return false;
    }

    std::unordered_set<int> visitedNets;
    std::unordered_set<int> gatesToEncode;
    std::queue<int> q;

    visitedNets.insert(targetNetId);
    q.push(targetNetId);

    while (!q.empty()) {
        const int currNetId = q.front();
        q.pop();

        if (!isValidNetId(currNetId)) {
            continue;
        }

        const Net& net = nets[currNetId];
        if (!isValidGateId(net.driverGateId)) {
            continue;
        }

        const Gate& driverGate = gates[net.driverGateId];
        if (driverGate.type == GateType::DFF) {
            continue;
        }

        if (gatesToEncode.insert(driverGate.id).second) {
            for (int inputNetId : driverGate.inputNetIds) {
                if (!isValidNetId(inputNetId)) {
                    continue;
                }
                if (visitedNets.insert(inputNetId).second) {
                    q.push(inputNetId);
                }
            }
        }
    }

    CaDiCaL::Solver solver;
    solver.set("factor", 0);
    solver.resize(static_cast<int>(nets.size()));

    for (int netId : visitedNets) {
        const Net& net = nets[netId];
        if (!net.isConst) {
            continue;
        }

        const int lit = netId + 1;
        if (net.name == "1'b0") {
            solver.add(-lit);
            solver.add(0);
        } else if (net.name == "1'b1") {
            solver.add(lit);
            solver.add(0);
        }
    }

    for (int gateId : gatesToEncode) {
        if (!isValidGateId(gateId)) {
            return false;
        }
        if (!addGateCnfClauses(solver, gates[gateId])) {
            return false;
        }
    }

    const int targetLit = targetNetId + 1;
    solver.add(value == 1 ? targetLit : -targetLit);
    solver.add(0);

    TimeLimitTerminator terminator(30.0);
    solver.connect_terminator(&terminator);
    const int result = solver.solve();
    solver.disconnect_terminator();

    const int SAT = 10;
    return result == SAT;
}

// 使用反例檢查判斷 scalar net 是否恆等於 constValue。
// 若 constValue=0，代表「不存在讓 net=1 的 assignment」。
// 若 constValue=1，代表「不存在讓 net=0 的 assignment」。
bool Netlist::isNetConstantFunction(const std::string& netName, int constValue) const {
    if (constValue != 0 && constValue != 1) {
        return false;
    }

    const std::vector<int> targetBits = expandNetToBits(netName);
    if (targetBits.size() != 1) {
        return false;
    }

    return !canNetBeValue(netName, constValue == 0 ? 1 : 0);
}

// 判斷 scalar net 是否恆為 0。
bool Netlist::isNetAlwaysZero(const std::string& netName) const {
    return isNetConstantFunction(netName, 0);
}

// 判斷 scalar net 是否恆為 1。
bool Netlist::isNetAlwaysOne(const std::string& netName) const {
    return isNetConstantFunction(netName, 1);
}

// 執行統一 FunctionQuery；這層只負責檢查參數與 dispatch 到底層 Boolean helper。
// Report 會保留主要 yes/no 結果，也會保留 canBeZero/canBeOne/status 方便 LLM 生成回答。
Netlist::FunctionReport Netlist::runFunctionQuery(const FunctionQuery& query) const {
    FunctionReport report;
    report.netNameA = query.netNameA;
    report.netNameB = query.netNameB;
    report.constValue = query.constValue;

    switch (query.type) {
    case FunctionQueryType::Equivalence:
        if (query.netNameA.empty() || query.netNameB.empty()) {
            report.message = "Equivalence requires netNameA and netNameB";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (expandNetToBits(query.netNameA).empty()) {
            report.message = "Net not found: " + query.netNameA;
            report.status = "NET_A_NOT_FOUND";
            return report;
        }
        if (expandNetToBits(query.netNameB).empty()) {
            report.message = "Net not found: " + query.netNameB;
            report.status = "NET_B_NOT_FOUND";
            return report;
        }

        report.ok = true;
        report.equivalent = areNetsEquivalent(query.netNameA, query.netNameB);
        report.exists = report.equivalent;
        report.netIdA = getNetId(query.netNameA);
        report.netIdB = getNetId(query.netNameB);
        report.message = report.equivalent ? "Functions are equivalent"
                                           : "Functions are not equivalent";
        report.status = report.equivalent ? "EQUIVALENT" : "NOT_EQUIVALENT";
        return report;

    case FunctionQueryType::CanBeValue:
        if (query.netNameA.empty() || (query.constValue != 0 && query.constValue != 1)) {
            report.message = "CanBeValue requires netNameA and constValue 0 or 1";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (expandNetToBits(query.netNameA).size() != 1) {
            report.message = "CanBeValue requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.exists = canNetBeValue(query.netNameA, query.constValue);
        report.canBeZero = query.constValue == 0 ? report.exists : canNetBeValue(query.netNameA, 0);
        report.canBeOne = query.constValue == 1 ? report.exists : canNetBeValue(query.netNameA, 1);
        report.message = report.exists ? "Target value is satisfiable"
                                       : "Target value is not satisfiable";
        report.status = report.exists ? "SATISFIABLE_VALUE" : "UNSATISFIABLE_VALUE";
        return report;

    case FunctionQueryType::ConstantFunction:
        if (query.netNameA.empty() || (query.constValue != 0 && query.constValue != 1)) {
            report.message = "ConstantFunction requires netNameA and constValue 0 or 1";
            report.status = "INVALID_ARGUMENT";
            return report;
        }
        if (expandNetToBits(query.netNameA).size() != 1) {
            report.message = "ConstantFunction requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.exists = isNetConstantFunction(query.netNameA, query.constValue);
        report.isConstant = report.exists;
        report.canBeZero = canNetBeValue(query.netNameA, 0);
        report.canBeOne = canNetBeValue(query.netNameA, 1);
        report.message = report.exists ? "Net is the requested constant function"
                                       : "Net is not the requested constant function";
        report.status = report.exists ? (query.constValue == 0 ? "ALWAYS_ZERO" : "ALWAYS_ONE")
                                      : "NOT_REQUESTED_CONSTANT";
        return report;

    case FunctionQueryType::AlwaysZero:
        if (query.netNameA.empty() || expandNetToBits(query.netNameA).size() != 1) {
            report.message = "AlwaysZero requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.constValue = 0;
        report.exists = isNetAlwaysZero(query.netNameA);
        report.isConstant = report.exists;
        report.canBeZero = canNetBeValue(query.netNameA, 0);
        report.canBeOne = canNetBeValue(query.netNameA, 1);
        report.message = report.exists ? "Net is always 0" : "Net is not always 0";
        report.status = report.exists ? "ALWAYS_ZERO" : "NOT_ALWAYS_ZERO";
        return report;

    case FunctionQueryType::AlwaysOne:
        if (query.netNameA.empty() || expandNetToBits(query.netNameA).size() != 1) {
            report.message = "AlwaysOne requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.constValue = 1;
        report.exists = isNetAlwaysOne(query.netNameA);
        report.isConstant = report.exists;
        report.canBeZero = canNetBeValue(query.netNameA, 0);
        report.canBeOne = canNetBeValue(query.netNameA, 1);
        report.message = report.exists ? "Net is always 1" : "Net is not always 1";
        report.status = report.exists ? "ALWAYS_ONE" : "NOT_ALWAYS_ONE";
        return report;

    case FunctionQueryType::TruthStatus:
        if (query.netNameA.empty() || expandNetToBits(query.netNameA).size() != 1) {
            report.message = "TruthStatus requires an existing scalar net";
            report.status = "SCALAR_NET_REQUIRED";
            return report;
        }

        report.ok = true;
        report.netIdA = getNetId(query.netNameA);
        report.canBeZero = canNetBeValue(query.netNameA, 0);
        report.canBeOne = canNetBeValue(query.netNameA, 1);
        if (report.canBeZero && !report.canBeOne) {
            report.exists = true;
            report.isConstant = true;
            report.constValue = 0;
            report.status = "ALWAYS_ZERO";
            report.message = "Net is always 0";
        } else if (!report.canBeZero && report.canBeOne) {
            report.exists = true;
            report.isConstant = true;
            report.constValue = 1;
            report.status = "ALWAYS_ONE";
            report.message = "Net is always 1";
        } else if (report.canBeZero && report.canBeOne) {
            report.exists = true;
            report.isConstant = false;
            report.constValue = -1;
            report.status = "NON_CONSTANT";
            report.message = "Net can be both 0 and 1";
        } else {
            report.exists = false;
            report.isConstant = false;
            report.constValue = -1;
            report.status = "UNSAT_OR_UNSUPPORTED";
            report.message = "Net cannot be proven satisfiable for either value";
        }
        return report;
    }

    report.message = "Unsupported FunctionQueryType";
    report.status = "UNSUPPORTED_QUERY_TYPE";
    return report;
}

//  支援 Bus 的 Transitive Fanin Cone (多源 BFS)
ConeResult Netlist::getTransitiveFaninCone(const std::string& netName) const {
    ConeResult result;
    
    // 展開 Bus，取得所有起點
    std::vector<int> startIds = expandNetToBits(netName);
    if (startIds.empty()) return result;

    result.rootNetIds = startIds;
    std::queue<int> q; // BFS

    // 將所有起點同時推入 Queue 並標記為已訪問
    for (int id : startIds) {
        q.push(id);
        result.netIds.insert(id);
    }

    // BFS 核心邏輯
    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        const Net& net = nets[currNetId];

        if (net.driverGateId >= 0) {
            const Gate& driver = gates[net.driverGateId];

            if (driver.type == GateType::DFF) continue;

            for (int i = 0; i < (int)driver.inputNetIds.size(); i++) {
                int inNetId = driver.inputNetIds[i];
                if (inNetId < 0) continue;
                result.children[currNetId].push_back(inNetId);
                
                if (result.netIds.insert(inNetId).second) { 
                    q.push(inNetId);
                }
            }
        }
    }

    return result;
}

//  支援 Bus 的 Transitive Fanout Cone (多源 BFS)
ConeResult Netlist::getTransitiveFanoutCone(const std::string& netName) const {
    ConeResult result;
    
    // 展開 Bus，取得所有起點
    std::vector<int> startIds = expandNetToBits(netName);
    if (startIds.empty()) return result;

    result.rootNetIds = startIds;
    std::queue<int> q;

    // 將所有起點同時推入 Queue
    for (int id : startIds) {
        q.push(id);
        result.netIds.insert(id);
    }

    // BFS 核心邏輯
    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        const Net& net = nets[currNetId];

        for (int i = 0; i < (int)net.loadGateIds.size(); i++) {
            int gateId = net.loadGateIds[i];
            const Gate& gate = gates[gateId];

            if (gate.type == GateType::DFF) continue;

            int outNetId = gate.outputNetId;
            if (outNetId < 0) continue;

            result.children[currNetId].push_back(outNetId);
            
            if (result.netIds.insert(outNetId).second) { 
                q.push(outNetId);
            }
        }
    }

    return result;
}

ConeResult Netlist::getGateTransitiveFaninCone(const std::string& gateName) const {
    int gateId = getGateId(gateName);
    if (gateId < 0) return ConeResult(); // 找不到該 Gate

    const Gate& gate = gates[gateId];
    
    // 如果這個 Gate 沒有輸出線 (例如輸出懸空)
    if (gate.outputNetId < 0) return ConeResult(); 

    // Gate 的 Fanin 錐，就是它「輸出線」的 Fanin 錐
    return getTransitiveFaninCone(nets[gate.outputNetId].name);
}

ConeResult Netlist::getGateTransitiveFanoutCone(const std::string& gateName) const {
    int gateId = getGateId(gateName);
    if (gateId < 0) return ConeResult();

    const Gate& gate = gates[gateId];
    if (gate.outputNetId < 0) return ConeResult();

    // Gate 的 Fanout 錐，就是它「輸出線」的 Fanout 錐
    return getTransitiveFanoutCone(nets[gate.outputNetId].name);
}

// 將 ConeResult 內的 net set 轉成排序後的 net ID 陣列，讓回傳結果穩定。
std::vector<int> Netlist::getConeNetIds(const ConeResult& cone) const {
    std::vector<int> netIds;
    netIds.reserve(cone.netIds.size());
    for (int netId : cone.netIds) {
        if (isValidNetId(netId)) {
            netIds.push_back(netId);
        }
    }
    std::sort(netIds.begin(), netIds.end());
    return netIds;
}

// 將 ConeResult 內的 net IDs 轉成 net names。
std::vector<std::string> Netlist::getConeNetNames(const ConeResult& cone) const {
    std::vector<std::string> netNames;
    const std::vector<int> netIds = getConeNetIds(cone);
    netNames.reserve(netIds.size());
    for (int netId : netIds) {
        netNames.push_back(nets[netId].name);
    }
    return netNames;
}

// 計算 ConeResult 內有效 net 數量。
size_t Netlist::getConeNetCount(const ConeResult& cone) const {
    return getConeNetIds(cone).size();
}

// 從 cone 的 net-to-net edge 反推參與 cone 的 gate IDs。
std::vector<int> Netlist::getConeGateIds(const ConeResult& cone) const {
    std::unordered_set<int> gateSet;
    for (const auto& edgeList : cone.children) {
        const int fromNetId = edgeList.first;
        if (!isValidNetId(fromNetId)) {
            continue;
        }

        for (int toNetId : edgeList.second) {
            if (!isValidNetId(toNetId)) {
                continue;
            }

            const int fromDriverId = nets[fromNetId].driverGateId;
            if (isValidGateId(fromDriverId)) {
                const Gate& gate = gates[fromDriverId];
                if (gate.type != GateType::DFF &&
                    std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), toNetId) !=
                        gate.inputNetIds.end()) {
                    gateSet.insert(fromDriverId);
                    continue;
                }
            }

            const int toDriverId = nets[toNetId].driverGateId;
            if (isValidGateId(toDriverId)) {
                const Gate& gate = gates[toDriverId];
                if (gate.type != GateType::DFF &&
                    std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), fromNetId) !=
                        gate.inputNetIds.end()) {
                    gateSet.insert(toDriverId);
                }
            }
        }
    }

    std::vector<int> gateIds(gateSet.begin(), gateSet.end());
    std::sort(gateIds.begin(), gateIds.end());
    return gateIds;
}

// 將 ConeResult 內參與 cone 的 gate IDs 轉成 gate names。
std::vector<std::string> Netlist::getConeGateNames(const ConeResult& cone) const {
    std::vector<std::string> gateNames;
    const std::vector<int> gateIds = getConeGateIds(cone);
    gateNames.reserve(gateIds.size());
    for (int gateId : gateIds) {
        gateNames.push_back(gates[gateId].instName);
    }
    return gateNames;
}

// 計算 ConeResult 內參與 cone 的 gate 數量。
size_t Netlist::getConeGateCount(const ConeResult& cone) const {
    return getConeGateIds(cone).size();
}

// --- 針對 Net 的 Fanin ---
std::vector<std::string> Netlist::getTransitiveFaninConeGateNames(const std::string& netName) const {
    return getConeGateNames(getTransitiveFaninCone(netName));
}

size_t Netlist::getTransitiveFaninConeGateCount(const std::string& netName) const {
    return getConeGateCount(getTransitiveFaninCone(netName));
}

// --- 針對 Net 的 Fanout ---
std::vector<std::string> Netlist::getTransitiveFanoutConeGateNames(const std::string& netName) const {
    return getConeGateNames(getTransitiveFanoutCone(netName));
}

size_t Netlist::getTransitiveFanoutConeGateCount(const std::string& netName) const {
    return getConeGateCount(getTransitiveFanoutCone(netName));
}

// --- 針對 Gate 的 Fanin ---
std::vector<std::string> Netlist::getGateTransitiveFaninConeGateNames(const std::string& gateName) const {
    return getConeGateNames(getGateTransitiveFaninCone(gateName));
}

size_t Netlist::getGateTransitiveFaninConeGateCount(const std::string& gateName) const {
    return getConeGateCount(getGateTransitiveFaninCone(gateName));
}

// --- 針對 Gate 的 Fanout ---
std::vector<std::string> Netlist::getGateTransitiveFanoutConeGateNames(const std::string& gateName) const {
    return getConeGateNames(getGateTransitiveFanoutCone(gateName));
}

size_t Netlist::getGateTransitiveFanoutConeGateCount(const std::string& gateName) const {
    return getConeGateCount(getGateTransitiveFanoutCone(gateName));
}

// 執行統一 ConeQuery；建立 transitive cone 並整理成高階 ConeReport。
Netlist::ConeReport Netlist::runConeQuery(const ConeQuery& query) const {
    ConeReport report;
    report.type = query.type;

    switch (query.type) {
    case ConeQueryType::NetTransitiveFanin:
        if (query.netName.empty()) {
            report.message = "NetTransitiveFanin requires netName";
            return report;
        }
        if (expandNetToBits(query.netName).empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        report.sourceName = query.netName;
        report.sourceId = getNetId(query.netName);
        report.cone = getTransitiveFaninCone(query.netName);
        report.message = "Net transitive fanin cone";
        break;

    case ConeQueryType::NetTransitiveFanout:
        if (query.netName.empty()) {
            report.message = "NetTransitiveFanout requires netName";
            return report;
        }
        if (expandNetToBits(query.netName).empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        report.sourceName = query.netName;
        report.sourceId = getNetId(query.netName);
        report.cone = getTransitiveFanoutCone(query.netName);
        report.message = "Net transitive fanout cone";
        break;

    case ConeQueryType::GateTransitiveFanin:
        if (query.gateName.empty()) {
            report.message = "GateTransitiveFanin requires gateName";
            return report;
        }
        report.sourceId = getGateId(query.gateName);
        if (!isValidGateId(report.sourceId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.sourceName = query.gateName;
        report.cone = getGateTransitiveFaninCone(query.gateName);
        report.message = "Gate transitive fanin cone";
        break;

    case ConeQueryType::GateTransitiveFanout:
        if (query.gateName.empty()) {
            report.message = "GateTransitiveFanout requires gateName";
            return report;
        }
        report.sourceId = getGateId(query.gateName);
        if (!isValidGateId(report.sourceId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.sourceName = query.gateName;
        report.cone = getGateTransitiveFanoutCone(query.gateName);
        report.message = "Gate transitive fanout cone";
        break;
    }

    if (report.cone.rootNetIds.empty()) {
        report.message += " is empty";
        return report;
    }

    report.ok = true;
    report.exists = true;
    report.netCount = getConeNetCount(report.cone);
    report.gateCount = getConeGateCount(report.cone);

    if (query.includeIds) {
        report.rootNetIds = report.cone.rootNetIds;
        report.netIds = getConeNetIds(report.cone);
        report.gateIds = getConeGateIds(report.cone);
    }

    if (query.includeNames) {
        for (int rootNetId : report.cone.rootNetIds) {
            if (isValidNetId(rootNetId)) {
                report.rootNetNames.push_back(nets[rootNetId].name);
            }
        }
        report.netNames = getConeNetNames(report.cone);
        report.gateNames = getConeGateNames(report.cone);
    }

    if (query.includeLocalPaths) {
        const std::pair<int, std::vector<int>> longest = findLongestPathInCone(report.cone);
        const std::pair<int, std::vector<int>> shortest = findShortestPathInCone(report.cone);
        report.longestDepth = longest.first;
        report.shortestDepth = shortest.first;

        if (query.includeIds) {
            report.longestPathNetIds = longest.second;
            report.shortestPathNetIds = shortest.second;
        }
        if (query.includeNames) {
            report.longestPathNetNames = coneNetPathToNames(*this, longest.second);
            report.shortestPathNetNames = coneNetPathToNames(*this, shortest.second);
        }
    }

    return report;
}

// 判斷指定的節點 (Net 或 Gate) 是否為終點。
// combinationalOnly = true: 如果下游只接 DFF，也視為終點。
// combinationalOnly = false: 必須真的沒有接任何東西才算終點。
bool Netlist::isEndpoint(const PathNode& node, bool combinationalOnly) const {
    int targetNetId = -1;

    // 找出要檢查的目標: Net ID
    if (node.type == PathNodeType::Net) {
        targetNetId = getNetId(node.name);
    } else {
        // 如果傳入的是 Gate，我們要檢查的是「這個 Gate 的輸出線」是不是終點
        int gateId = getGateId(node.name);
        if (gateId >= 0) {
            targetNetId = gates[gateId].outputNetId;
        } else {
            return false; // 找不到該 Gate
        }
    }

    // 如果找不到該 Net，或者 Gate 的輸出端本身就是懸空的 (-1)
    // 在圖論上，死路一條就是終點
    if (targetNetId < 0) {
        return true; 
    }

    const Net& net = nets[targetNetId];

    // 如果這條線沒有驅動任何下游 Gate，那它絕對是終點 (Absolute Endpoint)
    if (net.loadGateIds.empty()) {
        return true;
    }

    // 如果開啟了組合邏輯模式，檢查下游是不是「只有 DFF」
    if (combinationalOnly) {
        for (int gateId : net.loadGateIds) {
            // 只要發現下游有任何一個「非 DFF」的邏輯閘，就代表路還能繼續走
            if (gates[gateId].type != GateType::DFF) {
                return false; 
            }
        }
        // 迴圈跑完都沒 return，代表下游 100% 全部都是 DFF
        return true; 
    }

    // 如果是嚴格拓樸模式，且 loadGateIds 裡面有東西，就不是終點
    return false;
}

//  分析邏輯錐：尋找錐體內的最長路徑 (Local Critical Path)
//  回傳值：std::pair<深度, 路徑的 Net IDs>
std::pair<int, std::vector<int>> Netlist::findLongestPathInCone(const ConeResult& cone) const {
    // 記錄 <當前節點, <最大深度, 最佳路徑的下一個節點>>
    std::unordered_map<int, std::pair<int, int>> memo;
    std::unordered_set<int> visiting;

    // 內部 Lambda 遞迴函式：回傳「從目前 net 往下走的最大深度」。
    // 若偵測到 combinational loop，回傳 -1，避免無限遞迴。
    std::function<int(int)> dfsLongest = [&](int currNetId) -> int {
        if (memo.count(currNetId)) {
            return memo[currNetId].first;
        }

        if (visiting.count(currNetId) != 0) {
            return -1;
        }

        visiting.insert(currNetId);

        int maxLength = -1;
        int bestNextNode = -1; // -1 代表沒有下游 (自己是最底端)

        auto it = cone.children.find(currNetId);
        if (it != cone.children.end() && !it->second.empty()) {
            for (int childNetId : it->second) {
                
                int childLength = dfsLongest(childNetId);
                if (childLength < 0) {
                    continue;
                }
                
                // 如果這條路更長，就更新最大深度與「下一個節點」
                if (childLength + 1 > maxLength) {
                    maxLength = childLength + 1;
                    bestNextNode = childNetId;
                }
            }
        } else {
            maxLength = 0;
        }

        visiting.erase(currNetId);

        // 存檔並回傳
        memo[currNetId] = {maxLength, bestNextNode};
        return maxLength;
    };

    // 啟動引擎
    int globalMaxLength = -1;
    int bestRootId = -1;

    for (int rootId : cone.rootNetIds) {
        int length = dfsLongest(rootId);
        if (length > globalMaxLength) {
            globalMaxLength = length;
            bestRootId = rootId;
        }
    }

    // 防呆：如果沒有找到任何路徑
    if (globalMaxLength == -1) {
        return {-1, {}};
    }

    // 路徑重建 (只執行一次)
    std::vector<int> longestPath;
    int curr = bestRootId;
    std::unordered_set<int> rebuiltPath;
    
    while (curr != -1) {
        if (!rebuiltPath.insert(curr).second) {
            return {-1, {}};
        }
        longestPath.push_back(curr);
        curr = memo[curr].second; // 順藤摸瓜找下一個節點
    }

    return {globalMaxLength, longestPath};
}

std::pair<int, std::vector<int>> Netlist::findShortestPathInCone(const ConeResult& cone) const {
    if (cone.rootNetIds.empty()) return {-1, {}};

    std::queue<int> q;
    std::unordered_set<int> visited;
    std::unordered_map<int, int> parent; // 紀錄是誰擴展到當前節點的，用於重建路徑

    // 初始化多源 BFS
    for (int rootId : cone.rootNetIds) {
        q.push(rootId);
        visited.insert(rootId);
        parent[rootId] = -1; // -1 代表起點
    }

    int targetLeaf = -1;
    int currentDepth = 0;

    // BFS 擴展
    while (!q.empty()) {
        int levelSize = q.size();
        
        for (int i = 0; i < levelSize; ++i) {
            int curr = q.front();
            q.pop();

            auto it = cone.children.find(curr);
            
            // 如果這是一個葉節點 (沒有 downstream)
            if (it == cone.children.end() || it->second.empty()) {
                targetLeaf = curr;
                break; // 找到第一個葉節點就是全局最短路徑，直接中斷內圈！
            }

            for (int childNetId : it->second) {
                if (visited.find(childNetId) == visited.end()) {
                    visited.insert(childNetId);
                    parent[childNetId] = curr; // 紀錄路徑來源
                    q.push(childNetId);
                }
            }
        }

        if (targetLeaf != -1) {
            break; // 中斷外圈
        }
        currentDepth++;
    }

    if (targetLeaf == -1) return {-1, {}};

    // 路徑重建
    std::vector<int> path;
    int curr = targetLeaf;
    while (curr != -1) {
        path.push_back(curr);
        curr = parent[curr];
    }
    std::reverse(path.begin(), path.end()); // 因為是從葉節點往回追溯，所以需要反轉

    return {currentDepth, path};
}

// 針對 Net 的 Fanin Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFaninConeLongestPath(const std::string& netName) const {
    auto rawResult = findLongestPathInCone(getTransitiveFaninCone(netName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}
// 針對 Net 的 Fanin Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFaninConeShortestPath(const std::string& netName) const {
    auto rawResult = findShortestPathInCone(getTransitiveFaninCone(netName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}

// 針對 Net 的 Fanout Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFanoutConeLongestPath(const std::string& netName) const {
    auto rawResult = findLongestPathInCone(getTransitiveFanoutCone(netName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}
// 針對 Net 的 Fanout Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getTransitiveFanoutConeShortestPath(const std::string& netName) const {
    auto rawResult = findShortestPathInCone(getTransitiveFanoutCone(netName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}

// 針對 Gate 的 Fanin Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFaninConeLongestPath(const std::string& gateName) const {
    auto rawResult = findLongestPathInCone(getGateTransitiveFaninCone(gateName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}
// 針對 Gate 的 Fanin Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFaninConeShortestPath(const std::string& gateName) const {
    auto rawResult = findShortestPathInCone(getGateTransitiveFaninCone(gateName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}

// 針對 Gate 的 Fanout Cone 的最長路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFanoutConeLongestPath(const std::string& gateName) const {
    auto rawResult = findLongestPathInCone(getGateTransitiveFanoutCone(gateName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}
// 針對 Gate 的 Fanout Cone 的最短路徑
std::pair<int, std::vector<std::string>> Netlist::getGateTransitiveFanoutConeShortestPath(const std::string& gateName) const {
    auto rawResult = findShortestPathInCone(getGateTransitiveFanoutCone(gateName));
    return {rawResult.first, coneNetPathToNames(*this, rawResult.second)};
}
