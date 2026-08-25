#include "include/core/Netlist.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// isValidGateId()/isValidNetId() only bounds-check the underlying vector; they
// intentionally say nothing about tombstoned slots (soft-deleted gate =
// type==UNKNOWN, soft-deleted net = isRemoved==true). BasicAnalysis.cpp already
// established this "active object" convention for the same reason ("Active-object
// helpers keep public reports independent from tombstone slots"); Direct
// Connectivity Query results need the same treatment so that cleanup/optimization
// passes don't leave dead gates/nets visible in query output.
bool isActiveGate(const Netlist& netlist, int gateId) {
    return netlist.isValidGateId(gateId) &&
           netlist.getGate(gateId).type != GateType::UNKNOWN;
}

bool isActiveNet(const Netlist& netlist, int netId) {
    return netlist.isValidNetId(netId) && !netlist.getNet(netId).isRemoved;
}

bool hasConsistentActiveDriver(const Netlist& netlist, int netId, int gateId) {
    return isActiveNet(netlist, netId) &&
           isActiveGate(netlist, gateId) &&
           netlist.getNet(netId).driverGateId == gateId &&
           netlist.getGate(gateId).outputNetId == netId;
}

bool isConsistentActiveLoad(const Netlist& netlist, int netId, int gateId) {
    if (!isActiveNet(netlist, netId) || !isActiveGate(netlist, gateId)) {
        return false;
    }
    const std::vector<int>& loadGateIds = netlist.getNet(netId).loadGateIds;
    if (std::find(loadGateIds.begin(), loadGateIds.end(), gateId) ==
        loadGateIds.end()) {
        return false;
    }
    const std::vector<int>& inputNetIds = netlist.getGate(gateId).inputNetIds;
    return std::find(inputNetIds.begin(), inputNetIds.end(), netId) !=
           inputNetIds.end();
}

std::vector<int> activeNetIdsForName(const Netlist& netlist,
                                     const std::string& netName) {
    std::vector<int> netIds;
    const int scalarNetId = netlist.getNetId(netName);
    if (isActiveNet(netlist, scalarNetId)) {
        netIds.push_back(scalarNetId);
        return netIds;
    }

    const std::vector<int> bitNetIds = netlist.expandNetToBits(netName);
    netIds.reserve(bitNetIds.size());
    for (int bitNetId : bitNetIds) {
        if (isActiveNet(netlist, bitNetId)) {
            netIds.push_back(bitNetId);
        }
    }
    return netIds;
}

// 將 gate ID 陣列轉成 gate instance name 陣列；無效或已刪除(tombstone) 的 ID 會被略過。
std::vector<std::string> gateIdsToNames(const Netlist& netlist,
                                        const std::vector<int>& gateIds) {
    std::vector<std::string> names;
    names.reserve(gateIds.size());
    for (int gateId : gateIds) {
        if (isActiveGate(netlist, gateId)) {
            names.push_back(netlist.getGate(gateId).instName);
        }
    }
    return names;
}

// 將 gate ID 陣列依數值排序後去重，並略過無效或已刪除(tombstone) 的 gate ID。
// 沿用原始 getWireLoads() bus 分支的 sort+unique 策略：對大量 ID 而言，這比
// unordered_set 版本的雜湊插入更快，也是唯一分支原本就有的排序輸出慣例。
std::vector<int> uniqueValidGateIds(const Netlist& netlist,
                                    const std::vector<int>& gateIds) {
    std::vector<int> filtered;
    filtered.reserve(gateIds.size());
    for (int gateId : gateIds) {
        if (isActiveGate(netlist, gateId)) {
            filtered.push_back(gateId);
        }
    }
    std::sort(filtered.begin(), filtered.end());
    filtered.erase(std::unique(filtered.begin(), filtered.end()), filtered.end());
    return filtered;
}

std::vector<int> driverGateIdsForNetIds(const Netlist& netlist,
                                        const std::vector<int>& netIds) {
    std::vector<int> driverGateIds;
    driverGateIds.reserve(netIds.size());
    for (int netId : netIds) {
        const int driverGateId = netlist.getNet(netId).driverGateId;
        if (hasConsistentActiveDriver(netlist, netId, driverGateId)) {
            driverGateIds.push_back(driverGateId);
        }
    }
    return uniqueValidGateIds(netlist, driverGateIds);
}

std::vector<int> loadGateIdsForNetIds(const Netlist& netlist,
                                      const std::vector<int>& netIds) {
    std::vector<int> loadGateIds;
    for (int netId : netIds) {
        const Net& net = netlist.getNet(netId);
        for (int gateId : net.loadGateIds) {
            if (isConsistentActiveLoad(netlist, netId, gateId)) {
                loadGateIds.push_back(gateId);
            }
        }
    }
    return uniqueValidGateIds(netlist, loadGateIds);
}

std::vector<int> directlyConnectedNetIds(const Netlist& netlist,
                                         int gateId,
                                         const std::vector<int>& candidateNetIds) {
    std::vector<int> connectedNetIds;
    if (!isActiveGate(netlist, gateId)) {
        return connectedNetIds;
    }

    connectedNetIds.reserve(candidateNetIds.size());
    for (int netId : candidateNetIds) {
        if (hasConsistentActiveDriver(netlist, netId, gateId) ||
            isConsistentActiveLoad(netlist, netId, gateId)) {
            connectedNetIds.push_back(netId);
        }
    }
    return connectedNetIds;
}

// 將 pin name 正規化成大寫，方便辨識 DFF 的 D/CK/RN/SN。
std::string uppercasePinName(std::string pinName) {
    std::transform(pinName.begin(), pinName.end(), pinName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return pinName;
}

std::string inputPinName(const Gate& gate, size_t pinIndex) {
    if (pinIndex < gate.inputPinNames.size() &&
        !gate.inputPinNames[pinIndex].empty()) {
        return gate.inputPinNames[pinIndex];
    }
    return "IN" + std::to_string(pinIndex + 1);
}

ConnectivityPinRole inputPinRole(const Gate& gate, size_t pinIndex) {
    if (gate.type != GateType::DFF) {
        return ConnectivityPinRole::CombinationalInput;
    }
    const std::string pinName = uppercasePinName(inputPinName(gate, pinIndex));
    if (pinName == "D") return ConnectivityPinRole::DffData;
    if (pinName == "CK") return ConnectivityPinRole::DffClock;
    if (pinName == "RN" || pinName == "SN") {
        return ConnectivityPinRole::DffResetSet;
    }
    return ConnectivityPinRole::DffOther;
}

std::vector<ConnectivityPinRecord> driverPinRecordsForNetIds(
    const Netlist& netlist,
    const std::vector<int>& netIds) {
    std::vector<ConnectivityPinRecord> records;
    records.reserve(netIds.size());
    for (int netId : netIds) {
        const int gateId = netlist.getNet(netId).driverGateId;
        if (!hasConsistentActiveDriver(netlist, netId, gateId)) {
            continue;
        }
        const Net& net = netlist.getNet(netId);
        const Gate& gate = netlist.getGate(gateId);
        ConnectivityPinRecord record;
        record.netId = netId;
        record.netName = net.name;
        record.gateId = gateId;
        record.gateName = gate.instName;
        record.gateTypeName = netlist.gateTypeToString(gate.type);
        record.pinName = gate.type == GateType::DFF ? "Q" : "OUT";
        record.direction = ConnectivityPinDirection::Driver;
        record.role = ConnectivityPinRole::Output;
        records.push_back(std::move(record));
    }
    return records;
}

std::vector<ConnectivityPinRecord> loadPinRecordsForNetIds(
    const Netlist& netlist,
    const std::vector<int>& netIds) {
    std::vector<ConnectivityPinRecord> records;
    for (int netId : netIds) {
        const Net& net = netlist.getNet(netId);
        const std::vector<int> gateIds = uniqueValidGateIds(netlist, net.loadGateIds);
        for (int gateId : gateIds) {
            if (!isConsistentActiveLoad(netlist, netId, gateId)) {
                continue;
            }
            const Gate& gate = netlist.getGate(gateId);
            for (size_t pinIndex = 0; pinIndex < gate.inputNetIds.size(); ++pinIndex) {
                if (gate.inputNetIds[pinIndex] != netId) {
                    continue;
                }
                ConnectivityPinRecord record;
                record.netId = netId;
                record.netName = net.name;
                record.gateId = gateId;
                record.gateName = gate.instName;
                record.gateTypeName = netlist.gateTypeToString(gate.type);
                record.pinIndex = static_cast<int>(pinIndex);
                record.pinName = inputPinName(gate, pinIndex);
                record.direction = ConnectivityPinDirection::Load;
                record.role = inputPinRole(gate, pinIndex);
                records.push_back(std::move(record));
            }
        }
    }
    return records;
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

bool isValidFanoutFilter(FanoutPredicate predicate,
                         size_t lowerValue,
                         size_t upperValue) {
    switch (predicate) {
    case FanoutPredicate::None:
    case FanoutPredicate::Equal:
    case FanoutPredicate::NotEqual:
    case FanoutPredicate::GreaterThan:
    case FanoutPredicate::GreaterOrEqual:
    case FanoutPredicate::LessThan:
    case FanoutPredicate::LessOrEqual:
        return true;
    case FanoutPredicate::BetweenInclusive:
        return lowerValue <= upperValue;
    }
    return false;
}

bool matchesFanoutFilter(size_t fanout,
                         FanoutPredicate predicate,
                         size_t lowerValue,
                         size_t upperValue) {
    switch (predicate) {
    case FanoutPredicate::None: return false;
    case FanoutPredicate::Equal: return fanout == lowerValue;
    case FanoutPredicate::NotEqual: return fanout != lowerValue;
    case FanoutPredicate::GreaterThan: return fanout > lowerValue;
    case FanoutPredicate::GreaterOrEqual: return fanout >= lowerValue;
    case FanoutPredicate::LessThan: return fanout < lowerValue;
    case FanoutPredicate::LessOrEqual: return fanout <= lowerValue;
    case FanoutPredicate::BetweenInclusive:
        return fanout >= lowerValue && fanout <= upperValue;
    }
    return false;
}

bool isValidFanoutScope(FanoutScope scope) {
    switch (scope) {
    case FanoutScope::All:
    case FanoutScope::PrimaryInputs:
    case FanoutScope::PrimaryOutputs:
    case FanoutScope::Internal:
    case FanoutScope::GateOutputs:
    case FanoutScope::CombinationalOutputs:
    case FanoutScope::DffOutputs:
        return true;
    }
    return false;
}

bool isValidFanoutRankMode(FanoutRankMode mode) {
    switch (mode) {
    case FanoutRankMode::Highest:
    case FanoutRankMode::Lowest:
    case FanoutRankMode::NthHighest:
    case FanoutRankMode::NthLowest:
    case FanoutRankMode::Top:
    case FanoutRankMode::Bottom:
        return true;
    }
    return false;
}

bool isDescendingFanoutRank(FanoutRankMode mode) {
    return mode == FanoutRankMode::Highest ||
           mode == FanoutRankMode::NthHighest ||
           mode == FanoutRankMode::Top;
}

bool netMatchesFanoutScope(const Netlist& netlist,
                           const Net& net,
                           FanoutScope scope) {
    if (net.isRemoved) return false;

    if (scope == FanoutScope::All) return true;
    if (scope == FanoutScope::PrimaryInputs) return net.isPI;
    if (scope == FanoutScope::PrimaryOutputs) return net.isPO;
    if (scope == FanoutScope::Internal) {
        return !net.isPI && !net.isPO && !net.isConst;
    }

    const int driverGateId = net.driverGateId;
    if (!hasConsistentActiveDriver(netlist, net.id, driverGateId)) {
        return false;
    }
    if (scope == FanoutScope::GateOutputs) return true;

    const GateType driverType = netlist.getGate(driverGateId).type;
    if (scope == FanoutScope::DffOutputs) return driverType == GateType::DFF;
    if (scope == FanoutScope::CombinationalOutputs) {
        return driverType != GateType::DFF;
    }
    return false;
}

} // namespace

// getNetDriverGateId 的 ID 版本 (多載)
int Netlist::getNetDriverGateId(int netId) const {
    if (!isValidNetId(netId) || nets[netId].isRemoved) {
        return -1;
    }
    const int driverGateId = nets[netId].driverGateId;
    if (!hasConsistentActiveDriver(*this, netId, driverGateId)) {
        return -1;
    }
    return driverGateId;
}

// 取得指定 net 的唯一 driver gate ID；bus 或多 driver 情況請使用 getNetDriverGateIds。
int Netlist::getNetDriverGateId(const std::string& netName) const {
    const int netId = getNetId(netName);
    if (!isValidNetId(netId) || nets[netId].isRemoved) {
        return -1;
    }

    const int driverGateId = nets[netId].driverGateId;
    if (!hasConsistentActiveDriver(*this, netId, driverGateId)) {
        return -1;
    }
    return driverGateId;
}

// 取得指定 net / bus 的所有直接 driver gate IDs；bus 會展開每個 bit 並去重。
std::vector<int> Netlist::getNetDriverGateIds(const std::string& netName) const {
    const std::vector<int> netIds = expandNetToBits(netName);
    return driverGateIdsForNetIds(*this, netIds);
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
    if (nets[netId].isRemoved) {
        report.message = "Net has been removed";
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
                                                  bool includeZeroFanout,
                                                  FanoutPredicate fanoutPredicate,
                                                  size_t fanoutValue,
                                                  size_t fanoutUpperValue) const {
    GlobalFanoutReport report;
    report.fanoutLimit = maxFanoutLimit;
    report.primaryInputsOnly = primaryInputsOnly;
    report.includeZeroFanout = includeZeroFanout;
    report.fanoutPredicate = fanoutPredicate;
    report.fanoutValue = fanoutValue;
    report.fanoutUpperValue = fanoutUpperValue;
    report.fanoutFilterApplied = fanoutPredicate != FanoutPredicate::None;

    if (!isValidFanoutFilter(fanoutPredicate, fanoutValue, fanoutUpperValue)) {
        report.message = fanoutPredicate == FanoutPredicate::BetweenInclusive
            ? "Fanout filter lower bound exceeds upper bound"
            : "Unsupported fanout predicate";
        return report;
    }

    report.ok = true;
    report.message = report.fanoutFilterApplied
        ? (primaryInputsOnly ? "Primary-input fanout filter report"
                             : "Global fanout filter report")
        : (primaryInputsOnly ? "Primary-input fanout report"
                             : "Global fanout report");

    for (const Net& net : nets) {
        if (primaryInputsOnly && !net.isPI) {
            continue;
        }

        FanoutLoadReport netReport = getFanoutLoadReport(net.id);
        if (!netReport.ok) {
            continue;
        }

        // Every active in-scope net participates in extrema.  The zero-fanout
        // option only controls the full-detail netReports collection; filtering
        // zeroes before comparison would make an all-zero scope report no
        // maximum candidates even though every net ties at fanout zero.
        ++report.checkedNetCount;

        if (report.maxFanoutReports.empty() ||
            netReport.totalLoadCount > report.maxFanout) {
            report.maxFanout = netReport.totalLoadCount;
            report.maxFanoutReports.clear();
            report.maxFanoutReports.push_back(netReport);
        } else if (netReport.totalLoadCount == report.maxFanout) {
            report.maxFanoutReports.push_back(netReport);
        }

        if (includeZeroFanout || netReport.totalLoadCount != 0) {
            report.netReports.push_back(netReport);
        }

        if (report.fanoutFilterApplied &&
            matchesFanoutFilter(netReport.totalLoadCount,
                                fanoutPredicate,
                                fanoutValue,
                                fanoutUpperValue)) {
            report.matchedReports.push_back(netReport);
        }

        if (maxFanoutLimit >= 0 &&
            netReport.totalLoadCount > static_cast<size_t>(maxFanoutLimit)) {
            report.violatingReports.push_back(netReport);
        }
    }

    report.matchedNetCount = report.matchedReports.size();
    report.satisfiesLimit = report.violatingReports.empty();
    return report;
}

FanoutRankingReport Netlist::getFanoutRankingReport(
    FanoutScope scope,
    FanoutRankMode mode,
    size_t rankOrCount) const {
    FanoutRankingReport report;
    report.scope = scope;
    report.mode = mode;
    report.requestedRankOrCount = rankOrCount;

    if (!isValidFanoutScope(scope)) {
        report.message = "Unsupported fanout ranking scope";
        return report;
    }
    if (!isValidFanoutRankMode(mode)) {
        report.message = "Unsupported fanout ranking mode";
        return report;
    }
    if (rankOrCount == 0) {
        report.message = "Fanout rank/count must be greater than zero";
        return report;
    }
    if ((mode == FanoutRankMode::Highest ||
         mode == FanoutRankMode::Lowest) && rankOrCount != 1) {
        report.message = "Highest/lowest fanout ranking uses rank value 1";
        return report;
    }

    std::vector<FanoutRankEntry> candidates;
    candidates.reserve(nets.size());
    for (const Net& net : nets) {
        if (!netMatchesFanoutScope(*this, net, scope)) continue;

        const FanoutLoadReport fanout = getFanoutLoadReport(net.id);
        if (!fanout.ok) continue;

        FanoutRankEntry entry;
        entry.netId = net.id;
        entry.netName = net.name;
        entry.fanout = fanout.totalLoadCount;
        candidates.push_back(std::move(entry));
    }

    report.checkedNetCount = candidates.size();
    const bool descending = isDescendingFanoutRank(mode);
    std::sort(candidates.begin(), candidates.end(),
              [descending](const FanoutRankEntry& lhs,
                           const FanoutRankEntry& rhs) {
        if (lhs.fanout != rhs.fanout) {
            return descending ? lhs.fanout > rhs.fanout
                              : lhs.fanout < rhs.fanout;
        }
        if (lhs.netName != rhs.netName) return lhs.netName < rhs.netName;
        return lhs.netId < rhs.netId;
    });

    size_t currentRank = 0;
    size_t previousFanout = 0;
    bool havePrevious = false;
    for (FanoutRankEntry& entry : candidates) {
        if (!havePrevious || entry.fanout != previousFanout) {
            ++currentRank;
            previousFanout = entry.fanout;
            havePrevious = true;
        }
        entry.rank = currentRank;
    }
    report.distinctFanoutLevelCount = currentRank;

    const bool multiLevel = mode == FanoutRankMode::Top ||
                            mode == FanoutRankMode::Bottom;
    const size_t selectedLevelLimit = multiLevel ? rankOrCount : 1;
    const size_t selectedRank =
        (mode == FanoutRankMode::NthHighest ||
         mode == FanoutRankMode::NthLowest) ? rankOrCount : 1;

    for (const FanoutRankEntry& entry : candidates) {
        const bool selected = multiLevel
            ? entry.rank <= selectedLevelLimit
            : entry.rank == selectedRank;
        if (selected) report.rankedReports.push_back(entry);
    }

    report.selectedFanoutLevelCount = multiLevel
        ? std::min(rankOrCount, report.distinctFanoutLevelCount)
        : (selectedRank <= report.distinctFanoutLevelCount ? 1 : 0);
    report.resultNetCount = report.rankedReports.size();
    report.requestedRankExists = rankOrCount <= report.distinctFanoutLevelCount;
    report.ok = true;
    report.message = "Fanout ranking report";
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
    if (!isValidGateId(gateId) || gates[gateId].type == GateType::UNKNOWN) {
        return inputNetIds;
    }

    const Gate& gate = gates[gateId];
    inputNetIds.reserve(gate.inputNetIds.size());
    for (int netId : gate.inputNetIds) {
        if (isValidNetId(netId) && !nets[netId].isRemoved) {
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
    const int gateId = getGateId(gateInstName);
    if (!isValidGateId(gateId) || gates[gateId].type == GateType::UNKNOWN) {
        return "";
    }
    const int outputNetId = getGateOutputNetId(gateId);
    if (!hasConsistentActiveDriver(*this, outputNetId, gateId)) {
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
        if (hasConsistentActiveDriver(*this, netId, driverGateId)) {
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

// 判斷指定 gate 是否直接連到 scalar net 或 bus 的任一 active bit。
bool Netlist::isGateDirectlyConnectedToNet(const std::string& gateInstName,
                                           const std::string& netName) const {
    const int gateId = getGateId(gateInstName);
    const std::vector<int> candidateNetIds = activeNetIdsForName(*this, netName);
    return !directlyConnectedNetIds(*this, gateId, candidateNetIds).empty();
}

// 判斷指定 scalar net / bus 是否直接連到指定 gate；與上式同語意。
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
    case DirectConnectivityQueryType::NetDriverGates: {
        if (query.netName.empty()) {
            report.message = "NetDriverGates requires netName";
            return report;
        }
        const std::vector<int> activeNetIds = activeNetIdsForName(*this, query.netName);
        if (activeNetIds.empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        const int scalarNetId = getNetId(query.netName);
        report.netId = isActiveNet(*this, scalarNetId) ? scalarNetId : -1;
        report.ok = true;
        report.exists = true;
        report.message = "Net driver";
        {
            // 只解析一次 netName -> driver gate IDs（bus 情況下這一步是 O(所有 net 數)
            // 的線性掃描），names 直接從同一份結果轉換，避免重複掃描整個 netlist。
            const std::vector<int> driverGateIds =
                driverGateIdsForNetIds(*this, activeNetIds);
            if (query.includeIds) {
                report.gateIds = driverGateIds;
                if (report.gateIds.size() == 1) {
                    report.gateId = report.gateIds.front();
                }
            }
            if (query.includeNames) {
                report.gateNames = gateIdsToNames(*this, driverGateIds);
                if (report.gateNames.size() == 1) {
                    report.gateName = report.gateNames.front();
                }
            }
            report.count = driverGateIds.size();
            report.pinDetailsIncluded = query.includePinDetails;
            if (query.includePinDetails) {
                report.pinConnections =
                    driverPinRecordsForNetIds(*this, activeNetIds);
                report.pinConnectionCount = report.pinConnections.size();
            }
        }
        return report;
    }

    case DirectConnectivityQueryType::NetLoadGates: {
        if (query.netName.empty()) {
            report.message = "NetLoadGates requires netName";
            return report;
        }
        const std::vector<int> activeNetIds = activeNetIdsForName(*this, query.netName);
        if (activeNetIds.empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        const int scalarNetId = getNetId(query.netName);
        report.netId = isActiveNet(*this, scalarNetId) ? scalarNetId : -1;
        report.ok = true;
        report.exists = true;
        report.message = "Net loads";
        {
            // bus name 解析會線性掃描 netNameToId；這裡只解析一次，names/count
            // 都從同一份 active bit 與 load gate 結果推導。
            const std::vector<int> loadGateIds =
                loadGateIdsForNetIds(*this, activeNetIds);
            if (query.includeIds) {
                report.gateIds = loadGateIds;
            }
            if (query.includeNames) {
                report.gateNames = gateIdsToNames(*this, loadGateIds);
            }
            report.count = loadGateIds.size();
            report.pinDetailsIncluded = query.includePinDetails;
            if (query.includePinDetails) {
                report.pinConnections = loadPinRecordsForNetIds(*this, activeNetIds);
                report.pinConnectionCount = report.pinConnections.size();
            }
        }
        return report;
    }

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
                                                          query.includeZeroFanout,
                                                          query.fanoutPredicate,
                                                          query.fanoutValue,
                                                          query.fanoutUpperValue);
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

    case DirectConnectivityQueryType::FanoutRankingReport:
        report.fanoutRankingReport = getFanoutRankingReport(
            query.fanoutScope, query.fanoutRankMode, query.fanoutRankValue);
        report.ok = report.fanoutRankingReport.ok;
        report.exists = report.fanoutRankingReport.ok;
        report.message = report.fanoutRankingReport.message;
        report.count = report.fanoutRankingReport.resultNetCount;
        if (query.includeIds) {
            report.netIds.reserve(report.fanoutRankingReport.rankedReports.size());
            for (const FanoutRankEntry& entry :
                 report.fanoutRankingReport.rankedReports) {
                report.netIds.push_back(entry.netId);
            }
        }
        if (query.includeNames) {
            report.netNames.reserve(report.fanoutRankingReport.rankedReports.size());
            for (const FanoutRankEntry& entry :
                 report.fanoutRankingReport.rankedReports) {
                report.netNames.push_back(entry.netName);
            }
        }
        return report;

    case DirectConnectivityQueryType::GateInputs:
        if (query.gateName.empty()) {
            report.message = "GateInputs requires gateName";
            return report;
        }
        report.gateId = getGateId(query.gateName);
        if (!isValidGateId(report.gateId) || gates[report.gateId].type == GateType::UNKNOWN) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.message = "Gate input nets";
        {
            const std::vector<int> inputNetIds = getGateInputNetIds(query.gateName);
            report.count = inputNetIds.size();
            if (query.includeIds) {
                report.netIds = inputNetIds;
            }
            if (query.includeNames) {
                report.netNames.reserve(inputNetIds.size());
                for (int netId : inputNetIds) {
                    report.netNames.push_back(nets[netId].name);
                }
            }
        }
        return report;

    case DirectConnectivityQueryType::GateOutput:
        if (query.gateName.empty()) {
            report.message = "GateOutput requires gateName";
            return report;
        }
        report.gateId = getGateId(query.gateName);
        if (!isValidGateId(report.gateId) || gates[report.gateId].type == GateType::UNKNOWN) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.ok = true;
        report.exists = true;
        report.message = "Gate output net";
        {
            const int outputNetId = getGateOutputNetId(report.gateId);
            if (!hasConsistentActiveDriver(*this, outputNetId, report.gateId)) {
                return report;
            }
            report.netId = outputNetId;
            report.netName = nets[outputNetId].name;
            report.count = 1;
            if (query.includeIds) {
                report.netIds.push_back(outputNetId);
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
        if (!isValidGateId(report.gateId) || gates[report.gateId].type == GateType::UNKNOWN) {
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
        if (!isValidGateId(report.gateId) || gates[report.gateId].type == GateType::UNKNOWN) {
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

    case DirectConnectivityQueryType::DirectlyConnected: {
        if (query.gateName.empty() || query.netName.empty()) {
            report.message = "DirectlyConnected requires gateName and netName";
            return report;
        }
        report.gateId = getGateId(query.gateName);
        if (!isActiveGate(*this, report.gateId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        const std::vector<int> candidateNetIds =
            activeNetIdsForName(*this, query.netName);
        if (candidateNetIds.empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        const int scalarNetId = getNetId(query.netName);
        report.netId = isActiveNet(*this, scalarNetId) ? scalarNetId : -1;
        const std::vector<int> connectedNetIds =
            directlyConnectedNetIds(*this, report.gateId, candidateNetIds);
        report.ok = true;
        report.exists = true;
        report.connected = !connectedNetIds.empty();
        report.message = report.connected ? "Gate and net are directly connected"
                                           : "Gate and net are not directly connected";
        report.count = connectedNetIds.size();
        if (report.connected) {
            if (query.includeIds) {
                report.gateIds.push_back(report.gateId);
                report.netIds = connectedNetIds;
            }
            if (query.includeNames) {
                report.gateNames.push_back(query.gateName);
                report.netNames.reserve(connectedNetIds.size());
                for (int netId : connectedNetIds) {
                    report.netNames.push_back(nets[netId].name);
                }
            }
        }
        return report;
    }
    }

    report.message = "Unsupported DirectConnectivityQueryType";
    return report;
}

// 回傳指定 wire / bus 被多少個 gate input pins 直接使用
// return which gate input pins this wire is connected to.
std::vector<int> Netlist::getWireLoads(const std::string& wireName) const {
    return loadGateIdsForNetIds(*this, activeNetIdsForName(*this, wireName));
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
    if (gate.type == GateType::UNKNOWN) {
        return {};
    }
    if (!isValidNetId(gate.outputNetId) || nets[gate.outputNetId].isRemoved) {
        return {};
    }

    const Net& outNet = nets[gate.outputNetId];
    if (!hasConsistentActiveDriver(*this, outNet.id, gate.id)) {
        return {};
    }
    std::vector<int> consistentFanoutIds;
    consistentFanoutIds.reserve(outNet.loadGateIds.size());
    for (int gateId : outNet.loadGateIds) {
        if (isConsistentActiveLoad(*this, outNet.id, gateId)) {
            consistentFanoutIds.push_back(gateId);
        }
    }
    return uniqueValidGateIds(*this, consistentFanoutIds);
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
/*#include "include/core/Netlist.h"

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

// getNetDriverGateId 的 ID 版本 (多載)
int Netlist::getNetDriverGateId(int netId) const {
    const int driverGateId = nets[netId].driverGateId;
    if (!isValidGateId(driverGateId)) {
        return -1;
    }
    return driverGateId;
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
    case DirectConnectivityQueryType::NetDriverGates:
        if (query.netName.empty()) {
            report.message = "NetDriverGates requires netName";
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

    case DirectConnectivityQueryType::NetLoadGates:
        if (query.netName.empty()) {
            report.message = "NetLoadGates requires netName";
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
*/
