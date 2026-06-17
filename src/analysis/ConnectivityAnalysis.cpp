#include "include/core/Netlist.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

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

// 將 gate ID 依首次出現順序去重，並略過無效 gate ID。
std::vector<int> uniqueValidGateIds(const Netlist& netlist,
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

// 將 pin name 正規化成大寫，方便辨識 DFF 的 D/CK/RN/SN。
std::string uppercasePinName(std::string pinName) {
    std::transform(pinName.begin(), pinName.end(), pinName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return pinName;
}

// 將 rhs 的 fanout load 分類合併到 lhs；bus aggregate 會使用這個 helper。
void mergeFanoutLoadReport(FanoutLoadReport& lhs, const FanoutLoadReport& rhs) {
    lhs.ok = lhs.ok || rhs.ok;
    lhs.drivesPrimaryOutput = lhs.drivesPrimaryOutput || rhs.drivesPrimaryOutput;
    lhs.primaryOutputLoadCount += rhs.primaryOutputLoadCount;
    lhs.totalLoadCount += rhs.totalLoadCount;

    lhs.combinationalGateLoads.insert(lhs.combinationalGateLoads.end(),
                                      rhs.combinationalGateLoads.begin(),
                                      rhs.combinationalGateLoads.end());
    lhs.dffDataLoads.insert(lhs.dffDataLoads.end(),
                            rhs.dffDataLoads.begin(),
                            rhs.dffDataLoads.end());
    lhs.dffClockLoads.insert(lhs.dffClockLoads.end(),
                             rhs.dffClockLoads.begin(),
                             rhs.dffClockLoads.end());
    lhs.dffResetSetLoads.insert(lhs.dffResetSetLoads.end(),
                                rhs.dffResetSetLoads.begin(),
                                rhs.dffResetSetLoads.end());
    lhs.dffOtherLoads.insert(lhs.dffOtherLoads.end(),
                             rhs.dffOtherLoads.begin(),
                             rhs.dffOtherLoads.end());
    lhs.allGateLoadIds.insert(lhs.allGateLoadIds.end(),
                              rhs.allGateLoadIds.begin(),
                              rhs.allGateLoadIds.end());
}

} // namespace

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

// 依照 Problem A QA 的 fanout load 定義，回報指定 scalar net 的 pin-level loads。
FanoutLoadReport Netlist::getFanoutLoadReport(int netId) const {
    FanoutLoadReport report;
    report.netId = netId;

    if (!isValidNetId(netId)) {
        report.message = "Invalid net ID";
        return report;
    }

    const Net& net = nets[netId];
    report.ok = true;
    report.netName = net.name;
    report.message = "Fanout load report";

    std::vector<int> loadGateIds = uniqueValidGateIds(*this, net.loadGateIds);
    for (int gateId : loadGateIds) {
        const Gate& gate = gates[gateId];

        for (size_t pinIndex = 0; pinIndex < gate.inputNetIds.size(); ++pinIndex) {
            if (gate.inputNetIds[pinIndex] != netId) {
                continue;
            }

            report.allGateLoadIds.push_back(gateId);
            if (gate.type != GateType::DFF) {
                report.combinationalGateLoads.push_back(gateId);
                continue;
            }

            const std::string pinName =
                (pinIndex < gate.inputPinNames.size()) ? uppercasePinName(gate.inputPinNames[pinIndex])
                                                       : "";
            if (pinName == "D") {
                report.dffDataLoads.push_back(gateId);
            } else if (pinName == "CK") {
                report.dffClockLoads.push_back(gateId);
            } else if (pinName == "RN" || pinName == "SN") {
                report.dffResetSetLoads.push_back(gateId);
            } else {
                report.dffOtherLoads.push_back(gateId);
            }
        }
    }

    if (net.isPO) {
        report.drivesPrimaryOutput = true;
        report.primaryOutputLoadCount = 1;
    }

    report.totalLoadCount = report.allGateLoadIds.size() + report.primaryOutputLoadCount;
    return report;
}

// 依照 Problem A QA 的 fanout load 定義，回報指定 net / bus 的 pin-level loads。
FanoutLoadReport Netlist::getFanoutLoadReport(const std::string& netName) const {
    FanoutLoadReport report;
    report.netName = netName;

    const int scalarNetId = getNetId(netName);
    if (isValidNetId(scalarNetId)) {
        return getFanoutLoadReport(scalarNetId);
    }

    const std::vector<int> bitNetIds = expandNetToBits(netName);
    if (bitNetIds.empty()) {
        report.message = "Net not found: " + netName;
        return report;
    }

    report.message = "Fanout load report for bus";
    for (int bitNetId : bitNetIds) {
        mergeFanoutLoadReport(report, getFanoutLoadReport(bitNetId));
    }
    report.netId = -1;
    report.netName = netName;
    return report;
}

// 依照 Problem A QA 的 fanout load 定義，取得指定 net / bus 的總負載數。
size_t Netlist::getFanoutLoadCount(const std::string& netName) const {
    return getFanoutLoadReport(netName).totalLoadCount;
}

// 依照 Problem A QA 的 fanout load 定義掃描全設計或所有 primary inputs。
GlobalFanoutReport Netlist::getGlobalFanoutReport(int maxFanoutLimit,
                                                  bool primaryInputsOnly,
                                                  bool includeZeroFanout) const {
    GlobalFanoutReport report;
    report.ok = true;
    report.message = primaryInputsOnly ? "Primary-input fanout report"
                                       : "Global fanout report";
    report.fanoutLimit = maxFanoutLimit;
    report.primaryInputsOnly = primaryInputsOnly;
    report.includeZeroFanout = includeZeroFanout;

    for (const Net& net : nets) {
        if (primaryInputsOnly && !net.isPI) {
            continue;
        }

        FanoutLoadReport netReport = getFanoutLoadReport(net.id);
        if (!netReport.ok) {
            continue;
        }
        if (!includeZeroFanout && netReport.totalLoadCount == 0) {
            continue;
        }

        report.netReports.push_back(netReport);
        ++report.checkedNetCount;

        if (netReport.totalLoadCount > report.maxFanout) {
            report.maxFanout = netReport.totalLoadCount;
            report.maxFanoutReports.clear();
            report.maxFanoutReports.push_back(netReport);
        } else if (netReport.totalLoadCount == report.maxFanout) {
            report.maxFanoutReports.push_back(netReport);
        }

        if (maxFanoutLimit >= 0 &&
            netReport.totalLoadCount > static_cast<size_t>(maxFanoutLimit)) {
            report.violatingReports.push_back(netReport);
        }
    }

    report.satisfiesLimit = report.violatingReports.empty();
    return report;
}

// 判斷全設計是否符合指定 fanout limit；使用 Problem A QA fanout load 定義。
bool Netlist::satisfiesFanoutLimit(int maxFanoutLimit) const {
    if (maxFanoutLimit < 0) {
        return false;
    }
    return getGlobalFanoutReport(maxFanoutLimit).satisfiesLimit;
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

    case DirectConnectivityQueryType::FanoutLoadReport:
        if (query.netName.empty()) {
            report.message = "FanoutLoadReport requires netName";
            return report;
        }
        report.fanoutLoadReport = getFanoutLoadReport(query.netName);
        if (!report.fanoutLoadReport.ok) {
            report.message = report.fanoutLoadReport.message;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.netId = report.fanoutLoadReport.netId;
        report.netName = report.fanoutLoadReport.netName;
        report.message = "Fanout load report";
        report.count = report.fanoutLoadReport.totalLoadCount;
        if (query.includeIds) {
            report.gateIds = report.fanoutLoadReport.allGateLoadIds;
        }
        if (query.includeNames) {
            report.gateNames = gateIdsToNames(*this, report.fanoutLoadReport.allGateLoadIds);
        }
        return report;

    case DirectConnectivityQueryType::GlobalFanoutReport:
        report.globalFanoutReport = getGlobalFanoutReport(query.fanoutLimit,
                                                          query.primaryInputsOnly,
                                                          query.includeZeroFanout);
        report.ok = report.globalFanoutReport.ok;
        report.exists = report.globalFanoutReport.ok;
        report.message = report.globalFanoutReport.message;
        report.count = report.globalFanoutReport.checkedNetCount;
        if (!report.globalFanoutReport.maxFanoutReports.empty()) {
            report.netId = report.globalFanoutReport.maxFanoutReports.front().netId;
            report.netName = report.globalFanoutReport.maxFanoutReports.front().netName;
        }
        if (query.includeIds) {
            for (const FanoutLoadReport& maxReport : report.globalFanoutReport.maxFanoutReports) {
                report.netIds.push_back(maxReport.netId);
            }
        }
        if (query.includeNames) {
            for (const FanoutLoadReport& maxReport : report.globalFanoutReport.maxFanoutReports) {
                report.netNames.push_back(maxReport.netName);
            }
        }
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
    auto it = netNameToId.find(wireName);
    if (it != netNameToId.end()) {
        return nets[it->second].loadGateIds;
    }

    std::vector<int> allLoadGates;
    bool isBusFound = false;
    const std::string busPrefix = wireName + "[";

    for (const auto& pair : netNameToId) {
        if (pair.first.find(busPrefix) == 0) {
            const std::vector<int>& loads = nets[pair.second].loadGateIds;
            allLoadGates.insert(allLoadGates.end(), loads.begin(), loads.end());
            isBusFound = true;
        }
    }

    if (isBusFound) {
        std::sort(allLoadGates.begin(), allLoadGates.end());
        auto last = std::unique(allLoadGates.begin(), allLoadGates.end());
        allLoadGates.erase(last, allLoadGates.end());
        return allLoadGates;
    }

    return {};
}

// 回傳指定 wire / bus 直接 load 到的 gate instance names。
std::vector<std::string> Netlist::getWireLoadNames(const std::string& wireName) const {
    std::vector<std::string> result;
    std::vector<int> loadIds = getWireLoads(wireName);

    result.reserve(loadIds.size());
    for (int id : loadIds) {
        if (isValidGateId(id)) {
            result.push_back(gates[id].instName);
        }
    }

    return result;
}

// 回傳指定 wire / bus 直接 load 到的 gate 數量。
size_t Netlist::getWireLoadCount(const std::string& wireName) const {
    return getWireLoads(wireName).size();
}

// 回傳指定 gate output net 直接驅動多少個 gate input pins
// return which gate input pins are connected to this gate's output
std::vector<int> Netlist::getGateFanout(const std::string& gateInstName) const {
    auto it = gateNameToId.find(gateInstName);
    if (it == gateNameToId.end()) {
        return {};
    }

    const Gate& gate = gates[it->second];
    if (gate.outputNetId == -1) {
        return {};
    }

    const Net& outNet = nets[gate.outputNetId];
    return outNet.loadGateIds;
}

// 回傳指定 gate output net 直接 fanout 到的 gate instance names。
std::vector<std::string> Netlist::getGateFanoutNames(const std::string& gateInstName) const {
    std::vector<std::string> result;
    std::vector<int> fanoutIds = getGateFanout(gateInstName);

    result.reserve(fanoutIds.size());
    for (int id : fanoutIds) {
        if (isValidGateId(id)) {
            result.push_back(gates[id].instName);
        }
    }

    return result;
}

// 回傳指定 gate output net 直接 fanout 到的 gate 數量。
size_t Netlist::getGateFanoutCount(const std::string& gateInstName) const {
    return getGateFanout(gateInstName).size();
}
