#pragma once

#include <string>

#include "include/core/NetlistTypes.h"

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
    TrimDeadLogic,
    RemoveDanglingLogic,
    RemoveUnusedNets,
    MergeEquivalentGates,
    MergeStructurallyEquivalentGates,
    SimplifyConstants,
    SimplifySameInput,

    InsertBuffersForFanout,
    InsertBuffersForSpecificNet,
    InsertBuffersForDffControl,
    InsertBuffersOnEachLoad,
    InsertBufferAtDriver,
    InsertBufferBeforeGate,
    InsertBuffersByGateType,

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

    GateType gateType = GateType::UNKNOWN;
    bool processClock = false;
    bool processReset = false;
    bool bufferInputs = true;
    bool bufferOutputs = true;
    bool allowDuplicateLoads = false;

    bool validateEquivalence = false;
    bool rollbackOnFailure = true;
};
