#include "include/core/Netlist.h"
#include <iostream>
#include <algorithm>
#include <queue>
#include <utility>
#include <vector>
#include <functional>

namespace {

bool isActiveDepthGate(const Netlist& netlist, int gateId) {
    return netlist.isValidGateId(gateId) &&
           netlist.getGate(gateId).type != GateType::UNKNOWN;
}

bool isActiveDepthNet(const Netlist& netlist, int netId) {
    return netlist.isValidNetId(netId) && !netlist.getNet(netId).isRemoved;
}

bool hasConsistentDepthDriver(const Netlist& netlist, int netId, int gateId) {
    return isActiveDepthNet(netlist, netId) &&
           isActiveDepthGate(netlist, gateId) &&
           netlist.getNet(netId).driverGateId == gateId &&
           netlist.getGate(gateId).outputNetId == netId;
}

bool hasConsistentDepthLoad(const Netlist& netlist, int netId, int gateId) {
    if (!isActiveDepthNet(netlist, netId) ||
        !isActiveDepthGate(netlist, gateId)) {
        return false;
    }
    const Net& net = netlist.getNet(netId);
    const Gate& gate = netlist.getGate(gateId);
    return std::find(net.loadGateIds.begin(), net.loadGateIds.end(), gateId) !=
               net.loadGateIds.end() &&
           std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), netId) !=
               gate.inputNetIds.end();
}

std::vector<unsigned char> buildDepthInputConsistency(const Netlist& netlist) {
    const size_t gateCount = netlist.getGateCount();
    const size_t netCount = netlist.getNetCount();
    std::vector<int> requiredInputNetCount(gateCount, 0);
    std::vector<int> matchedInputNetCount(gateCount, 0);
    std::vector<int> lastInputGateByNet(netCount, -1);
    std::vector<int> lastMatchedNetByGate(gateCount, -1);
    std::vector<unsigned char> inputIdsValid(gateCount, 0);
    std::vector<unsigned char> allInputsConsistent(gateCount, 0);

    // Count distinct declared input nets. Tied pins are one graph edge even if
    // loadGateIds happens to contain one entry per physical pin.
    for (size_t gateIndex = 0; gateIndex < gateCount; ++gateIndex) {
        const int gateId = static_cast<int>(gateIndex);
        if (!isActiveDepthGate(netlist, gateId)) {
            continue;
        }

        const Gate& gate = netlist.getGate(gateId);
        if (gate.inputNetIds.empty()) {
            continue;
        }

        inputIdsValid[gateId] = 1;
        for (int inputNetId : gate.inputNetIds) {
            if (!isActiveDepthNet(netlist, inputNetId)) {
                inputIdsValid[gateId] = 0;
                continue;
            }
            if (lastInputGateByNet[inputNetId] != gateId) {
                lastInputGateByNet[inputNetId] = gateId;
                ++requiredInputNetCount[gateId];
            }
        }
    }

    // Scan reverse load edges once. Duplicate entries for tied pins count as
    // one matched net, while unrelated stale reverse edges are ignored.
    for (size_t netIndex = 0; netIndex < netCount; ++netIndex) {
        const int netId = static_cast<int>(netIndex);
        if (!isActiveDepthNet(netlist, netId)) {
            continue;
        }

        const Net& net = netlist.getNet(netId);
        for (int gateId : net.loadGateIds) {
            if (!isActiveDepthGate(netlist, gateId) ||
                lastMatchedNetByGate[gateId] == netId) {
                continue;
            }

            const Gate& gate = netlist.getGate(gateId);
            if (std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), netId) ==
                gate.inputNetIds.end()) {
                continue;
            }

            lastMatchedNetByGate[gateId] = netId;
            ++matchedInputNetCount[gateId];
        }
    }

    for (size_t gateIndex = 0; gateIndex < gateCount; ++gateIndex) {
        const int gateId = static_cast<int>(gateIndex);
        allInputsConsistent[gateId] =
            inputIdsValid[gateId] != 0 &&
            requiredInputNetCount[gateId] > 0 &&
            requiredInputNetCount[gateId] == matchedInputNetCount[gateId];
    }
    return allInputsConsistent;
}

void accumulateDepthStatus(DepthStatus status,
                           unsigned char& sawNoTimingPath,
                           unsigned char& sawGraphInconsistency,
                           unsigned char& sawAnalysisFailure) {
    switch (status) {
    case DepthStatus::Available:
        return;
    case DepthStatus::NoTimingPath:
        sawNoTimingPath = 1;
        return;
    case DepthStatus::GraphInconsistent:
        sawGraphInconsistency = 1;
        return;
    case DepthStatus::AnalysisFailure:
    case DepthStatus::Unknown:
        sawAnalysisFailure = 1;
        return;
    }
}

DepthStatus propagatedDepthStatus(unsigned char sawNoTimingPath,
                                  unsigned char sawGraphInconsistency,
                                  unsigned char sawAnalysisFailure) {
    if (sawGraphInconsistency != 0) {
        return DepthStatus::GraphInconsistent;
    }
    if (sawAnalysisFailure != 0) {
        return DepthStatus::AnalysisFailure;
    }
    if (sawNoTimingPath != 0) {
        return DepthStatus::NoTimingPath;
    }
    // A valid combinational gate whose inputs all have defined depth should
    // have been levelized. Reaching this case means the backend missed it.
    return DepthStatus::AnalysisFailure;
}

// Classify every active net whose numeric level is unavailable. This is a
// request-local, iterative propagation pass: undriven roots become
// NoTimingPath, that status propagates through otherwise valid gates, and
// malformed bidirectional graph edges remain distinct from an absent path.
std::vector<DepthStatus> classifyNetDepthStatuses(
    const Netlist& netlist,
    const std::vector<int>& netLevels) {
    const size_t netCount = netlist.getNetCount();
    const size_t gateCount = netlist.getGateCount();
    std::vector<DepthStatus> statuses(netCount, DepthStatus::Unknown);
    std::vector<unsigned char> eligibleGate(gateCount, 0);
    std::vector<int> pendingInputNetCount(gateCount, 0);
    std::vector<int> lastInputGateByNet(netCount, -1);
    std::vector<int> lastProcessedNetByGate(gateCount, -1);
    std::vector<unsigned char> sawNoTimingPath(gateCount, 0);
    std::vector<unsigned char> sawGraphInconsistency(gateCount, 0);
    std::vector<unsigned char> sawAnalysisFailure(gateCount, 0);
    const std::vector<unsigned char> gateInputsConsistent =
        buildDepthInputConsistency(netlist);
    std::queue<int> resolvedNets;

    auto resolveNet = [&](int netId, DepthStatus status) {
        if (!isActiveDepthNet(netlist, netId) ||
            statuses[netId] != DepthStatus::Unknown) {
            return;
        }
        statuses[netId] = status;
        resolvedNets.push(netId);
    };

    for (size_t netIndex = 0; netIndex < netCount; ++netIndex) {
        const int netId = static_cast<int>(netIndex);
        if (!isActiveDepthNet(netlist, netId)) {
            continue;
        }
        if (netId < static_cast<int>(netLevels.size()) &&
            netLevels[netId] >= 0) {
            resolveNet(netId, DepthStatus::Available);
            continue;
        }

        const Net& net = netlist.getNet(netId);
        const int driverGateId = net.driverGateId;
        if (driverGateId < 0) {
            resolveNet(netId, DepthStatus::NoTimingPath);
            continue;
        }
        if (!hasConsistentDepthDriver(netlist, netId, driverGateId)) {
            resolveNet(netId, DepthStatus::GraphInconsistent);
            continue;
        }

        const Gate& driverGate = netlist.getGate(driverGateId);
        if (driverGate.type == GateType::DFF) {
            // A consistent DFF.Q is a level-0 timing source. If it still has no
            // numeric level, the levelization result itself is inconsistent.
            resolveNet(netId, DepthStatus::AnalysisFailure);
            continue;
        }
        if (driverGate.inputNetIds.empty() ||
            gateInputsConsistent[driverGateId] == 0) {
            resolveNet(netId, DepthStatus::GraphInconsistent);
            continue;
        }

        eligibleGate[driverGateId] = 1;
    }

    // Count each distinct declared input once. Tied pins share one graph edge.
    for (size_t gateIndex = 0; gateIndex < gateCount; ++gateIndex) {
        const int gateId = static_cast<int>(gateIndex);
        if (eligibleGate[gateId] == 0) {
            continue;
        }
        const Gate& gate = netlist.getGate(gateId);
        for (int inputNetId : gate.inputNetIds) {
            if (!isActiveDepthNet(netlist, inputNetId)) {
                continue;
            }
            if (lastInputGateByNet[inputNetId] != gateId) {
                lastInputGateByNet[inputNetId] = gateId;
                ++pendingInputNetCount[gateId];
            }
        }
    }

    while (!resolvedNets.empty()) {
        const int netId = resolvedNets.front();
        resolvedNets.pop();
        const Net& net = netlist.getNet(netId);
        for (int gateId : net.loadGateIds) {
            if (!isActiveDepthGate(netlist, gateId) ||
                eligibleGate[gateId] == 0 ||
                lastProcessedNetByGate[gateId] == netId) {
                continue;
            }
            const Gate& gate = netlist.getGate(gateId);
            if (std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), netId) ==
                gate.inputNetIds.end()) {
                continue;
            }

            lastProcessedNetByGate[gateId] = netId;
            accumulateDepthStatus(statuses[netId],
                                  sawNoTimingPath[gateId],
                                  sawGraphInconsistency[gateId],
                                  sawAnalysisFailure[gateId]);
            --pendingInputNetCount[gateId];
            if (pendingInputNetCount[gateId] != 0) {
                continue;
            }

            resolveNet(gate.outputNetId,
                       propagatedDepthStatus(
                           sawNoTimingPath[gateId],
                           sawGraphInconsistency[gateId],
                           sawAnalysisFailure[gateId]));
        }
    }

    // With the contest DAG contract, unresolved active nets can only come from
    // a malformed cycle or another graph inconsistency.
    for (size_t netIndex = 0; netIndex < netCount; ++netIndex) {
        const int netId = static_cast<int>(netIndex);
        if (isActiveDepthNet(netlist, netId) &&
            statuses[netId] == DepthStatus::Unknown) {
            statuses[netId] = DepthStatus::GraphInconsistent;
        }
    }
    return statuses;
}

bool isValidDepthPredicate(DepthPredicate predicate) {
    switch (predicate) {
    case DepthPredicate::Equal:
    case DepthPredicate::NotEqual:
    case DepthPredicate::GreaterThan:
    case DepthPredicate::GreaterOrEqual:
    case DepthPredicate::LessThan:
    case DepthPredicate::LessOrEqual:
    case DepthPredicate::BetweenInclusive:
        return true;
    case DepthPredicate::None:
        return false;
    }
    return false;
}

bool isValidDepthFilterScope(DepthFilterScope scope) {
    switch (scope) {
    case DepthFilterScope::AllTimingEndpoints:
    case DepthFilterScope::PrimaryOutputs:
    case DepthFilterScope::DffD:
        return true;
    }
    return false;
}

bool matchesDepthPredicate(int depth,
                           DepthPredicate predicate,
                           int threshold,
                           int upperThreshold) {
    switch (predicate) {
    case DepthPredicate::Equal: return depth == threshold;
    case DepthPredicate::NotEqual: return depth != threshold;
    case DepthPredicate::GreaterThan: return depth > threshold;
    case DepthPredicate::GreaterOrEqual: return depth >= threshold;
    case DepthPredicate::LessThan: return depth < threshold;
    case DepthPredicate::LessOrEqual: return depth <= threshold;
    case DepthPredicate::BetweenInclusive:
        return depth >= threshold && depth <= upperThreshold;
    case DepthPredicate::None:
        return false;
    }
    return false;
}

Netlist::CombinationalPath buildCriticalPathToNet(
    const Netlist& netlist,
    int targetNetId,
    const std::vector<int>& netLevels) {
    Netlist::CombinationalPath path;
    if (!isActiveDepthNet(netlist, targetNetId) ||
        targetNetId >= static_cast<int>(netLevels.size()) ||
        netLevels[targetNetId] < 0) {
        return path;
    }

    int currentNetId = targetNetId;
    path.netIds.push_back(currentNetId);
    while (currentNetId >= 0 &&
           currentNetId < static_cast<int>(netlist.getNetCount())) {
        if (!isActiveDepthNet(netlist, currentNetId)) {
            break;
        }
        const Net& currentNet = netlist.getNet(currentNetId);
        const int driverGateId = currentNet.driverGateId;
        if (!hasConsistentDepthDriver(netlist, currentNetId, driverGateId)) {
            break;
        }

        const Gate& driverGate = netlist.getGate(driverGateId);
        if (driverGate.type == GateType::DFF || driverGate.inputNetIds.empty()) {
            break;
        }

        int bestInputNetId = -1;
        int bestInputLevel = -1;
        for (int inputNetId : driverGate.inputNetIds) {
            if (!hasConsistentDepthLoad(netlist, inputNetId, driverGateId) ||
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
    bool buildCriticalPath = true,
    const std::vector<DepthStatus>* precomputedDepthStatuses = nullptr) {
    Netlist::DepthReport report;
    const int netId = netlist.getNetId(netName);
    if (!isActiveDepthNet(netlist, netId)) {
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
        report.depthStatus = DepthStatus::Available;
    } else if (precomputedDepthStatuses != nullptr &&
               netId < static_cast<int>(precomputedDepthStatuses->size())) {
        report.depthStatus = (*precomputedDepthStatuses)[netId];
    } else {
        report.depthStatus = DepthStatus::AnalysisFailure;
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

std::vector<Netlist::DepthReport> collectPrimaryOutputDepthReports(
    const Netlist& netlist,
    const std::vector<int>& netLevels,
    const std::vector<DepthStatus>& depthStatuses,
    bool buildCriticalPath) {
    std::vector<Netlist::DepthReport> reports;
    const std::vector<int> outputNetIds = netlist.getPrimaryOutputNetIds();
    reports.reserve(outputNetIds.size());
    for (int netId : outputNetIds) {
        if (!isActiveDepthNet(netlist, netId)) {
            continue;
        }
        const Net& net = netlist.getNet(netId);
        reports.push_back(makeDepthReportForNet(
            netlist, net.name, Netlist::DepthEndpointType::PrimaryOutput,
            net.name, &netLevels, buildCriticalPath, &depthStatuses));
    }
    return reports;
}

std::vector<Netlist::DepthReport> collectDffDDepthReports(
    const Netlist& netlist,
    const std::vector<int>& netLevels,
    const std::vector<DepthStatus>& depthStatuses,
    bool buildCriticalPath) {
    std::vector<Netlist::DepthReport> reports;
    for (size_t gateIndex = 0; gateIndex < netlist.getGateCount(); ++gateIndex) {
        const int gateId = static_cast<int>(gateIndex);
        if (!isActiveDepthGate(netlist, gateId)) {
            continue;
        }
        const Gate& gate = netlist.getGate(gateId);
        if (gate.type != GateType::DFF) {
            continue;
        }

        Netlist::DepthReport report;
        report.endpointType = Netlist::DepthEndpointType::DffD;
        report.endpointName = gate.instName + ".D";
        const int dNetId = netlist.getGateInputNetId(gateId, "D");
        if (hasConsistentDepthLoad(netlist, dNetId, gateId)) {
            report = makeDepthReportForNet(
                netlist, netlist.getNet(dNetId).name,
                Netlist::DepthEndpointType::DffD, gate.instName + ".D",
                &netLevels, buildCriticalPath, &depthStatuses);
        } else {
            report.endpointNetId = isActiveDepthNet(netlist, dNetId) ? dNetId : -1;
            report.depthStatus = DepthStatus::GraphInconsistent;
        }
        reports.push_back(report);
    }
    return reports;
}

Netlist::DepthReport selectWorstDepthReport(
    const std::vector<Netlist::DepthReport>& reports) {
    Netlist::DepthReport worst;
    for (const Netlist::DepthReport& report : reports) {
        if (report.depth > worst.depth) {
            worst = report;
        }
    }
    return worst;
}

// Compute the maximum number of combinational gates from every available net
// to any timing endpoint. Nets are processed by descending arrival level, so
// every combinational output has already been solved before its input nets.
// Level buckets keep this pass iterative and O(V+E) without recursion or one
// query per gate.
std::vector<int> computeRemainingDepthToTimingEndpoints(
    const Netlist& netlist,
    const std::vector<int>& netLevels) {
    const size_t netCount = netlist.getNetCount();
    std::vector<int> remainingDepthByNet(netCount, -1);
    int maximumLevel = -1;

    for (size_t netIndex = 0; netIndex < netCount; ++netIndex) {
        const int netId = static_cast<int>(netIndex);
        if (isActiveDepthNet(netlist, netId) &&
            netId < static_cast<int>(netLevels.size())) {
            maximumLevel = std::max(maximumLevel, netLevels[netId]);
        }
    }
    if (maximumLevel < 0) {
        return remainingDepthByNet;
    }

    std::vector<std::vector<int>> netsByLevel(
        static_cast<size_t>(maximumLevel) + 1);
    for (size_t netIndex = 0; netIndex < netCount; ++netIndex) {
        const int netId = static_cast<int>(netIndex);
        if (!isActiveDepthNet(netlist, netId) ||
            netId >= static_cast<int>(netLevels.size()) ||
            netLevels[netId] < 0) {
            continue;
        }
        netsByLevel[netLevels[netId]].push_back(netId);
    }

    const std::vector<unsigned char> gateInputsConsistent =
        buildDepthInputConsistency(netlist);
    for (int level = maximumLevel; level >= 0; --level) {
        for (int netId : netsByLevel[level]) {
            const Net& net = netlist.getNet(netId);
            int best = net.isPO ? 0 : -1;
            for (int loadGateId : net.loadGateIds) {
                if (!hasConsistentDepthLoad(netlist, netId, loadGateId)) {
                    continue;
                }

                const Gate& loadGate = netlist.getGate(loadGateId);
                if (loadGate.type == GateType::DFF) {
                    if (netlist.getGateInputNetId(loadGateId, "D") == netId) {
                        best = std::max(best, 0);
                    }
                    continue;
                }

                if (gateInputsConsistent[loadGateId] == 0 ||
                    !hasConsistentDepthDriver(
                        netlist, loadGate.outputNetId, loadGateId) ||
                    loadGate.outputNetId >=
                        static_cast<int>(remainingDepthByNet.size()) ||
                    netLevels[loadGate.outputNetId] <= netLevels[netId]) {
                    continue;
                }
                const int downstreamDepth =
                    remainingDepthByNet[loadGate.outputNetId];
                if (downstreamDepth >= 0) {
                    best = std::max(best, downstreamDepth + 1);
                }
            }
            remainingDepthByNet[netId] = best;
        }
    }
    return remainingDepthByNet;
}

} // namespace

// 計算每個 net 的 combinational level；PI、constant、DFF.Q 視為 level 0。
std::vector<int> Netlist::computeNetLevels() const {
    std::vector<int> netLevels(nets.size(), -1);
    std::vector<unsigned char> processedGates(gates.size(), 0);
    const std::vector<unsigned char> gateInputsConsistent =
        buildDepthInputConsistency(*this);
    std::queue<int> readyNets;

    auto markSourceNet = [&](int netId) {
        if (!isActiveDepthNet(*this, netId)) {
            return;
        }
        if (netLevels[netId] == -1) {
            netLevels[netId] = 0;
            readyNets.push(netId);
        }
    };

    for (const Net& net : nets) {
        if (!net.isRemoved && (net.isPI || net.isConst)) {
            markSourceNet(net.id);
        }
    }

    for (const Gate& gate : gates) {
        if (gate.type == GateType::DFF &&
            hasConsistentDepthDriver(*this, gate.outputNetId, gate.id)) {
            markSourceNet(gate.outputNetId);
        }
    }

    while (!readyNets.empty()) {
        const int currentNetId = readyNets.front();
        readyNets.pop();

        if (!isActiveDepthNet(*this, currentNetId)) {
            continue;
        }

        const Net& currentNet = nets[currentNetId];
        for (int gateId : currentNet.loadGateIds) {
            if (!isActiveDepthGate(*this, gateId) ||
                processedGates[gateId] != 0) {
                continue;
            }

            const Gate& gate = gates[gateId];
            if (std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(),
                          currentNetId) == gate.inputNetIds.end()) {
                continue;
            }
            if (gate.type == GateType::DFF ||
                !hasConsistentDepthDriver(*this, gate.outputNetId, gateId) ||
                gate.inputNetIds.empty() ||
                gateInputsConsistent[gateId] == 0) {
                continue;
            }

            bool allInputsKnown = true;
            int maxInputLevel = -1;
            for (int inputNetId : gate.inputNetIds) {
                if (!isActiveDepthNet(*this, inputNetId) ||
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
    const std::vector<unsigned char> gateInputsConsistent =
        buildDepthInputConsistency(*this);
    std::vector<int> gateLevels(gates.size(), -1);

    for (const Gate& gate : gates) {
        if (!isActiveDepthGate(*this, gate.id) ||
            gate.type == GateType::DFF ||
            !hasConsistentDepthDriver(*this, gate.outputNetId, gate.id) ||
            gate.inputNetIds.empty() ||
            gateInputsConsistent[gate.id] == 0) {
            continue;
        }

        bool allInputsKnown = true;
        int maxInputLevel = -1;
        for (int inputNetId : gate.inputNetIds) {
            if (!isActiveDepthNet(*this, inputNetId) ||
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
    if (!isActiveDepthNet(*this, netId)) {
        return -1;
    }

    const std::vector<int> netLevels = computeNetLevels();
    return netLevels[netId];
}

// 找到到指定 net 的一條 critical path；不存在或無法計算時回傳空路徑。
Netlist::CombinationalPath Netlist::findCriticalPathToNet(const std::string& netName) const {
    const int targetNetId = getNetId(netName);
    if (!isActiveDepthNet(*this, targetNetId)) {
        return CombinationalPath();
    }

    const std::vector<int> netLevels = computeNetLevels();
    return buildCriticalPathToNet(*this, targetNetId, netLevels);
}

// 分析指定 net 的 depth、endpoint net ID 與 critical path。
Netlist::DepthReport Netlist::analyzeDepthToNet(const std::string& netName) const {
    const std::vector<int> netLevels = computeNetLevels();
    const std::vector<DepthStatus> depthStatuses =
        classifyNetDepthStatuses(*this, netLevels);
    return makeDepthReportForNet(
        *this, netName, DepthEndpointType::SpecificNet, netName,
        &netLevels, true, &depthStatuses);
}

// 分析所有 primary output net 的 depth；bus output 會展開成每一個 bit net。
std::vector<Netlist::DepthReport> Netlist::analyzePrimaryOutputDepths() const {
    const std::vector<int> netLevels = computeNetLevels();
    const std::vector<DepthStatus> depthStatuses =
        classifyNetDepthStatuses(*this, netLevels);
    return collectPrimaryOutputDepthReports(
        *this, netLevels, depthStatuses, true);
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
    const std::vector<int> netLevels = computeNetLevels();
    const std::vector<DepthStatus> depthStatuses =
        classifyNetDepthStatuses(*this, netLevels);
    return collectDffDDepthReports(*this, netLevels, depthStatuses, true);
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
        if (gate.type != GateType::DFF ||
            !isActiveDepthGate(*this, gate.id)) {
            continue;
        }

        const int dNetId = getGateInputNetId(gate.id, "D");
        if (!hasConsistentDepthLoad(*this, dNetId, gate.id) ||
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
    const std::vector<int> netLevels = computeNetLevels();
    const std::vector<DepthStatus> depthStatuses =
        classifyNetDepthStatuses(*this, netLevels);
    std::vector<DepthReport> reports =
        collectPrimaryOutputDepthReports(*this, netLevels, depthStatuses, false);
    std::vector<DepthReport> dffReports =
        collectDffDDepthReports(*this, netLevels, depthStatuses, false);
    reports.insert(reports.end(), dffReports.begin(), dffReports.end());

    DepthReport bestReport = selectWorstDepthReport(reports);
    if (bestReport.endpointNetId >= 0 && bestReport.depth >= 0) {
        bestReport.criticalPath =
            buildCriticalPathToNet(*this, bestReport.endpointNetId, netLevels);
    }
    return bestReport;
}

// 找出所有 depth 大於 maxDepth 的 timing endpoints；endpoint 範圍包含 PO 與 DFF.D。
std::vector<Netlist::DepthReport> Netlist::findEndpointsExceedingDepth(int maxDepth) const {
    std::vector<DepthReport> reports;
    const std::vector<int> netLevels = computeNetLevels();
    const std::vector<DepthStatus> depthStatuses =
        classifyNetDepthStatuses(*this, netLevels);
    std::vector<DepthReport> candidates =
        collectPrimaryOutputDepthReports(*this, netLevels, depthStatuses, false);
    std::vector<DepthReport> dffReports =
        collectDffDDepthReports(*this, netLevels, depthStatuses, false);
    candidates.insert(candidates.end(), dffReports.begin(), dffReports.end());

    for (DepthReport& report : candidates) {
        if (report.depth <= maxDepth) {
            continue;
        }
        report.criticalPath =
            buildCriticalPathToNet(*this, report.endpointNetId, netLevels);
        reports.push_back(std::move(report));
    }

    return reports;
}

// 執行統一 DepthQuery；這層只負責把既有 DepthAnalysis helper 整理成一致的 DepthReportSet。
Netlist::DepthReportSet Netlist::runDepthQuery(const DepthQuery& query) const {
    DepthReportSet result;
    result.type = query.type;
    result.threshold = query.threshold;
    result.upperThreshold = query.upperThreshold;
    result.filterScope = query.filterScope;
    result.predicate = query.predicate;
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

    auto updateAvailabilitySummary = [&](const std::vector<DepthReport>& reports) {
        result.definedDepthEndpointCount = 0;
        result.noTimingPathEndpointCount = 0;
        result.graphInconsistentEndpointCount = 0;
        result.analysisFailureEndpointCount = 0;
        for (const DepthReport& report : reports) {
            switch (report.depthStatus) {
            case DepthStatus::Available:
                if (report.depth >= 0) {
                    ++result.definedDepthEndpointCount;
                } else {
                    ++result.analysisFailureEndpointCount;
                }
                break;
            case DepthStatus::NoTimingPath:
                ++result.noTimingPathEndpointCount;
                break;
            case DepthStatus::GraphInconsistent:
                ++result.graphInconsistentEndpointCount;
                break;
            case DepthStatus::AnalysisFailure:
            case DepthStatus::Unknown:
                ++result.analysisFailureEndpointCount;
                break;
            }
        }
        result.unavailableEndpointCount =
            result.noTimingPathEndpointCount +
            result.graphInconsistentEndpointCount +
            result.analysisFailureEndpointCount;
        result.complete =
            result.graphInconsistentEndpointCount == 0 &&
            result.analysisFailureEndpointCount == 0;
    };

    // FIX(#2): same logic as analyzePrimaryOutputDepths() / analyzeDffDDepths(),
    // but parameterized on buildCriticalPath so query.includeCriticalPath can
    // actually skip the O(depth)-per-endpoint path reconstruction instead of
    // building every path and clearing it afterwards via
    // clearCriticalPathsIfNeeded(). Defined as member lambdas (not free
    // functions) so they keep the exact same private-member access as the
    // original member functions they mirror.
    auto collectPrimaryOutputReports = [&](
            const std::vector<int>& netLevels,
            const std::vector<DepthStatus>& depthStatuses,
            bool buildCriticalPath) {
        return collectPrimaryOutputDepthReports(
            *this, netLevels, depthStatuses, buildCriticalPath);
    };

    auto collectDffDReports = [&](const std::vector<int>& netLevels,
                                   const std::vector<DepthStatus>& depthStatuses,
                                   bool buildCriticalPath) {
        return collectDffDDepthReports(
            *this, netLevels, depthStatuses, buildCriticalPath);
    };

    auto collectAllEndpointReports = [&](const std::vector<int>& netLevels,
                                          const std::vector<DepthStatus>& depthStatuses,
                                          bool buildCriticalPath) {
        std::vector<DepthReport> reports =
            collectPrimaryOutputReports(
                netLevels, depthStatuses, buildCriticalPath);
        std::vector<DepthReport> dffReports =
            collectDffDReports(netLevels, depthStatuses, buildCriticalPath);
        reports.insert(reports.end(), dffReports.begin(), dffReports.end());
        return reports;
    };

    auto collectScopedEndpointReports = [&](
            const std::vector<int>& netLevels,
            const std::vector<DepthStatus>& depthStatuses) {
        switch (query.filterScope) {
        case DepthFilterScope::AllTimingEndpoints:
            return collectAllEndpointReports(netLevels, depthStatuses, false);
        case DepthFilterScope::PrimaryOutputs:
            return collectPrimaryOutputReports(netLevels, depthStatuses, false);
        case DepthFilterScope::DffD:
            return collectDffDReports(netLevels, depthStatuses, false);
        }
        return std::vector<DepthReport>();
    };

    auto appendMatchingReports = [&](std::vector<DepthReport> candidates,
                                     DepthPredicate predicate,
                                     int threshold,
                                     int upperThreshold,
                                     bool trackFilterCompleteness,
                                     bool buildCriticalPaths,
                                     const std::vector<int>& netLevels) {
        if (trackFilterCompleteness) {
            result.checkedEndpointCount = candidates.size();
        }
        updateAvailabilitySummary(candidates);
        for (DepthReport& report : candidates) {
            if (report.endpointNetId < 0 || report.depth < 0) {
                continue;
            }
            if (!matchesDepthPredicate(
                    report.depth, predicate, threshold, upperThreshold)) {
                continue;
            }
            if (buildCriticalPaths) {
                report.criticalPath = buildCriticalPathToNet(
                    *this, report.endpointNetId, netLevels);
            }
            result.reports.push_back(std::move(report));
        }
        if (trackFilterCompleteness) {
            result.matchedEndpointCount = result.reports.size();
        }
    };

    switch (query.type) {
    case DepthQueryType::SpecificNet: {
        if (query.netName.empty()) {
            result.message = "SpecificNet depth query requires netName";
            return result;
        }

        const int netId = getNetId(query.netName);
        if (!isActiveDepthNet(*this, netId)) {
            result.message = "Net not found or depth unavailable: " + query.netName;
            return result;
        }
        const std::vector<int> netLevels = computeNetLevels();
        const std::vector<DepthStatus> depthStatuses =
            classifyNetDepthStatuses(*this, netLevels);
        DepthReport report = makeDepthReportForNet(
            *this, query.netName, DepthEndpointType::SpecificNet,
            query.netName, &netLevels, query.includeCriticalPath,
            &depthStatuses);

        result.ok = true;
        result.reports.push_back(report);
        updateAvailabilitySummary(result.reports);
        updateSummary();
        if (report.depthStatus == DepthStatus::NoTimingPath) {
            result.message = "Specific net has no timing path";
        } else if (!result.complete) {
            result.message = "Specific net depth analysis is incomplete";
        } else {
            result.message = "Specific net depth";
        }
        return result;
    }

    case DepthQueryType::PrimaryOutputs: {
        const std::vector<int> netLevels = computeNetLevels();
        const std::vector<DepthStatus> depthStatuses =
            classifyNetDepthStatuses(*this, netLevels);
        result.reports = collectPrimaryOutputReports(
            netLevels, depthStatuses, query.includeCriticalPath);
        result.ok = true;
        result.message = "Primary output depth reports";
        updateAvailabilitySummary(result.reports);
        updateSummary();
        return result;
    }

    case DepthQueryType::DffD: {
        const std::vector<int> netLevels = computeNetLevels();
        const std::vector<DepthStatus> depthStatuses =
            classifyNetDepthStatuses(*this, netLevels);
        result.reports = collectDffDReports(
            netLevels, depthStatuses, query.includeCriticalPath);
        result.ok = true;
        result.message = "DFF D-pin depth reports";
        updateAvailabilitySummary(result.reports);
        updateSummary();
        return result;
    }

    case DepthQueryType::GlobalCriticalPath: {
        const std::vector<int> netLevels = computeNetLevels();
        const std::vector<DepthStatus> depthStatuses =
            classifyNetDepthStatuses(*this, netLevels);
        const std::vector<DepthReport> endpointReports =
            collectAllEndpointReports(netLevels, depthStatuses, false);
        updateAvailabilitySummary(endpointReports);
        result.worst = selectWorstDepthReport(endpointReports);
        if (result.worst.endpointNetId < 0 || result.worst.depth < 0) {
            result.ok = true;
            result.message = result.complete
                ? "No timing endpoint has a defined depth"
                : "Global critical path analysis is incomplete";
            clearCriticalPathsIfNeeded();
            return result;
        }
        result.ok = true;
        result.message = "Global critical path";
        if (query.includeCriticalPath) {
            result.worst.criticalPath = buildCriticalPathToNet(
                *this, result.worst.endpointNetId, netLevels);
        }
        result.reports.push_back(result.worst);
        result.count = result.reports.size();
        result.exists = true;
        clearCriticalPathsIfNeeded();
        return result;
    }

    case DepthQueryType::EndpointsExceedingDepth: {
        if (query.threshold < 0) {
            result.message = "EndpointsExceedingDepth requires non-negative threshold";
            return result;
        }
        const std::vector<int> netLevels = computeNetLevels();
        const std::vector<DepthStatus> depthStatuses =
            classifyNetDepthStatuses(*this, netLevels);
        appendMatchingReports(
            collectAllEndpointReports(netLevels, depthStatuses, false),
            DepthPredicate::GreaterThan, query.threshold, -1, false,
            query.includeCriticalPath, netLevels);
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
        const std::vector<DepthStatus> depthStatuses =
            classifyNetDepthStatuses(*this, netLevels);
        appendMatchingReports(
            collectPrimaryOutputReports(netLevels, depthStatuses, false),
            DepthPredicate::GreaterThan, query.threshold, -1, false,
            query.includeCriticalPath, netLevels);
        result.ok = true;
        result.message = "Primary outputs exceeding depth threshold";
        updateSummary();
        return result;
    }

    case DepthQueryType::EndpointDepthFilter: {
        result.filterApplied = true;
        if (!isValidDepthFilterScope(query.filterScope)) {
            result.complete = false;
            result.message = "EndpointDepthFilter requires a valid endpoint scope";
            return result;
        }
        if (!isValidDepthPredicate(query.predicate)) {
            result.complete = false;
            result.message = "EndpointDepthFilter requires a valid predicate";
            return result;
        }
        if (query.threshold < 0) {
            result.complete = false;
            result.message = "EndpointDepthFilter requires a non-negative threshold";
            return result;
        }
        if (query.predicate == DepthPredicate::BetweenInclusive &&
            (query.upperThreshold < 0 || query.threshold > query.upperThreshold)) {
            result.complete = false;
            result.message =
                "EndpointDepthFilter requires an inclusive range with lower <= upper";
            return result;
        }

        const std::vector<int> netLevels = computeNetLevels();
        const std::vector<DepthStatus> depthStatuses =
            classifyNetDepthStatuses(*this, netLevels);
        appendMatchingReports(
            collectScopedEndpointReports(netLevels, depthStatuses), query.predicate,
            query.threshold, query.upperThreshold, true, false, netLevels);
        result.ok = true;
        if (!result.complete) {
            result.message =
                "Timing endpoint depth filter completed with analysis failures";
        } else if (result.noTimingPathEndpointCount > 0) {
            result.message =
                "Timing endpoint depth filter; endpoints without timing paths excluded";
        } else {
            result.message = "Timing endpoint depth filter";
        }
        updateSummary();
        result.matchedEndpointCount = result.count;
        return result;
    }

    case DepthQueryType::GateOnCriticalPath: {
        if (query.gateName.empty()) {
            result.message = "GateOnCriticalPath requires gateName";
            return result;
        }

        const int gateId = getGateId(query.gateName);
        result.gateId = gateId;
        if (!isActiveDepthGate(*this, gateId)) {
            result.message = "Gate not found: " + query.gateName;
            return result;
        }

        const Gate& targetGate = gates[gateId];
        if (targetGate.type == GateType::DFF ||
            !hasConsistentDepthDriver(*this, targetGate.outputNetId, gateId) ||
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
        const std::vector<DepthStatus> depthStatuses =
            classifyNetDepthStatuses(*this, netLevels);
        {
            const std::vector<DepthReport> endpointReports =
                collectAllEndpointReports(netLevels, depthStatuses, false);
            updateAvailabilitySummary(endpointReports);
            DepthReport globalWorst;
            globalWorst = selectWorstDepthReport(endpointReports);
            if (query.includeCriticalPath && globalWorst.endpointNetId >= 0) {
                globalWorst.criticalPath = buildCriticalPathToNet(
                    *this, globalWorst.endpointNetId, netLevels);
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
            if (hasConsistentDepthLoad(*this, inputNetId, gateId) &&
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

        const std::vector<int> remainingDepthByNet =
            computeRemainingDepthToTimingEndpoints(*this, netLevels);

        const int remainingDepth = remainingDepthByNet[targetGate.outputNetId];
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

    case DepthQueryType::CriticalGateBatch: {
        result.criticalGateBatchApplied = true;
        for (GateType type : {GateType::AND, GateType::OR, GateType::NAND,
                              GateType::NOR, GateType::NOT, GateType::BUF,
                              GateType::XOR, GateType::XNOR}) {
            result.criticalGateTypeCounts[type] = 0;
        }
        const std::vector<int> netLevels = computeNetLevels();
        const std::vector<DepthStatus> depthStatuses =
            classifyNetDepthStatuses(*this, netLevels);
        const std::vector<DepthReport> endpointReports =
            collectAllEndpointReports(netLevels, depthStatuses, false);
        updateAvailabilitySummary(endpointReports);
        result.worst = selectWorstDepthReport(endpointReports);
        if (query.includeCriticalPath && result.worst.endpointNetId >= 0 &&
            result.worst.depth >= 0) {
            result.worst.criticalPath = buildCriticalPathToNet(
                *this, result.worst.endpointNetId, netLevels);
        }

        const std::vector<int> remainingDepthByNet =
            computeRemainingDepthToTimingEndpoints(*this, netLevels);
        const std::vector<unsigned char> gateInputsConsistent =
            buildDepthInputConsistency(*this);

        for (size_t gateIndex = 0; gateIndex < gates.size(); ++gateIndex) {
            const int gateId = static_cast<int>(gateIndex);
            if (!isActiveDepthGate(*this, gateId) ||
                gates[gateId].type == GateType::DFF) {
                continue;
            }
            ++result.checkedGateCount;

            const Gate& gate = gates[gateId];
            if (gate.inputNetIds.empty() ||
                gateInputsConsistent[gateId] == 0 ||
                !hasConsistentDepthDriver(
                    *this, gate.outputNetId, gateId)) {
                ++result.graphInconsistentGateCount;
                continue;
            }

            const int outputNetId = gate.outputNetId;
            if (outputNetId < 0 ||
                outputNetId >= static_cast<int>(depthStatuses.size()) ||
                outputNetId >= static_cast<int>(netLevels.size()) ||
                outputNetId >= static_cast<int>(remainingDepthByNet.size())) {
                ++result.graphInconsistentGateCount;
                continue;
            }

            switch (depthStatuses[outputNetId]) {
            case DepthStatus::GraphInconsistent:
                ++result.graphInconsistentGateCount;
                continue;
            case DepthStatus::AnalysisFailure:
            case DepthStatus::Unknown:
                ++result.analysisFailureGateCount;
                continue;
            case DepthStatus::NoTimingPath:
                ++result.noTimingPathGateCount;
                continue;
            case DepthStatus::Available:
                break;
            }

            const int arrivalDepth = netLevels[outputNetId];
            const int remainingDepth = remainingDepthByNet[outputNetId];
            if (arrivalDepth < 0) {
                ++result.analysisFailureGateCount;
                continue;
            }
            if (remainingDepth < 0) {
                ++result.noTimingPathGateCount;
                continue;
            }

            ++result.analyzableGateCount;
            if (result.worst.depth < 0 ||
                arrivalDepth + remainingDepth != result.worst.depth) {
                continue;
            }

            CriticalGateReport critical;
            critical.gateName = gate.instName;
            critical.gateId = gateId;
            critical.gateTypeName = gateTypeToString(gate.type);
            critical.outputNetName = nets[outputNetId].name;
            critical.outputNetId = outputNetId;
            critical.arrivalDepth = arrivalDepth;
            critical.remainingDepth = remainingDepth;
            ++result.criticalGateTypeCounts[gate.type];
            result.criticalGates.push_back(std::move(critical));
        }

        result.criticalGateCount = result.criticalGates.size();
        result.count = result.criticalGateCount;
        result.exists = result.criticalGateCount > 0;
        result.complete = result.complete &&
            result.graphInconsistentGateCount == 0 &&
            result.analysisFailureGateCount == 0;
        result.ok = true;
        if (!result.complete) {
            result.message =
                "Critical gate batch completed with analysis failures";
        } else if (result.worst.depth < 0) {
            result.message = "No timing endpoint has a defined depth";
        } else {
            result.message = "Critical gates on global maximum-depth paths";
        }
        clearCriticalPathsIfNeeded();
        return result;
    }

    case DepthQueryType::DeepestOutputCone: {
        const std::vector<int> netLevels = computeNetLevels();
        const std::vector<DepthStatus> depthStatuses =
            classifyNetDepthStatuses(*this, netLevels);
        result.reports = collectPrimaryOutputReports(
            netLevels, depthStatuses, query.includeCriticalPath);
        if (result.reports.empty()) {
            result.message = "No primary outputs available";
            return result;
        }
        result.ok = true;
        result.message = "Deepest primary output fanin cone";
        updateAvailabilitySummary(result.reports);
        updateSummary();
        if (result.worst.depth < 0) {
            result.exists = false;
            result.message = result.complete
                ? "No primary output has a timing path"
                : "Deepest primary output analysis is incomplete";
        }
        return result;
    }
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
