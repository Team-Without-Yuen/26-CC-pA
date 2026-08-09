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
    //
    // ============================ 整體流程總覽 ============================
    // 依「題目約束的類型」分派到不同路徑。約束分三大類：
    //
    //  (A) 純基底題（bannedTypes 空 + allowed 剛好是 AIG 或 XAG 的閘集）
    //        → 對應 mockturtle 網路直接優化，優化完即輸出，不做反相吸收
    //
    //  (B) 全域一般限制題（允許複合閘、或禁止某些基礎閘，但非純 AIG/XAG）
    //        → 先用 XAG 自由優化取得最小深度
    //        → 【基底強制】把「被禁止的基礎閘」換成允許的等價組合（合規，不可選）
    //        → 【反相吸收】把散落的 NOT 吃進 NAND/NOR/XNOR（省深度，機會型）
    //        → 輸出
    //
    //  (C) 局部 Cone 限制題（某個 cone 內部只能用特定閘集）
    //        → 先把整個電路當「無限制」自由優化（XAG）
    //        → 切出受限 cone（K-feasible cut 界定範圍）
    //        → 對該 cone 依受限基底重合成（exact synthesis / 受限 resynth）
    //        → 縫回原電路
    //        → 輸出
    //
    // 每個階段之後都做 trimDeadLogic 清死邏輯，結算前必做一次確保面積正確。
    // =====================================================================
    OptimizationResult executeCriticalPathOptimization(Netlist& netlist, 
                                                       TechMapper& techMapper,
                                                       const ConeReport& targetConeReport,
                                                       const std::vector<GateType>& allowedTypes = {},
                                                       const std::vector<GateType>& bannedTypes = {},
                                                       bool verbose = false);

private:
    friend struct DepthOptimizerTestAccess;

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

    // 全 netlist 的 NOT(NOT x) → x 消除。
    int eliminateDoubleInverters(Netlist& netlist);
};
