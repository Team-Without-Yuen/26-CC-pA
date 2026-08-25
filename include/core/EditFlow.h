#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "include/core/NetlistTypes.h"
#include "include/core/RequestTimeBudget.h"

// Defines where a transformation/edit command is allowed to scan and rewrite.
enum class TargetScope {
    WHOLE_NETLIST,
    NET_FANIN,
    NET_FANOUT,
    GATE_FANIN,
    GATE_FANOUT,
    SINGLE_GATE
};

class Netlist;

// Resolves the graph region that a transformation or optimization may rewrite.
// Net-fanin scopes follow read-only cone-query semantics: a DFF/Q net is a
// sequential boundary and resolves to an empty combinational cone. Gate-fanin
// scopes that explicitly name a DFF instance may still select the D-pin data
// cone because the target is the sequential cell, not its Q signal.
struct RewriteScopeResolution {
    bool ok = false;
    bool wholeNetlist = false;
    bool resolvedThroughDffDataPin = false;
    std::string requestedName;
    std::string resolvedRootNetName;
    std::string message;
    ConeResult cone;
};

RewriteScopeResolution resolveRewriteScope(
    const Netlist& netlist,
    TargetScope scope,
    const std::string& name);

enum class EditCommandKind {
    Unknown,

    RenameGate,
    RenameNet,
    DisconnectGateInput,
    ConnectGateInput,
    RemoveGate,
    ReplaceAllLoadsOfNet,

    CleanupBuffers,
    CollapseDoubleInverter,
    LocalSimplificationFixpoint,
    SafeCleanupFixpoint,
    TrimDeadLogic,
    RemoveDanglingLogic,
    RemoveDeadLogic, // TrimDeadLogic 和 RemoveDanglingLogic 意思一樣，統一入口
    RemoveUnusedNets,
    MergeEquivalentGates, // Legacy/internal structural merge alias；不可作為 functional merge 對外公開
    MergeStructurallyEquivalentGates,
    MergeFunctionallyEquivalentGates,
    MergeDuplicateDffs,
    SimplifyConstants,
    SimplifySameInput,
    RemoveRedundantLogic,

    InsertBuffersForFanout,
    InsertBuffersForSpecificNet,
    InsertBuffersForDffControl,
    InsertBuffersOnEachLoad,
    InsertBufferAtDriver,
    InsertBufferBeforeGate,
    InsertBuffersByGateType,

    ConvertToBasis,
    ReplaceGateType,

    ReplaceGateWithNet,
    ReplaceGateWithConstant,
    ReplaceGateWithNotOfNet,
    ReplaceDriverOfNet,
    RewireGateOutputToExistingNet,
    ReplaceNetFunctionKeepingName,
    MergeNetIntoNet,
    BypassNetKeepingPortSemantics,
    RedirectAllLoads,
    RemoveNetIfUnused
};

struct EditApplyRequest {
    EditCommandKind kind = EditCommandKind::Unknown;

    std::string gateName;
    std::string netName;
    std::string oldName;
    std::string newName;
    std::string targetGateName;
    std::string scopeName;

    int gateId = -1;
    int netId = -1;
    int oldNetId = -1;
    int newNetId = -1;
    int sourceNetId = -1;
    int targetNetId = -1;
    int replacementNetId = -1;
    int newDriverGateId = -1;
    int newOutputNetId = -1;
    int maxFanout = -1;
    int pinIndex = -1;
    int constValue = -1;
    int inputCount = -1;
    size_t simulationPatternCount = 256;
    double timeLimitSeconds = request_time_budget::kGeneralToolBudgetSeconds;

    GateType gateType = GateType::UNKNOWN;
    GateType targetGateType = GateType::UNKNOWN;
    TargetScope scope = TargetScope::WHOLE_NETLIST;
    std::vector<GateType> allowedTypes;
    std::vector<GateType> bannedTypes;

    bool processClock = false;
    bool processReset = false;
    bool bufferInputs = true;
    bool bufferOutputs = true;
    bool allowDuplicateLoads = false;
    bool verbose = false;

    bool validateEquivalence = false;
    bool rollbackOnFailure = true;

    bool includeSequential = false;
};

struct DeadLogicOptions {
    bool includeSequential = false;
    const request_time_budget::RequestDeadline* deadline = nullptr;
};

struct RedundancyRemovalOptions {
    size_t simulationPatternCount = 1024;
    double perQuerySeconds = 0.5;
    size_t maxSatChecks = 20000;

    // 各階段的進入門檻：剩餘時間必須高於「總預算 × fraction」才開始該階段。
    // 用比例而非固定秒數，題目給 30 秒或 290 秒都能得到合理的階段分配。
    // floor 是絕對下限，避免預算極小時用 3 秒去啟動一個昂貴階段
    // —— 那只會在內部 timeout，把時間浪費掉而沒有任何產出。
    double minStageFraction      = 0.05;   // 便宜的結構化簡
    double minStageFloorSeconds  = 1.0;

    double minSatStageFraction     = 0.15;  // Layer A / Layer B
    double minSatStageFloorSeconds = 5.0;

    double minFunctionalMergeFraction     = 0.30;  // functional merge 最貴
    double minFunctionalMergeFloorSeconds = 15.0;

    // deadline 為 nullptr 時假定的總預算，只用來換算 fraction。
    double assumedBudgetSeconds = request_time_budget::kGeneralToolBudgetSeconds;

    const request_time_budget::RequestDeadline* deadline = nullptr;
};
