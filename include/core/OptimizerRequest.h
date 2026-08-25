#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include "include/core/Netlist.h"
#include "include/core/NetlistQueries.h"
#include "include/core/TechMapper.h"

namespace opt {

// =========================================================================
// 「哪一個 cone」的可重新解析參照。
// 刻意不存 gate id：每動一次結構 id 就失效，必須能重查。
// =========================================================================
struct ConeRef {
    ConeQueryType type = ConeQueryType::NetTransitiveFanin;
    std::string   sourceName;

    bool valid() const { return !sourceName.empty(); }
};

// =========================================================================
// 成本函數（= prompt 裡明寫的 "The cost function is ..."）
//
// 重要：這個欄位「不」改變最佳化的動作路徑。不論 metric 是什麼，
// 引擎一律做全域最佳化——全域 balancing/rewriting 本來就會順帶壓到
// 任何 cone。metric 只決定三件事：
//   (1) 多個候選解之間挑哪一個
//   (2) 迭代什麼時候該停（"Report original if already optimal" 的判準）
//   (3) OptimizationResult 主要回報哪個數字
// =========================================================================
enum class CostMetric {
    GlobalMaxDepth,
    ConeDepth,
    GlobalGateCount,
    ConeGateCount
};

struct CostTarget {
    CostMetric metric = CostMetric::GlobalMaxDepth;
    std::optional<ConeRef> cone;   // 只有 metric == ConeDepth 時有值
};

// =========================================================================
// 基底約束（= prompt 裡的 "ensuring ... only ... gates"）
//   scope == nullopt : 約束整張 netlist
//   scope 有值       : 只約束該 cone，cone 外不受限
//
// allowed/banned 搬進來，語意才不會隨外部旗標飄移。
// 注意：這裡的 scope 與 CostTarget::cone 是完全獨立的兩件事。
// =========================================================================
struct BasisConstraint {
    std::optional<ConeRef> scope;
    std::vector<GateType>  allowed;
    std::vector<GateType>  banned;

    bool isWholeNetlist() const { return !scope.has_value(); }
    bool constrains()     const { return !allowed.empty() || !banned.empty(); }
};

// =========================================================================
// 一次深度最佳化請求。
// basisConstraints 用 vector 是為了將來的多 cone 異質約束；
// 只接受 size() <= 1，超過直接拒絕，不走沒測過的路徑。
// =========================================================================
struct OptimizationRequest {
    CostTarget                   cost;
    std::vector<BasisConstraint> basisConstraints;
};

struct RequestValidation {
    bool ok = false;
    std::string message;
};

RequestValidation validateRequest(const OptimizationRequest& request);
std::string describeCost(const CostTarget& cost);
std::string describeBasis(const BasisConstraint& basis);

// -------------------------------------------------------------------------
// Cone 解析：ConeRef -> 當下最新的 gate id 集合
// -------------------------------------------------------------------------
struct ConeResolution {
    bool ok = false;
    std::string message;
    std::string rootNetName;
    bool resolvedThroughDffDataPin = false;
    std::unordered_set<int> gateIds;   // 只含 combinational gate（排除 DFF/UNKNOWN）
};

ConeResolution resolveConeGates(Netlist& netlist, const ConeRef& ref);

// -------------------------------------------------------------------------
// 成本量測。
// 四個數字全部量出來（cone 的兩個只是在 cone 上跑一次 DFS 與一次計數，
// 很便宜），由 betterThan() 依 metric 決定字典序。
// -------------------------------------------------------------------------
struct CostMeasurement {
    bool ok = false;
    int  globalDepth   = -1;
    int  coneDepth     = -1;   // request 沒指定 cost cone 時為 -1
    int  gateCount     = -1;   // 全域組合閘數（不含 DFF）
    int  coneGateCount = -1;   // request 沒指定 cost cone 時為 -1
    std::string message;

    int primary(CostMetric m) const {
        switch (m) {
            case CostMetric::GlobalMaxDepth:  return globalDepth;
            case CostMetric::ConeDepth:       return coneDepth;
            case CostMetric::GlobalGateCount: return gateCount;
            case CostMetric::ConeGateCount:   return coneGateCount;
        }
        return globalDepth;
    }

    // 主要指標優先；打平時用「另一個維度」當 tie-break。
    //   深度目標：(primary, gates, globalDepth)
    //   面積目標：(primary, globalDepth, gates)
    // 兩者對稱——都不會為了 tie-break 而犧牲主要指標。
    bool betterThan(const CostMeasurement& other, CostMetric metric) const;
};

CostMeasurement measureCost(Netlist& netlist, const CostTarget& cost);

// -------------------------------------------------------------------------
// 舊介面轉接：維持既有呼叫端語意不變
//   coneReport 有效 -> basis scope = 該 cone
//   coneReport 無效 -> basis scope = 全域
//   cost 一律 GlobalMaxDepth（舊版本來就只量 findGlobalCriticalPath）
// -------------------------------------------------------------------------
OptimizationRequest makeLegacyRequest(const ConeReport& coneReport,
                                      const std::vector<GateType>& allowedTypes,
                                      const std::vector<GateType>& bannedTypes);

// -------------------------------------------------------------------------
// 迭代最佳化的候選挑選策略。
// patience：連續幾輪沒有改善才停止。設 1 等同舊行為（第一次沒進步就停）。
// mockturtle 的 balancing/rewriting/resub 收益是非單調的——常見模式是
// 中間一兩輪打平、之後才掉深度——所以預設給 3。
// -------------------------------------------------------------------------
struct IterationPolicy {
    int maxIterations = 15;

    // 軟停止：連續幾輪沒改善後，開始把「剩餘時間」納入考量。
    int patience = 2;

    // 硬停止：不管剩多少時間都停。防止在不動點上空轉。
    int hardPatience = 6;

    // 軟停止後還要繼續探索，至少需要幾輪的時間餘裕。
    double explorationSlack = 2.0;
};

// 候選追蹤器：記住迭代過程中「依 cost metric 最好的那一個」。
// mockturtle 的網路型別是 template 參數，所以放在 header 裡。
template <typename NtkT>
class BestCandidateTracker {
public:
    BestCandidateTracker(CostMetric metric, const IterationPolicy& policy)
        : metric_(metric), policy_(policy) {}

    // 回傳 true 表示這個候選是目前最好的
    bool consider(const NtkT& ntk, const CostMeasurement& cost) {
        if (!cost.ok) { ++stagnant_; return false; }
        if (!hasBest_ || cost.betterThan(best_, metric_)) {
            best_     = cost;
            bestNtk_  = ntk;
            hasBest_  = true;
            stagnant_ = 0;
            return true;
        }
        ++stagnant_;
        return false;
    }

    bool shouldStop()  const { return stagnant_ >= policy_.patience; }
    bool hasBest()     const { return hasBest_; }
    const NtkT& best() const { return bestNtk_; }
    const CostMeasurement& bestCost() const { return best_; }
    int  stagnantRounds() const { return stagnant_; }

private:
    CostMetric      metric_;
    IterationPolicy policy_;
    NtkT            bestNtk_{};
    CostMeasurement best_{};
    bool            hasBest_  = false;
    int             stagnant_ = 0;
};

inline bool isDepthMetric(CostMetric m) {
    return m == CostMetric::GlobalMaxDepth || m == CostMetric::ConeDepth;
}
inline bool isConeMetric(CostMetric m) {
    return m == CostMetric::ConeDepth || m == CostMetric::ConeGateCount;
}
inline const char* toString(CostMetric m) {
    switch (m) {
        case CostMetric::GlobalMaxDepth:  return "global_maximum_depth";
        case CostMetric::ConeDepth:       return "scoped_fanin_cone_depth";
        case CostMetric::GlobalGateCount: return "global_gate_count";
        case CostMetric::ConeGateCount:   return "scoped_fanin_cone_gate_count";
    }
    return "unknown";
}

inline OptimizationGoal goalOf(CostMetric m) {
    return isDepthMetric(m) ? OptimizationGoal::DEPTH : OptimizationGoal::AREA;
}

} // namespace opt