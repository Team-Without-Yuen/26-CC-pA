#include "include/core/Netlist.h"
#include "include/core/DepthOptimizer.h"
#include "include/core/TechMapper.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

namespace {

std::string optPassKindName(OptPassKind passKind) {
    switch (passKind) {
        case OptPassKind::CleanupBufferChain:
            return "cleanup_buffer_chain";
        case OptPassKind::CollapseDoubleInverter:
            return "collapse_double_inverter";
        case OptPassKind::LocalSimplificationFixpoint:
            return "local_simplification_fixpoint";
        case OptPassKind::CriticalPathDepth:
            return "critical_path_depth";
        default:
            return "unknown";
    }
}

std::string optimizationStatusName(OptimizationStatus status) {
    switch (status) {
        case OptimizationStatus::SUCCESS:
            return "SUCCESS";
        case OptimizationStatus::NO_IMPROVEMENT:
            return "NO_IMPROVEMENT";
        case OptimizationStatus::ERROR_INVALID_REQUEST:
            return "ERROR_INVALID_REQUEST";
        case OptimizationStatus::ERROR_CONSTRAINT_UNSATISFIED:
            return "ERROR_CONSTRAINT_UNSATISFIED";
        case OptimizationStatus::ERROR_NOT_EQUIVALENT:
            return "ERROR_NOT_EQUIVALENT";
        case OptimizationStatus::ERROR_AREA_EXCEEDED:
            return "ERROR_AREA_EXCEEDED";
        default:
            return "UNKNOWN";
    }
}

std::string targetScopeName(TargetScope scope) {
    switch (scope) {
        case TargetScope::WHOLE_NETLIST:
            return "whole_netlist";
        case TargetScope::NET_FANIN:
            return "net_fanin";
        case TargetScope::NET_FANOUT:
            return "net_fanout";
        case TargetScope::GATE_FANIN:
            return "gate_fanin";
        case TargetScope::GATE_FANOUT:
            return "gate_fanout";
        default:
            return "unknown";
    }
}

std::string depthObjectiveName(OptDepthObjective objective) {
    return objective == OptDepthObjective::ScopedFaninCone
        ? "scoped_fanin_cone_depth"
        : "global_maximum_depth";
}

bool isCombinationalGateType(GateType type) {
    return type != GateType::DFF && type != GateType::UNKNOWN;
}

bool gateTypeAllowed(
    GateType type,
    const std::vector<GateType>& allowedTypes,
    const std::vector<GateType>& bannedTypes)
{
    if (std::find(bannedTypes.begin(), bannedTypes.end(), type) != bannedTypes.end()) {
        return false;
    }
    return allowedTypes.empty() ||
           std::find(allowedTypes.begin(), allowedTypes.end(), type) != allowedTypes.end();
}

bool hasValidGateTypeConstraints(
    const std::vector<GateType>& allowedTypes,
    const std::vector<GateType>& bannedTypes)
{
    for (GateType type : allowedTypes) {
        if (!isCombinationalGateType(type)) return false;
    }
    for (GateType type : bannedTypes) {
        if (!isCombinationalGateType(type)) return false;
        if (std::find(allowedTypes.begin(), allowedTypes.end(), type) != allowedTypes.end()) {
            return false;
        }
    }
    return true;
}

bool scopeSatisfiesGateConstraints(
    const Netlist& netlist,
    TargetScope scope,
    const std::string& scopeName,
    const std::vector<GateType>& allowedTypes,
    const std::vector<GateType>& bannedTypes)
{
    if (allowedTypes.empty() && bannedTypes.empty()) return true;

    std::vector<int> gateIds;
    if (scope == TargetScope::WHOLE_NETLIST) {
        gateIds.reserve(netlist.getGateCount());
        for (int gateId = 0; gateId < static_cast<int>(netlist.getGateCount()); ++gateId) {
            gateIds.push_back(gateId);
        }
    } else {
        const RewriteScopeResolution resolved =
            resolveRewriteScope(netlist, scope, scopeName);
        if (!resolved.ok) return false;
        gateIds = netlist.getConeGateIds(resolved.cone);
    }

    for (int gateId : gateIds) {
        if (!netlist.isValidGateId(gateId) || netlist.isGateRemoved(gateId)) continue;
        const GateType type = netlist.getGate(gateId).type;
        if (!isCombinationalGateType(type)) continue;
        if (!gateTypeAllowed(type, allowedTypes, bannedTypes)) return false;
    }
    return true;
}

ConeQueryType coneQueryTypeForScope(TargetScope scope) {
    switch (scope) {
        case TargetScope::NET_FANOUT:
            return ConeQueryType::NetTransitiveFanout;
        case TargetScope::GATE_FANIN:
            return ConeQueryType::GateTransitiveFanin;
        case TargetScope::GATE_FANOUT:
            return ConeQueryType::GateTransitiveFanout;
        case TargetScope::NET_FANIN:
        default:
            return ConeQueryType::NetTransitiveFanin;
    }
}

ConeReport buildOptimizationConeReport(
    const Netlist& netlist,
    TargetScope scope,
    const std::string& scopeName)
{
    ConeReport report;
    if (scope == TargetScope::WHOLE_NETLIST) return report;

    const RewriteScopeResolution resolved =
        resolveRewriteScope(netlist, scope, scopeName);
    if (!resolved.ok) return report;

    report.cone = resolved.cone;
    report.gateIds = netlist.getConeGateIds(report.cone);
    report.rootNetIds = report.cone.rootNetIds;
    report.sourceName = scopeName;
    report.type = coneQueryTypeForScope(scope);
    report.gateCount = report.gateIds.size();
    report.exists = !report.rootNetIds.empty();
    report.ok = report.exists;
    return report;
}

int measureDepthObjective(const Netlist& netlist, const OptApplyRequest& request) {
    if (request.depthObjective == OptDepthObjective::GlobalMaximum) {
        return netlist.findGlobalCriticalPath().depth;
    }

    const RewriteScopeResolution resolved =
        resolveRewriteScope(netlist, request.scope, request.scopeName);
    if (!resolved.ok) return -1;

    std::string rootName = resolved.resolvedRootNetName;
    if (rootName.empty() && !resolved.cone.rootNetIds.empty()) {
        const int rootNetId = resolved.cone.rootNetIds.front();
        if (netlist.isValidNetId(rootNetId)) {
            rootName = netlist.getNet(rootNetId).name;
        }
    }
    return rootName.empty() ? -1 : netlist.getMaxDepthToNet(rootName);
}

DepthChange buildOptimizationDepthChange(
    const Netlist& before,
    const Netlist& after,
    const OptApplyRequest& request)
{
    DepthChange change;
    change.endpointName = request.depthObjective == OptDepthObjective::GlobalMaximum
        ? before.findGlobalCriticalPath().endpointName
        : request.scopeName;
    change.beforeDepth = measureDepthObjective(before, request);
    change.afterDepth = measureDepthObjective(after, request);
    change.targetDepth = request.targetDepth;
    change.improved =
        change.beforeDepth >= 0 &&
        change.afterDepth >= 0 &&
        change.afterDepth < change.beforeDepth;
    change.meetsTarget =
        request.targetDepth >= 0 &&
        change.afterDepth >= 0 &&
        change.afterDepth <= request.targetDepth;
    return change;
}

struct ScopedDepthLowerBoundProof {
    bool proven = false;
    int lowerBound = -1;
    std::string reason;
};

bool hasExactAllowedBasis(
    const std::vector<GateType>& allowedTypes,
    const std::vector<GateType>& bannedTypes,
    GateType first,
    GateType second)
{
    return bannedTypes.empty() &&
           allowedTypes.size() == 2 &&
           std::find(allowedTypes.begin(), allowedTypes.end(), first) !=
               allowedTypes.end() &&
           std::find(allowedTypes.begin(), allowedTypes.end(), second) !=
               allowedTypes.end();
}

bool isIndependentBoundaryNet(const Netlist& netlist, int netId) {
    if (!netlist.isValidNetId(netId)) return false;
    const Net& net = netlist.getNet(netId);
    if (net.isConst) return false;
    if (net.isPI) return true;
    return net.driverGateId >= 0 &&
           netlist.isValidGateId(net.driverGateId) &&
           !netlist.isGateRemoved(net.driverGateId) &&
           netlist.getGate(net.driverGateId).type == GateType::DFF;
}

ScopedDepthLowerBoundProof proveScopedDepthLowerBound(
    const Netlist& netlist,
    const OptApplyRequest& request,
    const RewriteScopeResolution& scope)
{
    ScopedDepthLowerBoundProof proof;
    if (request.depthObjective != OptDepthObjective::ScopedFaninCone) {
        return proof;
    }

    const int currentDepth = measureDepthObjective(netlist, request);
    if (currentDepth == 0) {
        proof.proven = true;
        proof.lowerBound = 0;
        proof.reason =
            "The scoped root already has depth 0, which is the absolute lower bound.";
        return proof;
    }

    if (currentDepth != 2 ||
        !hasExactAllowedBasis(
            request.allowedTypes,
            request.bannedTypes,
            GateType::NAND,
            GateType::NOT)) {
        return proof;
    }

    const int rootNetId = netlist.getNetId(scope.resolvedRootNetName);
    if (!netlist.isValidNetId(rootNetId)) return proof;
    const Net& rootNet = netlist.getNet(rootNetId);
    if (rootNet.driverGateId < 0 ||
        !netlist.isValidGateId(rootNet.driverGateId) ||
        netlist.isGateRemoved(rootNet.driverGateId)) {
        return proof;
    }

    const Gate& rootGate = netlist.getGate(rootNet.driverGateId);
    if (rootGate.type != GateType::NOT ||
        rootGate.inputNetIds.size() != 1 ||
        !netlist.isValidNetId(rootGate.inputNetIds.front())) {
        return proof;
    }

    const Net& nandOutput = netlist.getNet(rootGate.inputNetIds.front());
    if (nandOutput.driverGateId < 0 ||
        !netlist.isValidGateId(nandOutput.driverGateId) ||
        netlist.isGateRemoved(nandOutput.driverGateId)) {
        return proof;
    }

    const Gate& nandGate = netlist.getGate(nandOutput.driverGateId);
    if (nandGate.type != GateType::NAND ||
        nandGate.inputNetIds.size() != 2 ||
        nandGate.inputNetIds[0] == nandGate.inputNetIds[1] ||
        !isIndependentBoundaryNet(netlist, nandGate.inputNetIds[0]) ||
        !isIndependentBoundaryNet(netlist, nandGate.inputNetIds[1])) {
        return proof;
    }

    proof.proven = true;
    proof.lowerBound = 2;
    proof.reason =
        "The scoped function is AND of two independent boundary signals. "
        "In a NAND/NOT-only basis it requires NAND followed by NOT, so depth 2 "
        "is optimal.";
    return proof;
}

OptCandidate makeGateCandidate(
    int id,
    OptPassKind passKind,
    const std::string& reason,
    const std::vector<int>& gateIds,
    const Netlist& netlist,
    int estimatedGateDelta,
    int estimatedNetDelta)
{
    OptCandidate candidate;
    candidate.id = id;
    candidate.passKind = passKind;
    candidate.reason = reason;
    candidate.gateIds = gateIds;
    candidate.estimatedGateDelta = estimatedGateDelta;
    candidate.estimatedNetDelta = estimatedNetDelta;

    for (int gateId : gateIds) {
        if (!netlist.isValidGateId(gateId)) continue;
        const Gate& gate = netlist.getGate(gateId);
        candidate.gateNames.push_back(gate.instName);
        if (gate.outputNetId >= 0 && netlist.isValidNetId(gate.outputNetId)) {
            candidate.netIds.push_back(gate.outputNetId);
            candidate.netNames.push_back(netlist.getNet(gate.outputNetId).name);
        }
        for (int inputNetId : gate.inputNetIds) {
            if (inputNetId < 0 || !netlist.isValidNetId(inputNetId)) continue;
            candidate.netIds.push_back(inputNetId);
            candidate.netNames.push_back(netlist.getNet(inputNetId).name);
        }
    }

    return candidate;
}

NetlistEditReport makeFailedOptApplyReport(
    const Netlist& netlist,
    OptPassKind passKind,
    const std::string& message)
{
    NetlistEditReport report;
    report.operationKind = passKind == OptPassKind::CriticalPathDepth
        ? NetlistEditOperationKind::DepthOptimization
        : NetlistEditOperationKind::CustomRewrite;
    report.operationName = "opt_apply:" + optPassKindName(passKind);
    report.message = message;
    report.beforeStats = netlist.collectNetlistStats();
    report.afterStats = report.beforeStats;
    report.diff = Netlist::diffStats(report.beforeStats, report.afterStats);
    report.validation = Netlist::validateEditResult(netlist, netlist);
    report.success = false;
    report.changed = false;
    report.rolledBack = false;
    report.validation.messages.push_back(message);
    return report;
}

} // namespace

OptQueryReport Netlist::runOptQuery(const OptQueryRequest& request) const {
    OptQueryReport report;
    report.passKind = request.passKind;
    report.scopeName = request.scopeName;

    int candidateId = 0;
    switch (request.passKind) {
        case OptPassKind::CleanupBufferChain: {
            const std::vector<int> bufferGateIds = findAllRemovableBufferGates();
            for (int gateId : bufferGateIds) {
                report.candidates.push_back(makeGateCandidate(
                    candidateId++,
                    request.passKind,
                    "Removable BUF gate can be bypassed safely.",
                    {gateId},
                    *this,
                    -1,
                    0));
            }
            report.ok = true;
            report.message = "Found " + std::to_string(report.candidates.size()) +
                             " cleanup_buffer_chain candidate(s).";
            break;
        }
        case OptPassKind::CollapseDoubleInverter: {
            const std::vector<std::pair<int, int>> pairs = findDoubleInverterPairs();
            for (const auto& pair : pairs) {
                report.candidates.push_back(makeGateCandidate(
                    candidateId++,
                    request.passKind,
                    "Back-to-back NOT gates can be collapsed.",
                    {pair.first, pair.second},
                    *this,
                    -2,
                    0));
            }
            report.ok = true;
            report.message = "Found " + std::to_string(report.candidates.size()) +
                             " collapse_double_inverter candidate(s).";
            break;
        }
        case OptPassKind::LocalSimplificationFixpoint: {
            OptCandidate candidate;
            candidate.id = candidateId++;
            candidate.passKind = request.passKind;
            candidate.reason = "Run constant and same-input simplification until no local changes remain.";
            candidate.estimatedGateDelta = 0;
            candidate.estimatedNetDelta = 0;
            report.candidates.push_back(candidate);
            report.ok = true;
            report.message = "Local simplification fixpoint is available as a pass-level candidate.";
            report.warnings.push_back("This first opt_query version does not enumerate every sub-simplification candidate.");
            break;
        }
        case OptPassKind::CriticalPathDepth: {
            OptCandidate candidate;
            candidate.id = candidateId++;
            candidate.passKind = request.passKind;
            candidate.reason =
                "Run best-effort critical-path restructuring, then validate depth, constraints, and whole-design equivalence.";
            candidate.estimatedGateDelta = 0;
            candidate.estimatedNetDelta = 0;
            report.candidates.push_back(candidate);
            report.ok = true;
            report.message = "Critical-path depth optimization is available as a pass-level candidate.";
            break;
        }
        default:
            report.ok = false;
            report.message = "Unsupported optimization pass kind.";
            break;
    }

    if (report.ok && !request.scopeName.empty()) {
        report.warnings.push_back("scopeName is recorded but not enforced in the first OPT flow skeleton.");
    }
    return report;
}

NetlistEditReport Netlist::runOptApply(const OptApplyRequest& request) {
    NetlistEditReport report;

    switch (request.passKind) {
        case OptPassKind::CleanupBufferChain:
            report = cleanupAllRemovableBuffersWithReport();
            report.operationName = "opt_apply:cleanup_buffer_chain";
            break;
        case OptPassKind::CollapseDoubleInverter:
            report = collapseBackToBackInvertersWithReport();
            report.operationName = "opt_apply:collapse_double_inverter";
            break;
        case OptPassKind::LocalSimplificationFixpoint:
            report = runLocalSimplificationFixpointWithReport();
            report.operationName = "opt_apply:local_simplification_fixpoint";
            break;
        case OptPassKind::CriticalPathDepth: {
            if (request.timeLimitSeconds <= 0.0) {
                return makeFailedOptApplyReport(
                    *this, request.passKind, "timeLimitSeconds must be positive.");
            }
            if (request.targetDepth < -1) {
                return makeFailedOptApplyReport(
                    *this, request.passKind, "targetDepth must be -1 or a non-negative depth.");
            }
            if (!hasValidGateTypeConstraints(request.allowedTypes, request.bannedTypes)) {
                return makeFailedOptApplyReport(
                    *this,
                    request.passKind,
                    "allowedTypes/bannedTypes must contain valid, non-overlapping combinational gate types.");
            }
            if (request.depthObjective == OptDepthObjective::ScopedFaninCone &&
                request.scope != TargetScope::NET_FANIN &&
                request.scope != TargetScope::GATE_FANIN) {
                return makeFailedOptApplyReport(
                    *this,
                    request.passKind,
                    "ScopedFaninCone depth requires NET_FANIN or GATE_FANIN scope.");
            }

            const RewriteScopeResolution originalScope =
                resolveRewriteScope(*this, request.scope, request.scopeName);
            if (!originalScope.ok) {
                return makeFailedOptApplyReport(
                    *this,
                    request.passKind,
                    "Failed to resolve optimization scope: " + originalScope.message);
            }

            const auto startedAt = std::chrono::steady_clock::now();
            auto elapsedSeconds = [&]() {
                return std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - startedAt).count();
            };

            const Netlist original = cloneForRollback();
            Netlist working = original.cloneForRollback();
            const bool baselineConstraintsSatisfied = scopeSatisfiesGateConstraints(
                original,
                request.scope,
                request.scopeName,
                request.allowedTypes,
                request.bannedTypes);

            DepthOptimizationSummary summary;
            summary.objectiveMetric = depthObjectiveName(request.depthObjective);
            summary.scope = targetScopeName(request.scope);
            summary.requestedScopeName = request.scopeName;
            summary.resolvedRootNetName = originalScope.resolvedRootNetName;
            summary.resolvedThroughDffDataPin =
                originalScope.resolvedThroughDffDataPin;
            summary.allowedTypes = request.allowedTypes;
            summary.bannedTypes = request.bannedTypes;
            summary.baselineConstraintsSatisfied = baselineConstraintsSatisfied;
            summary.timeBudgetSeconds = request.timeLimitSeconds;

            if (request.scope != TargetScope::WHOLE_NETLIST &&
                original.getConeGateCount(originalScope.cone) == 0) {
                NetlistEditReport originalReport = Netlist::buildEditReport(
                    original,
                    original,
                    "opt_apply:critical_path_depth",
                    NetlistEditOperationKind::DepthOptimization);
                originalReport.depthChange =
                    buildOptimizationDepthChange(original, original, request);

                summary.coreStatus = "NO_IMPROVEMENT";
                summary.coreMessage =
                    "The resolved optimization scope contains no combinational gates; "
                    "the original design was retained.";
                summary.finalConstraintsSatisfied = true;
                summary.candidateGenerated = false;
                summary.candidateAccepted = false;
                summary.elapsedSeconds = elapsedSeconds();
                originalReport.depthOptimization = summary;
                Netlist::certifyEquivalence(
                    originalReport,
                    EquivalenceCheckMethod::StructuralIdentity,
                    "The original design was retained because the resolved optimization scope is empty.");

                const bool targetMet =
                    request.targetDepth < 0 ||
                    (originalReport.depthChange.has_value() &&
                     originalReport.depthChange->meetsTarget);
                if (!targetMet) {
                    originalReport.success = false;
                    originalReport.changed = false;
                    originalReport.rolledBack = false;
                    originalReport.message =
                        "The resolved optimization scope is empty and the original design "
                        "does not meet targetDepth.";
                } else {
                    originalReport.success = true;
                    originalReport.changed = false;
                    originalReport.rolledBack = false;
                    originalReport.message =
                        "The resolved optimization scope is empty; the original design was retained.";
                }
                return originalReport;
            }

            const ScopedDepthLowerBoundProof lowerBound =
                proveScopedDepthLowerBound(original, request, originalScope);
            if (baselineConstraintsSatisfied && lowerBound.proven) {
                NetlistEditReport originalReport = Netlist::buildEditReport(
                    original,
                    original,
                    "opt_apply:critical_path_depth",
                    NetlistEditOperationKind::DepthOptimization);
                originalReport.depthChange =
                    buildOptimizationDepthChange(original, original, request);

                summary.coreStatus = "NO_IMPROVEMENT";
                summary.coreMessage = lowerBound.reason;
                summary.finalConstraintsSatisfied = true;
                summary.candidateGenerated = false;
                summary.candidateAccepted = false;
                summary.elapsedSeconds = elapsedSeconds();
                originalReport.depthOptimization = summary;
                Netlist::certifyEquivalence(
                    originalReport,
                    EquivalenceCheckMethod::StructuralIdentity,
                    "The original design was retained under a proven scoped-depth lower bound.");

                if (request.targetDepth >= 0 &&
                    lowerBound.lowerBound > request.targetDepth) {
                    originalReport.success = false;
                    originalReport.message =
                        "targetDepth is below the proven scoped-depth lower bound; "
                        "the original design was retained.";
                } else {
                    originalReport.success = true;
                    originalReport.changed = false;
                    originalReport.rolledBack = false;
                    originalReport.message =
                        "The scoped design is already optimal under the requested "
                        "gate basis; the original design was retained.";
                }
                return originalReport;
            }

            const bool hasGateConstraints =
                !request.allowedTypes.empty() || !request.bannedTypes.empty();
            const ConeReport coneReport =
                request.scope != TargetScope::WHOLE_NETLIST && hasGateConstraints
                    ? buildOptimizationConeReport(
                          working, request.scope, request.scopeName)
                    : ConeReport{};
            TechMapper techMapper;
            DepthOptimizer optimizer;
            const OptimizationResult core = optimizer.executeCriticalPathOptimization(
                working,
                techMapper,
                coneReport,
                request.allowedTypes,
                request.bannedTypes,
                request.verbose);

            summary.coreStatus = optimizationStatusName(core.status);
            summary.coreMessage = core.message;
            summary.candidateGenerated = core.changed;
            summary.finalConstraintsSatisfied = scopeSatisfiesGateConstraints(
                working,
                request.scope,
                request.scopeName,
                request.allowedTypes,
                request.bannedTypes);

            auto buildCandidateReport = [&]() {
                NetlistEditReport candidate = Netlist::buildEditReport(
                    original,
                    working,
                    "opt_apply:critical_path_depth",
                    NetlistEditOperationKind::DepthOptimization);
                candidate.changed = candidate.changed || core.changed;
                candidate.depthChange =
                    buildOptimizationDepthChange(original, working, request);
                return candidate;
            };

            auto finishReport = [&](NetlistEditReport candidate) {
                summary.elapsedSeconds = elapsedSeconds();
                candidate.depthOptimization = summary;
                if (!request.validateEquivalence) {
                    candidate.addWarning(
                        "CriticalPathDepth always performs mandatory whole-design equivalence even when validateEquivalence=false.");
                }
                if (!request.rollbackOnFailure) {
                    candidate.addWarning(
                        "CriticalPathDepth always keeps the original design on failure; rollbackOnFailure=false is ignored.");
                }
                if (!request.candidateIds.empty()) {
                    candidate.addWarning(
                        "CriticalPathDepth is a pass-level search; candidateIds are currently ignored.");
                }
                return candidate;
            };

            if (core.status != OptimizationStatus::SUCCESS &&
                core.status != OptimizationStatus::NO_IMPROVEMENT) {
                report = buildCandidateReport();
                report.success = false;
                report.rolledBack = core.changed;
                report.message = "Depth optimization candidate generation failed: " + core.message;
                return finishReport(std::move(report));
            }

            report = buildCandidateReport();
            if (!report.success) {
                report.rolledBack = core.changed;
                report.message =
                    "Depth optimization candidate failed structural validation and was discarded.";
                return finishReport(std::move(report));
            }
            if (!summary.finalConstraintsSatisfied) {
                report.success = false;
                report.rolledBack = core.changed;
                report.message =
                    "Depth optimization candidate violated the requested gate constraints and was discarded.";
                return finishReport(std::move(report));
            }

            const bool depthImproved =
                report.depthChange.has_value() && report.depthChange->improved;
            const bool targetMet =
                request.targetDepth < 0 ||
                (report.depthChange.has_value() && report.depthChange->meetsTarget);
            if (!targetMet) {
                report.success = false;
                report.rolledBack = core.changed;
                report.message =
                    "Depth optimization candidate did not meet targetDepth and was discarded.";
                return finishReport(std::move(report));
            }

            if (request.requireDepthImprovement &&
                baselineConstraintsSatisfied &&
                !depthImproved) {
                NetlistEditReport originalReport = Netlist::buildEditReport(
                    original,
                    original,
                    "opt_apply:critical_path_depth",
                    NetlistEditOperationKind::DepthOptimization);
                originalReport.success = true;
                originalReport.changed = false;
                originalReport.rolledBack = core.changed;
                originalReport.depthChange =
                    buildOptimizationDepthChange(original, original, request);
                Netlist::certifyEquivalence(
                    originalReport,
                    EquivalenceCheckMethod::StructuralIdentity,
                    "The original design was retained because no accepted depth improvement was found.");
                originalReport.message =
                    "No improving candidate was accepted; the original design was retained.";
                return finishReport(std::move(originalReport));
            }

            if (request.requireDepthImprovement &&
                !baselineConstraintsSatisfied &&
                !depthImproved) {
                report.addWarning(
                    "The candidate did not improve depth, but it is eligible because the original design violated a hard gate constraint.");
            }

            const double remainingTime =
                request.timeLimitSeconds - elapsedSeconds();
            if (remainingTime <= 0.0) {
                summary.wholeDesignTimedOut = true;
                report.success = false;
                report.rolledBack = core.changed;
                report.message =
                    "No time remained for mandatory whole-design equivalence; candidate was discarded.";
                return finishReport(std::move(report));
            }

            const WholeDesignEquivalenceReport equivalence =
                working.checkWholeDesignEquivalence(original, remainingTime);
            summary.wholeDesignEquivalenceChecked = true;
            summary.wholeDesignEquivalent =
                equivalence.ok && equivalence.equivalent;
            summary.wholeDesignTimedOut = equivalence.timeBudgetExceeded;
            summary.comparedOutputCount = equivalence.comparedOutputCount;
            summary.comparedDffDCount = equivalence.comparedDffDCount;

            report.validation.equivalenceChecked = true;
            report.validation.functionallyEquivalent =
                equivalence.ok && equivalence.equivalent;
            report.validation.equivalenceMethod =
                EquivalenceCheckMethod::WholeDesignSat;
            report.validation.messages.push_back(equivalence.message);
            for (const std::string& warning : equivalence.warnings) {
                report.addWarning(warning);
            }
            for (const std::string& reason : equivalence.unsupportedReasons) {
                report.addWarning(reason);
            }

            if (!equivalence.ok || !equivalence.equivalent) {
                report.success = false;
                report.rolledBack = core.changed;
                report.message = equivalence.timeBudgetExceeded
                    ? "Whole-design equivalence timed out; depth candidate was discarded."
                    : "Whole-design equivalence failed; depth candidate was discarded.";
                return finishReport(std::move(report));
            }

            restoreFrom(working);
            summary.candidateAccepted = true;
            report.success = true;
            report.changed = core.changed;
            report.rolledBack = false;
            report.message = depthImproved
                ? "Depth optimization candidate was accepted after whole-design equivalence proof."
                : "Constraint-compliant candidate was accepted after whole-design equivalence proof.";
            return finishReport(std::move(report));
        }
        default:
            return makeFailedOptApplyReport(*this, request.passKind, "Unsupported optimization pass kind.");
    }

    if (!request.candidateIds.empty()) {
        report.addWarning("candidateIds are accepted for API compatibility, but first-version opt_apply applies the whole pass.");
    }
    if (request.validateEquivalence) {
        report.addWarning("validateEquivalence requested, but whole-design equivalence is not integrated yet.");
    }
    if (!request.rollbackOnFailure) {
        report.addWarning("rollbackOnFailure=false is recorded, but current WithReport pass wrappers always rollback on validation failure.");
    }

    return report;
}
