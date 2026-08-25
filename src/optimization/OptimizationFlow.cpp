#include "include/core/Netlist.h"
#include "include/core/Optimizer.h"
#include "include/core/TechMapper.h"
#include "include/core/OptimizationFlow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
        case OptPassKind::DepthMinimization:
            return "critical_path_depth";   // 對外協定名，不隨型別改名而變
        case OptPassKind::GateCountMinimization:
            return "gate_count_minimization";
        default:
            return "unknown";
    }
}

// passKind 決定量什麼、costScope 決定量哪裡。兩者合起來就是核心的 CostMetric。
// 這是「request -> metric」的唯一定義處；報表字串一律由 opt::toString 產生，
// 避免同一組字串在 CostChange 與 OptimizationSummary 各寫一份而漂移。
opt::CostMetric costMetricOf(const OptApplyRequest& request) {
    const bool areaGoal  = (request.passKind == OptPassKind::GateCountMinimization);
    const bool coneScoped = (request.costScope == OptCostScope::ScopedFaninCone);
    return areaGoal ? (coneScoped ? opt::CostMetric::ConeGateCount
                                  : opt::CostMetric::GlobalGateCount)
                    : (coneScoped ? opt::CostMetric::ConeDepth
                                  : opt::CostMetric::GlobalMaxDepth);
}

std::string optimizationStatusName(OptimizationStatus status) {
    switch (status) {
        case OptimizationStatus::SUCCESS:
            return "SUCCESS";
        case OptimizationStatus::NO_IMPROVEMENT:
            return "NO_IMPROVEMENT";
        case OptimizationStatus::TIMEOUT:
            return "TIMEOUT";
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
        case TargetScope::SINGLE_GATE: 
            return "SINGLE_GATE";
        default:
            return "unknown";
    }
}

bool isCombinationalGateType(GateType type) {
    switch (type) {
        case GateType::AND:
        case GateType::OR:
        case GateType::NAND:
        case GateType::NOR:
        case GateType::NOT:
        case GateType::BUF:
        case GateType::XOR:
        case GateType::XNOR:
            return true;
        default:
            return false;
    }
}

bool isValidCostScope(OptCostScope scope) {
    switch (scope) {
        case OptCostScope::WholeDesign:
        case OptCostScope::ScopedFaninCone:
            return true;
        default:
            return false;
    }
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

// 兩區域的 gate 約束檢查。
//   basisScope == WHOLE_NETLIST：整張 netlist 對 outside*Types 檢查
//   basisScope == cone         ：cone 內對 cone*Types，cone 外對 outside*Types
// 舊版只檢查單一 scope，cone 外完全沒驗 —— 異質 basis 題會給出錯誤的 true，
// 讓一張違反 cone 外約束的電路被接受。
bool designSatisfiesGateConstraints(
    const Netlist& netlist,
    TargetScope basisScope,
    const std::string& basisScopeName,
    const std::vector<GateType>& coneAllowed,
    const std::vector<GateType>& coneBanned,
    const std::vector<GateType>& outsideAllowed,
    const std::vector<GateType>& outsideBanned)
{
    const bool checkCone    = !coneAllowed.empty()    || !coneBanned.empty();
    const bool checkOutside = !outsideAllowed.empty() || !outsideBanned.empty();
    if (!checkCone && !checkOutside) return true;

    std::unordered_set<int> coneGates;
    if (basisScope != TargetScope::WHOLE_NETLIST) {
        const RewriteScopeResolution resolved =
            resolveRewriteScope(netlist, basisScope, basisScopeName);
        if (!resolved.ok) return false;
        const std::vector<int> ids = netlist.getConeGateIds(resolved.cone);
        coneGates.insert(ids.begin(), ids.end());
    }

    for (int gateId = 0; gateId < static_cast<int>(netlist.getGateCount()); ++gateId) {
        if (!netlist.isValidGateId(gateId) || netlist.isGateRemoved(gateId)) continue;
        const GateType type = netlist.getGate(gateId).type;
        if (!isCombinationalGateType(type)) continue;

        if (coneGates.count(gateId)) {
            if (checkCone && !gateTypeAllowed(type, coneAllowed, coneBanned)) return false;
        } else {
            if (checkOutside && !gateTypeAllowed(type, outsideAllowed, outsideBanned)) return false;
        }
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
        case TargetScope::SINGLE_GATE:
            // opt_apply 不支援單一 gate scope（parsePublicOptApply 已拒絕）。
            // 退回 gate fanin 只是為了讓 switch 完整，不應該實際走到。
            return ConeQueryType::GateTransitiveFanin;
        case TargetScope::NET_FANIN:
        default:
            return ConeQueryType::NetTransitiveFanin;
    }
}

opt::ConeRef makeConeRef(TargetScope scope, const std::string& name) {
    opt::ConeRef ref;
    ref.type       = coneQueryTypeForScope(scope);
    ref.sourceName = name;
    return ref;
}

// basisScope 未明確設定時的相容推導。
// 舊呼叫端只有一組 scope/scopeName，語意是「有 gate 約束 + 非全域 scope
// = 約束該 cone」。只在 GlobalMaximum 時套用 —— ScopedFaninCone 代表
// scope 講的是成本函數，不是基底範圍。
struct ResolvedBasisScope {
    TargetScope scope = TargetScope::WHOLE_NETLIST;
    std::string name;
    bool inferred = false;
};

ResolvedBasisScope resolveBasisScope(const OptApplyRequest& request) {
    ResolvedBasisScope out;
    if (request.basisScope != TargetScope::WHOLE_NETLIST &&
        !request.basisScopeName.empty()) {
        out.scope = request.basisScope;
        out.name  = request.basisScopeName;
        return out;
    }
    // 舊呼叫端只有一組約束，語意是「有 gate 約束 + 非全域 scope = 約束該 cone」。
    // 正規化之後那組約束在 outside*Types，推導成立時要搬回 cone 側。
    const bool hasGateConstraints =
        !request.outsideAllowedTypes.empty() || !request.outsideBannedTypes.empty();
    if (hasGateConstraints &&
        request.scope != TargetScope::WHOLE_NETLIST &&
        request.costScope == OptCostScope::WholeDesign) {
        out.scope    = request.scope;
        out.name     = request.scopeName;
        out.inferred = true;
    }
    return out;
}

int measureCostObjective(const Netlist& netlist, const OptApplyRequest& request) {
    const bool areaGoal = (request.passKind == OptPassKind::GateCountMinimization);
    const bool coneScoped = (request.costScope == OptCostScope::ScopedFaninCone);

    if (!coneScoped) {
        if (!areaGoal) return netlist.findGlobalCriticalPath().depth;
        // 全域閘數含 DFF：DFF 數量在最佳化前後不變（常數偏移，不影響挑候選），
        // 但回報的絕對值要跟評分器算的一致。
        int total = 0;
        for (const auto& pair : netlist.countGatesByType()) total += pair.second;
        return total;
    }

    const RewriteScopeResolution resolved =
        resolveRewriteScope(netlist, request.scope, request.scopeName);
    if (!resolved.ok) return -1;

    if (areaGoal) {
        return static_cast<int>(netlist.getConeGateCount(resolved.cone));
    }

    std::string rootName = resolved.resolvedRootNetName;
    if (rootName.empty() && !resolved.cone.rootNetIds.empty()) {
        const int rootNetId = resolved.cone.rootNetIds.front();
        if (netlist.isValidNetId(rootNetId)) {
            rootName = netlist.getNet(rootNetId).name;
        }
    }
    return rootName.empty() ? -1 : netlist.getMaxDepthToNet(rootName);
}

CostChange buildOptimizationCostChange(
    const Netlist& before,
    const Netlist& after,
    const OptApplyRequest& request)
{
    const bool areaGoal = (request.passKind == OptPassKind::GateCountMinimization);
    const bool coneScoped = (request.costScope == OptCostScope::ScopedFaninCone);

    CostChange change;
    change.metricName = opt::toString(costMetricOf(request));
    change.targetName = coneScoped
        ? request.scopeName
        : (areaGoal ? std::string("whole_netlist")
                    : before.findGlobalCriticalPath().endpointName);

    change.beforeValue = measureCostObjective(before, request);
    change.afterValue  = measureCostObjective(after, request);
    change.targetValue = request.targetCost;
    change.improved =
        change.beforeValue >= 0 &&
        change.afterValue  >= 0 &&
        change.afterValue  <  change.beforeValue;
    change.meetsTarget =
        request.targetCost >= 0 &&
        change.afterValue  >= 0 &&
        change.afterValue  <= request.targetCost;
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
    const RewriteScopeResolution& scope,
    const std::vector<GateType>& uniformAllowed,
    const std::vector<GateType>& uniformBanned)
{
    ScopedDepthLowerBoundProof proof;
    if (request.costScope != OptCostScope::ScopedFaninCone) {
        return proof;
    }

    const int currentDepth = measureCostObjective(netlist, request);
    if (currentDepth == 0) {
        proof.proven = true;
        proof.lowerBound = 0;
        proof.reason =
            "The scoped root already has depth 0, which is the absolute lower bound.";
        return proof;
    }

    if (currentDepth != 2 ||
        !hasExactAllowedBasis(uniformAllowed, uniformBanned,
                              GateType::NAND, GateType::NOT)) {
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
    report.operationKind =
        passKind == OptPassKind::GateCountMinimization
            ? NetlistEditOperationKind::AreaOptimization
            : (passKind == OptPassKind::DepthMinimization
                   ? NetlistEditOperationKind::DepthOptimization
                   : NetlistEditOperationKind::CustomRewrite);
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

bool isLegacyWholeDesignPass(OptPassKind passKind) {
    return passKind == OptPassKind::CleanupBufferChain ||
           passKind == OptPassKind::CollapseDoubleInverter ||
           passKind == OptPassKind::LocalSimplificationFixpoint;
}

std::vector<std::string> unsupportedLegacyOptApplyFields(
    const OptApplyRequest& request)
{
    const OptApplyRequest defaults;
    std::vector<std::string> fields;
    if (request.candidateIds != defaults.candidateIds) fields.push_back("candidateIds");
    if (request.scope != defaults.scope) fields.push_back("scope");
    if (request.scopeName != defaults.scopeName) fields.push_back("scopeName");
    if (request.costScope != defaults.costScope) {
        fields.push_back("costScope");
    }
    if (request.basisScope != defaults.basisScope) fields.push_back("basisScope");
    if (request.basisScopeName != defaults.basisScopeName) fields.push_back("basisScopeName");
    if (request.allowedTypes != defaults.allowedTypes) fields.push_back("allowedTypes");
    if (request.bannedTypes != defaults.bannedTypes) fields.push_back("bannedTypes");
    if (request.targetCost != defaults.targetCost) fields.push_back("targetCost");
    if (request.timeLimitSeconds != defaults.timeLimitSeconds) {
        fields.push_back("timeLimitSeconds");
    }
    if (request.requireCostImprovement != defaults.requireCostImprovement) {
        fields.push_back("requireCostImprovement");
    }
    if (request.validateEquivalence != defaults.validateEquivalence) {
        fields.push_back("validateEquivalence");
    }
    if (request.rollbackOnFailure != defaults.rollbackOnFailure) {
        fields.push_back("rollbackOnFailure");
    }
    if (request.outsideAllowedTypes != defaults.outsideAllowedTypes)
        fields.push_back("outsideAllowedTypes");
    if (request.outsideBannedTypes != defaults.outsideBannedTypes)
        fields.push_back("outsideBannedTypes");
    return fields;
}

std::string formatUnsupportedLegacyFields(
    const std::vector<std::string>& fields)
{
    std::string message =
        "This legacy OptApply pass only supports its default whole-design operation; "
        "unsupported request field(s): ";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) message += ", ";
        message += fields[i];
    }
    message += ". Use EditApply for scoped or explicitly constrained cleanup.";
    return message;
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
        case OptPassKind::DepthMinimization:
        case OptPassKind::GateCountMinimization: {
            const bool areaGoal =
                (request.passKind == OptPassKind::GateCountMinimization);

            OptCandidate candidate;
            candidate.id = candidateId++;
            candidate.passKind = request.passKind;
            candidate.reason = areaGoal
                ? "Run best-effort gate-count minimization, then validate the "
                  "gate-type constraints."
                : "Run best-effort critical-path restructuring, then validate depth "
                  "and gate-type constraints.";
            candidate.estimatedGateDelta = 0;
            candidate.estimatedNetDelta = 0;
            // 這個 pass 不自己做全設計 CEC；等價驗證交給 equiv_query。
            candidate.requiresEquivalenceCheck = false;
            report.candidates.push_back(candidate);
            report.ok = true;
            report.message = areaGoal
                ? "Gate-count minimization is available as a pass-level candidate."
                : "Critical-path depth optimization is available as a pass-level candidate.";
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
    const request_time_budget::RequestDeadline deadline(request.timeLimitSeconds);

    if (isLegacyWholeDesignPass(request.passKind)) {
        const std::vector<std::string> unsupportedFields =
            unsupportedLegacyOptApplyFields(request);
        if (!unsupportedFields.empty()) {
            return makeFailedOptApplyReport(
                *this,
                request.passKind,
                formatUnsupportedLegacyFields(unsupportedFields));
        }
    }

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
            report = runLocalSimplificationFixpointWithReport(&deadline);
            report.operationName = "opt_apply:local_simplification_fixpoint";
            break;
        case OptPassKind::DepthMinimization:
        case OptPassKind::GateCountMinimization: {
            // 這一整段 case 由 passKind 決定量深度還是量閘數。
            // 這三個值在後面每個 report 建構點都要用，集中在這裡算一次。
            const bool areaGoal =
                (request.passKind == OptPassKind::GateCountMinimization);
            const char* opName = areaGoal ? "opt_apply:gate_count_minimization"
                                          : "opt_apply:critical_path_depth";
            const NetlistEditOperationKind opKind =
                areaGoal ? NetlistEditOperationKind::AreaOptimization
                         : NetlistEditOperationKind::DepthOptimization;
            // 所有對外訊息都用這個字，否則面積模式會回報 "Depth optimization ..."。
            const char* goalLabel = areaGoal ? "Gate count optimization"
                                             : "Depth optimization";
            const char* targetLabel = areaGoal ? "targetCost (gate count)"
                                               : "targetCost (depth)";

            auto elapsedSeconds = [&]() { return deadline.elapsedSeconds(); };

            if (!std::isfinite(request.timeLimitSeconds) ||
                request.timeLimitSeconds <= 0.0) {
                return makeFailedOptApplyReport(
                    *this,
                    request.passKind,
                    "timeLimitSeconds must be finite and positive.");
            }
            if (request.targetCost < -1) {
                return makeFailedOptApplyReport(
                    *this, request.passKind, "targetCost must be -1 or a non-negative value.");
            }
            if (!isValidCostScope(request.costScope)) {
                return makeFailedOptApplyReport(
                    *this, request.passKind, "Unsupported cost scope.");
            }
            if (!hasValidGateTypeConstraints(request.allowedTypes, request.bannedTypes) ||
                !hasValidGateTypeConstraints(request.outsideAllowedTypes,
                                             request.outsideBannedTypes)) {
                return makeFailedOptApplyReport(
                    *this, request.passKind,
                    "allowedTypes/bannedTypes must contain valid, non-overlapping "
                    "combinational gate types.");
            }
            if (request.basisScope == TargetScope::WHOLE_NETLIST &&
                (!request.allowedTypes.empty() || !request.bannedTypes.empty())) {
                return makeFailedOptApplyReport(
                    *this, request.passKind,
                    "allowedTypes/bannedTypes are cone-only; without basisScope put the "
                    "constraint in outsideAllowedTypes/outsideBannedTypes.");
            }
            if (request.costScope == OptCostScope::ScopedFaninCone &&
                request.scope != TargetScope::NET_FANIN &&
                request.scope != TargetScope::GATE_FANIN) {
                return makeFailedOptApplyReport(
                    *this,
                    request.passKind,
                    "A cone-scoped cost function requires NET_FANIN or GATE_FANIN scope.");
            }

            const RewriteScopeResolution originalScope =
                resolveRewriteScope(*this, request.scope, request.scopeName);
            if (!originalScope.ok) {
                return makeFailedOptApplyReport(
                    *this,
                    request.passKind,
                    "Failed to resolve optimization scope: " + originalScope.message);
            }

            const ResolvedBasisScope basisScope = resolveBasisScope(request);
            // 推導成立時，request 的 outside* 其實是那個 cone 的約束，搬過來。
            std::vector<GateType> coneAllowed    = request.allowedTypes;
            std::vector<GateType> coneBanned     = request.bannedTypes;
            std::vector<GateType> outsideAllowed = request.outsideAllowedTypes;
            std::vector<GateType> outsideBanned  = request.outsideBannedTypes;
            if (basisScope.inferred) {
                coneAllowed = std::move(outsideAllowed);
                coneBanned  = std::move(outsideBanned);
                outsideAllowed.clear();
                outsideBanned.clear();
            }
            if (basisScope.scope != TargetScope::WHOLE_NETLIST) {
                const RewriteScopeResolution basisResolution =
                    resolveRewriteScope(*this, basisScope.scope, basisScope.name);
                if (!basisResolution.ok) {
                    return makeFailedOptApplyReport(
                        *this, request.passKind,
                        "Failed to resolve the gate-constraint scope: " + basisResolution.message);
                }
            }

            const Netlist original = cloneForRollback();
            Netlist working = original.cloneForRollback();
            // 約束檢查一律以「基底作用域」為準，不是成本作用域。
            const bool baselineConstraintsSatisfied = designSatisfiesGateConstraints(
                original, basisScope.scope, basisScope.name,
                coneAllowed, coneBanned, outsideAllowed, outsideBanned);

            OptimizationSummary summary;
            summary.objectiveMetric = opt::toString(costMetricOf(request));
            summary.scope = targetScopeName(request.scope);
            summary.requestedScopeName = request.scopeName;
            summary.resolvedRootNetName = originalScope.resolvedRootNetName;
            summary.resolvedThroughDffDataPin =
                originalScope.resolvedThroughDffDataPin;
            // 有 cone 時回報 cone 的約束（那是 basisScope 指向的範圍）；
            // 沒有 cone 時 coneAllowed 為空，回報全域那組。
            summary.allowedTypes = coneAllowed.empty() ? outsideAllowed : coneAllowed;
            summary.bannedTypes  = coneBanned.empty()  ? outsideBanned  : coneBanned;
            summary.outsideAllowedTypes = outsideAllowed;
            summary.outsideBannedTypes  = outsideBanned;
            summary.basisScope     = targetScopeName(basisScope.scope);
            summary.basisScopeName = basisScope.name;
            summary.baselineConstraintsSatisfied = baselineConstraintsSatisfied;
            summary.timeBudgetSeconds = request.timeLimitSeconds;

            auto makePreCoreTimeoutReport = [&]() {
                NetlistEditReport timeoutReport = Netlist::buildEditReport(
                    original,
                    original,
                    opName,
                    opKind);
                timeoutReport.success = false;
                timeoutReport.changed = false;
                timeoutReport.rolledBack = false;
                timeoutReport.costChange =
                    buildOptimizationCostChange(original, original, request);
                summary.coreStatus = "TIMEOUT";
                summary.coreMessage =
                    "The transaction time budget was exhausted before optimizer core execution.";
                summary.finalConstraintsSatisfied = baselineConstraintsSatisfied;
                summary.candidateGenerated = false;
                summary.candidateAccepted = false;
                summary.elapsedSeconds = elapsedSeconds();
                timeoutReport.optimization = summary;
                Netlist::certifyEquivalence(
                    timeoutReport,
                    EquivalenceCheckMethod::StructuralIdentity,
                    "The original design was retained because no optimizer candidate was started.");
                timeoutReport.message = summary.coreMessage;
                return timeoutReport;
            };

            if (elapsedSeconds() >= request.timeLimitSeconds) {
                return makePreCoreTimeoutReport();
            }

            if (request.scope != TargetScope::WHOLE_NETLIST &&
                original.getConeGateCount(originalScope.cone) == 0) {
                NetlistEditReport originalReport = Netlist::buildEditReport(
                    original,
                    original,
                    opName,
                    opKind);
                originalReport.costChange =
                    buildOptimizationCostChange(original, original, request);

                summary.coreStatus = "NO_IMPROVEMENT";
                summary.coreMessage =
                    "The resolved optimization scope contains no combinational gates; "
                    "the original design was retained.";
                summary.finalConstraintsSatisfied = true;
                summary.candidateGenerated = false;
                summary.candidateAccepted = false;
                summary.elapsedSeconds = elapsedSeconds();
                originalReport.optimization = summary;
                Netlist::certifyEquivalence(
                    originalReport,
                    EquivalenceCheckMethod::StructuralIdentity,
                    "The original design was retained because the resolved optimization scope is empty.");

                const bool targetMet =
                    request.targetCost < 0 ||
                    (originalReport.costChange.has_value() &&
                     originalReport.costChange->meetsTarget);
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

            // 這個 lower bound 證的是「NAND/NOT 下 AND(a,b) 至少要 2 層」，
            // 前提是單一均勻 basis、而且目標是深度。
            // 面積目標下它證的東西無關（閘數的下界是另一回事），
            // 異質 basis 下前提也不成立。
            const bool uniformBasis =
                (basisScope.scope == TargetScope::WHOLE_NETLIST) &&
                coneAllowed.empty() && coneBanned.empty();
            const bool lowerBoundApplicable =
                uniformBasis && !areaGoal &&
                request.costScope == OptCostScope::ScopedFaninCone;

            const ScopedDepthLowerBoundProof lowerBound = lowerBoundApplicable
                ? proveScopedDepthLowerBound(original, request, originalScope,
                                             outsideAllowed, outsideBanned)
                : ScopedDepthLowerBoundProof{};
            if (elapsedSeconds() >= request.timeLimitSeconds) {
                return makePreCoreTimeoutReport();
            }
            if (baselineConstraintsSatisfied && lowerBound.proven) {
                NetlistEditReport originalReport = Netlist::buildEditReport(
                    original,
                    original,
                    opName,
                    opKind);
                originalReport.costChange =
                    buildOptimizationCostChange(original, original, request);

                summary.coreStatus = "NO_IMPROVEMENT";
                summary.coreMessage = lowerBound.reason;
                summary.finalConstraintsSatisfied = true;
                summary.candidateGenerated = false;
                summary.candidateAccepted = false;
                summary.elapsedSeconds = elapsedSeconds();
                originalReport.optimization = summary;
                Netlist::certifyEquivalence(
                    originalReport,
                    EquivalenceCheckMethod::StructuralIdentity,
                    "The original design was retained under a proven scoped-depth lower bound.");

                if (request.targetCost >= 0 &&
                    lowerBound.lowerBound > request.targetCost) {
                    originalReport.success = false;
                    originalReport.message =
                        "targetCost is below the proven scoped-depth lower bound; "
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

            opt::OptimizationRequest coreRequest;
            {
                coreRequest.cost.metric = costMetricOf(request);
                if (request.costScope == OptCostScope::ScopedFaninCone) {
                    coreRequest.cost.cone = makeConeRef(request.scope, request.scopeName);
                }
            }
            {
                // cone 外（無 cone 時就是全域）。永遠推進去，即使沒有約束 ——
                // 核心用「沒有 scope 的那組」來判斷 cone 外的 basis。
                opt::BasisConstraint outside;
                outside.allowed = outsideAllowed;
                outside.banned  = outsideBanned;
                coreRequest.basisConstraints.push_back(std::move(outside));

                if (basisScope.scope != TargetScope::WHOLE_NETLIST) {
                    opt::BasisConstraint cone;
                    cone.allowed = coneAllowed;
                    cone.banned  = coneBanned;
                    cone.scope   = makeConeRef(basisScope.scope, basisScope.name);
                    coreRequest.basisConstraints.push_back(std::move(cone));
                }
            }

            if (elapsedSeconds() >= request.timeLimitSeconds) {
                return makePreCoreTimeoutReport();
            }

            TechMapper techMapper(&deadline);
            Optimizer optimizer;
            const OptimizationResult core = optimizer.executeOptimization(
                working, techMapper, coreRequest, request.verbose, &deadline);

            summary.coreStatus = optimizationStatusName(core.status);
            summary.coreMessage = core.message;
            summary.candidateGenerated = core.changed;
            summary.finalConstraintsSatisfied = designSatisfiesGateConstraints(
                working, basisScope.scope, basisScope.name,
                coneAllowed, coneBanned, outsideAllowed, outsideBanned);

            auto buildCandidateReport = [&]() {
                NetlistEditReport candidate = Netlist::buildEditReport(
                    original,
                    working,
                    opName,
                    opKind);
                candidate.changed = candidate.changed || core.changed;
                candidate.costChange =
                    buildOptimizationCostChange(original, working, request);
                return candidate;
            };

            auto finishReport = [&](NetlistEditReport candidate) {
                summary.elapsedSeconds = elapsedSeconds();
                candidate.optimization = summary;
                if (basisScope.inferred) {
                    candidate.addWarning(
                        "The gate-constraint scope was inferred from 'scope'. Set basisScope/"
                        "basisScopeName explicitly when the cost function and the gate "
                        "constraint apply to different parts of the design.");
                }
                if (request.validateEquivalence) {
                    candidate.addWarning(
                        "This pass does not run whole-design equivalence checking; use the "
                        "dedicated equivalence-verification command instead.");
                }
                if (!request.rollbackOnFailure) {
                    candidate.addWarning(
                        std::string(goalLabel) +
                        " always keeps the original design on failure; "
                        "rollbackOnFailure=false is ignored.");
                }
                if (!request.candidateIds.empty()) {
                    candidate.addWarning(
                        std::string(goalLabel) +
                        " is a pass-level search; candidateIds are currently ignored.");
                }
                return candidate;
            };

            if (core.status == OptimizationStatus::ERROR_NOT_EQUIVALENT) {
                report = buildCandidateReport();
                report.success = false;
                report.changed = false;
                report.rolledBack = true;
                report.validation.equivalenceChecked = true;
                report.validation.functionallyEquivalent = false;
                report.validation.equivalenceMethod = EquivalenceCheckMethod::WholeDesignSat;
                report.validation.messages.push_back(core.message);
                summary.candidateAccepted = false;
                summary.wholeDesignEquivalenceChecked = true;
                summary.wholeDesignEquivalent = false;
                report.message =
                    "The optimized candidate was not functionally equivalent and was discarded; "
                    "the original design was retained.";
                return finishReport(std::move(report));
            }

            if (core.status != OptimizationStatus::SUCCESS &&
                core.status != OptimizationStatus::NO_IMPROVEMENT) {
                report = buildCandidateReport();
                report.success = false;
                report.rolledBack = core.changed;
                report.message = std::string(goalLabel) +
                                 " did not produce a usable candidate: " + core.message;
                return finishReport(std::move(report));
            }

            report = buildCandidateReport();
            if (!report.success) {
                report.rolledBack = core.changed;
                report.message = std::string(goalLabel) +
                                 " candidate failed structural validation and was discarded.";
                return finishReport(std::move(report));
            }
            if (!summary.finalConstraintsSatisfied) {
                report.success = false;
                report.rolledBack = core.changed;
                report.message = std::string(goalLabel) +
                                 " candidate violated the requested gate constraints and was discarded.";
                return finishReport(std::move(report));
            }

            const bool costImproved =
                report.costChange.has_value() && report.costChange->improved;
            const bool targetMet =
                request.targetCost < 0 ||
                (report.costChange.has_value() && report.costChange->meetsTarget);
            if (!targetMet) {
                report.success = false;
                report.rolledBack = core.changed;
                report.message = std::string(goalLabel) + " candidate did not meet " +
                                 targetLabel + " and was discarded.";
                return finishReport(std::move(report));
            }

            if (!report.changed) {
                report.success = true;
                report.changed = false;
                report.rolledBack = false;
                summary.candidateGenerated = false;
                summary.candidateAccepted = false;
                summary.wholeDesignEquivalenceChecked = false;
                summary.wholeDesignEquivalent = true;
                summary.wholeDesignTimedOut = false;
                Netlist::certifyEquivalence(
                    report,
                    EquivalenceCheckMethod::StructuralIdentity,
                    "The optimizer produced no graph change; the original design is structurally identical.");
                report.message =
                    "The design is already optimal; the original was retained.";
                return finishReport(std::move(report));
            }

            if (request.requireCostImprovement &&
                baselineConstraintsSatisfied &&
                !costImproved) {
                NetlistEditReport originalReport = Netlist::buildEditReport(
                    original,
                    original,
                    opName,
                    opKind);
                originalReport.success = true;
                originalReport.changed = false;
                originalReport.rolledBack = core.changed;
                originalReport.costChange =
                    buildOptimizationCostChange(original, original, request);
                Netlist::certifyEquivalence(
                    originalReport,
                    EquivalenceCheckMethod::StructuralIdentity,
                    "The original design was retained because no accepted depth improvement was found.");
                originalReport.message =
                    std::string("No ") + (areaGoal ? "gate count" : "depth") +
                    " improvement was found; the original design was retained.";
                return finishReport(std::move(originalReport));
            }

            if (request.requireCostImprovement &&
                !baselineConstraintsSatisfied &&
                !costImproved) {
                report.addWarning(
                    std::string("The candidate did not improve ") +
                    (areaGoal ? "gate count" : "depth") +
                    ", but it is eligible because the original design violated a "
                    "hard gate constraint.");
            }

            // Competition runtime skips whole-design CEC. The accepted candidate
            // comes from the qualified function-preserving rewrite/lowering pipeline.
            restoreFrom(working);
            summary.candidateAccepted = true;
            summary.wholeDesignEquivalenceChecked = false;
            summary.wholeDesignTimedOut = false;

            report.success = true;
            report.changed = core.changed;
            report.rolledBack = false;
            Netlist::certifyEquivalence(
                report,
                EquivalenceCheckMethod::CertifiedRewrite,
                "Equivalence certified by the qualified function-preserving optimization and lowering pipeline; whole-design SAT was not executed.");

            if (report.costChange.has_value()) {
                report.message = std::string(areaGoal ? "Gate count" : "Depth")
                               + " reduced from "
                               + std::to_string(report.costChange->beforeValue) + " to "
                               + std::to_string(report.costChange->afterValue) + ".";
            } else {
                report.message = areaGoal ? "Gate count optimization applied."
                                          : "Depth optimization applied.";
            }
            return finishReport(std::move(report));
        }
        default:
            return makeFailedOptApplyReport(*this, request.passKind, "Unsupported optimization pass kind.");
    }

    if (isLegacyWholeDesignPass(request.passKind) && request.verbose) {
        report.addWarning(
            "verbose is not implemented for this legacy whole-design OptApply pass.");
    }

    return report;
}
