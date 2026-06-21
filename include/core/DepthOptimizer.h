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
    bool enableBufferBypass = true;
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

    // 針對多個 Candidates 進行批次深度縮減
    // 可傳入由 Netlist::findOptimizationCandidatesExceedingDepth() 找出的目標清單
    std::vector<OptimizationResult> optimizeDesign(Netlist& netlist, const std::vector<OptimizationCandidate>& candidates);

private:
    DepthOptimizerConfig m_config;
    // -------------------------------------------------------------------------
    // 底層縮減策略 (Reduction Strategies)
    // 這些策略由 reduceDepth 內部依序呼叫，嘗試不同的邏輯轉換手法
    // -------------------------------------------------------------------------

    // 策略 1: Buffer Bypass / Cleanup
    // 檢查 Critical Path 上是否有不必要的 BUF 閘可以被 Bypass，這是最便宜且縮減深度的改法
    bool tryBufferBypass(Netlist& netlist, const OptimizationCandidate& candidate, OptimizationResult& result);

    // 策略 2: DeMorgan Pushing (NOT 閘推移)
    // 將 NOT 閘推過 AND/OR 等閘，試圖消除冗餘的層級
    bool tryDeMorganPushing(Netlist& netlist, const OptimizationCandidate& candidate, OptimizationResult& result);

    // 策略 3: Cone Resynthesis / Technology Mapping
    // 針對 Critical Path 所屬的 Fanin Cone，呼叫 TechMapper 進行局部的重新合成
    bool tryConeResynthesis(Netlist& netlist, const OptimizationCandidate& candidate, OptimizationResult& result);

    // -------------------------------------------------------------------------
    // 輔助函式 (Helper Methods)
    // -------------------------------------------------------------------------
    
    // 用於評估單一策略執行後的成果，並決定是否要 Rollback
    // 如果修改有效且面積成本合理，回傳 true 接受修改；否則 Rollback 回傳 false
    bool evaluateAndCommit(Netlist& netlist, const Netlist& backup, int oldDepth, OptimizationResult& result);
};