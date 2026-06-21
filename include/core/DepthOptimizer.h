#pragma once

#include "include/core/Netlist.h"
#include "include/core/OptimizationTypes.h"
#include "include/core/TechMapper.h" 
#include <vector>

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

    // 策略開關 (Strategy Toggles)
    bool enableBufferAndNotBypass = true;
    bool enableDeMorganPushing = true;
    bool enableConeResynthesis = true;
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

    // 針對單一 Candidate 進行深度縮減
    OptimizationResult reduceDepth(Netlist& netlist, const OptimizationCandidate& candidate);

    // 不應該在最一開始就把所有 Candidate 的 Cone 和 Path 都死死地算好，若有電路重構發生，將無法只用unknow來判斷新舊
    // 針對多個 Candidates 進行批次深度縮減
    // 可傳入由 Netlist::findOptimizationCandidatesExceedingDepth() 找出的目標清單
    // std::vector<OptimizationResult> optimizeDesign(Netlist& netlist, const std::vector<OptimizationCandidate>& candidates);

private:
    DepthOptimizerConfig config; // 用來儲存引擎的設定值

    // 輔助函式：針對特定的 Fanin Cone 執行 Buffer 與 連續 NOT 的消除
    // 回傳值：是否有對電路進行任何修改
    bool applyBufferAndNotBypass(Netlist& netlist, const OptimizationCandidate& candidate);
};