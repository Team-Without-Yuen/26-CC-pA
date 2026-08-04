#include "include/core/Netlist.h"
#include <iostream>
#include <algorithm>
#include <queue>
#include <vector>
#include <functional>

namespace {

Netlist::CombinationalPath buildCriticalPathToNet(
    const Netlist& netlist,
    int targetNetId,
    const std::vector<int>& netLevels) {
    Netlist::CombinationalPath path;
    if (targetNetId < 0 ||
        targetNetId >= static_cast<int>(netlist.getNetCount()) ||
        targetNetId >= static_cast<int>(netLevels.size()) ||
        netLevels[targetNetId] < 0) {
        return path;
    }

    int currentNetId = targetNetId;
    path.netIds.push_back(currentNetId);
    while (currentNetId >= 0 &&
           currentNetId < static_cast<int>(netlist.getNetCount())) {
        const Net& currentNet = netlist.getNet(currentNetId);
        const int driverGateId = currentNet.driverGateId;
        if (driverGateId < 0 ||
            driverGateId >= static_cast<int>(netlist.getGateCount())) {
            break;
        }

        const Gate& driverGate = netlist.getGate(driverGateId);
        if (driverGate.type == GateType::DFF || driverGate.inputNetIds.empty()) {
            break;
        }

        int bestInputNetId = -1;
        int bestInputLevel = -1;
        for (int inputNetId : driverGate.inputNetIds) {
            if (inputNetId < 0 ||
                inputNetId >= static_cast<int>(netLevels.size()) ||
                netLevels[inputNetId] < 0) {
                continue;
            }
            if (netLevels[inputNetId] > bestInputLevel) {
                bestInputLevel = netLevels[inputNetId];
                bestInputNetId = inputNetId;
            }
        }

        if (bestInputNetId < 0) {
            break;
        }
        path.gateIds.push_back(driverGateId);
        path.netIds.push_back(bestInputNetId);
        currentNetId = bestInputNetId;
    }

    std::reverse(path.netIds.begin(), path.netIds.end());
    std::reverse(path.gateIds.begin(), path.gateIds.end());
    return path;
}

// 建立單一 endpoint 的 DepthReport，集中填入 endpoint metadata 與 critical path。
Netlist::DepthReport makeDepthReportForNet(
    const Netlist& netlist,
    const std::string& netName,
    Netlist::DepthEndpointType endpointType,
    const std::string& endpointName,
    const std::vector<int>* precomputedNetLevels = nullptr,
    bool buildCriticalPath = true) {
    Netlist::DepthReport report;
    const int netId = netlist.getNetId(netName);
    if (netId < 0 || netId >= static_cast<int>(netlist.getNetCount())) {
        return report;
    }

    report.endpointType = endpointType;
    report.endpointName = endpointName;
    report.endpointNetId = netId;
    if (precomputedNetLevels != nullptr &&
        netId < static_cast<int>(precomputedNetLevels->size())) {
        report.depth = (*precomputedNetLevels)[netId];
    } else {
        report.depth = netlist.getMaxDepthToNet(netName);
    }
    // buildCriticalPath defaults to true so every existing call site
    // (analyzeDepthToNet / analyzePrimaryOutputDepths / analyzeDffDDepths)
    // keeps its original behavior unchanged. Only runDepthQuery() passes
    // false, and only when query.includeCriticalPath is false, to avoid
    // reconstructing a path that is going to be thrown away anyway.
    if (report.depth >= 0 && buildCriticalPath) {
        report.criticalPath = precomputedNetLevels != nullptr
            ? buildCriticalPathToNet(netlist, netId, *precomputedNetLevels)
            : netlist.findCriticalPathToNet(netName);
    }
    return report;
}

} // namespace

// 計算每個 net 的 combinational level；PI、constant、DFF.Q 視為 level 0。
std::vector<int> Netlist::computeNetLevels() const {
    std::vector<int> netLevels(nets.size(), -1);
    std::vector<unsigned char> processedGates(gates.size(), 0);
    std::queue<int> readyNets;

    auto markSourceNet = [&](int netId) {
        if (netId < 0 || netId >= static_cast<int>(nets.size())) {
            return;
        }
        if (netLevels[netId] == -1) {
            netLevels[netId] = 0;
            readyNets.push(netId);
        }
    };

    for (const Net& net : nets) {
        if (net.isPI || net.isConst) {
            markSourceNet(net.id);
        }
    }

    for (const Gate& gate : gates) {
        if (gate.type == GateType::DFF) {
            markSourceNet(gate.outputNetId);
        }
    }

    while (!readyNets.empty()) {
        const int currentNetId = readyNets.front();
        readyNets.pop();

        if (currentNetId < 0 || currentNetId >= static_cast<int>(nets.size())) {
            continue;
        }

        const Net& currentNet = nets[currentNetId];
        for (int gateId : currentNet.loadGateIds) {
            if (gateId < 0 || gateId >= static_cast<int>(gates.size()) ||
                processedGates[gateId] != 0) {
                continue;
            }

            const Gate& gate = gates[gateId];
            if (gate.type == GateType::DFF ||
                gate.outputNetId < 0 ||
                gate.outputNetId >= static_cast<int>(nets.size()) ||
                gate.inputNetIds.empty()) {
                continue;
            }

            bool allInputsKnown = true;
            int maxInputLevel = -1;
            for (int inputNetId : gate.inputNetIds) {
                if (inputNetId < 0 ||
                    inputNetId >= static_cast<int>(nets.size()) ||
                    netLevels[inputNetId] < 0) {
                    allInputsKnown = false;
                    break;
                }
                maxInputLevel = std::max(maxInputLevel, netLevels[inputNetId]);
            }

            if (!allInputsKnown) {
                continue;
            }

            const int outputLevel = maxInputLevel + 1;
            processedGates[gateId] = 1;
            if (netLevels[gate.outputNetId] < outputLevel) {
                netLevels[gate.outputNetId] = outputLevel;
                readyNets.push(gate.outputNetId);
            }
        }
    }

    return netLevels;
}

// 計算每個 gate output 的 combinational level；DFF 或無法計算者回傳 -1。
std::vector<int> Netlist::computeGateLevels() const {
    const std::vector<int> netLevels = computeNetLevels();
    std::vector<int> gateLevels(gates.size(), -1);

    for (const Gate& gate : gates) {
        if (gate.type == GateType::DFF ||
            gate.outputNetId < 0 ||
            gate.outputNetId >= static_cast<int>(nets.size()) ||
            gate.inputNetIds.empty()) {
            continue;
        }

        bool allInputsKnown = true;
        int maxInputLevel = -1;
        for (int inputNetId : gate.inputNetIds) {
            if (inputNetId < 0 ||
                inputNetId >= static_cast<int>(nets.size()) ||
                netLevels[inputNetId] < 0) {
                allInputsKnown = false;
                break;
            }
            maxInputLevel = std::max(maxInputLevel, netLevels[inputNetId]);
        }

        if (allInputsKnown) {
            gateLevels[gate.id] = maxInputLevel + 1;
        }
    }

    return gateLevels;
}

// 查詢指定 net 的最大 fanin depth；找不到或無法計算時回傳 -1。
int Netlist::getMaxDepthToNet(const std::string& netName) const {
    const int netId = getNetId(netName);
    if (netId < 0 || netId >= static_cast<int>(nets.size())) {
        return -1;
    }

    const std::vector<int> netLevels = computeNetLevels();
    return netLevels[netId];
}

// 找到到指定 net 的一條 critical path；不存在或無法計算時回傳空路徑。
Netlist::CombinationalPath Netlist::findCriticalPathToNet(const std::string& netName) const {
    const int targetNetId = getNetId(netName);
    if (targetNetId < 0 || targetNetId >= static_cast<int>(nets.size())) {
        return CombinationalPath();
    }

    const std::vector<int> netLevels = computeNetLevels();
    return buildCriticalPathToNet(*this, targetNetId, netLevels);
}

// 分析指定 net 的 depth、endpoint net ID 與 critical path。
Netlist::DepthReport Netlist::analyzeDepthToNet(const std::string& netName) const {
    return makeDepthReportForNet(*this, netName, DepthEndpointType::SpecificNet, netName);
}

// 分析所有 primary output net 的 depth；bus output 會展開成每一個 bit net。
std::vector<Netlist::DepthReport> Netlist::analyzePrimaryOutputDepths() const {
    std::vector<DepthReport> reports;
    const std::vector<int> outputNetIds = getPrimaryOutputNetIds();
    const std::vector<int> netLevels = computeNetLevels();
    reports.reserve(outputNetIds.size());

    for (int netId : outputNetIds) {
        if (netId < 0 || netId >= static_cast<int>(nets.size())) {
            continue;
        }
        reports.push_back(makeDepthReportForNet(*this, nets[netId].name,
                                                DepthEndpointType::PrimaryOutput,
                                                nets[netId].name,
                                                &netLevels));
    }
    return reports;
}

// 列出所有 depth 大於 limit 的 primary output 名稱；bus bit 會使用展開後的 net 名稱。
std::vector<std::string> Netlist::getPrimaryOutputsWithDepthGreaterThan(int limit) const {
    std::vector<std::string> outputNames;
    const std::vector<DepthReport> reports = analyzePrimaryOutputDepths();

    for (const DepthReport& report : reports) {
        if (report.endpointNetId >= 0 &&
            report.endpointNetId < static_cast<int>(nets.size()) &&
            report.depth > limit) {
            outputNames.push_back(nets[report.endpointNetId].name);
        }
    }
    return outputNames;
}

// 分析所有 DFF D-pin 的 depth；D pin 未連接或無法計算時保留 depth = -1。
std::vector<Netlist::DepthReport> Netlist::analyzeDffDDepths() const {
    std::vector<DepthReport> reports;
    const std::vector<int> netLevels = computeNetLevels();

    for (const Gate& gate : gates) {
        if (gate.type != GateType::DFF) {
            continue;
        }

        DepthReport report;
        const int dNetId = getGateInputNetId(gate.id, "D");
        if (dNetId >= 0 && dNetId < static_cast<int>(nets.size())) {
            report = makeDepthReportForNet(*this, nets[dNetId].name,
                                           DepthEndpointType::DffD,
                                           gate.instName + ".D",
                                           &netLevels);
        }
        reports.push_back(report);
    }
    return reports;
}

// 列出所有 D-pin depth 大於 limit 的 DFF instance name。
std::vector<std::string> Netlist::getDffsWithDDepthGreaterThan(int limit) const {
    std::vector<std::string> dffNames;
    // FIX(#1): compute net levels ONCE for the whole design instead of
    // calling getMaxDepthToNet() (which internally reruns computeNetLevels()
    // over the entire netlist) inside the per-DFF loop. The old code was
    // O(numDFF * (V+E)); this is O(V+E) total, same as
    // getPrimaryOutputsWithDepthGreaterThan() / analyzeDffDDepths().
    const std::vector<int> netLevels = computeNetLevels();

    for (const Gate& gate : gates) {
        if (gate.type != GateType::DFF) {
            continue;
        }

        const int dNetId = getGateInputNetId(gate.id, "D");
        if (dNetId < 0 || dNetId >= static_cast<int>(nets.size()) ||
            dNetId >= static_cast<int>(netLevels.size())) {
            continue;
        }

        const int depth = netLevels[dNetId];
        if (depth > limit) {
            dffNames.push_back(gate.instName);
        }
    }
    return dffNames;
}

// 在所有 primary output 與 DFF D-pin endpoints 中，找出 depth 最大的一條 critical path。
Netlist::DepthReport Netlist::findGlobalCriticalPath() const {
    DepthReport bestReport;

    auto considerReport = [&](const DepthReport& report) {
        if (report.depth > bestReport.depth) {
            bestReport = report;
        }
    };

    for (const DepthReport& report : analyzePrimaryOutputDepths()) {
        considerReport(report);
    }

    for (const DepthReport& report : analyzeDffDDepths()) {
        considerReport(report);
    }

    return bestReport;
}

// 找出所有 depth 大於 maxDepth 的 timing endpoints；endpoint 範圍包含 PO 與 DFF.D。
std::vector<Netlist::DepthReport> Netlist::findEndpointsExceedingDepth(int maxDepth) const {
    std::vector<DepthReport> reports;

    auto appendIfExceeded = [&](const DepthReport& report) {
        if (report.depth > maxDepth) {
            reports.push_back(report);
        }
    };

    for (const DepthReport& report : analyzePrimaryOutputDepths()) {
        appendIfExceeded(report);
    }

    for (const DepthReport& report : analyzeDffDDepths()) {
        appendIfExceeded(report);
    }

    return reports;
}

// 執行統一 DepthQuery；這層只負責把既有 DepthAnalysis helper 整理成一致的 DepthReportSet。
Netlist::DepthReportSet Netlist::runDepthQuery(const DepthQuery& query) const {
    DepthReportSet result;
    result.type = query.type;
    result.threshold = query.threshold;
    result.gateName = query.gateName;

    auto clearCriticalPathsIfNeeded = [&]() {
        if (query.includeCriticalPath) {
            return;
        }
        for (DepthReport& report : result.reports) {
            report.criticalPath = CombinationalPath();
        }
        result.worst.criticalPath = CombinationalPath();
    };

    auto updateSummary = [&]() {
        result.count = result.reports.size();
        result.exists = result.count > 0;
        result.worst = DepthReport();
        for (const DepthReport& report : result.reports) {
            if (report.depth > result.worst.depth) {
                result.worst = report;
            }
        }
        clearCriticalPathsIfNeeded();
    };

    // FIX(#2): same logic as analyzePrimaryOutputDepths() / analyzeDffDDepths(),
    // but parameterized on buildCriticalPath so query.includeCriticalPath can
    // actually skip the O(depth)-per-endpoint path reconstruction instead of
    // building every path and clearing it afterwards via
    // clearCriticalPathsIfNeeded(). Defined as member lambdas (not free
    // functions) so they keep the exact same private-member access as the
    // original member functions they mirror.
    auto collectPrimaryOutputReports = [&](const std::vector<int>& netLevels,
                                            bool buildCriticalPath) {
        std::vector<DepthReport> reports;
        const std::vector<int> outputNetIds = getPrimaryOutputNetIds();
        reports.reserve(outputNetIds.size());
        for (int netId : outputNetIds) {
            if (netId < 0 || netId >= static_cast<int>(nets.size())) {
                continue;
            }
            reports.push_back(makeDepthReportForNet(*this, nets[netId].name,
                                                      DepthEndpointType::PrimaryOutput,
                                                      nets[netId].name,
                                                      &netLevels, buildCriticalPath));
        }
        return reports;
    };

    auto collectDffDReports = [&](const std::vector<int>& netLevels,
                                   bool buildCriticalPath) {
        std::vector<DepthReport> reports;
        for (const Gate& gate : gates) {
            if (gate.type != GateType::DFF) {
                continue;
            }
            DepthReport report;
            const int dNetId = getGateInputNetId(gate.id, "D");
            if (dNetId >= 0 && dNetId < static_cast<int>(nets.size())) {
                report = makeDepthReportForNet(*this, nets[dNetId].name,
                                                DepthEndpointType::DffD,
                                                gate.instName + ".D",
                                                &netLevels, buildCriticalPath);
            }
            reports.push_back(report);
        }
        return reports;
    };

    switch (query.type) {
    case DepthQueryType::SpecificNet: {
        if (query.netName.empty()) {
            result.message = "SpecificNet depth query requires netName";
            return result;
        }

        DepthReport report = analyzeDepthToNet(query.netName);
        if (report.endpointNetId < 0 || report.depth < 0) {
            result.message = "Net not found or depth unavailable: " + query.netName;
            return result;
        }

        result.ok = true;
        result.message = "Specific net depth";
        result.reports.push_back(report);
        updateSummary();
        return result;
    }

    case DepthQueryType::PrimaryOutputs: {
        const std::vector<int> netLevels = computeNetLevels();
        result.reports = collectPrimaryOutputReports(netLevels, query.includeCriticalPath);
        result.ok = true;
        result.message = "Primary output depth reports";
        updateSummary();
        return result;
    }

    case DepthQueryType::DffD: {
        const std::vector<int> netLevels = computeNetLevels();
        result.reports = collectDffDReports(netLevels, query.includeCriticalPath);
        result.ok = true;
        result.message = "DFF D-pin depth reports";
        updateSummary();
        return result;
    }

    case DepthQueryType::GlobalCriticalPath:
        result.worst = findGlobalCriticalPath();
        if (result.worst.endpointNetId < 0 || result.worst.depth < 0) {
            result.message = "Global critical path is unavailable";
            clearCriticalPathsIfNeeded();
            return result;
        }
        result.ok = true;
        result.message = "Global critical path";
        result.reports.push_back(result.worst);
        result.count = result.reports.size();
        result.exists = true;
        clearCriticalPathsIfNeeded();
        return result;

    case DepthQueryType::EndpointsExceedingDepth: {
        if (query.threshold < 0) {
            result.message = "EndpointsExceedingDepth requires non-negative threshold";
            return result;
        }
        if (query.includeCriticalPath) {
            // Unchanged behavior: caller wants full paths, keep using the
            // existing documented helper.
            result.reports = findEndpointsExceedingDepth(query.threshold);
        } else {
            const std::vector<int> netLevels = computeNetLevels();
            for (const DepthReport& report : collectPrimaryOutputReports(netLevels, false)) {
                if (report.depth > query.threshold) {
                    result.reports.push_back(report);
                }
            }
            for (const DepthReport& report : collectDffDReports(netLevels, false)) {
                if (report.depth > query.threshold) {
                    result.reports.push_back(report);
                }
            }
        }
        result.ok = true;
        result.message = "Timing endpoints exceeding depth threshold";
        updateSummary();
        return result;
    }

    case DepthQueryType::PrimaryOutputsExceedingDepth: {
        if (query.threshold < 0) {
            result.message = "PrimaryOutputsExceedingDepth requires non-negative threshold";
            return result;
        }
        const std::vector<int> netLevels = computeNetLevels();
        for (const DepthReport& report :
             collectPrimaryOutputReports(netLevels, query.includeCriticalPath)) {
            if (report.depth > query.threshold) {
                result.reports.push_back(report);
            }
        }
        result.ok = true;
        result.message = "Primary outputs exceeding depth threshold";
        updateSummary();
        return result;
    }

    case DepthQueryType::GateOnCriticalPath: {
        if (query.gateName.empty()) {
            result.message = "GateOnCriticalPath requires gateName";
            return result;
        }

        const int gateId = getGateId(query.gateName);
        result.gateId = gateId;
        if (gateId < 0 || gateId >= static_cast<int>(gates.size()) ||
            gates[gateId].type == GateType::UNKNOWN) {
            result.message = "Gate not found: " + query.gateName;
            return result;
        }

        const Gate& targetGate = gates[gateId];
        if (targetGate.type == GateType::DFF ||
            targetGate.outputNetId < 0 ||
            targetGate.outputNetId >= static_cast<int>(nets.size()) ||
            targetGate.inputNetIds.empty()) {
            result.ok = true;
            result.message = "Gate is not a combinational gate with a valid output";
            result.gateOnCriticalPath = false;
            return result;
        }

        // FIX(#2/#3-perf): compute net levels once and reuse for both the
        // global-worst-endpoint lookup and the gate's own input levels below,
        // instead of findGlobalCriticalPath() (2 internal computeNetLevels()
        // calls) plus a 3rd computeNetLevels() call right after it. Path
        // reconstruction is also skipped here when includeCriticalPath is
        // false.
        const std::vector<int> netLevels = computeNetLevels();
        {
            DepthReport globalWorst;
            for (const DepthReport& report :
                 collectPrimaryOutputReports(netLevels, query.includeCriticalPath)) {
                if (report.depth > globalWorst.depth) {
                    globalWorst = report;
                }
            }
            for (const DepthReport& report :
                 collectDffDReports(netLevels, query.includeCriticalPath)) {
                if (report.depth > globalWorst.depth) {
                    globalWorst = report;
                }
            }
            result.worst = globalWorst;
        }
        if (result.worst.endpointNetId < 0 || result.worst.depth < 0) {
            result.message = "Global critical path is unavailable";
            clearCriticalPathsIfNeeded();
            return result;
        }

        int bestInputLevel = -1;
        for (int inputNetId : targetGate.inputNetIds) {
            if (inputNetId >= 0 &&
                inputNetId < static_cast<int>(netLevels.size()) &&
                netLevels[inputNetId] > bestInputLevel) {
                bestInputLevel = netLevels[inputNetId];
            }
        }

        if (bestInputLevel < 0) {
            result.ok = true;
            result.message = "Gate input depth is unavailable";
            result.gateOnCriticalPath = false;
            return result;
        }

        std::vector<int> memo(nets.size(), -2);
        std::vector<unsigned char> visiting(nets.size(), 0);
        std::function<int(int)> maxRemainingDepthFromNet = [&](int netId) -> int {
            if (netId < 0 || netId >= static_cast<int>(nets.size())) {
                return -1;
            }
            if (memo[netId] != -2) {
                return memo[netId];
            }
            if (visiting[netId] != 0) {
                return -1;
            }

            visiting[netId] = 1;
            int best = nets[netId].isPO ? 0 : -1;

            for (int loadGateId : nets[netId].loadGateIds) {
                if (loadGateId < 0 || loadGateId >= static_cast<int>(gates.size())) {
                    continue;
                }

                const Gate& loadGate = gates[loadGateId];
                if (loadGate.type == GateType::DFF) {
                    if (getGateInputNetId(loadGateId, "D") == netId) {
                        best = std::max(best, 0);
                    }
                    continue;
                }

                if (loadGate.type == GateType::UNKNOWN ||
                    loadGate.outputNetId < 0 ||
                    loadGate.outputNetId >= static_cast<int>(nets.size())) {
                    continue;
                }

                const int downstreamDepth = maxRemainingDepthFromNet(loadGate.outputNetId);
                if (downstreamDepth >= 0) {
                    best = std::max(best, downstreamDepth + 1);
                }
            }

            visiting[netId] = 0;
            memo[netId] = best;
            return best;
        };

        const int remainingDepth = maxRemainingDepthFromNet(targetGate.outputNetId);
        result.gateOnCriticalPath =
            remainingDepth >= 0 &&
            bestInputLevel + 1 + remainingDepth == result.worst.depth;

        result.ok = true;
        result.exists = result.gateOnCriticalPath;
        result.message = result.gateOnCriticalPath
            ? "Gate lies on at least one global maximum-depth path"
            : "Gate does not lie on any global maximum-depth path";
        result.reports.push_back(result.worst);
        result.count = result.reports.size();
        clearCriticalPathsIfNeeded();
        return result;
    }

    case DepthQueryType::DeepestOutputCone:
        result.reports = collectPrimaryOutputReports(computeNetLevels(), query.includeCriticalPath);
        if (result.reports.empty()) {
            result.message = "No primary outputs available";
            return result;
        }
        result.ok = true;
        result.message = "Deepest primary output fanin cone";
        updateSummary();
        return result;
    }

    result.message = "Unsupported DepthQueryType";
    return result;
}
/*#include "include/core/Netlist.h"
#include <iostream>
#include <algorithm>
#include <queue>
#include <vector>
#include <functional>

namespace {

Netlist::CombinationalPath buildCriticalPathToNet(
    const Netlist& netlist,
    int targetNetId,
    const std::vector<int>& netLevels) {
    Netlist::CombinationalPath path;
    if (targetNetId < 0 ||
        targetNetId >= static_cast<int>(netlist.getNetCount()) ||
        targetNetId >= static_cast<int>(netLevels.size()) ||
        netLevels[targetNetId] < 0) {
        return path;
    }

    int currentNetId = targetNetId;
    path.netIds.push_back(currentNetId);
    while (currentNetId >= 0 &&
           currentNetId < static_cast<int>(netlist.getNetCount())) {
        const Net& currentNet = netlist.getNet(currentNetId);
        const int driverGateId = currentNet.driverGateId;
        if (driverGateId < 0 ||
            driverGateId >= static_cast<int>(netlist.getGateCount())) {
            break;
        }

        const Gate& driverGate = netlist.getGate(driverGateId);
        if (driverGate.type == GateType::DFF || driverGate.inputNetIds.empty()) {
            break;
        }

        int bestInputNetId = -1;
        int bestInputLevel = -1;
        for (int inputNetId : driverGate.inputNetIds) {
            if (inputNetId < 0 ||
                inputNetId >= static_cast<int>(netLevels.size()) ||
                netLevels[inputNetId] < 0) {
                continue;
            }
            if (netLevels[inputNetId] > bestInputLevel) {
                bestInputLevel = netLevels[inputNetId];
                bestInputNetId = inputNetId;
            }
        }

        if (bestInputNetId < 0) {
            break;
        }
        path.gateIds.push_back(driverGateId);
        path.netIds.push_back(bestInputNetId);
        currentNetId = bestInputNetId;
    }

    std::reverse(path.netIds.begin(), path.netIds.end());
    std::reverse(path.gateIds.begin(), path.gateIds.end());
    return path;
}

// 建立單一 endpoint 的 DepthReport，集中填入 endpoint metadata 與 critical path。
Netlist::DepthReport makeDepthReportForNet(
    const Netlist& netlist,
    const std::string& netName,
    Netlist::DepthEndpointType endpointType,
    const std::string& endpointName,
    const std::vector<int>* precomputedNetLevels = nullptr) {
    Netlist::DepthReport report;
    const int netId = netlist.getNetId(netName);
    if (netId < 0 || netId >= static_cast<int>(netlist.getNetCount())) {
        return report;
    }

    report.endpointType = endpointType;
    report.endpointName = endpointName;
    report.endpointNetId = netId;
    if (precomputedNetLevels != nullptr &&
        netId < static_cast<int>(precomputedNetLevels->size())) {
        report.depth = (*precomputedNetLevels)[netId];
    } else {
        report.depth = netlist.getMaxDepthToNet(netName);
    }
    if (report.depth >= 0) {
        report.criticalPath = precomputedNetLevels != nullptr
            ? buildCriticalPathToNet(netlist, netId, *precomputedNetLevels)
            : netlist.findCriticalPathToNet(netName);
    }
    return report;
}

} // namespace

// 計算每個 net 的 combinational level；PI、constant、DFF.Q 視為 level 0。
std::vector<int> Netlist::computeNetLevels() const {
    std::vector<int> netLevels(nets.size(), -1);
    std::vector<unsigned char> processedGates(gates.size(), 0);
    std::queue<int> readyNets;

    auto markSourceNet = [&](int netId) {
        if (netId < 0 || netId >= static_cast<int>(nets.size())) {
            return;
        }
        if (netLevels[netId] == -1) {
            netLevels[netId] = 0;
            readyNets.push(netId);
        }
    };

    for (const Net& net : nets) {
        if (net.isPI || net.isConst) {
            markSourceNet(net.id);
        }
    }

    for (const Gate& gate : gates) {
        if (gate.type == GateType::DFF) {
            markSourceNet(gate.outputNetId);
        }
    }

    while (!readyNets.empty()) {
        const int currentNetId = readyNets.front();
        readyNets.pop();

        if (currentNetId < 0 || currentNetId >= static_cast<int>(nets.size())) {
            continue;
        }

        const Net& currentNet = nets[currentNetId];
        for (int gateId : currentNet.loadGateIds) {
            if (gateId < 0 || gateId >= static_cast<int>(gates.size()) ||
                processedGates[gateId] != 0) {
                continue;
            }

            const Gate& gate = gates[gateId];
            if (gate.type == GateType::DFF ||
                gate.outputNetId < 0 ||
                gate.outputNetId >= static_cast<int>(nets.size()) ||
                gate.inputNetIds.empty()) {
                continue;
            }

            bool allInputsKnown = true;
            int maxInputLevel = -1;
            for (int inputNetId : gate.inputNetIds) {
                if (inputNetId < 0 ||
                    inputNetId >= static_cast<int>(nets.size()) ||
                    netLevels[inputNetId] < 0) {
                    allInputsKnown = false;
                    break;
                }
                maxInputLevel = std::max(maxInputLevel, netLevels[inputNetId]);
            }

            if (!allInputsKnown) {
                continue;
            }

            const int outputLevel = maxInputLevel + 1;
            processedGates[gateId] = 1;
            if (netLevels[gate.outputNetId] < outputLevel) {
                netLevels[gate.outputNetId] = outputLevel;
                readyNets.push(gate.outputNetId);
            }
        }
    }

    return netLevels;
}

// 計算每個 gate output 的 combinational level；DFF 或無法計算者回傳 -1。
std::vector<int> Netlist::computeGateLevels() const {
    const std::vector<int> netLevels = computeNetLevels();
    std::vector<int> gateLevels(gates.size(), -1);

    for (const Gate& gate : gates) {
        if (gate.type == GateType::DFF ||
            gate.outputNetId < 0 ||
            gate.outputNetId >= static_cast<int>(nets.size()) ||
            gate.inputNetIds.empty()) {
            continue;
        }

        bool allInputsKnown = true;
        int maxInputLevel = -1;
        for (int inputNetId : gate.inputNetIds) {
            if (inputNetId < 0 ||
                inputNetId >= static_cast<int>(nets.size()) ||
                netLevels[inputNetId] < 0) {
                allInputsKnown = false;
                break;
            }
            maxInputLevel = std::max(maxInputLevel, netLevels[inputNetId]);
        }

        if (allInputsKnown) {
            gateLevels[gate.id] = maxInputLevel + 1;
        }
    }

    return gateLevels;
}

// 查詢指定 net 的最大 fanin depth；找不到或無法計算時回傳 -1。
int Netlist::getMaxDepthToNet(const std::string& netName) const {
    const int netId = getNetId(netName);
    if (netId < 0 || netId >= static_cast<int>(nets.size())) {
        return -1;
    }

    const std::vector<int> netLevels = computeNetLevels();
    return netLevels[netId];
}

// 找到到指定 net 的一條 critical path；不存在或無法計算時回傳空路徑。
Netlist::CombinationalPath Netlist::findCriticalPathToNet(const std::string& netName) const {
    const int targetNetId = getNetId(netName);
    if (targetNetId < 0 || targetNetId >= static_cast<int>(nets.size())) {
        return CombinationalPath();
    }

    const std::vector<int> netLevels = computeNetLevels();
    return buildCriticalPathToNet(*this, targetNetId, netLevels);
}

// 分析指定 net 的 depth、endpoint net ID 與 critical path。
Netlist::DepthReport Netlist::analyzeDepthToNet(const std::string& netName) const {
    return makeDepthReportForNet(*this, netName, DepthEndpointType::SpecificNet, netName);
}

// 分析所有 primary output net 的 depth；bus output 會展開成每一個 bit net。
std::vector<Netlist::DepthReport> Netlist::analyzePrimaryOutputDepths() const {
    std::vector<DepthReport> reports;
    const std::vector<int> outputNetIds = getPrimaryOutputNetIds();
    const std::vector<int> netLevels = computeNetLevels();
    reports.reserve(outputNetIds.size());

    for (int netId : outputNetIds) {
        if (netId < 0 || netId >= static_cast<int>(nets.size())) {
            continue;
        }
        reports.push_back(makeDepthReportForNet(*this, nets[netId].name,
                                                DepthEndpointType::PrimaryOutput,
                                                nets[netId].name,
                                                &netLevels));
    }
    return reports;
}

// 列出所有 depth 大於 limit 的 primary output 名稱；bus bit 會使用展開後的 net 名稱。
std::vector<std::string> Netlist::getPrimaryOutputsWithDepthGreaterThan(int limit) const {
    std::vector<std::string> outputNames;
    const std::vector<DepthReport> reports = analyzePrimaryOutputDepths();

    for (const DepthReport& report : reports) {
        if (report.endpointNetId >= 0 &&
            report.endpointNetId < static_cast<int>(nets.size()) &&
            report.depth > limit) {
            outputNames.push_back(nets[report.endpointNetId].name);
        }
    }
    return outputNames;
}

// 分析所有 DFF D-pin 的 depth；D pin 未連接或無法計算時保留 depth = -1。
std::vector<Netlist::DepthReport> Netlist::analyzeDffDDepths() const {
    std::vector<DepthReport> reports;
    const std::vector<int> netLevels = computeNetLevels();

    for (const Gate& gate : gates) {
        if (gate.type != GateType::DFF) {
            continue;
        }

        DepthReport report;
        const int dNetId = getGateInputNetId(gate.id, "D");
        if (dNetId >= 0 && dNetId < static_cast<int>(nets.size())) {
            report = makeDepthReportForNet(*this, nets[dNetId].name,
                                           DepthEndpointType::DffD,
                                           gate.instName + ".D",
                                           &netLevels);
        }
        reports.push_back(report);
    }
    return reports;
}

// 列出所有 D-pin depth 大於 limit 的 DFF instance name。
std::vector<std::string> Netlist::getDffsWithDDepthGreaterThan(int limit) const {
    std::vector<std::string> dffNames;

    for (const Gate& gate : gates) {
        if (gate.type != GateType::DFF) {
            continue;
        }

        const int dNetId = getGateInputNetId(gate.id, "D");
        if (dNetId < 0 || dNetId >= static_cast<int>(nets.size())) {
            continue;
        }

        const int depth = getMaxDepthToNet(nets[dNetId].name);
        if (depth > limit) {
            dffNames.push_back(gate.instName);
        }
    }
    return dffNames;
}

// 在所有 primary output 與 DFF D-pin endpoints 中，找出 depth 最大的一條 critical path。
Netlist::DepthReport Netlist::findGlobalCriticalPath() const {
    DepthReport bestReport;

    auto considerReport = [&](const DepthReport& report) {
        if (report.depth > bestReport.depth) {
            bestReport = report;
        }
    };

    for (const DepthReport& report : analyzePrimaryOutputDepths()) {
        considerReport(report);
    }

    for (const DepthReport& report : analyzeDffDDepths()) {
        considerReport(report);
    }

    return bestReport;
}

// 找出所有 depth 大於 maxDepth 的 timing endpoints；endpoint 範圍包含 PO 與 DFF.D。
std::vector<Netlist::DepthReport> Netlist::findEndpointsExceedingDepth(int maxDepth) const {
    std::vector<DepthReport> reports;

    auto appendIfExceeded = [&](const DepthReport& report) {
        if (report.depth > maxDepth) {
            reports.push_back(report);
        }
    };

    for (const DepthReport& report : analyzePrimaryOutputDepths()) {
        appendIfExceeded(report);
    }

    for (const DepthReport& report : analyzeDffDDepths()) {
        appendIfExceeded(report);
    }

    return reports;
}

// 執行統一 DepthQuery；這層只負責把既有 DepthAnalysis helper 整理成一致的 DepthReportSet。
Netlist::DepthReportSet Netlist::runDepthQuery(const DepthQuery& query) const {
    DepthReportSet result;
    result.type = query.type;
    result.threshold = query.threshold;
    result.gateName = query.gateName;

    auto clearCriticalPathsIfNeeded = [&]() {
        if (query.includeCriticalPath) {
            return;
        }
        for (DepthReport& report : result.reports) {
            report.criticalPath = CombinationalPath();
        }
        result.worst.criticalPath = CombinationalPath();
    };

    auto updateSummary = [&]() {
        result.count = result.reports.size();
        result.exists = result.count > 0;
        result.worst = DepthReport();
        for (const DepthReport& report : result.reports) {
            if (report.depth > result.worst.depth) {
                result.worst = report;
            }
        }
        clearCriticalPathsIfNeeded();
    };

    switch (query.type) {
    case DepthQueryType::SpecificNet: {
        if (query.netName.empty()) {
            result.message = "SpecificNet depth query requires netName";
            return result;
        }

        DepthReport report = analyzeDepthToNet(query.netName);
        if (report.endpointNetId < 0 || report.depth < 0) {
            result.message = "Net not found or depth unavailable: " + query.netName;
            return result;
        }

        result.ok = true;
        result.message = "Specific net depth";
        result.reports.push_back(report);
        updateSummary();
        return result;
    }

    case DepthQueryType::PrimaryOutputs:
        result.reports = analyzePrimaryOutputDepths();
        result.ok = true;
        result.message = "Primary output depth reports";
        updateSummary();
        return result;

    case DepthQueryType::DffD:
        result.reports = analyzeDffDDepths();
        result.ok = true;
        result.message = "DFF D-pin depth reports";
        updateSummary();
        return result;

    case DepthQueryType::GlobalCriticalPath:
        result.worst = findGlobalCriticalPath();
        if (result.worst.endpointNetId < 0 || result.worst.depth < 0) {
            result.message = "Global critical path is unavailable";
            clearCriticalPathsIfNeeded();
            return result;
        }
        result.ok = true;
        result.message = "Global critical path";
        result.reports.push_back(result.worst);
        result.count = result.reports.size();
        result.exists = true;
        clearCriticalPathsIfNeeded();
        return result;

    case DepthQueryType::EndpointsExceedingDepth:
        if (query.threshold < 0) {
            result.message = "EndpointsExceedingDepth requires non-negative threshold";
            return result;
        }
        result.reports = findEndpointsExceedingDepth(query.threshold);
        result.ok = true;
        result.message = "Timing endpoints exceeding depth threshold";
        updateSummary();
        return result;

    case DepthQueryType::PrimaryOutputsExceedingDepth:
        if (query.threshold < 0) {
            result.message = "PrimaryOutputsExceedingDepth requires non-negative threshold";
            return result;
        }
        for (const DepthReport& report : analyzePrimaryOutputDepths()) {
            if (report.depth > query.threshold) {
                result.reports.push_back(report);
            }
        }
        result.ok = true;
        result.message = "Primary outputs exceeding depth threshold";
        updateSummary();
        return result;

    case DepthQueryType::GateOnCriticalPath: {
        if (query.gateName.empty()) {
            result.message = "GateOnCriticalPath requires gateName";
            return result;
        }

        const int gateId = getGateId(query.gateName);
        result.gateId = gateId;
        if (gateId < 0 || gateId >= static_cast<int>(gates.size()) ||
            gates[gateId].type == GateType::UNKNOWN) {
            result.message = "Gate not found: " + query.gateName;
            return result;
        }

        const Gate& targetGate = gates[gateId];
        if (targetGate.type == GateType::DFF ||
            targetGate.outputNetId < 0 ||
            targetGate.outputNetId >= static_cast<int>(nets.size()) ||
            targetGate.inputNetIds.empty()) {
            result.ok = true;
            result.message = "Gate is not a combinational gate with a valid output";
            result.gateOnCriticalPath = false;
            return result;
        }

        result.worst = findGlobalCriticalPath();
        if (result.worst.endpointNetId < 0 || result.worst.depth < 0) {
            result.message = "Global critical path is unavailable";
            clearCriticalPathsIfNeeded();
            return result;
        }

        const std::vector<int> netLevels = computeNetLevels();
        int bestInputLevel = -1;
        for (int inputNetId : targetGate.inputNetIds) {
            if (inputNetId >= 0 &&
                inputNetId < static_cast<int>(netLevels.size()) &&
                netLevels[inputNetId] > bestInputLevel) {
                bestInputLevel = netLevels[inputNetId];
            }
        }

        if (bestInputLevel < 0) {
            result.ok = true;
            result.message = "Gate input depth is unavailable";
            result.gateOnCriticalPath = false;
            return result;
        }

        std::vector<int> memo(nets.size(), -2);
        std::vector<unsigned char> visiting(nets.size(), 0);
        std::function<int(int)> maxRemainingDepthFromNet = [&](int netId) -> int {
            if (netId < 0 || netId >= static_cast<int>(nets.size())) {
                return -1;
            }
            if (memo[netId] != -2) {
                return memo[netId];
            }
            if (visiting[netId] != 0) {
                return -1;
            }

            visiting[netId] = 1;
            int best = nets[netId].isPO ? 0 : -1;

            for (int loadGateId : nets[netId].loadGateIds) {
                if (loadGateId < 0 || loadGateId >= static_cast<int>(gates.size())) {
                    continue;
                }

                const Gate& loadGate = gates[loadGateId];
                if (loadGate.type == GateType::DFF) {
                    if (getGateInputNetId(loadGateId, "D") == netId) {
                        best = std::max(best, 0);
                    }
                    continue;
                }

                if (loadGate.type == GateType::UNKNOWN ||
                    loadGate.outputNetId < 0 ||
                    loadGate.outputNetId >= static_cast<int>(nets.size())) {
                    continue;
                }

                const int downstreamDepth = maxRemainingDepthFromNet(loadGate.outputNetId);
                if (downstreamDepth >= 0) {
                    best = std::max(best, downstreamDepth + 1);
                }
            }

            visiting[netId] = 0;
            memo[netId] = best;
            return best;
        };

        const int remainingDepth = maxRemainingDepthFromNet(targetGate.outputNetId);
        result.gateOnCriticalPath =
            remainingDepth >= 0 &&
            bestInputLevel + 1 + remainingDepth == result.worst.depth;

        result.ok = true;
        result.exists = result.gateOnCriticalPath;
        result.message = result.gateOnCriticalPath
            ? "Gate lies on at least one global maximum-depth path"
            : "Gate does not lie on any global maximum-depth path";
        result.reports.push_back(result.worst);
        result.count = result.reports.size();
        clearCriticalPathsIfNeeded();
        return result;
    }

    case DepthQueryType::DeepestOutputCone:
        result.reports = analyzePrimaryOutputDepths();
        if (result.reports.empty()) {
            result.message = "No primary outputs available";
            return result;
        }
        result.ok = true;
        result.message = "Deepest primary output fanin cone";
        updateSummary();
        return result;
    }

    result.message = "Unsupported DepthQueryType";
    return result;
}
*/