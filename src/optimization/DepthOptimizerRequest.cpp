#include "include/core/DepthOptimizerRequest.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace depth_opt {
namespace {

// ConeQueryType -> TargetScope。
// 原本的實作用 default 把所有未列舉的 type 都當 NET_FANIN，
// 導致 SharedFaninGates 會拿不存在的 net name 去查而靜默失敗。改成明確拒絕。
std::optional<TargetScope> toTargetScope(ConeQueryType type) {
    switch (type) {
        case ConeQueryType::NetTransitiveFanin:   return TargetScope::NET_FANIN;
        case ConeQueryType::NetTransitiveFanout:  return TargetScope::NET_FANOUT;
        case ConeQueryType::GateTransitiveFanin:  return TargetScope::GATE_FANIN;
        case ConeQueryType::GateTransitiveFanout: return TargetScope::GATE_FANOUT;
        // 查詢階段已把「最大的那個 PO」解析成具體 net name，之後就是一般 fanin cone
        case ConeQueryType::LargestOutputCone:    return TargetScope::NET_FANIN;
        // 兩個 cone 的交集不是可重寫的範圍，沒有單一 root 可重查
        case ConeQueryType::SharedFaninGates:     return std::nullopt;
        // ranking 可能同時選出多個 outputs，不是單一可重寫 scope
        case ConeQueryType::OutputConeRanking:    return std::nullopt;
        // predicate filter 也是 batch query，不是單一可重寫 scope
        case ConeQueryType::OutputConeFilter:     return std::nullopt;
    }
    return std::nullopt;
}

bool isCombinational(GateType t) {
    return t != GateType::UNKNOWN && t != GateType::DFF;
}

// 在 gate 子集合上算最長路徑（以 gate 層數計）。
// 邊界：driver 不在集合內、或是 PI/DFF.Q -> 視為 level 0。
// 用顯式 stack 的 DFS，避免 100 萬閘遞迴爆 stack。
int longestPathInGateSet(Netlist& netlist, const std::unordered_set<int>& gateSet) {
    if (gateSet.empty()) return 0;

    std::unordered_map<int, int> level;
    std::unordered_set<int> inProgress;   // 防禦性：理論上組合電路無環
    level.reserve(gateSet.size() * 2);

    int best = 0;
    std::vector<std::pair<int, bool>> stack;   // (gateId, 子節點是否已展開)

    auto driverInSet = [&](int netId) -> int {
        if (!netlist.isValidNetId(netId)) return -1;
        const int drv = netlist.getNet(netId).driverGateId;
        if (drv < 0 || !gateSet.count(drv)) return -1;
        return drv;
    };

    for (int root : gateSet) {
        if (level.count(root)) continue;
        stack.clear();
        stack.emplace_back(root, false);

        while (!stack.empty()) {
            const int  gid      = stack.back().first;
            const bool expanded = stack.back().second;
            stack.pop_back();

            if (expanded) {
                inProgress.erase(gid);
                int lv = 1;
                for (int netId : netlist.getGate(gid).inputNetIds) {
                    const int drv = driverInSet(netId);
                    if (drv < 0) continue;
                    auto it = level.find(drv);
                    if (it != level.end()) lv = std::max(lv, it->second + 1);
                }
                level[gid] = lv;
                best = std::max(best, lv);
                continue;
            }

            if (level.count(gid) || inProgress.count(gid)) continue;
            inProgress.insert(gid);
            stack.emplace_back(gid, true);
            for (int netId : netlist.getGate(gid).inputNetIds) {
                const int drv = driverInSet(netId);
                if (drv >= 0 && !level.count(drv) && !inProgress.count(drv)) {
                    stack.emplace_back(drv, false);
                }
            }
        }
    }
    return best;
}

} // namespace

// -------------------------------------------------------------------------

RequestValidation validateRequest(const OptimizationRequest& request) {
    RequestValidation v;

    if (request.basisConstraints.size() > 1) {
        v.message = "multiple basis scopes are not supported yet (got "
                  + std::to_string(request.basisConstraints.size()) + ").";
        return v;
    }

    if (request.cost.metric == CostMetric::ConeDepth) {
        if (!request.cost.cone.has_value() || !request.cost.cone->valid()) {
            v.message = "cost metric is ConeDepth but no cone was specified.";
            return v;
        }
        if (!toTargetScope(request.cost.cone->type).has_value()) {
            v.message = "cost cone uses a query type that cannot be resolved to a rewrite scope.";
            return v;
        }
    } else if (request.cost.cone.has_value()) {
        v.message = "cost metric is GlobalMaxDepth but a cost cone was specified; "
                    "did you mean to set the basis scope instead?";
        return v;
    }

    for (const auto& basis : request.basisConstraints) {
        if (basis.scope.has_value()) {
            if (!basis.scope->valid()) {
                v.message = "basis scope cone has an empty source name.";
                return v;
            }
            if (!toTargetScope(basis.scope->type).has_value()) {
                v.message = "basis scope uses a query type that cannot be resolved "
                            "to a rewrite scope.";
                return v;
            }
        }
        for (GateType b : basis.banned) {
            if (std::find(basis.allowed.begin(), basis.allowed.end(), b) != basis.allowed.end()) {
                v.message = "gate type appears in both the allowed and banned list.";
                return v;
            }
        }
    }

    v.ok = true;
    return v;
}

std::string describeCost(const CostTarget& cost) {
    if (cost.metric == CostMetric::ConeDepth && cost.cone.has_value()) {
        return "depth of cone '" + cost.cone->sourceName + "'";
    }
    return "global maximum combinational depth";
}

std::string describeBasis(const BasisConstraint& basis) {
    if (!basis.constrains()) return "unconstrained";
    return basis.isWholeNetlist() ? "whole netlist"
                                  : ("cone '" + basis.scope->sourceName + "'");
}

// -------------------------------------------------------------------------

ConeResolution resolveConeGates(Netlist& netlist, const ConeRef& ref) {
    ConeResolution out;

    if (!ref.valid()) {
        out.message = "cone reference has an empty source name.";
        return out;
    }
    const auto scope = toTargetScope(ref.type);
    if (!scope.has_value()) {
        out.message = "cone query type cannot be resolved to a rewrite scope.";
        return out;
    }

    // NET_FANIN 把 DFF.Q 當 sequential boundary；若呼叫端明確指向一顆 DFF，
    // resolveRewriteScope 會改解析 D pin 的 data cone。
    const RewriteScopeResolution resolved =
        resolveRewriteScope(netlist, *scope, ref.sourceName);
    if (!resolved.ok) {
        out.message = "failed to resolve cone '" + ref.sourceName + "': " + resolved.message;
        return out;
    }

    out.rootNetName = resolved.resolvedRootNetName;
    out.resolvedThroughDffDataPin = resolved.resolvedThroughDffDataPin;

    for (int g : netlist.getConeGateIds(resolved.cone)) {
        if (g < 0 || !netlist.isValidGateId(g)) continue;
        if (netlist.isGateRemoved(g)) continue;
        if (!isCombinational(netlist.getGate(g).type)) continue;
        out.gateIds.insert(g);   // root 閘已由 cone 函式本身納入
    }

    out.ok = true;
    out.message = resolved.message;
    return out;
}

// -------------------------------------------------------------------------

bool CostMeasurement::betterThan(const CostMeasurement& other, CostMetric metric) const {
    if (!ok) return false;
    if (!other.ok) return true;

    const bool coneComparable = (coneDepth >= 0 && other.coneDepth >= 0);

    if (metric == CostMetric::ConeDepth && coneComparable) {
        if (coneDepth   != other.coneDepth)   return coneDepth   < other.coneDepth;
        if (globalDepth != other.globalDepth) return globalDepth < other.globalDepth;
        return gateCount < other.gateCount;
    }

    if (globalDepth != other.globalDepth) return globalDepth < other.globalDepth;
    if (coneComparable && coneDepth != other.coneDepth) return coneDepth < other.coneDepth;
    return gateCount < other.gateCount;
}

CostMeasurement measureCost(Netlist& netlist, const CostTarget& cost) {
    CostMeasurement m;
    m.globalDepth = netlist.findGlobalCriticalPath().depth;

    int total = 0;
    for (const auto& p : netlist.countGatesByType()) total += p.second;
    m.gateCount = total;

    if (cost.cone.has_value()) {
        const ConeResolution cone = resolveConeGates(netlist, *cost.cone);
        if (!cone.ok) {
            if (cost.metric == CostMetric::ConeDepth) {
                m.message = "cannot measure cone depth: " + cone.message;
                return m;   // ok = false：主要指標算不出來，視為失敗
            }
            // 只是 tie-break 用的話，量不到就算了，不影響主要指標
            m.coneDepth = -1;
        } else {
            // cone 被完全化簡掉（塌成常數或直接接線）—— 深度 0 是合法答案
            m.coneDepth = cone.gateIds.empty() ? 0 : longestPathInGateSet(netlist, cone.gateIds);
        }
    }

    m.ok = true;
    return m;
}

// -------------------------------------------------------------------------

OptimizationRequest makeLegacyRequest(const ConeReport& coneReport,
                                      const std::vector<GateType>& allowedTypes,
                                      const std::vector<GateType>& bannedTypes) {
    OptimizationRequest req;
    req.cost.metric = CostMetric::GlobalMaxDepth;   // 舊版一律量全域

    BasisConstraint basis;
    basis.allowed = allowedTypes;
    basis.banned  = bannedTypes;

    if (coneReport.ok && coneReport.exists) {
        ConeRef ref;
        ref.type       = coneReport.type;
        ref.sourceName = coneReport.sourceName;
        basis.scope    = ref;
    }

    req.basisConstraints.push_back(std::move(basis));
    return req;
}

} // namespace depth_opt
