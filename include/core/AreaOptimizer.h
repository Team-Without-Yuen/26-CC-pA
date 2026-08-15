#pragma once

// =========================================================================
// AreaOptimizer.h
//
// 新增檔案，完全不修改任何原始檔案（DepthOptimizer.h/.cpp、
// OptimizationFlow.h/.cpp、TechMapper.h/.cpp、Netlist.h 都維持原樣）。
//
// 這個檔案做的事情很單純：把「減少 gate 數量（面積優化）」這個目標，
// 用跟現有「減少深度（CriticalPathDepth）」完全一樣的骨架做一遍。
//
// 下面每一段程式碼旁邊都會註明是：
//   [REUSED]  直接借用原專案已經公開（public / 已在某個 .h 宣告）的型別或函式，
//             完全沒有複製貼上，只是 #include 進來使用。
//   [COPIED]  原本專案裡已經寫好、但因為是 private 或藏在 .cpp 的匿名 namespace
//             裡拿不到，所以把邏輯複製一份過來（不是抄襲，是同一個專案內的
//             程式碼重用，只是因為存取權限問題只能複製而不能直接呼叫）。
//   [NEW]     原本完全沒有，這次才新寫的部分。
// =========================================================================

// [REUSED] 這幾個 #include 全部是原專案既有的公開 header，
// 裡面的型別我們直接拿來用，不做任何修改。
#include "include/core/Netlist.h"                 // Netlist, NetlistEditReport, WholeDesignEquivalenceReport
#include "include/core/OptimizationTypes.h"        // OptimizationResult, OptimizationStatus, OptimizationCandidate
#include "include/core/OptimizationFlow.h"         // OptQueryReport（沿用同一份 read-only report 外型）
#include "include/core/EditFlow.h"                 // TargetScope, RewriteScopeResolution, resolveRewriteScope()
#include "include/core/NetlistQueries.h"           // ConeReport
#include "include/core/TechMapper.h"                // TechMapper, OptimizationGoal（AREA/DEPTH 早就定義好了）
#include "include/core/RequestTimeBudget.h"         // request_time_budget::RequestDeadline

#include <string>
#include <vector>
#include <unordered_set>

// =========================================================================
// 1. 面積優化引擎本體（對應原本的 DepthOptimizer class）
// =========================================================================

// [NEW] 面積優化的 config，形狀比照 DepthOptimizerConfig，但目前只有一個欄位。
struct AreaOptimizerConfig {
    // -1 表示不限制單一 candidate 允許增加的深度（面積優化正常不該讓深度變差，
    // 但保留這個欄位方便之後需要「面積優先、深度可退讓 N 層」時使用）。
    int maxDepthIncreasePerPath = -1;
};

// [NEW + COPIED] 面積優化引擎。
//
// 對外的高階入口 executeAreaOptimization() 是全新寫的，但內部會刻意呼叫跟
// DepthOptimizer 完全一樣的 mockturtle 元件（cut_rewriting / resubstitution），
// 只是關掉 DepthOptimizer 裡「保持深度」的開關，讓它以減少 gate 數為目標。
//
// K-feasible cut 的挑選邏輯（KCut 結構、MFFC 計分 evaluateAreaCut）在原本的
// DepthOptimizer.cpp 裡「已經寫好」（OptimizationGoal::AREA 分支），但那個
// 函式是 DepthOptimizer 的 private method，外部拿不到，所以這裡的作法是：
// 局部 cone 限制的情況直接重用同一個 OptimizationGoal::AREA 概念，呼叫
// TechMapper::optimizePattern(..., OptimizationGoal::AREA, ...)——這個函式
// 是 public 的（TechMapper.h 已宣告），本來就支援 AREA 目標，只是目前全專案
// 沒有任何地方呼叫它。這裡是第一個呼叫它的地方。
class AreaOptimizer {
public:
    explicit AreaOptimizer(const AreaOptimizerConfig& config = AreaOptimizerConfig());

    // [NEW] 高階入口，簽名刻意比照
    // DepthOptimizer::executeCriticalPathOptimization()，方便未來維護者一眼
    //看出兩者是同一種模式的兩個目標版本。
    //
    // 內部流程（對比 DepthOptimizer 的三段式流程）：
    //
    //   Stage 1  約束分類：跟深度版一樣，先判斷是純 AIG / 純 XAG / 一般限制 / 局部 cone。
    //            [REUSED] 這段邏輯（isPureAIG/isPureXAG 判斷）本身很單純，
    //            直接照抄一份到 .cpp，不特別複製整個 helper。
    //
    //   Stage 2  全域面積壓縮：
    //            [COPIED + 修改] 沿用 DepthOptimizer 同一套
    //            NetlistToXag / XagToNetlist 轉換（MockturtleConverter.h，
    //            這個是 [REUSED]，完全公開），但迴圈內容改成：
    //              - 不做 balancing（balancing 是用面積換深度，面積優化不需要）
    //              - cut_rewriting_params.preserve_depth = false（原本深度版是 true）
    //              - cut_rewriting_params.allow_zero_gain = false（原本深度版是 true；
    //                面積優化只接受「真的變小」的改寫，不接受打平的改寫）
    //              - resubstitution 保留（resubstitution 本來就是面積導向技巧，
    //                深度版拿來當副產品，這裡是主要手段）
    //            每輪迭代用 netlist.countGatesByType() 加總後的 gate 數判斷有沒有進步，
    //            取代深度版用 findGlobalCriticalPath().depth 判斷進步。
    //
    //   Stage 3  局部 cone 限制（若有指定 allowedTypes/bannedTypes 且非 whole scope）：
    //            [REUSED] 呼叫 techMapper.optimizePattern(netlist, lhsTarget,
    //            OptimizationGoal::AREA, targetCone, ...)——這個函式已經支援
    //            AREA 目標，只是之前沒人呼叫過。
    //
    // 回傳值沿用 [REUSED] 的 OptimizationResult（本來就有 oldGateCount /
    // newGateCount / areaDelta 三個欄位，不用新增任何欄位）。
    OptimizationResult executeAreaOptimization(
        Netlist& netlist,
        TechMapper& techMapper,
        const ConeReport& targetConeReport,
        const std::vector<GateType>& allowedTypes = {},
        const std::vector<GateType>& bannedTypes = {},
        bool verbose = false,
        const request_time_budget::RequestDeadline* requestDeadline = nullptr);

private:
    AreaOptimizerConfig config;

    // [COPIED] 全 netlist 的 NOT(NOT x) -> x 清除。
    // DepthOptimizer::eliminateDoubleInverters() 做的是完全一樣的事，但它是
    // DepthOptimizer 的 private method，這裡複製同一份邏輯（不是新演算法，
    // 純粹是因為存取權限只能複製）。
    int eliminateDoubleInverters(Netlist& netlist);
};

// =========================================================================
// 2. Orchestration 層（對應原本 OptimizationFlow.h 裡的 OptApplyRequest /
//    Netlist::runOptApply()）
// =========================================================================

// [NEW] 不新增 OptPassKind enum 值（那要改 OptimizationFlow.h），改成獨立的
// pass kind，只有一個值，之後真的要合併時再手動接進 OptPassKind。
enum class AreaPassKind {
    Unknown,
    GateCountMinimization
};

// [NEW] Request 型別，形狀刻意比照 [REUSED] 的 OptApplyRequest
// （OptimizationFlow.h），把 depthObjective/targetDepth 換成
// areaObjective/targetGateCount，其他欄位語意完全相同。
enum class AreaObjective {
    GlobalGateCount,          // 對應 OptDepthObjective::GlobalMaximum
    ScopedFaninConeGateCount  // 對應 OptDepthObjective::ScopedFaninCone
};

struct AreaOptApplyRequest {
    AreaPassKind passKind = AreaPassKind::GateCountMinimization;

    // [REUSED] TargetScope 直接沿用 EditFlow.h 的定義，不用另外做一份。
    TargetScope scope = TargetScope::WHOLE_NETLIST;
    std::string scopeName;

    AreaObjective areaObjective = AreaObjective::GlobalGateCount;

    std::vector<GateType> allowedTypes;
    std::vector<GateType> bannedTypes;

    int targetGateCount = -1;   // -1 = best effort

    // [REUSED] 跟 OptApplyRequest 用同一個時間預算慣例。
    double timeLimitSeconds = request_time_budget::kGeneralToolBudgetSeconds;

    bool requireGateCountImprovement = true;
    bool verbose = false;

    // 跟 CriticalPathDepth 一樣固定為 true，只保留欄位做介面對稱，
    // runAreaOptApply() 內部會忽略使用者傳 false 並加 warning
    // （這行為完全比照 OptimizationFlow.cpp 對 validateEquivalence /
    // rollbackOnFailure 的處理方式）。
    bool validateEquivalence = true;
    bool rollbackOnFailure = true;
};

// [NEW] 因為 NetlistEditReport（NetlistEditReport.h）目前沒有「面積優化摘要」
// 這個欄位、而且我們不能改那個檔案去加欄位，所以面積優化的摘要另外包成一個
// 獨立 struct，回傳時跟 NetlistEditReport 一起打包，而不是塞進它裡面。
// 欄位形狀比照 [REUSED] 的 DepthOptimizationSummary（NetlistEditReport.h），
// 只是把 depth 相關欄位換成 gate count 相關欄位。
struct GateCountOptimizationSummary {
    std::string objectiveMetric = "CombinationalGateCount";

    std::string scope;                 // TargetScope 轉成字串，沿用跟深度版一樣的作法
    std::string requestedScopeName;
    std::string resolvedRootNetName;
    bool resolvedThroughDffDataPin = false;

    std::vector<GateType> allowedTypes;
    std::vector<GateType> bannedTypes;

    bool baselineConstraintsSatisfied = false;
    bool finalConstraintsSatisfied = false;

    std::string coreStatus;
    std::string coreMessage;

    bool candidateGenerated = false;
    bool candidateAccepted = false;

    int gateCountBefore = -1;
    int gateCountAfter = -1;
    int targetGateCount = -1;
    bool improved = false;
    bool meetsTarget = false;

    // [REUSED] 這三個欄位對應的檢查機制直接呼叫
    // Netlist::checkWholeDesignEquivalence()（Netlist.h 已公開），
    // 跟深度版共用同一個 whole-design SAT commit 關卡，不重寫。
    bool wholeDesignEquivalenceChecked = false;
    bool wholeDesignEquivalent = false;
    bool wholeDesignTimedOut = false;
    int comparedOutputCount = 0;
    int comparedDffDCount = 0;

    double timeBudgetSeconds = 0.0;
    double elapsedSeconds = 0.0;
};

// [NEW] 打包回傳型別：NetlistEditReport 整個直接重用、不修改，
// 面積摘要另外掛在旁邊。
struct AreaOptApplyResult {
    NetlistEditReport report;                       // [REUSED]，型別完全沒改
    GateCountOptimizationSummary areaOptimization;   // [NEW]
};

// [NEW] 高階入口，是自由函式（不是 Netlist:: 的 member），因為要新增
// Netlist 的 member function 必須改 Netlist.h。
//
// Transaction 流程刻意跟 Netlist::runOptApply() 的 CriticalPathDepth 分支
// 一模一樣（OptimizationFlow.cpp 第 550-905 行左右），只是把「深度」
// 換成「gate 數」：
//
//   original snapshot（Netlist::cloneForRollback()，[REUSED]）
//     -> resolveRewriteScope()（EditFlow.h，[REUSED]）解析 scope
//     -> AreaOptimizer::executeAreaOptimization() 產生候選（本檔案，[NEW]）
//     -> gate-basis constraint 檢查（沿用跟深度版一樣的白名單/黑名單邏輯）
//     -> gate count 是否改善 / 是否達到 targetGateCount
//     -> Netlist::checkWholeDesignEquivalence()（Netlist.h，[REUSED]）
//        全部通過才 commit，否則 Netlist::restoreFrom(original)（[REUSED]）
//
// 注意：resolveRewriteScope() 之後那些「判斷 scope 是否為 DFF.Q boundary
// 空 cone」「baseline 是否已符合 constraint」的細節判斷，在原本
// OptimizationFlow.cpp 裡是寫在匿名 namespace 裡的 file-local 函式
// （scopeSatisfiesGateConstraints 等），拿不到，需要在 .cpp 實作時
// 重新寫一份等價邏輯（不算複製，因為原函式本來就不到 20 行、邏輯很單純，
// 直接照著同樣的判斷式重寫即可）。
AreaOptApplyResult runAreaOptApply(Netlist& netlist, const AreaOptApplyRequest& request);

// [NEW] Read-only 版本，形狀比照 [REUSED] 的 OptQueryReport（OptimizationFlow.h），
// 回答「目前 gate 數是多少」，不修改 netlist。
struct AreaOptQueryReport {
    bool ok = false;
    int totalGateCount = -1;
    int scopedGateCount = -1;   // scope 為 WHOLE_NETLIST 時填 -1
    std::string message;
};

AreaOptQueryReport runAreaOptQuery(const Netlist& netlist, const AreaOptApplyRequest& request);