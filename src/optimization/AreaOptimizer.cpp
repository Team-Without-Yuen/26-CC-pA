// AreaOptimizer.cpp
//
// New file. Does not modify DepthOptimizer.cpp, OptimizationFlow.cpp,
// TechMapper.cpp, or Netlist.cpp. Implements include/core/AreaOptimizer.h.
//
// Tags used throughout, same convention as AreaOptimizer.h:
//   [REUSED]  calls an existing public function/type as-is.
//   [COPIED]  logic that exists elsewhere but was private/file-local, so the
//             same logic is duplicated here.
//   [NEW]     genuinely new code written for this file.

#include "include/core/AreaOptimizer.h"

#include "include/core/MockturtleConverter.h"   // [REUSED]

#include <mockturtle/networks/aig.hpp>
#include <mockturtle/networks/xag.hpp>
#include <mockturtle/views/fanout_view.hpp>
#include <mockturtle/views/depth_view.hpp>
#include <mockturtle/algorithms/cleanup.hpp>
#include <mockturtle/algorithms/cut_rewriting.hpp>
#include <mockturtle/algorithms/resubstitution.hpp>
#include <mockturtle/algorithms/aig_resub.hpp>
#include <mockturtle/algorithms/xag_resub.hpp>
#include <mockturtle/algorithms/node_resynthesis/xag_npn.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <unordered_set>

// =========================================================================
// Part 1: AreaOptimizer engine (mirrors DepthOptimizer's Stage 1/2/3 shape)
// =========================================================================

namespace {

// [COPIED] Same gate-basis allow/ban check used in DepthOptimizer.cpp's
// isGateAllowed(). Logic is ~10 lines, duplicated rather than exposed.
bool isGateAllowedArea(GateType type,
                        const std::vector<GateType>& allowedTypes,
                        const std::vector<GateType>& bannedTypes) {
    if (std::find(bannedTypes.begin(), bannedTypes.end(), type) != bannedTypes.end()) {
        return false;
    }
    if (!allowedTypes.empty() &&
        std::find(allowedTypes.begin(), allowedTypes.end(), type) == allowedTypes.end()) {
        return false;
    }
    return true;
}

// [NEW] gate count helper: total *combinational* (non-DFF, non-UNKNOWN,
// non-removed) gates. Uses [REUSED] Netlist::countGatesByType().
int totalCombinationalGateCount(const Netlist& netlist) {
    int total = 0;
    for (const auto& pair : netlist.countGatesByType()) {
        if (pair.first == GateType::DFF || pair.first == GateType::UNKNOWN) continue;
        total += pair.second;
    }
    return total;
}

} // namespace

AreaOptimizer::AreaOptimizer(const AreaOptimizerConfig& config) : config(config) {}

// [COPIED] Forward declaration; defined further below, right after
// executeAreaOptimization(). Same structural-identity comparison used
// internally by DepthOptimizer.cpp's anonymous-namespace sameNetlistGraph().
bool sameNetlistGraphAsOriginal(const Netlist& lhs, const Netlist& rhs);


// [COPIED] Identical logic to DepthOptimizer::eliminateDoubleInverters().
// Duplicated because the original is a private method.
int AreaOptimizer::eliminateDoubleInverters(Netlist& netlist) {
    int removed = 0;

    std::queue<int> worklist;
    std::unordered_set<int> queued;
    auto enqueue = [&](int gateId) {
        if (gateId < 0 || gateId >= (int)netlist.getGateCount()) return;
        if (netlist.getGate(gateId).type != GateType::NOT) return;
        if (queued.insert(gateId).second) worklist.push(gateId);
    };
    for (int g = 0; g < (int)netlist.getGateCount(); ++g) enqueue(g);

    while (!worklist.empty()) {
        const int g = worklist.front();
        worklist.pop();
        queued.erase(g);

        Gate& gate = netlist.getGateMutable(g);
        if (gate.type != GateType::NOT) continue;
        if (gate.inputNetIds.empty() || gate.inputNetIds[0] < 0) continue;

        int midNet = gate.inputNetIds[0];
        int drv = netlist.getNet(midNet).driverGateId;
        if (drv < 0 || drv >= (int)netlist.getGateCount()) continue;
        if (netlist.getGate(drv).type != GateType::NOT) continue;
        if (netlist.getGate(drv).inputNetIds.empty()) continue;

        int srcNet = netlist.getGate(drv).inputNetIds[0];
        int outNet = gate.outputNetId;
        if (srcNet < 0 || outNet < 0) continue;
        if (netlist.getNet(outNet).isPO) continue;

        const std::vector<int> rewiredLoads = netlist.getNet(outNet).loadGateIds;
        redirectNetLoads(netlist, outNet, srcNet);  // [REUSED] free function, MockturtleConverter.h
        netlist.removeGate(g);
        // Only remove the upstream NOT if nothing else still reads midNet.
        if (netlist.getNet(midNet).loadGateIds.empty()) {
            netlist.removeGate(drv);
        }
        ++removed;

        for (int loadGateId : rewiredLoads) enqueue(loadGateId);
    }

    return removed;
}

// [NEW] High-level entry point. Mirrors
// DepthOptimizer::executeCriticalPathOptimization()'s three-stage shape,
// re-scored for gate count instead of depth.
OptimizationResult AreaOptimizer::executeAreaOptimization(
    Netlist& netlist,
    TechMapper& techMapper,
    const ConeReport& targetConeReport,
    const std::vector<GateType>& allowedTypes,
    const std::vector<GateType>& bannedTypes,
    bool verbose,
    const request_time_budget::RequestDeadline* requestDeadline) {

    OptimizationResult result;
    result.passName = "Opt_Area";
    const Netlist originalSnapshot = netlist.cloneForRollback();  // [REUSED]

    result.oldDepth = netlist.findGlobalCriticalPath().depth;     // [REUSED], kept for parity
    result.oldGateCount = totalCombinationalGateCount(netlist);   // [NEW]

    auto requestTimedOut = [&]() {
        return requestDeadline != nullptr && requestDeadline->expired();
    };
    auto returnTimeout = [&](const std::string& message) {
        netlist.restoreFrom(originalSnapshot);   // [REUSED]
        result.status = OptimizationStatus::TIMEOUT;
        result.changed = false;
        result.depthImproved = false;
        result.newDepth = result.oldDepth;
        result.newGateCount = result.oldGateCount;
        result.areaDelta = 0;
        result.message = message;
        return result;
    };

    if (requestTimedOut()) {
        return returnTimeout("Area optimization could not start because the request deadline expired.");
    }

    if (verbose) {
        std::cout << "\n=================================================\n";
        std::cout << "[Area Flow Start] Old Gate Count: " << result.oldGateCount << "\n";
    }

    // -----------------------------------------------------------------
    // Stage 1: constraint classification. [COPIED] same isPureAIG/isPureXAG
    // shape as DepthOptimizer, short enough to re-derive rather than expose.
    // -----------------------------------------------------------------
    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());
    bool isPureAIG = bannedTypes.empty() && allowedSet.size() == 2 &&
                     allowedSet.count(GateType::AND) && allowedSet.count(GateType::NOT);
    bool hasLocalConeConstraint = targetConeReport.ok && targetConeReport.exists;
    bool useGlobalAIG = !hasLocalConeConstraint && isPureAIG;

    // -----------------------------------------------------------------
    // Stage 2: global gate-count compression via mockturtle.
    //
    // [COPIED + MODIFIED] Same NetlistToXag/XagToNetlist round-trip as
    // DepthOptimizer Stage 2, but deliberately configured for size instead
    // of depth:
    //   - no balancing() pass (balancing trades area for depth; not wanted
    //     here)
    //   - cut_rewriting_params.preserve_depth = false (depth version: true)
    //   - cut_rewriting_params.allow_zero_gain = false (depth version: true;
    //     area optimization only accepts strictly smaller rewrites)
    //   - progress measured by totalCombinationalGateCount() instead of
    //     findGlobalCriticalPath().depth
    //   - resubstitution kept: it is fundamentally a size-reduction
    //     technique already used by the depth version as a side effect;
    //     here it is a primary tool.
    // -----------------------------------------------------------------
    constexpr double kStage2TimeLimitSeconds = 120.0;
    const double stage2BudgetSeconds = requestDeadline != nullptr
        ? requestDeadline->boundedStageSeconds(kStage2TimeLimitSeconds)
        : kStage2TimeLimitSeconds;
    const auto stage2Start = std::chrono::steady_clock::now();
    auto stage2TimeUp = [&]() {
        std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - stage2Start;
        return requestTimedOut() || elapsed.count() >= stage2BudgetSeconds;
    };

    if (stage2BudgetSeconds <= 0.0) {
        return returnTimeout("Area optimization could not enter Stage 2 because the request deadline expired.");
    }

    if (useGlobalAIG) {
        if (verbose) std::cout << "[Area Step 2] Global AIG size optimization...\n";

        mockturtle::aig_network aig0 = mockturtle::cleanup_dangling(NetlistToAig(netlist));  // [REUSED]
        mockturtle::aig_network candA = aig0, best = aig0;
        Netlist bestProbe = AigToNetlist(candA, netlist);
        eliminateDoubleInverters(bestProbe);
        int bestGateCount = totalCombinationalGateCount(bestProbe);

        for (int i = 0; i < 10; ++i) {
            if (stage2TimeUp()) {
                if (verbose) std::cout << "  -> Stage 2 time limit reached at iteration " << i << ".\n";
                break;
            }

            mockturtle::cut_rewriting_params cr;
            cr.cut_enumeration_ps.cut_size = 4;
            cr.preserve_depth = false;
            cr.allow_zero_gain = false;
            mockturtle::xag_npn_resynthesis<mockturtle::aig_network> resyn;
            candA = mockturtle::cleanup_dangling(mockturtle::cut_rewriting(candA, resyn, cr));

            {
                mockturtle::resubstitution_params rp;
                mockturtle::fanout_view fv{candA};
                mockturtle::depth_view dv{fv};  // [REUSED] resubstitution requires level(); same wrapper DepthOptimizer.cpp uses
                mockturtle::aig_resubstitution(dv, rp);
                candA = mockturtle::cleanup_dangling(candA);
            }

            Netlist probe = AigToNetlist(candA, netlist);
            eliminateDoubleInverters(probe);
            const int gateCount = totalCombinationalGateCount(probe);
            if (gateCount < bestGateCount) {
                bestGateCount = gateCount;
                best = candA;
            } else {
                break;
            }
        }
        netlist = AigToNetlist(best, netlist);
        eliminateDoubleInverters(netlist);
        if (verbose) std::cout << "  -> AIG netlist gate count: " << totalCombinationalGateCount(netlist) << "\n";

    } else {
        if (verbose) std::cout << "[Area Step 2] Global XAG size optimization...\n";

        mockturtle::xag_network xag = mockturtle::cleanup_dangling(NetlistToXag(netlist));  // [REUSED]
        mockturtle::xag_network best = xag;
        Netlist initialProbe = XagToNetlist(xag, netlist);
        eliminateDoubleInverters(initialProbe);
        int bestGateCount = totalCombinationalGateCount(initialProbe);

        for (int iter = 0; iter < 10; ++iter) {
            if (stage2TimeUp()) {
                if (verbose) std::cout << "  -> Stage 2 time limit reached at iteration " << iter << ".\n";
                break;
            }

            mockturtle::cut_rewriting_params cr_ps;
            cr_ps.cut_enumeration_ps.cut_size = 4;
            cr_ps.preserve_depth = false;
            cr_ps.allow_zero_gain = false;
            mockturtle::xag_npn_resynthesis<mockturtle::xag_network> resyn;
            xag = mockturtle::cleanup_dangling(mockturtle::cut_rewriting(xag, resyn, cr_ps));

            {
                mockturtle::resubstitution_params rp;
                mockturtle::fanout_view fv{xag};
                mockturtle::depth_view dv{fv};  // [REUSED] same wrapper DepthOptimizer.cpp uses
                mockturtle::xag_resubstitution(dv, rp);
                xag = mockturtle::cleanup_dangling(xag);
            }

            Netlist probe = XagToNetlist(xag, netlist);
            eliminateDoubleInverters(probe);
            const int gateCount = totalCombinationalGateCount(probe);
            if (gateCount < bestGateCount) {
                bestGateCount = gateCount;
                best = xag;
            } else {
                break;
            }
        }
        netlist = XagToNetlist(best, netlist);
        eliminateDoubleInverters(netlist);
        if (verbose) std::cout << "  -> XAG best gate count: " << totalCombinationalGateCount(netlist) << "\n";
    }

    if (requestTimedOut()) {
        return returnTimeout("Area optimization exhausted the request time budget during Stage 2.");
    }

    netlist.trimDeadLogic();  // [REUSED]

    // -----------------------------------------------------------------
    // Stage 3: local cone gate-basis enforcement.
    //
    // KNOWN SIMPLIFICATION (documented, not silently skipped): unlike the
    // depth version, this does NOT run a true cone-isolated minimum-gate
    // resynthesis (DepthOptimizer's extractBestKFeasibleCut / evaluateAreaCut
    // / extractLhsFromCut pipeline is private and was not duplicated here
    // because it is a large, tightly-coupled block). Instead this stage only
    // enforces the requested gate-basis inside the cone via [REUSED]
    // techMapper.convertToBasisOnGateSet(), the same call the depth version
    // uses for its own basis-enforcement step. Stage 2 already ran globally
    // free beforehand, so the cone's gate count still benefits from global
    // size optimization; it just is not re-minimized a second time under the
    // cone-local basis constraint the way depth is. This mirrors the
    // documented limitation already recorded for CriticalPathDepth in
    // OPT_APPLY_API.md section 7.8 ("scoped optimization 目前仍可能先做
    // global AIG/XAG restructuring，再重套局部 basis").
    // -----------------------------------------------------------------
    if (hasLocalConeConstraint) {
        if (requestTimedOut()) {
            return returnTimeout("Area optimization exhausted the request time budget before local cone enforcement.");
        }
        if (verbose) {
            std::cout << "[Area Step 3] Local cone basis enforcement on '"
                      << targetConeReport.sourceName << "'...\n";
        }

        TargetScope rewriteScope = TargetScope::NET_FANIN;
        switch (targetConeReport.type) {
            case ConeQueryType::NetTransitiveFanout: rewriteScope = TargetScope::NET_FANOUT; break;
            case ConeQueryType::GateTransitiveFanin: rewriteScope = TargetScope::GATE_FANIN; break;
            case ConeQueryType::GateTransitiveFanout: rewriteScope = TargetScope::GATE_FANOUT; break;
            case ConeQueryType::NetTransitiveFanin:
            default: rewriteScope = TargetScope::NET_FANIN; break;
        }

        const RewriteScopeResolution resolved =    // [REUSED]
            resolveRewriteScope(netlist, rewriteScope, targetConeReport.sourceName);
        if (!resolved.ok) {
            result.status = OptimizationStatus::ERROR_INVALID_REQUEST;
            result.message = "Cone could not be re-resolved after Stage 2 for local basis enforcement.";
            return result;
        }

        std::unordered_set<int> coneGates;
        for (int g : netlist.getConeGateIds(resolved.cone)) {   // [REUSED]
            if (g < 0 || !netlist.isValidGateId(g)) continue;
            GateType t = netlist.getGate(g).type;
            if (t == GateType::UNKNOWN || t == GateType::DFF) continue;
            coneGates.insert(g);
        }
        if (coneGates.empty()) {
            result.status = OptimizationStatus::ERROR_INVALID_REQUEST;
            result.message = "Cone gate set empty; cannot enforce local basis.";
            return result;
        }

        TechMapReport coneRep = techMapper.convertToBasisOnGateSet(   // [REUSED]
            netlist, coneGates, allowedTypes, bannedTypes, verbose);
        if (coneRep.status != TechMapStatus::SUCCESS) {
            result.status = OptimizationStatus::ERROR_CONSTRAINT_UNSATISFIED;
            result.message = "Local cone basis enforcement failed: " + coneRep.message;
            return result;
        }

        netlist.trimDeadLogic();  // [REUSED]
    }

    // -----------------------------------------------------------------
    // Finalize
    // -----------------------------------------------------------------
    result.newDepth = netlist.findGlobalCriticalPath().depth;      // [REUSED]
    result.newGateCount = totalCombinationalGateCount(netlist);    // [NEW]
    result.areaDelta = result.newGateCount - result.oldGateCount;
    result.changed = !sameNetlistGraphAsOriginal(netlist, originalSnapshot);  // see below
    result.depthImproved = result.newDepth < result.oldDepth;
    result.status = result.newGateCount < result.oldGateCount
        ? OptimizationStatus::SUCCESS
        : OptimizationStatus::NO_IMPROVEMENT;
    result.message = result.status == OptimizationStatus::SUCCESS
        ? ("Gate count reduced from " + std::to_string(result.oldGateCount) +
           " to " + std::to_string(result.newGateCount) + ".")
        : "No gate-count-reducing candidate was found.";

    if (verbose) {
        std::cout << "[Area Flow End] " << result.message << "\n";
    }
    return result;
}

namespace {
// [COPIED] Same structural-identity comparison used internally by
// DepthOptimizer.cpp's anonymous namespace (sameNetlistGraph). Duplicated
// here under a different name to avoid a symbol clash if both .cpp files
// are ever linked into the same translation unit set with internal linkage
// collisions (unlikely, but harmless to rename defensively).
bool samePortsArea(const std::vector<Port>& lhs, const std::vector<Port>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].name != rhs[i].name || lhs[i].msb != rhs[i].msb ||
            lhs[i].lsb != rhs[i].lsb || lhs[i].netIds != rhs[i].netIds) {
            return false;
        }
    }
    return true;
}
} // namespace

bool sameNetlistGraphAsOriginal(const Netlist& lhs, const Netlist& rhs) {
    if (lhs.getGateCount() != rhs.getGateCount() ||
        lhs.getNetCount() != rhs.getNetCount() ||
        !samePortsArea(lhs.getPrimaryInputs(), rhs.getPrimaryInputs()) ||
        !samePortsArea(lhs.getPrimaryOutputs(), rhs.getPrimaryOutputs())) {
        return false;
    }
    for (size_t i = 0; i < lhs.getGateCount(); ++i) {
        const Gate& a = lhs.getGate(static_cast<int>(i));
        const Gate& b = rhs.getGate(static_cast<int>(i));
        if (a.instName != b.instName || a.type != b.type ||
            a.inputNetIds != b.inputNetIds || a.outputNetId != b.outputNetId) {
            return false;
        }
    }
    return true;
}

// =========================================================================
// Part 2: Orchestration layer (mirrors Netlist::runOptApply's
// CriticalPathDepth branch in OptimizationFlow.cpp, lines ~550-905)
// =========================================================================

namespace {

// [COPIED] Same shape as OptimizationFlow.cpp's file-local
// hasValidGateTypeConstraints()/scopeSatisfiesGateConstraints(); re-derived
// here because the originals have internal (anonymous-namespace) linkage
// and are not reachable from this new file.
bool isCombinationalGateTypeArea(GateType type) {
    return type != GateType::DFF && type != GateType::UNKNOWN;
}

bool hasValidAreaGateTypeConstraints(
    const std::vector<GateType>& allowedTypes,
    const std::vector<GateType>& bannedTypes) {
    for (GateType type : allowedTypes) {
        if (!isCombinationalGateTypeArea(type)) return false;
    }
    for (GateType type : bannedTypes) {
        if (!isCombinationalGateTypeArea(type)) return false;
        if (std::find(allowedTypes.begin(), allowedTypes.end(), type) != allowedTypes.end()) {
            return false;
        }
    }
    return true;
}

bool areaScopeSatisfiesGateConstraints(
    const Netlist& netlist,
    TargetScope scope,
    const std::string& scopeName,
    const std::vector<GateType>& allowedTypes,
    const std::vector<GateType>& bannedTypes) {
    if (allowedTypes.empty() && bannedTypes.empty()) return true;

    std::vector<int> gateIds;
    if (scope == TargetScope::WHOLE_NETLIST) {
        gateIds.reserve(netlist.getGateCount());
        for (int gateId = 0; gateId < static_cast<int>(netlist.getGateCount()); ++gateId) {
            gateIds.push_back(gateId);
        }
    } else {
        const RewriteScopeResolution resolved = resolveRewriteScope(netlist, scope, scopeName);  // [REUSED]
        if (!resolved.ok) return false;
        gateIds = netlist.getConeGateIds(resolved.cone);  // [REUSED]
    }

    for (int gateId : gateIds) {
        if (!netlist.isValidGateId(gateId) || netlist.isGateRemoved(gateId)) continue;  // [REUSED]
        const GateType type = netlist.getGate(gateId).type;
        if (!isCombinationalGateTypeArea(type)) continue;
        if (!isGateAllowedArea(type, allowedTypes, bannedTypes)) return false;
    }
    return true;
}

ConeQueryType coneQueryTypeForAreaScope(TargetScope scope) {
    switch (scope) {
        case TargetScope::NET_FANOUT: return ConeQueryType::NetTransitiveFanout;
        case TargetScope::GATE_FANIN: return ConeQueryType::GateTransitiveFanin;
        case TargetScope::GATE_FANOUT: return ConeQueryType::GateTransitiveFanout;
        case TargetScope::NET_FANIN:
        default: return ConeQueryType::NetTransitiveFanin;
    }
}

// [COPIED] Same shape as OptimizationFlow.cpp's buildOptimizationConeReport().
ConeReport buildAreaOptimizationConeReport(
    const Netlist& netlist, TargetScope scope, const std::string& scopeName) {
    ConeReport report;
    if (scope == TargetScope::WHOLE_NETLIST) return report;

    const RewriteScopeResolution resolved = resolveRewriteScope(netlist, scope, scopeName);  // [REUSED]
    if (!resolved.ok) return report;

    report.cone = resolved.cone;
    report.gateIds = netlist.getConeGateIds(report.cone);
    report.rootNetIds = report.cone.rootNetIds;
    report.sourceName = scopeName;
    report.type = coneQueryTypeForAreaScope(scope);
    report.gateCount = report.gateIds.size();
    report.exists = !report.rootNetIds.empty();
    report.ok = report.exists;
    return report;
}

// [NEW] gate-count analog of OptimizationFlow.cpp's measureDepthObjective().
int measureAreaObjective(const Netlist& netlist, const AreaOptApplyRequest& request) {
    if (request.areaObjective == AreaObjective::GlobalGateCount) {
        int total = 0;
        for (const auto& pair : netlist.countGatesByType()) {  // [REUSED]
            if (pair.first == GateType::DFF || pair.first == GateType::UNKNOWN) continue;
            total += pair.second;
        }
        return total;
    }
    const RewriteScopeResolution resolved =
        resolveRewriteScope(netlist, request.scope, request.scopeName);  // [REUSED]
    if (!resolved.ok) return -1;
    return static_cast<int>(netlist.getConeGateCount(resolved.cone));  // [REUSED]
}

std::string targetScopeNameArea(TargetScope scope) {
    switch (scope) {
        case TargetScope::WHOLE_NETLIST: return "WHOLE_NETLIST";
        case TargetScope::NET_FANIN: return "NET_FANIN";
        case TargetScope::NET_FANOUT: return "NET_FANOUT";
        case TargetScope::GATE_FANIN: return "GATE_FANIN";
        case TargetScope::GATE_FANOUT: return "GATE_FANOUT";
    }
    return "UNKNOWN";
}

std::string optimizationStatusNameArea(OptimizationStatus status) {
    switch (status) {
        case OptimizationStatus::SUCCESS: return "SUCCESS";
        case OptimizationStatus::NO_IMPROVEMENT: return "NO_IMPROVEMENT";
        case OptimizationStatus::TIMEOUT: return "TIMEOUT";
        case OptimizationStatus::ERROR_INVALID_REQUEST: return "ERROR_INVALID_REQUEST";
        case OptimizationStatus::ERROR_CONSTRAINT_UNSATISFIED: return "ERROR_CONSTRAINT_UNSATISFIED";
        case OptimizationStatus::ERROR_NOT_EQUIVALENT: return "ERROR_NOT_EQUIVALENT";
        case OptimizationStatus::ERROR_AREA_EXCEEDED: return "ERROR_AREA_EXCEEDED";
    }
    return "UNKNOWN";
}

AreaOptApplyResult makeFailedAreaOptApplyReport(const Netlist& netlist, const std::string& message) {
    AreaOptApplyResult out;
    out.report = Netlist::buildEditReport(   // [REUSED]
        netlist, netlist, "opt_apply:area_gate_count", NetlistEditOperationKind::CustomRewrite);
    out.report.success = false;
    out.report.changed = false;
    out.report.rolledBack = false;
    out.report.message = message;
    return out;
}

} // namespace

AreaOptQueryReport runAreaOptQuery(const Netlist& netlist, const AreaOptApplyRequest& request) {
    AreaOptQueryReport report;
    report.totalGateCount = 0;
    for (const auto& pair : netlist.countGatesByType()) {  // [REUSED]
        if (pair.first == GateType::DFF || pair.first == GateType::UNKNOWN) continue;
        report.totalGateCount += pair.second;
    }
    if (request.scope != TargetScope::WHOLE_NETLIST) {
        const RewriteScopeResolution resolved =
            resolveRewriteScope(netlist, request.scope, request.scopeName);  // [REUSED]
        if (!resolved.ok) {
            report.ok = false;
            report.message = "Failed to resolve scope: " + resolved.message;
            return report;
        }
        report.scopedGateCount = static_cast<int>(netlist.getConeGateCount(resolved.cone));  // [REUSED]
    }
    report.ok = true;
    report.message = "Gate count computed.";
    return report;
}

// [NEW] Transaction flow. Structurally identical to
// Netlist::runOptApply()'s CriticalPathDepth branch (OptimizationFlow.cpp
// ~550-905), with depth swapped for gate count throughout.
AreaOptApplyResult runAreaOptApply(Netlist& netlist, const AreaOptApplyRequest& request) {
    if (!std::isfinite(request.timeLimitSeconds) || request.timeLimitSeconds <= 0.0) {
        return makeFailedAreaOptApplyReport(netlist, "timeLimitSeconds must be finite and positive.");
    }
    if (request.targetGateCount < -1) {
        return makeFailedAreaOptApplyReport(netlist, "targetGateCount must be -1 or a non-negative count.");
    }
    if (!hasValidAreaGateTypeConstraints(request.allowedTypes, request.bannedTypes)) {
        return makeFailedAreaOptApplyReport(
            netlist, "allowedTypes/bannedTypes must contain valid, non-overlapping combinational gate types.");
    }
    if (request.areaObjective == AreaObjective::ScopedFaninConeGateCount &&
        request.scope != TargetScope::NET_FANIN && request.scope != TargetScope::GATE_FANIN) {
        return makeFailedAreaOptApplyReport(
            netlist, "ScopedFaninConeGateCount requires NET_FANIN or GATE_FANIN scope.");
    }

    const request_time_budget::RequestDeadline deadline(request.timeLimitSeconds);  // [REUSED]
    auto elapsedSeconds = [&]() { return deadline.elapsedSeconds(); };

    const RewriteScopeResolution originalScope =
        resolveRewriteScope(netlist, request.scope, request.scopeName);  // [REUSED]
    if (!originalScope.ok) {
        return makeFailedAreaOptApplyReport(netlist, "Failed to resolve optimization scope: " + originalScope.message);
    }

    const Netlist original = netlist.cloneForRollback();  // [REUSED]
    Netlist working = original.cloneForRollback();         // [REUSED]
    const bool baselineConstraintsSatisfied = areaScopeSatisfiesGateConstraints(
        original, request.scope, request.scopeName, request.allowedTypes, request.bannedTypes);

    GateCountOptimizationSummary summary;
    summary.scope = targetScopeNameArea(request.scope);
    summary.requestedScopeName = request.scopeName;
    summary.resolvedRootNetName = originalScope.resolvedRootNetName;
    summary.resolvedThroughDffDataPin = originalScope.resolvedThroughDffDataPin;
    summary.allowedTypes = request.allowedTypes;
    summary.bannedTypes = request.bannedTypes;
    summary.baselineConstraintsSatisfied = baselineConstraintsSatisfied;
    summary.timeBudgetSeconds = request.timeLimitSeconds;
    summary.gateCountBefore = measureAreaObjective(original, request);
    summary.targetGateCount = request.targetGateCount;

    if (elapsedSeconds() >= request.timeLimitSeconds) {
        AreaOptApplyResult timeoutResult;
        timeoutResult.report = Netlist::buildEditReport(  // [REUSED]
            original, original, "opt_apply:area_gate_count", NetlistEditOperationKind::CustomRewrite);
        timeoutResult.report.success = false;
        timeoutResult.report.changed = false;
        timeoutResult.report.rolledBack = false;
        summary.coreStatus = "TIMEOUT";
        summary.coreMessage = "The transaction time budget was exhausted before optimizer core execution.";
        summary.gateCountAfter = summary.gateCountBefore;
        summary.elapsedSeconds = elapsedSeconds();
        timeoutResult.areaOptimization = summary;
        Netlist::certifyEquivalence(  // [REUSED]
            timeoutResult.report, EquivalenceCheckMethod::StructuralIdentity,
            "The original design was retained because no optimizer candidate was started.");
        timeoutResult.report.message = summary.coreMessage;
        return timeoutResult;
    }

    if (request.scope != TargetScope::WHOLE_NETLIST &&
        original.getConeGateCount(originalScope.cone) == 0) {  // [REUSED]
        AreaOptApplyResult emptyResult;
        emptyResult.report = Netlist::buildEditReport(
            original, original, "opt_apply:area_gate_count", NetlistEditOperationKind::CustomRewrite);
        summary.coreStatus = "NO_IMPROVEMENT";
        summary.coreMessage =
            "The resolved optimization scope contains no combinational gates; the original design was retained.";
        summary.finalConstraintsSatisfied = true;
        summary.gateCountAfter = summary.gateCountBefore;
        summary.elapsedSeconds = elapsedSeconds();
        emptyResult.areaOptimization = summary;
        Netlist::certifyEquivalence(
            emptyResult.report, EquivalenceCheckMethod::StructuralIdentity,
            "The original design was retained because the resolved optimization scope is empty.");
        const bool targetMet = request.targetGateCount < 0 || summary.gateCountBefore <= request.targetGateCount;
        emptyResult.report.success = targetMet;
        emptyResult.report.changed = false;
        emptyResult.report.rolledBack = false;
        emptyResult.report.message = targetMet
            ? "The resolved optimization scope is empty; the original design was retained."
            : "The resolved optimization scope is empty and the original design does not meet targetGateCount.";
        return emptyResult;
    }

    const bool hasGateConstraints = !request.allowedTypes.empty() || !request.bannedTypes.empty();
    const ConeReport coneReport =
        request.scope != TargetScope::WHOLE_NETLIST && hasGateConstraints
            ? buildAreaOptimizationConeReport(working, request.scope, request.scopeName)
            : ConeReport{};

    if (elapsedSeconds() >= request.timeLimitSeconds) {
        AreaOptApplyResult timeoutResult;
        timeoutResult.report = Netlist::buildEditReport(
            original, original, "opt_apply:area_gate_count", NetlistEditOperationKind::CustomRewrite);
        timeoutResult.report.success = false;
        summary.coreStatus = "TIMEOUT";
        summary.coreMessage = "The transaction time budget was exhausted before optimizer core execution.";
        summary.gateCountAfter = summary.gateCountBefore;
        summary.elapsedSeconds = elapsedSeconds();
        timeoutResult.areaOptimization = summary;
        Netlist::certifyEquivalence(
            timeoutResult.report, EquivalenceCheckMethod::StructuralIdentity,
            "The original design was retained because no optimizer candidate was started.");
        return timeoutResult;
    }

    TechMapper techMapper(&deadline);   // [REUSED]
    AreaOptimizer optimizer;            // [NEW]
    const OptimizationResult core = optimizer.executeAreaOptimization(
        working, techMapper, coneReport, request.allowedTypes, request.bannedTypes,
        request.verbose, &deadline);

    summary.coreStatus = optimizationStatusNameArea(core.status);
    summary.coreMessage = core.message;
    summary.candidateGenerated = core.changed;
    summary.gateCountAfter = measureAreaObjective(working, request);
    summary.finalConstraintsSatisfied = areaScopeSatisfiesGateConstraints(
        working, request.scope, request.scopeName, request.allowedTypes, request.bannedTypes);

    auto buildCandidateResult = [&]() {
        AreaOptApplyResult candidate;
        candidate.report = Netlist::buildEditReport(   // [REUSED]
            original, working, "opt_apply:area_gate_count", NetlistEditOperationKind::CustomRewrite);
        candidate.report.changed = candidate.report.changed || core.changed;
        return candidate;
    };

    auto finishResult = [&](AreaOptApplyResult candidate) {
        summary.elapsedSeconds = elapsedSeconds();
        summary.improved = summary.gateCountAfter >= 0 && summary.gateCountBefore >= 0 &&
                            summary.gateCountAfter < summary.gateCountBefore;
        summary.meetsTarget = request.targetGateCount < 0 ||
                               (summary.gateCountAfter >= 0 && summary.gateCountAfter <= request.targetGateCount);
        candidate.areaOptimization = summary;
        return candidate;
    };

    if (core.status != OptimizationStatus::SUCCESS && core.status != OptimizationStatus::NO_IMPROVEMENT) {
        AreaOptApplyResult candidate = buildCandidateResult();
        candidate.report.success = false;
        candidate.report.rolledBack = core.changed;
        candidate.report.message = "Area optimization candidate generation failed: " + core.message;
        summary.candidateAccepted = false;
        return finishResult(std::move(candidate));
    }

    AreaOptApplyResult candidate = buildCandidateResult();
    if (!candidate.report.success) {
        candidate.report.rolledBack = core.changed;
        candidate.report.message = "Area optimization candidate failed structural validation and was discarded.";
        summary.candidateAccepted = false;
        return finishResult(std::move(candidate));
    }
    if (!summary.finalConstraintsSatisfied) {
        candidate.report.success = false;
        candidate.report.rolledBack = core.changed;
        candidate.report.message = "Area optimization candidate violated the requested gate constraints and was discarded.";
        summary.candidateAccepted = false;
        return finishResult(std::move(candidate));
    }

    const bool gateCountImproved =
        summary.gateCountAfter >= 0 && summary.gateCountBefore >= 0 &&
        summary.gateCountAfter < summary.gateCountBefore;
    const bool targetMet = request.targetGateCount < 0 ||
                            (summary.gateCountAfter >= 0 && summary.gateCountAfter <= request.targetGateCount);
    if (!targetMet) {
        candidate.report.success = false;
        candidate.report.rolledBack = core.changed;
        candidate.report.message = "Area optimization candidate did not meet targetGateCount and was discarded.";
        summary.candidateAccepted = false;
        return finishResult(std::move(candidate));
    }

    if (!candidate.report.changed) {
        candidate.report.success = true;
        candidate.report.changed = false;
        candidate.report.rolledBack = false;
        summary.candidateGenerated = false;
        summary.candidateAccepted = false;
        summary.wholeDesignEquivalenceChecked = false;
        summary.wholeDesignEquivalent = true;
        Netlist::certifyEquivalence(
            candidate.report, EquivalenceCheckMethod::StructuralIdentity,
            "The optimizer produced no graph change; the original design is structurally identical.");
        candidate.report.message = "The optimizer produced no graph change; the original design was retained.";
        return finishResult(std::move(candidate));
    }

    if (request.requireGateCountImprovement && baselineConstraintsSatisfied && !gateCountImproved) {
        AreaOptApplyResult originalResult;
        originalResult.report = Netlist::buildEditReport(
            original, original, "opt_apply:area_gate_count", NetlistEditOperationKind::CustomRewrite);
        originalResult.report.success = true;
        originalResult.report.changed = false;
        originalResult.report.rolledBack = core.changed;
        Netlist::certifyEquivalence(
            originalResult.report, EquivalenceCheckMethod::StructuralIdentity,
            "The original design was retained because no accepted gate-count improvement was found.");
        originalResult.report.message = "No improving candidate was accepted; the original design was retained.";
        summary.candidateAccepted = false;
        summary.gateCountAfter = summary.gateCountBefore;
        return finishResult(std::move(originalResult));
    }

    if (request.requireGateCountImprovement && !baselineConstraintsSatisfied && !gateCountImproved) {
        candidate.report.addWarning(  // [REUSED] NetlistEditReport::addWarning()
            "The candidate did not improve gate count, but it is eligible because the original design violated a hard gate constraint.");
    }

    const double remainingTime = deadline.remainingSeconds();  // [REUSED]
    if (remainingTime <= 0.0) {
        summary.wholeDesignTimedOut = true;
        candidate.report.success = false;
        candidate.report.rolledBack = core.changed;
        candidate.report.message = "No time remained for mandatory whole-design equivalence; candidate was discarded.";
        summary.candidateAccepted = false;
        return finishResult(std::move(candidate));
    }

    // [REUSED] Same mandatory commit gate as CriticalPathDepth: whole-design
    // SAT before accepting the candidate.
    const WholeDesignEquivalenceReport equivalence =
        working.checkWholeDesignEquivalence(original, remainingTime);

    summary.wholeDesignEquivalenceChecked = true;
    summary.wholeDesignEquivalent = equivalence.equivalent;
    summary.comparedOutputCount = equivalence.comparedOutputCount;
    summary.comparedDffDCount = equivalence.comparedDffDCount;

    if (equivalence.ok && equivalence.equivalent) {
        candidate.report.success = true;
        candidate.report.changed = true;
        candidate.report.rolledBack = false;
        Netlist::certifyEquivalence(
            candidate.report, EquivalenceCheckMethod::WholeDesignSat,
            "Whole-design equivalence proved for all primary outputs and DFF.D endpoints.");
        summary.candidateAccepted = true;
        netlist.restoreFrom(working);  // [REUSED] commit
        candidate.report.message = "Area optimization candidate accepted and committed.";
        return finishResult(std::move(candidate));
    }

    if (equivalence.ok && !equivalence.equivalent) {
        candidate.report.success = false;
        candidate.report.rolledBack = core.changed;
        candidate.report.message =
            "Area optimization candidate is not equivalent to the original design; it was discarded.";
        summary.candidateAccepted = false;
        return finishResult(std::move(candidate));
    }

    // equivalence.ok == false: SAT could not reach a conclusion (timeout or
    // unsupported logic). Not a proven mismatch, but not safe to commit.
    summary.wholeDesignTimedOut = true;
    candidate.report.success = false;
    candidate.report.rolledBack = core.changed;
    candidate.report.message =
        "Whole-design equivalence check could not be completed for the area optimization candidate; it was discarded.";
    summary.candidateAccepted = false;
    return finishResult(std::move(candidate));
}