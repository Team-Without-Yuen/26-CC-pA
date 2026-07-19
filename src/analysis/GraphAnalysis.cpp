#include "include/core/Netlist.h"

#include <algorithm>
#include <queue>
#include <unordered_set>
#include <vector>

namespace {

struct CombinationalNetGraph {
    std::vector<std::vector<int>> successors;
    std::vector<std::vector<int>> predecessors;
    std::vector<int> topologicalOrder;
    bool acyclic = true;
};

bool isActiveNet(const Netlist& netlist, int netId) {
    return netlist.isValidNetId(netId) && !netlist.getNet(netId).isRemoved;
}

CombinationalNetGraph buildCombinationalNetGraph(const Netlist& netlist) {
    const size_t netSlots = netlist.getNetCount();
    CombinationalNetGraph graph;
    graph.successors.resize(netSlots);
    graph.predecessors.resize(netSlots);

    for (size_t gateId = 0; gateId < netlist.getGateCount(); ++gateId) {
        const Gate& gate = netlist.getGate(static_cast<int>(gateId));
        if (gate.type == GateType::UNKNOWN || gate.type == GateType::DFF ||
            !isActiveNet(netlist, gate.outputNetId)) {
            continue;
        }
        for (int inputNetId : gate.inputNetIds) {
            if (!isActiveNet(netlist, inputNetId)) {
                continue;
            }
            graph.successors[inputNetId].push_back(gate.outputNetId);
            graph.predecessors[gate.outputNetId].push_back(inputNetId);
        }
    }

    for (size_t netId = 0; netId < netSlots; ++netId) {
        auto& successors = graph.successors[netId];
        std::sort(successors.begin(), successors.end());
        successors.erase(std::unique(successors.begin(), successors.end()), successors.end());
        auto& predecessors = graph.predecessors[netId];
        std::sort(predecessors.begin(), predecessors.end());
        predecessors.erase(std::unique(predecessors.begin(), predecessors.end()), predecessors.end());
    }

    std::vector<int> indegree(netSlots, 0);
    std::queue<int> ready;
    size_t activeNetCount = 0;
    for (size_t netId = 0; netId < netSlots; ++netId) {
        if (!isActiveNet(netlist, static_cast<int>(netId))) {
            continue;
        }
        ++activeNetCount;
        indegree[netId] = static_cast<int>(graph.predecessors[netId].size());
        if (indegree[netId] == 0) {
            ready.push(static_cast<int>(netId));
        }
    }

    while (!ready.empty()) {
        const int netId = ready.front();
        ready.pop();
        graph.topologicalOrder.push_back(netId);
        for (int nextNetId : graph.successors[netId]) {
            if (--indegree[nextNetId] == 0) {
                ready.push(nextNetId);
            }
        }
    }

    graph.acyclic = graph.topologicalOrder.size() == activeNetCount;
    return graph;
}

struct DominatorInfo {
    std::vector<int> immediateDominator;
    std::vector<int> depth;
};

int intersectDominators(int first,
                        int second,
                        const std::vector<int>& immediateDominator,
                        const std::vector<int>& depth) {
    while (first != second) {
        while (depth[first] > depth[second]) {
            first = immediateDominator[first];
        }
        while (depth[second] > depth[first]) {
            second = immediateDominator[second];
        }
        if (first != second) {
            first = immediateDominator[first];
            second = immediateDominator[second];
        }
    }
    return first;
}

DominatorInfo computeDominators(const CombinationalNetGraph& graph, int sourceNetId) {
    DominatorInfo result;
    result.immediateDominator.assign(graph.successors.size(), -1);
    result.depth.assign(graph.successors.size(), -1);
    result.immediateDominator[sourceNetId] = sourceNetId;
    result.depth[sourceNetId] = 0;

    for (int netId : graph.topologicalOrder) {
        if (netId == sourceNetId) {
            continue;
        }
        int commonDominator = -1;
        for (int predecessor : graph.predecessors[netId]) {
            if (result.immediateDominator[predecessor] < 0) {
                continue;
            }
            commonDominator = commonDominator < 0
                ? predecessor
                : intersectDominators(
                    commonDominator,
                    predecessor,
                    result.immediateDominator,
                    result.depth);
        }
        if (commonDominator >= 0) {
            result.immediateDominator[netId] = commonDominator;
            result.depth[netId] = result.depth[commonDominator] + 1;
        }
    }
    return result;
}

bool dominates(const DominatorInfo& dominators, int candidateNetId, int targetNetId) {
    if (candidateNetId < 0 || targetNetId < 0 ||
        candidateNetId >= static_cast<int>(dominators.depth.size()) ||
        targetNetId >= static_cast<int>(dominators.depth.size()) ||
        dominators.depth[candidateNetId] < 0 || dominators.depth[targetNetId] < 0) {
        return false;
    }

    int current = targetNetId;
    while (dominators.depth[current] > dominators.depth[candidateNetId]) {
        current = dominators.immediateDominator[current];
    }
    return current == candidateNetId;
}

std::vector<int> collectPortBitIds(const Netlist& netlist, bool primaryInputs) {
    std::vector<int> ids;
    std::unordered_set<int> seen;
    const std::vector<Port>& ports = primaryInputs
        ? netlist.getPrimaryInputs()
        : netlist.getPrimaryOutputs();
    for (const Port& port : ports) {
        for (int netId : port.netIds) {
            if (isActiveNet(netlist, netId) && seen.insert(netId).second) {
                ids.push_back(netId);
            }
        }
    }
    return ids;
}

GraphReport makeCycleReport(GraphReport report) {
    report.unsupported = true;
    report.combinationalCycleDetected = true;
    report.status = "COMBINATIONAL_CYCLE";
    report.message = "Directed cut analysis requires an acyclic combinational net graph.";
    return report;
}

} // namespace

Netlist::GraphReport Netlist::runGraphQuery(const GraphQuery& query) const {
    GraphReport report;

    if (query.type == GraphQueryType::IsCutNetBetweenPiPo) {
        report.candidateNetName = query.netName;
        report.candidateNetId = getNetId(query.netName);
        if (!isActiveNet(*this, report.candidateNetId)) {
            report.status = "NET_NOT_FOUND";
            report.message = "Cut candidate net not found: " + query.netName;
            return report;
        }
        const Net& candidate = getNet(report.candidateNetId);
        if (candidate.isPI || candidate.isPO || candidate.isConst) {
            report.status = "INTERNAL_NET_REQUIRED";
            report.message = "PI-to-PO cut candidate must be an internal non-constant net.";
            return report;
        }

        const CombinationalNetGraph graph = buildCombinationalNetGraph(*this);
        if (!graph.acyclic) {
            return makeCycleReport(report);
        }

        const std::vector<int> primaryInputs = collectPortBitIds(*this, true);
        const std::vector<int> primaryOutputs = collectPortBitIds(*this, false);
        report.checkedPrimaryInputCount = primaryInputs.size();
        report.checkedPrimaryOutputCount = primaryOutputs.size();
        const DominatorInfo fromCandidate = computeDominators(graph, report.candidateNetId);

        for (int piNetId : primaryInputs) {
            const DominatorInfo dominators = computeDominators(graph, piNetId);
            for (int poNetId : primaryOutputs) {
                if (dominators.depth[report.candidateNetId] < 0 ||
                    fromCandidate.depth[poNetId] < 0) {
                    continue;
                }
                report.pathExists = true;
                if (dominates(dominators, report.candidateNetId, poNetId)) {
                    report.ok = true;
                    report.exists = true;
                    report.isCut = true;
                    report.witnessPrimaryInput = getNet(piNetId).name;
                    report.witnessPrimaryOutput = getNet(poNetId).name;
                    report.status = "CUT_NET";
                    report.message = "Candidate disconnects at least one directed PI-to-PO pair when removed.";
                    return report;
                }
            }
        }

        report.ok = true;
        report.exists = false;
        report.isCut = false;
        report.status = "NOT_CUT_NET";
        report.message = "Candidate is not a cut for any directed PI-to-PO pair.";
        return report;
    }

    if (query.type == GraphQueryType::ArticulationPointsBetween) {
        report.sourceNetName = query.sourceNetName;
        report.targetNetName = query.targetNetName;
        report.sourceNetId = getNetId(query.sourceNetName);
        report.targetNetId = getNetId(query.targetNetName);
        if (!isActiveNet(*this, report.sourceNetId)) {
            report.status = "SOURCE_NOT_FOUND";
            report.message = "Source net not found: " + query.sourceNetName;
            return report;
        }
        if (!isActiveNet(*this, report.targetNetId)) {
            report.status = "TARGET_NOT_FOUND";
            report.message = "Target net not found: " + query.targetNetName;
            return report;
        }

        const CombinationalNetGraph graph = buildCombinationalNetGraph(*this);
        if (!graph.acyclic) {
            return makeCycleReport(report);
        }
        const DominatorInfo dominators = computeDominators(graph, report.sourceNetId);
        if (dominators.depth[report.targetNetId] < 0) {
            report.ok = true;
            report.exists = false;
            report.pathExists = false;
            report.status = "NO_PATH";
            report.message = "No directed combinational path exists between source and target.";
            return report;
        }

        report.pathExists = true;
        int current = dominators.immediateDominator[report.targetNetId];
        while (current >= 0 && current != report.sourceNetId) {
            report.articulationNetIds.push_back(current);
            current = dominators.immediateDominator[current];
        }
        std::reverse(report.articulationNetIds.begin(), report.articulationNetIds.end());
        for (int netId : report.articulationNetIds) {
            report.articulationNetNames.push_back(getNet(netId).name);
        }

        report.ok = true;
        report.exists = !report.articulationNetIds.empty();
        report.status = report.exists ? "ARTICULATION_POINTS_FOUND" : "NO_ARTICULATION_POINTS";
        report.message = report.exists
            ? "Directed source-to-target articulation nets found."
            : "Source and target are connected but have no internal articulation nets.";
        return report;
    }

    report.status = "UNSUPPORTED_QUERY_TYPE";
    report.message = "Unsupported GraphQueryType";
    return report;
}
