#include "include/core/Netlist.h"

// 根據單一 endpoint report 建立 optimization candidate；只整理資訊，不修改 netlist。
Netlist::OptimizationCandidate Netlist::buildOptimizationCandidate(
    const DepthReport& endpoint,
    int targetDepth) const {
    OptimizationCandidate candidate;
    candidate.endpoint = endpoint;
    candidate.targetDepth = targetDepth;
    candidate.criticalPath = endpoint.criticalPath;

    if (endpoint.endpointNetId < 0 ||
        endpoint.endpointNetId >= static_cast<int>(nets.size())) {
        return candidate;
    }

    const std::string& endpointNetName = nets[endpoint.endpointNetId].name;
    candidate.faninCone = getTransitiveFaninCone(endpointNetName);
    if (!candidate.criticalPath.exists()) {
        candidate.criticalPath = findCriticalPathToNet(endpointNetName);
    }
    return candidate;
}

// 找出所有 depth 大於 targetDepth 的 timing endpoints，並轉成 optimization candidates。
std::vector<Netlist::OptimizationCandidate>
Netlist::findOptimizationCandidatesExceedingDepth(int targetDepth) const {
    std::vector<OptimizationCandidate> candidates;
    const std::vector<DepthReport> exceededEndpoints =
        findEndpointsExceedingDepth(targetDepth);
    candidates.reserve(exceededEndpoints.size());

    for (const DepthReport& endpoint : exceededEndpoints) {
        candidates.push_back(buildOptimizationCandidate(endpoint, targetDepth));
    }
    return candidates;
}

// 找出 candidate critical path 上的 BUF gate；只偵測，不修改 netlist。
std::vector<int> Netlist::findBufferGatesOnCriticalPath(
    const OptimizationCandidate& candidate) const {
    std::vector<int> bufferGateIds;

    for (int gateId : candidate.criticalPath.gateIds) {
        if (gateId < 0 || gateId >= static_cast<int>(gates.size())) {
            continue;
        }
        if (gates[gateId].type == GateType::BUF) {
            bufferGateIds.push_back(gateId);
        }
    }

    return bufferGateIds;
}

// 找出 candidate critical path 上第一版可安全 bypass 的 BUF gate；只偵測，不修改 netlist。
std::vector<int> Netlist::findRemovableBufferGatesOnCriticalPath(
    const OptimizationCandidate& candidate) const {
    std::vector<int> removableGateIds;
    const std::vector<int> bufferGateIds = findBufferGatesOnCriticalPath(candidate);

    for (int gateId : bufferGateIds) {
        if (gateId < 0 || gateId >= static_cast<int>(gates.size())) {
            continue;
        }

        const Gate& gate = gates[gateId];
        if (gate.type != GateType::BUF || gate.inputNetIds.size() != 1) {
            continue;
        }

        const int inputNetId = gate.inputNetIds.front();
        const int outputNetId = gate.outputNetId;
        if (inputNetId < 0 || inputNetId >= static_cast<int>(nets.size()) ||
            outputNetId < 0 || outputNetId >= static_cast<int>(nets.size()) ||
            inputNetId == outputNetId) {
            continue;
        }

        const Net& outputNet = nets[outputNetId];
        if (outputNet.isPO || outputNet.isConst || outputNet.loadGateIds.empty()) {
            continue;
        }

        removableGateIds.push_back(gateId);
    }

    return removableGateIds;
}
