#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "include/core/NetlistTypes.h"

// Defines where a transformation/edit command is allowed to scan and rewrite.
enum class TargetScope {
    WHOLE_NETLIST,
    NET_FANIN,
    NET_FANOUT,
    GATE_FANIN,
    GATE_FANOUT
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
    RemoveUnusedNets,
    MergeEquivalentGates, // Legacy/internal structural merge alias；不可作為 functional merge 對外公開
    MergeStructurallyEquivalentGates,
    MergeFunctionallyEquivalentGates,
    SimplifyConstants,
    SimplifySameInput,

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
    double timeLimitSeconds = 30.0;

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
};
