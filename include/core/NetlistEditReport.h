#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "include/core/NetlistTypes.h"
#include "include/core/EditFlow.h"

// =========================================================================
// Unified Netlist Edit Report Types
//
// This header defines the shared result format for netlist-changing flows:
// cleanup, simplification, buffer insertion, technology mapping, and future
// optimization apply commands.
//
// Read-only query APIs keep their own focused report types. NetlistEditReport
// is only for operations that may mutate the design and therefore need
// before/after summaries, validation state, rollback state, and warnings.
// =========================================================================

enum class NetlistEditOperationKind {
    Unknown,
    Cleanup,
    Simplification,
    BufferInsertion,
    TechnologyMapping,
    PrimitiveMutation,
    DepthOptimization,
    CustomRewrite
};

enum class EquivalenceCheckMethod {
    NotChecked,
    StructuralIdentity,
    LocalRewriteRule,
    CertifiedRewrite,
    WholeDesignSat
};

// Compact design statistics used to summarize before/after state.
struct NetlistStats {
    size_t gateCount = 0;
    size_t activeGateCount = 0;
    size_t removedGateCount = 0;
    size_t netCount = 0;
    size_t activeNetCount = 0;
    size_t removedNetCount = 0;
    size_t primaryInputCount = 0;
    size_t primaryOutputCount = 0;
    size_t dffCount = 0;
    size_t combinationalGateCount = 0;
    std::map<GateType, int> gateTypeCounts;
};

// Difference between two NetlistStats snapshots.
struct NetlistDiff {
    int gateCountDelta = 0;
    int activeGateCountDelta = 0;
    int netCountDelta = 0;
    int activeNetCountDelta = 0;
    int dffCountDelta = 0;
    int combinationalGateCountDelta = 0;
    std::map<GateType, int> gateTypeCountDelta;
};

// Validation results after an edit attempt.
struct EditValidationResult {
    bool structureChecked = false;
    bool structureValid = false;
    bool problemAConstraintsChecked = false;
    bool problemAConstraintsBaselineValid = false;
    bool problemAConstraintsValid = false;
    bool problemAConstraintsRegressed = false;
    bool equivalenceChecked = false;
    bool functionallyEquivalent = false;
    EquivalenceCheckMethod equivalenceMethod = EquivalenceCheckMethod::NotChecked;
    std::vector<std::string> messages;
    std::vector<std::string> newProblemAConstraintViolations;
};

// Depth/timing change summary for depth-driven rewrites.
struct DepthChange {
    std::string endpointName;
    int beforeDepth = -1;
    int afterDepth = -1;
    int targetDepth = -1;
    bool improved = false;
    bool meetsTarget = false;
};

// Fanout change summary for buffer insertion or fanout optimization.
struct FanoutChange {
    int beforeMaxFanout = -1;
    int afterMaxFanout = -1;
    int targetFanout = -1;
    bool improved = false;
    bool meetsConstraint = false;
    std::vector<std::string> violatingNetNames;
};

// Technology mapping delta, normalized from mapper-specific reports.
struct MappingDelta {
    std::map<GateType, int> removedCountByType;
    std::map<GateType, int> addedCountByType;
    std::map<GateType, int> finalGateCountByType;
    std::vector<std::string> modifiedGateNames;
};

// Candidate and outcome details for one scoped constant-propagation pass.
// UNKNOWN/-1 filters mean "all" and preserve the legacy global behavior.
struct ConstantSimplificationSummary {
    GateType targetGateType = GateType::UNKNOWN;
    int targetConstValue = -1;
    int targetInputCount = -1;

    size_t candidateCount = 0;
    size_t simplifiedCount = 0;
    size_t skippedCount = 0;
    int eliminatedTargetGateCount = 0;

    std::vector<int> candidateGateIds;
    std::vector<int> simplifiedGateIds;
    std::vector<int> skippedGateIds;
    std::vector<std::string> candidateGateNames;
    std::vector<std::string> simplifiedGateNames;
    std::vector<std::string> skippedGateNames;
};

struct FunctionalGateMergeRecord {
    int representativeGateId = -1;
    int representativeNetId = -1;
    int removedGateId = -1;
    int removedNetId = -1;
    std::string representativeGateName;
    std::string representativeNetName;
    std::string removedGateName;
    std::string removedNetName;
};

struct FunctionalMergeSummary {
    std::string scope;
    std::string scopeName;
    GateType gateTypeFilter = GateType::UNKNOWN;
    std::string searchStatus;
    bool searchComplete = false;
    bool searchTimedOut = false;
    bool wholeDesignEquivalenceChecked = false;
    bool wholeDesignEquivalent = false;
    bool wholeDesignTimedOut = false;
    size_t candidateGateCount = 0;
    size_t equivalenceClassCount = 0;
    size_t equivalentPairCount = 0;
    size_t satChecks = 0;
    size_t mergedGateCount = 0;
    size_t skippedGateCount = 0;
    double searchElapsedSeconds = 0.0;
    double totalElapsedSeconds = 0.0;
    std::vector<FunctionalGateMergeRecord> records;
    std::vector<std::string> skippedGateNames;
};

struct DepthOptimizationSummary {
    std::string objectiveMetric;
    std::string scope;
    std::string requestedScopeName;
    std::string basisScope;       // targetScopeName(basisScope.scope)
    std::string basisScopeName;
    std::string resolvedRootNetName;
    std::string coreStatus;
    std::string coreMessage;

    std::vector<GateType> allowedTypes;
    std::vector<GateType> bannedTypes;

    bool resolvedThroughDffDataPin = false;
    bool baselineConstraintsSatisfied = false;
    bool finalConstraintsSatisfied = false;
    bool candidateGenerated = false;
    bool candidateAccepted = false;
    bool wholeDesignEquivalenceChecked = false;
    bool wholeDesignEquivalent = false;
    bool wholeDesignTimedOut = false;

    int comparedOutputCount = 0;
    int comparedDffDCount = 0;
    double timeBudgetSeconds = 0.0;
    double elapsedSeconds = 0.0;
};

// Dead logic 移除結果。removedGateCount 是「移除了幾個 gate」類 prompt 的
// 唯一正式答案來源；不要用 diff.activeGateCountDelta 反推。
struct DeadLogicSummary {
    size_t removedGateCount = 0;
    size_t removedNetCount  = 0;
    size_t removedDffCount  = 0;   // 只有 includeSequential 才可能非 0

    bool includeSequential = false;
    bool timedOut          = false;
};

// 內部診斷用，不進 report。
struct RedundancyDiagnostics {
    size_t constantNetCandidateCount = 0;
    size_t constantNetTiedCount      = 0;
    size_t constantNetSkippedPoCount = 0;
    size_t constantNetSkippedCount   = 0;
    size_t pinCandidateCount         = 0;
    size_t pinRejectedBySimulation   = 0;
    size_t pinSkippedReconvergent    = 0;
    size_t pinSkippedDffControl      = 0;
    size_t pinExaminedBySat          = 0;
    size_t satChecks                 = 0;
    size_t abortedCandidateCount     = 0;
    size_t deadLogicRemovedGateCount = 0;
    bool   constantPhaseComplete = false;
    bool   satPhaseComplete      = false;
    bool   fraigComplete         = false;
};

// Redundancy removal 結果。
// 用語對齊業界 ATPG fault class：
//   provenRedundantCount  ≈ RE (Redundant，已證明不可測)
//   abortedCandidateCount ≈ AU (ATPG Untestable，未證出，不可計入答案)
struct RedundancyRemovalSummary {
    // 正式答案：最終移除的 gate 總數。
    size_t removedGateCount = 0;
    size_t removedNetCount  = 0;

    // 完整性。false 代表這是時間預算內找到的數量，不是全部。
    bool complete  = false;
    bool timedOut  = false;

    // 已證明 untestable 的 stuck-at fault 數（ATPG 的 RE 分類）。
    // 未證明的候選（AU）刻意不回報 —— 它不是答案的一部分，
    // 讓 LLM 看到只會誤把它加進總數。
    size_t provenRedundantPinCount = 0;

    double elapsedSeconds = 0.0;
};

// Single shared report for mutation / optimization / transformation flows.
struct NetlistEditReport {
    bool success = false;
    bool changed = false;
    bool rolledBack = false;

    NetlistEditOperationKind operationKind = NetlistEditOperationKind::Unknown;
    std::string operationName;
    std::string message;

    NetlistStats beforeStats;
    NetlistStats afterStats;
    NetlistDiff diff;

    EditValidationResult validation;

    std::optional<DepthChange> depthChange;
    std::optional<FanoutChange> fanoutChange;
    std::optional<MappingDelta> mappingDelta;
    std::optional<ConstantSimplificationSummary> constantSimplification;
    std::optional<FunctionalMergeSummary> functionalMerge;
    std::optional<DepthOptimizationSummary> depthOptimization;
    std::optional<DeadLogicSummary> deadLogic;
    std::optional<RedundancyRemovalSummary> redundancyRemoval;

    std::vector<int> changedGateIds;
    std::vector<int> changedNetIds;
    std::vector<std::string> changedGateNames;
    std::vector<std::string> changedNetNames;
    std::vector<std::string> warnings;

    void addWarning(const std::string& warning) {
        warnings.push_back(warning);
    }
};

struct WholeDesignEquivalenceReport {
    bool ok = false;
    bool equivalent = false;
    bool timeBudgetExceeded = false;
    std::string message;

    int comparedOutputCount = 0;
    int skippedOutputCount = 0;
    int comparedDffDCount = 0;
    int skippedDffDCount = 0;
    double timeBudgetSeconds = 0.0;
    std::vector<std::string> matchedOutputNames;
    std::vector<std::string> mismatchedOutputNames;
    std::vector<std::string> skippedOutputNames;
    std::vector<std::string> matchedDffDNames;
    std::vector<std::string> mismatchedDffDNames;
    std::vector<std::string> skippedDffDNames;
    std::vector<std::string> missingInputNames;
    std::vector<std::string> extraInputNames;
    std::vector<std::string> missingOutputNames;
    std::vector<std::string> extraOutputNames;
    std::vector<std::string> missingDffNames;
    std::vector<std::string> extraDffNames;
    std::vector<std::string> unsupportedReasons;
    std::vector<std::string> warnings;

    EquivalenceCheckMethod method = EquivalenceCheckMethod::NotChecked;
};
