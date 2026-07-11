#pragma once

#include "include/core/Netlist.h"
#include "include/core/MockturtleConverter.h"
#include "include/core/TechMapper.h" 
#include <vector>
#include <algorithm>
#include <mockturtle/networks/aig.hpp>
#include <mockturtle/networks/xag.hpp>
#include <mockturtle/views/depth_view.hpp>
#include <mockturtle/views/fanout_view.hpp>
#include <mockturtle/views/mapping_view.hpp>  
#include <mockturtle/networks/klut.hpp>              
#include <mockturtle/algorithms/node_resynthesis.hpp> 
#include <mockturtle/networks/mig.hpp>
#include <mockturtle/algorithms/node_resynthesis/shannon.hpp> 
#include <mockturtle/algorithms/mig_algebraic_rewriting.hpp>
#include <mockturtle/algorithms/cleanup.hpp>
#include <mockturtle/algorithms/balancing.hpp>
#include <mockturtle/algorithms/balancing/sop_balancing.hpp>
#include <mockturtle/algorithms/balancing/esop_balancing.hpp>
#include <mockturtle/algorithms/cut_rewriting.hpp>
#include <mockturtle/algorithms/resubstitution.hpp>
#include <mockturtle/algorithms/aig_resub.hpp>
#include <mockturtle/algorithms/xag_resub.hpp>
#include <mockturtle/algorithms/lut_mapping.hpp>       
#include <mockturtle/algorithms/collapse_mapped.hpp> 
#include <mockturtle/algorithms/node_resynthesis/xag_npn.hpp>
#include <mockturtle/algorithms/mig_resub.hpp>
#include <mockturtle/algorithms/node_resynthesis/mig_npn.hpp>
#include <mockturtle/algorithms/refactoring.hpp>

// 定義常數
constexpr int MAX_K = 8;
constexpr int MAX_CUTS_PER_NODE = 8; // 每個節點最多保留 8 個最好的 Cut，避免組合爆炸

struct KCut {
    int leafNetIds[MAX_K]; // 使用固定陣列，避免 std::vector 的動態配置開銷
    int size = 0;          // 紀錄目前實際的 Leaf 數量
    uint64_t signature = 0; // 快速比對用的特徵簽章 (Bitmask)

    // 輔助函式：確保內部排序、去重，並更新簽章
    void normalize() {
        if (size <= 1) {
            updateSignature();
            return;
        }

        // 1. 排序
        std::sort(leafNetIds, leafNetIds + size);

        // 2. 去重 (In-place)
        int uniqueCount = 1;
        for (int i = 1; i < size; ++i) {
            if (leafNetIds[i] != leafNetIds[uniqueCount - 1]) {
                leafNetIds[uniqueCount] = leafNetIds[i];
                uniqueCount++;
            }
        }
        size = uniqueCount;

        // 3. 更新簽章
        updateSignature();
    }

    // 更新 64-bit 簽章 (利用 Net ID 對 64 取餘數作為 Bit 位置)
    void updateSignature() {
        signature = 0;
        for (int i = 0; i < size; ++i) {
            signature |= (1ULL << (leafNetIds[i] & 0x3F)); // & 0x3F 相當於 % 64
        }
    }

    // 判斷兩個 Cut 是否完全相同
    bool operator==(const KCut& other) const {
        // 第一關：長度不同，絕對不同
        if (size != other.size) return false;
        
        // 第二關：簽章不同，絕對不同 (O(1) 極速排除)
        if (signature != other.signature) return false;
        
        // 第三關：才真的去比對陣列內容
        for (int i = 0; i < size; ++i) {
            if (leafNetIds[i] != other.leafNetIds[i]) return false;
        }
        return true;
    }
};

struct CutScore {
    int primaryScore;   // 首要指標 (AREA 模式為 MFFC；DEPTH 模式為 Critical Volume)
    int secondaryScore; // 次要指標 (用來打破平局)
    int penalty;        // 懲罰指標 (例如 Leaf 數量，越小越好，所以存負值或自定義比較邏輯)

    // 定義比較運算子：分數越大代表 Cut 越好
    bool operator<(const CutScore& other) const {
        if (primaryScore != other.primaryScore) return primaryScore < other.primaryScore;
        if (penalty != other.penalty) return penalty < other.penalty; // 注意 penalty 的方向
        return secondaryScore < other.secondaryScore;
    }
};

// =========================================================================
// 深度最佳化引擎設定 (Hyperparameters & Constraints)
// =========================================================================
struct DepthOptimizerConfig {
    // 面積限制設定 (Area Constraints)
    // -1 表示「不限制」單一 Critical Path 最佳化時增加的邏輯閘數量 (Default)
    int maxAreaIncreasePerPath = -1;    

    // 深度限制設定 (Depth Constraints)
    // -1 表示尊重個別 OptimizationCandidate 內帶的 targetDepth (Default)
    // 若大於 0，則作為單一 Critical Path 最佳化時的目標深度限制
    int targetDepthPerPath = -1;

    /*// 策略開關 (Strategy Toggles)
    bool enableBufferAndNotBypass = true; // 啟用冗餘 BUF/NOT 消除
    bool enableTreeBalancing = true;      // 啟用代數樹平衡 (Algebraic Tree Balancing)
    bool enableDeMorganPushing = true;    // 啟用德摩根推擠 (DeMorgan Pushing)
    bool enableConeResynthesis = true;    // 啟用 K-feasible Cut + Exact Synthesis*/
};

// =========================================================================
// DepthOptimizer (Critical Path Depth Reduction Engine)
//
// 獨立的核心引擎，專門處理 Critical Path 的邏輯深度縮減。
// 它會吃進 Netlist 與 OptimizationCandidate (包含 endpoint, cone, targetDepth)，
// 並運用各種重構策略 (例如 Cone Resynthesis、DeMorgan Pushing 等) 來嘗試縮減深度。
//
// 縮減成功與否以及面積的變化會回傳在 OptimizationResult 之中。
// =========================================================================
class DepthOptimizer {
public:
    // 初始化時傳入設定，若不傳則使用預設值
    explicit DepthOptimizer(const DepthOptimizerConfig& config = DepthOptimizerConfig());

    // -------------------------------------------------------------------------
    // 高階入口 API (High-Level APIs)
    // -------------------------------------------------------------------------

    // Critical Path 最佳化主控流程
    OptimizationResult executeCriticalPathOptimization(Netlist& netlist, 
                                                       TechMapper& techMapper,
                                                       const std::vector<GateType>& allowedTypes,
                                                       const ConeReport& targetConeReport,
                                                       const std::vector<GateType>& bannedTypes = {},
                                                       bool verbose = false);

    // -------------------------------------------------------------------------
    // 底層 API (Low-Level APIs)
    // -------------------------------------------------------------------------

    // K-feasible Cut 精確合成 (One-Shot Pass)
    /*bool runExactDepthPass(Netlist& netlist, 
                           TechMapper& mapper, 
                           OptimizationCandidate& candidate, 
                           const std::vector<GateType>& allowedTypes, 
                           const std::vector<GateType>& bannedTypes, bool verbose); 
                           
    bool runExactAreaPass(Netlist& netlist, 
                          TechMapper& mapper, 
                          OptimizationCandidate& candidate, 
                          int strictDepthLimit, 
                          const std::vector<GateType>& allowedTypes, 
                          const std::vector<GateType>& bannedTypes, 
                          bool verbose);*/

private:
    DepthOptimizerConfig config; // 用來儲存引擎的設定值

    // 輔助函式：給定 Root 與 Cut 邊界，從 Netlist 走訪並建立 PatternNode (AST)，同時收集 TargetCone
    std::shared_ptr<PatternNode> extractLhsFromCut(Netlist& netlist, 
                                                   int rootGateId, 
                                                   const KCut& cut, 
                                                   std::unordered_set<int>& outTargetCone,
                                                   int& outActualN);

    // 在乾淨的 Cone 裡面找出最佳的 K-feasible Cut
    KCut extractBestKFeasibleCut(Netlist& netlist, const OptimizationCandidate& candidate, OptimizationGoal goal);
    
    // 輔助函式：AREA 模式下的 Cut 評分機制
    CutScore evaluateAreaCut(Netlist& netlist, const KCut& cut, int rootGateId);

    // 輔助函式：DEPTH 模式下的 Cut 評分機制
    CutScore evaluateDepthCut(Netlist& netlist, const KCut& cut, int rootGateId, const std::unordered_set<int>& criticalGateSet);
};