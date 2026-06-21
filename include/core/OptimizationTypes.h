#pragma once

#include <string>

#include "include/core/NetlistTypes.h"
#include "include/core/PathTypes.h"

// =========================================================================
// Optimization Planning / Result Types
//
// 這個 header 只放 optimization planning 相關的純資料型別。
// 不放 rewrite pass、不放 validation 實作、不直接修改 netlist。
// Netlist.h 會用 using alias 保留 Netlist::Xxx 的既有寫法。
// =========================================================================

// 最佳化結果的狀態碼
enum class OptimizationStatus {
    SUCCESS,                 // 成功最佳化（例如深度縮減），且等價檢查通過
    NO_IMPROVEMENT,          // 找不到可以最佳化的改法
    ERROR_NOT_EQUIVALENT,    // 改動後等價檢查失敗 (應 Rollback)
    ERROR_AREA_EXCEEDED      // 最佳化雖達成，但超過了面積限制 (應 Rollback)
};

// 保存單次 optimization pass 嘗試的結果。
//
// 注意：
// - 這是早期 optimization pass 的 transitional result format。
// - 後續若導入 NetlistEditReport，這個 struct 應被 NetlistEditReport 取代或包進其中。
struct OptimizationResult {
    OptimizationStatus status = OptimizationStatus::NO_IMPROVEMENT;
    
    bool changed = false;            // netlist 是否真的被修改
    bool depthImproved = false;      // 修改後 depth 是否變小
    bool equivalenceChecked = false; // 是否已做等價檢查
    bool equivalent = false;         // 等價檢查是否通過
    
    int oldDepth = -1;               // 修改前 endpoint depth
    int newDepth = -1;               // 修改後 endpoint depth
    
    int oldGateCount = -1;           // 修改前的邏輯閘數量
    int newGateCount = -1;           // 修改後的邏輯閘數量
    int areaDelta = 0;               // 面積/閘數變化：正數代表面積增加，負數代表面積減少
    
    std::string passName;            // pass 名稱
    std::string message;             // 給 debug / LLM response 的說明
};

// 保存單一 depth optimization candidate。
//
// 這個 struct 只描述「可能要被最佳化的目標」：
// - endpoint：目前過深或被指定的 timing endpoint
// - targetDepth：希望達成的 depth 上限
// - faninCone：可被局部 resynthesis / rewrite 參考的 cone
// - criticalPath：目前最需要處理的一條 critical path
struct OptimizationCandidate {
    DepthReport endpoint;
    int targetDepth = -1;
    ConeResult faninCone;
    CombinationalPath criticalPath;
};
