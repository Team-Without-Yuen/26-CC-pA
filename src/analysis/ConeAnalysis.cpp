#include "include/core/Netlist.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

bool isActiveConeGate(const Netlist& netlist, int gateId) {
    return netlist.isValidGateId(gateId) &&
           netlist.getGate(gateId).type != GateType::UNKNOWN;
}

bool isActiveConeNet(const Netlist& netlist, int netId) {
    return netlist.isValidNetId(netId) && !netlist.getNet(netId).isRemoved;
}

bool hasConsistentConeDriver(const Netlist& netlist, int netId, int gateId) {
    return isActiveConeNet(netlist, netId) &&
           isActiveConeGate(netlist, gateId) &&
           netlist.getNet(netId).driverGateId == gateId &&
           netlist.getGate(gateId).outputNetId == netId;
}

bool hasConsistentConeLoad(const Netlist& netlist, int netId, int gateId) {
    if (!isActiveConeNet(netlist, netId) ||
        !isActiveConeGate(netlist, gateId)) {
        return false;
    }

    const Net& net = netlist.getNet(netId);
    const Gate& gate = netlist.getGate(gateId);
    return std::find(net.loadGateIds.begin(), net.loadGateIds.end(), gateId) !=
               net.loadGateIds.end() &&
           std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), netId) !=
               gate.inputNetIds.end();
}

std::vector<int> activeConeNetIdsForName(const Netlist& netlist,
                                         const std::string& netName) {
    std::vector<int> activeNetIds;
    const int scalarNetId = netlist.getNetId(netName);
    if (isActiveConeNet(netlist, scalarNetId)) {
        activeNetIds.push_back(scalarNetId);
        return activeNetIds;
    }

    const std::vector<int> bitNetIds = netlist.expandNetToBits(netName);
    activeNetIds.reserve(bitNetIds.size());
    for (int bitNetId : bitNetIds) {
        if (isActiveConeNet(netlist, bitNetId)) {
            activeNetIds.push_back(bitNetId);
        }
    }
    return activeNetIds;
}

// 將 cone path 回傳的 net ID 序列轉成 net name 序列。
// findLongestPathInCone / findShortestPathInCone 的 raw path 存的是 net ID，不是 gate ID。
std::vector<std::string> coneNetPathToNames(
    const Netlist& netlist,
    const std::vector<int>& netPath) {
    std::vector<std::string> pathNames;
    pathNames.reserve(netPath.size());
    for (int netId : netPath) {
        if (isActiveConeNet(netlist, netId)) {
            pathNames.push_back(netlist.getNet(netId).name);
        }
    }
    return pathNames;
}

bool isValidConeRankMetric(ConeRankMetric metric) {
    switch (metric) {
    case ConeRankMetric::ScopeGateCount:
    case ConeRankMetric::FilteredGateCount:
    case ConeRankMetric::NetCount:
        return true;
    }
    return false;
}

bool isValidConeRankMode(ConeRankMode mode) {
    switch (mode) {
    case ConeRankMode::Highest:
    case ConeRankMode::Lowest:
    case ConeRankMode::NthHighest:
    case ConeRankMode::NthLowest:
    case ConeRankMode::Top:
    case ConeRankMode::Bottom:
        return true;
    }
    return false;
}

bool isDescendingConeRank(ConeRankMode mode) {
    return mode == ConeRankMode::Highest ||
           mode == ConeRankMode::NthHighest ||
           mode == ConeRankMode::Top;
}

bool isValidConeMetricPredicate(ConeMetricPredicate predicate) {
    switch (predicate) {
    case ConeMetricPredicate::Equal:
    case ConeMetricPredicate::NotEqual:
    case ConeMetricPredicate::GreaterThan:
    case ConeMetricPredicate::GreaterOrEqual:
    case ConeMetricPredicate::LessThan:
    case ConeMetricPredicate::LessOrEqual:
    case ConeMetricPredicate::BetweenInclusive:
        return true;
    }
    return false;
}

bool matchesConeMetricPredicate(size_t metricValue,
                                ConeMetricPredicate predicate,
                                size_t value,
                                size_t upperValue) {
    switch (predicate) {
    case ConeMetricPredicate::Equal: return metricValue == value;
    case ConeMetricPredicate::NotEqual: return metricValue != value;
    case ConeMetricPredicate::GreaterThan: return metricValue > value;
    case ConeMetricPredicate::GreaterOrEqual: return metricValue >= value;
    case ConeMetricPredicate::LessThan: return metricValue < value;
    case ConeMetricPredicate::LessOrEqual: return metricValue <= value;
    case ConeMetricPredicate::BetweenInclusive:
        return metricValue >= value && metricValue <= upperValue;
    }
    return false;
}

size_t coneMetricValue(const ConeFilterEntry& entry,
                       ConeRankMetric metric) {
    switch (metric) {
    case ConeRankMetric::ScopeGateCount: return entry.scopeGateCount;
    case ConeRankMetric::FilteredGateCount: return entry.filteredGateCount;
    case ConeRankMetric::NetCount: return entry.netCount;
    }
    return 0;
}

} // namespace

//  支援 Bus 的 Transitive Fanin Cone (多源 BFS)
ConeResult Netlist::getTransitiveFaninCone(const std::string& netName) const {
    ConeResult result;
    
    // 展開 Bus，取得所有起點
    std::vector<int> startIds = activeConeNetIdsForName(*this, netName);
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

        if (!isActiveConeNet(*this, currNetId)) {
            continue;
        }
        const Net& net = nets[currNetId];

        if (hasConsistentConeDriver(*this, currNetId, net.driverGateId)) {
            const Gate& driver = gates[net.driverGateId];

            if (driver.type == GateType::DFF) {
                result.boundaryGateIds.insert(driver.id);
                continue;
            }

            for (int i = 0; i < (int)driver.inputNetIds.size(); i++) {
                int inNetId = driver.inputNetIds[i];
                if (!hasConsistentConeLoad(*this, inNetId, driver.id)) continue;
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
    std::vector<int> startIds = activeConeNetIdsForName(*this, netName);
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

        if (!isActiveConeNet(*this, currNetId)) {
            continue;
        }
        const Net& net = nets[currNetId];

        for (int i = 0; i < (int)net.loadGateIds.size(); i++) {
            int gateId = net.loadGateIds[i];
            if (!hasConsistentConeLoad(*this, currNetId, gateId)) continue;
            const Gate& gate = gates[gateId];

            if (gate.type == GateType::DFF) continue;

            int outNetId = gate.outputNetId;
            if (!hasConsistentConeDriver(*this, outNetId, gateId)) continue;

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
    if (!isActiveConeGate(*this, gateId)) return ConeResult(); // 找不到該 Gate

    const Gate& gate = gates[gateId];
    
    // 如果這個 Gate 沒有輸出線 (例如輸出懸空)
    if (!hasConsistentConeDriver(*this, gate.outputNetId, gateId)) return ConeResult();

    // Gate 的 Fanin 錐，就是它「輸出線」的 Fanin 錐
    return getTransitiveFaninCone(nets[gate.outputNetId].name);
}

ConeResult Netlist::getGateTransitiveFanoutCone(const std::string& gateName) const {
    int gateId = getGateId(gateName);
    if (!isActiveConeGate(*this, gateId)) return ConeResult();

    const Gate& gate = gates[gateId];
    if (!hasConsistentConeDriver(*this, gate.outputNetId, gateId)) return ConeResult();

    // Gate 的 Fanout 錐，就是它「輸出線」的 Fanout 錐
    return getTransitiveFanoutCone(nets[gate.outputNetId].name);
}

// 將 ConeResult 內的 net set 轉成排序後的 net ID 陣列，讓回傳結果穩定。
std::vector<int> Netlist::getConeNetIds(const ConeResult& cone) const {
    std::vector<int> netIds;
    netIds.reserve(cone.netIds.size());
    for (int netId : cone.netIds) {
        if (isActiveConeNet(*this, netId)) {
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
        if (!isActiveConeNet(*this, fromNetId)) {
            continue;
        }

        for (int toNetId : edgeList.second) {
            if (!isActiveConeNet(*this, toNetId)) {
                continue;
            }

            const int fromDriverId = nets[fromNetId].driverGateId;
            if (hasConsistentConeDriver(*this, fromNetId, fromDriverId)) {
                const Gate& gate = gates[fromDriverId];
                if (gate.type != GateType::DFF &&
                    hasConsistentConeLoad(*this, toNetId, fromDriverId)) {
                    gateSet.insert(fromDriverId);
                    continue;
                }
            }

            const int toDriverId = nets[toNetId].driverGateId;
            if (hasConsistentConeDriver(*this, toNetId, toDriverId)) {
                const Gate& gate = gates[toDriverId];
                if (gate.type != GateType::DFF &&
                    hasConsistentConeLoad(*this, fromNetId, toDriverId)) {
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

// Cone report gate membership includes sequential gates reached at a fanin
// boundary. The structural helper above intentionally remains combinational so
// rewrite, mapping, optimization, and functional-analysis scopes do not change.
std::vector<int> Netlist::getConeGateIdsIncludingBoundaries(
    const ConeResult& cone) const {
    std::vector<int> gateIds = getConeGateIds(cone);
    gateIds.reserve(gateIds.size() + cone.boundaryGateIds.size());
    for (int gateId : cone.boundaryGateIds) {
        if (!isActiveConeGate(*this, gateId) ||
            gates[gateId].type != GateType::DFF) {
            continue;
        }
        gateIds.push_back(gateId);
    }
    std::sort(gateIds.begin(), gateIds.end());
    gateIds.erase(std::unique(gateIds.begin(), gateIds.end()), gateIds.end());
    return gateIds;
}

std::vector<std::string> Netlist::getConeGateNamesIncludingBoundaries(
    const ConeResult& cone) const {
    std::vector<std::string> gateNames;
    const std::vector<int> gateIds = getConeGateIdsIncludingBoundaries(cone);
    gateNames.reserve(gateIds.size());
    for (int gateId : gateIds) {
        gateNames.push_back(gates[gateId].instName);
    }
    return gateNames;
}

size_t Netlist::getConeGateCountIncludingBoundaries(
    const ConeResult& cone) const {
    return getConeGateIdsIncludingBoundaries(cone).size();
}

// --- 針對 Net 的 Fanin ---
std::vector<std::string> Netlist::getTransitiveFaninConeGateNames(const std::string& netName) const {
    return getConeGateNamesIncludingBoundaries(getTransitiveFaninCone(netName));
}

size_t Netlist::getTransitiveFaninConeGateCount(const std::string& netName) const {
    return getConeGateCountIncludingBoundaries(getTransitiveFaninCone(netName));
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
    return getConeGateNamesIncludingBoundaries(getGateTransitiveFaninCone(gateName));
}

size_t Netlist::getGateTransitiveFaninConeGateCount(const std::string& gateName) const {
    return getConeGateCountIncludingBoundaries(getGateTransitiveFaninCone(gateName));
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
    report.gateDetailsIncluded = query.includeGateDetails;

    std::set<GateType> gateTypeFilterSet;
    for (GateType gateType : query.gateTypeFilters) {
        if (gateType == GateType::UNKNOWN) {
            report.message = "gateTypeFilters cannot contain UNKNOWN";
            return report;
        }
        gateTypeFilterSet.insert(gateType);
    }
    report.gateTypeFilterApplied = !gateTypeFilterSet.empty();
    report.appliedGateTypeFilters.assign(
        gateTypeFilterSet.begin(), gateTypeFilterSet.end());

    const auto populateGateResults = [&](const std::vector<int>& scopeGateIds) {
        std::vector<int> filteredGateIds;
        filteredGateIds.reserve(scopeGateIds.size());
        std::unordered_set<int> seenGateIds;
        seenGateIds.reserve(scopeGateIds.size());

        for (int gateId : scopeGateIds) {
            if (!isActiveConeGate(*this, gateId) ||
                !seenGateIds.insert(gateId).second) {
                continue;
            }
            ++report.scopeGateCount;

            const GateType gateType = gates[gateId].type;
            if (!gateTypeFilterSet.empty() &&
                gateTypeFilterSet.find(gateType) == gateTypeFilterSet.end()) {
                continue;
            }

            filteredGateIds.push_back(gateId);
            ++report.gateTypeCounts[gateType];
            if (query.includeNames) {
                report.gateNames.push_back(gates[gateId].instName);
            }
            if (query.includeGateDetails) {
                report.gateConnections.push_back(
                    buildGateConnectionSummary(gateId));
            }
        }

        report.gateCount = filteredGateIds.size();
        if (query.includeIds) {
            report.gateIds = std::move(filteredGateIds);
        }
    };

    const auto collectOutputConeEntries = [&](ConeRankMetric metric) {
        const std::vector<int> outputNetIds = getPrimaryOutputNetIds();
        std::vector<ConeFilterEntry> entries;
        entries.reserve(outputNetIds.size());

        for (int outputNetId : outputNetIds) {
            if (!isActiveConeNet(*this, outputNetId)) {
                continue;
            }

            const ConeResult cone =
                getTransitiveFaninCone(nets[outputNetId].name);
            const std::vector<int> scopeGateIds =
                getConeGateIdsIncludingBoundaries(cone);

            ConeFilterEntry entry;
            entry.outputNetId = outputNetId;
            entry.outputNetName = nets[outputNetId].name;
            entry.scopeGateCount = scopeGateIds.size();
            entry.netCount = getConeNetCount(cone);
            if (gateTypeFilterSet.empty()) {
                entry.filteredGateCount = entry.scopeGateCount;
            } else {
                entry.filteredGateCount = static_cast<size_t>(std::count_if(
                    scopeGateIds.begin(), scopeGateIds.end(),
                    [&](int gateId) {
                        return isActiveConeGate(*this, gateId) &&
                               gateTypeFilterSet.count(gates[gateId].type) != 0;
                    }));
            }
            entry.metricValue = coneMetricValue(entry, metric);
            entries.push_back(std::move(entry));
        }

        return entries;
    };

    switch (query.type) {
    case ConeQueryType::NetTransitiveFanin: {
        if (query.netName.empty()) {
            report.message = "NetTransitiveFanin requires netName";
            return report;
        }
        const std::vector<int> activeRootIds =
            activeConeNetIdsForName(*this, query.netName);
        if (activeRootIds.empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        report.sourceName = query.netName;
        const int scalarNetId = getNetId(query.netName);
        report.sourceId = isActiveConeNet(*this, scalarNetId)
            ? scalarNetId : activeRootIds.front();
        report.cone = getTransitiveFaninCone(query.netName);
        report.message = "Net transitive fanin cone";
        break;
    }

    case ConeQueryType::NetTransitiveFanout: {
        if (query.netName.empty()) {
            report.message = "NetTransitiveFanout requires netName";
            return report;
        }
        const std::vector<int> activeRootIds =
            activeConeNetIdsForName(*this, query.netName);
        if (activeRootIds.empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        report.sourceName = query.netName;
        const int scalarNetId = getNetId(query.netName);
        report.sourceId = isActiveConeNet(*this, scalarNetId)
            ? scalarNetId : activeRootIds.front();
        report.cone = getTransitiveFanoutCone(query.netName);
        report.message = "Net transitive fanout cone";
        break;
    }

    case ConeQueryType::GateTransitiveFanin:
        if (query.gateName.empty()) {
            report.message = "GateTransitiveFanin requires gateName";
            return report;
        }
        report.sourceId = getGateId(query.gateName);
        if (!isActiveConeGate(*this, report.sourceId)) {
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
        if (!isActiveConeGate(*this, report.sourceId)) {
            report.message = "Gate not found: " + query.gateName;
            return report;
        }
        report.sourceName = query.gateName;
        report.cone = getGateTransitiveFanoutCone(query.gateName);
        report.message = "Gate transitive fanout cone";
        break;

    case ConeQueryType::LargestOutputCone: {
        const std::vector<int> outputNetIds = getPrimaryOutputNetIds();
        std::vector<int> activeOutputNetIds;
        activeOutputNetIds.reserve(outputNetIds.size());
        for (int outputNetId : outputNetIds) {
            if (isActiveConeNet(*this, outputNetId)) {
                activeOutputNetIds.push_back(outputNetId);
            }
        }
        report.checkedOutputCount = activeOutputNetIds.size();
        if (activeOutputNetIds.empty()) {
            report.message = "No primary outputs available";
            return report;
        }

        bool found = false;
        size_t bestGateCount = 0;
        size_t bestNetCount = 0;
        ConeResult bestCone;
        int bestNetId = -1;

        for (int netId : activeOutputNetIds) {
            ConeResult cone = getTransitiveFaninCone(nets[netId].name);
            const size_t gateCount = getConeGateCountIncludingBoundaries(cone);
            const size_t netCount = getConeNetCount(cone);
            if (!found ||
                gateCount > bestGateCount ||
                (gateCount == bestGateCount && netCount > bestNetCount)) {
                found = true;
                bestGateCount = gateCount;
                bestNetCount = netCount;
                bestCone = cone;
                bestNetId = netId;
            }
        }

        if (!found || bestNetId < 0) {
            report.message = "No valid primary output cones available";
            return report;
        }

        report.sourceName = nets[bestNetId].name;
        report.sourceId = bestNetId;
        report.cone = bestCone;
        report.message = "Largest primary output fanin cone";
        break;
    }

    case ConeQueryType::OutputConeRanking: {
        ConeRankingReport& ranking = report.rankingReport;
        ranking.metric = query.rankMetric;
        ranking.mode = query.rankMode;
        ranking.requestedRankOrCount = query.rankValue;

        if (!isValidConeRankMetric(query.rankMetric)) {
            report.message = "Unsupported output cone ranking metric";
            return report;
        }
        if (!isValidConeRankMode(query.rankMode)) {
            report.message = "Unsupported output cone ranking mode";
            return report;
        }
        if (query.rankValue == 0) {
            report.message = "Output cone rank/count must be greater than zero";
            return report;
        }
        if ((query.rankMode == ConeRankMode::Highest ||
             query.rankMode == ConeRankMode::Lowest) &&
            query.rankValue != 1) {
            report.message =
                "Highest/lowest output cone ranking uses rank value 1";
            return report;
        }

        const std::vector<ConeFilterEntry> summaries =
            collectOutputConeEntries(query.rankMetric);
        std::vector<ConeRankEntry> candidates;
        candidates.reserve(summaries.size());
        for (const ConeFilterEntry& summary : summaries) {
            ConeRankEntry entry;
            entry.outputNetId = summary.outputNetId;
            entry.outputNetName = summary.outputNetName;
            entry.scopeGateCount = summary.scopeGateCount;
            entry.filteredGateCount = summary.filteredGateCount;
            entry.netCount = summary.netCount;
            entry.metricValue = summary.metricValue;
            candidates.push_back(std::move(entry));
        }

        ranking.checkedOutputCount = candidates.size();
        report.checkedOutputCount = ranking.checkedOutputCount;
        const bool descending = isDescendingConeRank(query.rankMode);
        std::sort(candidates.begin(), candidates.end(),
                  [descending](const ConeRankEntry& lhs,
                               const ConeRankEntry& rhs) {
            if (lhs.metricValue != rhs.metricValue) {
                return descending ? lhs.metricValue > rhs.metricValue
                                  : lhs.metricValue < rhs.metricValue;
            }
            if (lhs.outputNetName != rhs.outputNetName) {
                return lhs.outputNetName < rhs.outputNetName;
            }
            return lhs.outputNetId < rhs.outputNetId;
        });

        size_t currentRank = 0;
        size_t previousMetricValue = 0;
        bool havePrevious = false;
        for (ConeRankEntry& entry : candidates) {
            if (!havePrevious || entry.metricValue != previousMetricValue) {
                ++currentRank;
                previousMetricValue = entry.metricValue;
                havePrevious = true;
            }
            entry.rank = currentRank;
        }
        ranking.distinctMetricLevelCount = currentRank;

        const bool multiLevel = query.rankMode == ConeRankMode::Top ||
                                query.rankMode == ConeRankMode::Bottom;
        const size_t selectedLevelLimit = multiLevel ? query.rankValue : 1;
        const size_t selectedRank =
            (query.rankMode == ConeRankMode::NthHighest ||
             query.rankMode == ConeRankMode::NthLowest)
                ? query.rankValue : 1;
        for (const ConeRankEntry& entry : candidates) {
            const bool selected = multiLevel
                ? entry.rank <= selectedLevelLimit
                : entry.rank == selectedRank;
            if (selected) {
                ranking.rankedOutputs.push_back(entry);
            }
        }

        ranking.selectedMetricLevelCount = multiLevel
            ? std::min(query.rankValue, ranking.distinctMetricLevelCount)
            : (selectedRank <= ranking.distinctMetricLevelCount ? 1 : 0);
        ranking.resultOutputCount = ranking.rankedOutputs.size();
        ranking.requestedRankExists =
            query.rankValue <= ranking.distinctMetricLevelCount;
        ranking.ok = true;
        ranking.message = "Primary output cone ranking report";

        report.ok = true;
        report.exists = true;
        report.message = ranking.message;
        if (ranking.rankedOutputs.empty()) {
            return report;
        }

        const ConeRankEntry& primary = ranking.rankedOutputs.front();
        report.sourceName = primary.outputNetName;
        report.sourceId = primary.outputNetId;
        report.cone = getTransitiveFaninCone(primary.outputNetName);
        break;
    }

    case ConeQueryType::OutputConeFilter: {
        ConeFilterReport& filter = report.filterReport;
        filter.metric = query.rankMetric;
        filter.predicate = query.metricPredicate;
        filter.value = query.metricValue;
        filter.upperValue = query.metricUpperValue;

        if (!isValidConeRankMetric(query.rankMetric)) {
            report.message = "Unsupported output cone filter metric";
            return report;
        }
        if (!isValidConeMetricPredicate(query.metricPredicate)) {
            report.message = "Unsupported output cone metric predicate";
            return report;
        }
        if (query.metricPredicate == ConeMetricPredicate::BetweenInclusive &&
            query.metricValue > query.metricUpperValue) {
            report.message =
                "Output cone filter lower bound exceeds upper bound";
            return report;
        }
        if (query.includeGateDetails || query.includeLocalPaths) {
            report.message =
                "OutputConeFilter is summary-only and does not support gate details or local paths";
            return report;
        }

        std::vector<ConeFilterEntry> candidates =
            collectOutputConeEntries(query.rankMetric);
        filter.checkedOutputCount = candidates.size();
        report.checkedOutputCount = filter.checkedOutputCount;

        for (ConeFilterEntry& entry : candidates) {
            if (matchesConeMetricPredicate(entry.metricValue,
                                           query.metricPredicate,
                                           query.metricValue,
                                           query.metricUpperValue)) {
                filter.matchedOutputs.push_back(std::move(entry));
            }
        }
        std::sort(filter.matchedOutputs.begin(), filter.matchedOutputs.end(),
                  [](const ConeFilterEntry& lhs,
                     const ConeFilterEntry& rhs) {
            if (lhs.outputNetName != rhs.outputNetName) {
                return lhs.outputNetName < rhs.outputNetName;
            }
            return lhs.outputNetId < rhs.outputNetId;
        });

        filter.matchedOutputCount = filter.matchedOutputs.size();
        filter.ok = true;
        filter.message = "Primary output cone metric filter report";
        report.ok = true;
        report.exists = true;
        report.message = filter.message;
        return report;
    }

    case ConeQueryType::SharedFaninGates: {
        if (query.netName.empty() || query.secondNetName.empty()) {
            report.message = "SharedFaninGates requires netName and secondNetName";
            return report;
        }
        const std::vector<int> firstRootIds =
            activeConeNetIdsForName(*this, query.netName);
        if (firstRootIds.empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        const std::vector<int> secondRootIds =
            activeConeNetIdsForName(*this, query.secondNetName);
        if (secondRootIds.empty()) {
            report.message = "Net not found: " + query.secondNetName;
            return report;
        }

        report.sourceName = query.netName;
        report.sourceId = getNetId(query.netName);
        if (!isActiveConeNet(*this, report.sourceId)) {
            report.sourceId = firstRootIds.front();
        }
        report.secondSourceName = query.secondNetName;
        report.secondSourceId = getNetId(query.secondNetName);
        if (!isActiveConeNet(*this, report.secondSourceId)) {
            report.secondSourceId = secondRootIds.front();
        }

        const std::vector<int> firstGateIds =
            getConeGateIdsIncludingBoundaries(
                getTransitiveFaninCone(query.netName));
        const std::vector<int> secondGateIds =
            getConeGateIdsIncludingBoundaries(
                getTransitiveFaninCone(query.secondNetName));
        std::vector<int> sharedGateIds;
        std::set_intersection(
            firstGateIds.begin(), firstGateIds.end(),
            secondGateIds.begin(), secondGateIds.end(),
            std::back_inserter(sharedGateIds));

        report.ok = true;
        report.exists = true;
        report.message = !sharedGateIds.empty()
            ? "Shared fanin cone gates"
            : "No gates are shared between the two fanin cones";

        if (query.includeIds) {
            report.rootNetIds = {report.sourceId, report.secondSourceId};
        }
        if (query.includeNames) {
            report.rootNetNames = {report.sourceName, report.secondSourceName};
        }

        populateGateResults(sharedGateIds);
        return report;
    }
    }

    if (report.cone.rootNetIds.empty()) {
        report.message += " is empty";
        return report;
    }

    report.ok = true;
    report.exists = true;
    report.netCount = getConeNetCount(report.cone);
    const std::vector<int> coneGateIds =
        getConeGateIdsIncludingBoundaries(report.cone);
    populateGateResults(coneGateIds);

    if (query.includeIds) {
        report.rootNetIds = report.cone.rootNetIds;
        report.netIds = getConeNetIds(report.cone);
    }

    if (query.includeNames) {
        for (int rootNetId : report.cone.rootNetIds) {
            if (isActiveConeNet(*this, rootNetId)) {
                report.rootNetNames.push_back(nets[rootNetId].name);
            }
        }
        report.netNames = getConeNetNames(report.cone);
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

//  分析邏輯錐：尋找錐體內的最長路徑 (Local Critical Path)
//  回傳值：std::pair<深度, 路徑的 Net IDs>
//
//  NOTE (bugfix): 原本用遞迴 DFS (std::function + call stack) 走訪 cone，
//  在極深的 combinational chain（實測約 7~8 萬層 net，預設 8MB thread
//  stack）會 stack overflow / segfault。ConeResult 是 BFS 建出來的
//  net-to-net DAG，深度沒有上限保證，所以這裡改成「顯式 stack 的
//  iterative post-order traversal」，語意（cycle 偵測、tie-break 規則、
//  path 重建）與原本遞迴版完全一致，只是不再吃掉 C++ call stack。
std::pair<int, std::vector<int>> Netlist::findLongestPathInCone(const ConeResult& cone) const {
    // 記錄 <當前節點, <最大深度, 最佳路徑的下一個節點>>；語意與原遞迴版的 memo 相同。
    std::unordered_map<int, std::pair<int, int>> memo;
    // 目前仍在「進行中」路徑上的節點（尚未 finalize），用來偵測 combinational loop。
    std::unordered_set<int> onStack;

    // 顯式 DFS stack：每個 frame 是 (目前 net, 下一個要處理的 child index)。
    std::vector<std::pair<int, size_t>> frames;

    auto processRoot = [&](int rootId) {
        if (memo.count(rootId)) {
            return; // 已經在之前的 root 走訪中 finalize 過
        }

        frames.clear();
        frames.push_back({rootId, static_cast<size_t>(0)});
        onStack.insert(rootId);

        while (!frames.empty()) {
            const size_t topIdx = frames.size() - 1;
            const int currNetId = frames[topIdx].first;
            const size_t childIdx = frames[topIdx].second;

            auto it = cone.children.find(currNetId);
            const std::vector<int>* childList =
                (it != cone.children.end()) ? &it->second : nullptr;

            if (childList == nullptr || childIdx >= childList->size()) {
                // 所有 child 都處理過了，可以 finalize 這個節點。
                int maxLength = -1;
                int bestNextNode = -1; // -1 代表沒有下游 (自己是最底端)

                if (childList != nullptr && !childList->empty()) {
                    for (int childNetId : *childList) {
                        auto memoIt = memo.find(childNetId);
                        if (memoIt == memo.end()) {
                            // child 還在進行中的路徑上（combinational loop），
                            // 等同原遞迴版 dfsLongest 回傳 -1 的情況：跳過。
                            continue;
                        }
                        int childLength = memoIt->second.first;
                        if (childLength < 0) {
                            continue;
                        }
                        if (childLength + 1 > maxLength) {
                            maxLength = childLength + 1;
                            bestNextNode = childNetId;
                        }
                    }
                } else {
                    maxLength = 0;
                }

                memo[currNetId] = {maxLength, bestNextNode};
                onStack.erase(currNetId);
                frames.pop_back();
                continue;
            }

            const int childNetId = (*childList)[childIdx];
            // 先把 index 前進，之後不管這個 child 是否需要展開，父 frame 的
            // 進度都已經記錄好；這樣後面 push_back 造成 frames 重新配置也不會
            // 有 dangling reference 的問題。
            frames[topIdx].second = childIdx + 1;

            if (memo.count(childNetId) || onStack.count(childNetId)) {
                // 已經 finalize，或正在進行中 (loop) -> 不需要再展開。
                continue;
            }

            frames.push_back({childNetId, static_cast<size_t>(0)});
            onStack.insert(childNetId);
        }
    };

    // 啟動引擎
    int globalMaxLength = -1;
    int bestRootId = -1;

    for (int rootId : cone.rootNetIds) {
        processRoot(rootId);
        int length = memo.count(rootId) ? memo[rootId].first : -1;
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
/*#include "include/core/Netlist.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
            if (isValidGateId(fromDriverId) && !isGateRemoved(fromDriverId)) {
                const Gate& gate = gates[fromDriverId];
                if (gate.type != GateType::DFF &&
                    std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), toNetId) !=
                        gate.inputNetIds.end()) {
                    gateSet.insert(fromDriverId);
                    continue;
                }
            }

            const int toDriverId = nets[toNetId].driverGateId;
            if (isValidGateId(toDriverId) && !isGateRemoved(toDriverId)) {
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

    case ConeQueryType::LargestOutputCone: {
        const std::vector<int> outputNetIds = getPrimaryOutputNetIds();
        report.checkedOutputCount = outputNetIds.size();
        if (outputNetIds.empty()) {
            report.message = "No primary outputs available";
            return report;
        }

        bool found = false;
        size_t bestGateCount = 0;
        size_t bestNetCount = 0;
        ConeResult bestCone;
        int bestNetId = -1;

        for (int netId : outputNetIds) {
            if (!isValidNetId(netId)) {
                continue;
            }

            ConeResult cone = getTransitiveFaninCone(nets[netId].name);
            const size_t gateCount = getConeGateCount(cone);
            const size_t netCount = getConeNetCount(cone);
            if (!found ||
                gateCount > bestGateCount ||
                (gateCount == bestGateCount && netCount > bestNetCount)) {
                found = true;
                bestGateCount = gateCount;
                bestNetCount = netCount;
                bestCone = cone;
                bestNetId = netId;
            }
        }

        if (!found || bestNetId < 0) {
            report.message = "No valid primary output cones available";
            return report;
        }

        report.sourceName = nets[bestNetId].name;
        report.sourceId = bestNetId;
        report.cone = bestCone;
        report.message = "Largest primary output fanin cone";
        break;
    }

    case ConeQueryType::SharedFaninGates: {
        if (query.netName.empty() || query.secondNetName.empty()) {
            report.message = "SharedFaninGates requires netName and secondNetName";
            return report;
        }
        const std::vector<int> firstRootIds = expandNetToBits(query.netName);
        if (firstRootIds.empty()) {
            report.message = "Net not found: " + query.netName;
            return report;
        }
        const std::vector<int> secondRootIds = expandNetToBits(query.secondNetName);
        if (secondRootIds.empty()) {
            report.message = "Net not found: " + query.secondNetName;
            return report;
        }

        report.sourceName = query.netName;
        report.sourceId = getNetId(query.netName);
        if (!isValidNetId(report.sourceId)) {
            report.sourceId = firstRootIds.front();
        }
        report.secondSourceName = query.secondNetName;
        report.secondSourceId = getNetId(query.secondNetName);
        if (!isValidNetId(report.secondSourceId)) {
            report.secondSourceId = secondRootIds.front();
        }

        const std::vector<int> firstGateIds =
            getConeGateIds(getTransitiveFaninCone(query.netName));
        const std::vector<int> secondGateIds =
            getConeGateIds(getTransitiveFaninCone(query.secondNetName));
        std::vector<int> sharedGateIds;
        std::set_intersection(
            firstGateIds.begin(), firstGateIds.end(),
            secondGateIds.begin(), secondGateIds.end(),
            std::back_inserter(sharedGateIds));

        report.ok = true;
        report.exists = !sharedGateIds.empty();
        report.gateCount = sharedGateIds.size();
        report.rootNetIds = {report.sourceId, report.secondSourceId};
        report.rootNetNames = {report.sourceName, report.secondSourceName};
        report.message = report.exists
            ? "Shared fanin cone gates"
            : "No gates are shared between the two fanin cones";

        for (int gateId : sharedGateIds) {
            if (!isValidGateId(gateId) || isGateRemoved(gateId)) {
                continue;
            }
            ++report.gateTypeCounts[gates[gateId].type];
            if (query.includeNames) {
                report.gateNames.push_back(gates[gateId].instName);
            }
        }
        if (query.includeIds) {
            report.gateIds = std::move(sharedGateIds);
        }
        return report;
    }
    }

    if (report.cone.rootNetIds.empty()) {
        report.message += " is empty";
        return report;
    }

    report.ok = true;
    report.exists = true;
    report.netCount = getConeNetCount(report.cone);
    report.gateCount = getConeGateCount(report.cone);
    const std::vector<int> coneGateIds = getConeGateIds(report.cone);
    for (int gateId : coneGateIds) {
        if (isValidGateId(gateId) && !isGateRemoved(gateId)) {
            ++report.gateTypeCounts[gates[gateId].type];
        }
    }

    if (query.includeIds) {
        report.rootNetIds = report.cone.rootNetIds;
        report.netIds = getConeNetIds(report.cone);
        report.gateIds = coneGateIds;
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
}*/
