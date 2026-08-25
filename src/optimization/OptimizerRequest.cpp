#include "include/core/OptimizerRequest.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace opt {
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

// 在 gate 子集合上算組合閘數量。longestPathInGateSet 已經有了，
// 這個更簡單——集合本身就只含組合閘（resolveConeGates 已濾掉 DFF/UNKNOWN）。
int gateCountInSet(const std::unordered_set<int>& gateSet) {
    return static_cast<int>(gateSet.size());
}

} // namespace

// -------------------------------------------------------------------------

RequestValidation validateRequest(const OptimizationRequest& request) {
    RequestValidation v;

    // basisConstraints 的形狀約束：
    //   最多一組沒有 scope（= cone 外，無 cone 時就是整張 netlist）
    //   最多一組有 scope（= 該 cone 內）
    //
    // 兩組全域無法決定誰蓋誰；兩個 cone 各自不同 basis 也不支援 ——
    // GenericLowering 的 per-node basisId 只有 default / cone 兩種值。
    {
        int unscopedCount = 0;
        int scopedCount   = 0;
        for (const auto& basis : request.basisConstraints) {
            if (basis.isWholeNetlist()) ++unscopedCount;
            else                        ++scopedCount;
        }
        if (unscopedCount > 1) {
            v.message = "at most one whole-netlist basis constraint is supported (got "
                      + std::to_string(unscopedCount) + ").";
            return v;
        }
        if (scopedCount > 1) {
            v.message = "at most one cone-scoped basis constraint is supported (got "
                      + std::to_string(scopedCount)
                      + "); heterogeneous constraints across multiple cones are not "
                        "implemented.";
            return v;
        }
    }

    if (isConeMetric(request.cost.metric)) {
        if (!request.cost.cone.has_value() || !request.cost.cone->valid()) {
            v.message = "a cone-scoped cost metric requires a cone reference.";
            return v;
        }
        if (!toTargetScope(request.cost.cone->type).has_value()) {
            v.message = "cost cone uses a query type that cannot be resolved to a rewrite scope.";
            return v;
        }
    } else if (request.cost.cone.has_value()) {
        v.message = "a global cost metric was requested but a cost cone was specified; "
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
            // 指定了 cone 卻沒給任何約束 = 這組什麼都不做，而且會讓
            // hasLocalBasisScope 誤判為真、走進 lowering 的 cone 分支。
            if (!basis.constrains()) {
                v.message = "a cone-scoped basis constraint must list at least one "
                            "allowed or banned gate type.";
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
    const bool depth = isDepthMetric(cost.metric);
    if (isConeMetric(cost.metric) && cost.cone.has_value()) {
        return std::string(depth ? "depth" : "gate count")
             + " of cone '" + cost.cone->sourceName + "'";
    }
    return depth ? "global maximum combinational depth"
                 : "total gate count including DFFs";
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

    const int a = primary(metric);
    const int b = other.primary(metric);
    // 主要指標量不出來（-1）時不可比，退回全域深度避免亂挑。
    if (a >= 0 && b >= 0 && a != b) return a < b;

    if (isDepthMetric(metric)) {
        if (gateCount >= 0 && other.gateCount >= 0 && gateCount != other.gateCount)
            return gateCount < other.gateCount;
        if (globalDepth != other.globalDepth) return globalDepth < other.globalDepth;
    } else {
        if (globalDepth != other.globalDepth) return globalDepth < other.globalDepth;
        if (gateCount >= 0 && other.gateCount >= 0 && gateCount != other.gateCount)
            return gateCount < other.gateCount;
    }
    return false;
}

CostMeasurement measureCost(Netlist& netlist, const CostTarget& cost) {
    CostMeasurement m;
    m.globalDepth = netlist.findGlobalCriticalPath().depth;

    // 面積一律只算組合閘：DFF 數量在最佳化前後不變，計進去只會稀釋差異。
    int total = 0;
    for (const auto& p : netlist.countGatesByType()) {
        if (p.first == GateType::UNKNOWN) continue;
        total += p.second;
    }
    m.gateCount = total;

    if (cost.cone.has_value()) {
        const ConeResolution cone = resolveConeGates(netlist, *cost.cone);
        if (!cone.ok) {
            if (isConeMetric(cost.metric)) {
                m.message = "cannot measure the cost cone: " + cone.message;
                return m;   // ok = false：主要指標算不出來
            }
            m.coneDepth = -1;
            m.coneGateCount = -1;
        } else {
            m.coneDepth     = cone.gateIds.empty()
                            ? 0 : longestPathInGateSet(netlist, cone.gateIds);
            m.coneGateCount = gateCountInSet(cone.gateIds);
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

} // namespace opt
