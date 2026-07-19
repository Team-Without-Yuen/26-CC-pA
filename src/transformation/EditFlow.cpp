#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"

namespace {

struct EditRequestValidation {
    bool ok = true;
    std::string message;
};

std::string editCommandKindName(EditCommandKind kind) {
    switch (kind) {
        case EditCommandKind::RenameGate: return "rename_gate";
        case EditCommandKind::RenameNet: return "rename_net";
        case EditCommandKind::DisconnectGateInput: return "disconnect_gate_input";
        case EditCommandKind::ConnectGateInput: return "connect_gate_input";
        case EditCommandKind::RemoveGate: return "remove_gate";
        case EditCommandKind::ReplaceAllLoadsOfNet: return "replace_all_loads_of_net";
        case EditCommandKind::CleanupBuffers: return "cleanup_buffers";
        case EditCommandKind::CollapseDoubleInverter: return "collapse_double_inverter";
        case EditCommandKind::LocalSimplificationFixpoint: return "local_simplification_fixpoint";
        case EditCommandKind::SafeCleanupFixpoint: return "safe_cleanup_fixpoint";
        case EditCommandKind::TrimDeadLogic: return "trim_dead_logic";
        case EditCommandKind::RemoveDanglingLogic: return "remove_dangling_logic";
        case EditCommandKind::RemoveUnusedNets: return "remove_unused_nets";
        case EditCommandKind::MergeEquivalentGates: return "merge_equivalent_gates";
        case EditCommandKind::MergeStructurallyEquivalentGates: return "merge_structurally_equivalent_gates";
        case EditCommandKind::MergeFunctionallyEquivalentGates: return "merge_functionally_equivalent_gates";
        case EditCommandKind::SimplifyConstants: return "simplify_constants";
        case EditCommandKind::SimplifySameInput: return "simplify_same_input";
        case EditCommandKind::InsertBuffersForFanout: return "insert_buffers_for_fanout";
        case EditCommandKind::InsertBuffersForSpecificNet: return "insert_buffers_for_specific_net";
        case EditCommandKind::InsertBuffersForDffControl: return "insert_buffers_for_dff_control";
        case EditCommandKind::InsertBuffersOnEachLoad: return "insert_buffers_on_each_load";
        case EditCommandKind::InsertBufferAtDriver: return "insert_buffer_at_driver";
        case EditCommandKind::InsertBufferBeforeGate: return "insert_buffer_before_gate";
        case EditCommandKind::InsertBuffersByGateType: return "insert_buffers_by_gate_type";
        case EditCommandKind::ConvertToBasis: return "convert_basis";
        case EditCommandKind::ReplaceGateType: return "replace_type";
        case EditCommandKind::ReplaceGateWithNet: return "replace_gate_with_net";
        case EditCommandKind::ReplaceGateWithConstant: return "replace_gate_with_constant";
        case EditCommandKind::ReplaceGateWithNotOfNet: return "replace_gate_with_not_of_net";
        case EditCommandKind::ReplaceDriverOfNet: return "replace_driver_of_net";
        case EditCommandKind::RewireGateOutputToExistingNet: return "rewire_gate_output_to_existing_net";
        case EditCommandKind::ReplaceNetFunctionKeepingName: return "replace_net_function_keeping_name";
        case EditCommandKind::MergeNetIntoNet: return "merge_net_into_net";
        case EditCommandKind::BypassNetKeepingPortSemantics: return "bypass_net_keeping_port_semantics";
        case EditCommandKind::RedirectAllLoads: return "redirect_all_loads";
        case EditCommandKind::RemoveNetIfUnused: return "remove_net_if_unused";
        default: return "unknown";
    }
}

bool isTechnologyMappingCommand(EditCommandKind kind) {
    return kind == EditCommandKind::ConvertToBasis ||
           kind == EditCommandKind::ReplaceGateType;
}

int resolveGateId(const Netlist& netlist, int gateId, const std::string& gateName) {
    if (gateId >= 0) return gateId;
    if (!gateName.empty()) return netlist.getGateId(gateName);
    return -1;
}

int resolveNetId(const Netlist& netlist, int netId, const std::string& netName) {
    if (netId >= 0) return netId;
    if (!netName.empty()) return netlist.getNetId(netName);
    return -1;
}

int fanoutLimitOrDefault(int maxFanout) {
    return maxFanout > 0 ? maxFanout : 4;
}

bool isActiveGateId(const Netlist& netlist, int gateId) {
    return netlist.isValidGateId(gateId) && !netlist.isGateRemoved(gateId);
}

bool isActiveNetId(const Netlist& netlist, int netId) {
    return netlist.isValidNetId(netId) && !netlist.getNet(netId).isRemoved;
}

EditRequestValidation okValidation() {
    return {};
}

EditRequestValidation failedValidation(const std::string& message) {
    EditRequestValidation result;
    result.ok = false;
    result.message = message;
    return result;
}

bool isInternalLowLevelPrimitive(EditCommandKind kind) {
    switch (kind) {
        case EditCommandKind::DisconnectGateInput:
        case EditCommandKind::ConnectGateInput:
        case EditCommandKind::RemoveGate:
        case EditCommandKind::ReplaceAllLoadsOfNet:
        case EditCommandKind::ReplaceGateWithNet:
        case EditCommandKind::ReplaceGateWithConstant:
        case EditCommandKind::ReplaceGateWithNotOfNet:
        case EditCommandKind::ReplaceDriverOfNet:
        case EditCommandKind::RewireGateOutputToExistingNet:
        case EditCommandKind::ReplaceNetFunctionKeepingName:
        case EditCommandKind::MergeNetIntoNet:
        case EditCommandKind::BypassNetKeepingPortSemantics:
        case EditCommandKind::RedirectAllLoads:
            return true;
        default:
            return false;
    }
}

EditRequestValidation requireGateName(const Netlist& netlist, const std::string& gateName, const std::string& fieldName) {
    if (gateName.empty()) {
        return failedValidation("Missing required argument: " + fieldName + ".");
    }
    const int gateId = netlist.getGateId(gateName);
    if (!isActiveGateId(netlist, gateId)) {
        return failedValidation("Gate not found or already removed: " + gateName + ".");
    }
    return okValidation();
}

EditRequestValidation requireNetName(const Netlist& netlist, const std::string& netName, const std::string& fieldName) {
    if (netName.empty()) {
        return failedValidation("Missing required argument: " + fieldName + ".");
    }
    const int netId = netlist.getNetId(netName);
    if (!isActiveNetId(netlist, netId)) {
        return failedValidation("Net not found or already removed: " + netName + ".");
    }
    return okValidation();
}

EditRequestValidation requireGateId(const Netlist& netlist, int gateId, const std::string& fieldName) {
    if (!isActiveGateId(netlist, gateId)) {
        return failedValidation("Invalid or removed gate id for " + fieldName + ": " + std::to_string(gateId) + ".");
    }
    return okValidation();
}

EditRequestValidation requireNetId(const Netlist& netlist, int netId, const std::string& fieldName) {
    if (!isActiveNetId(netlist, netId)) {
        return failedValidation("Invalid or removed net id for " + fieldName + ": " + std::to_string(netId) + ".");
    }
    return okValidation();
}

EditRequestValidation requireResolvedGate(const Netlist& netlist, int gateId, const std::string& gateName, const std::string& fieldName) {
    if (gateId < 0 && gateName.empty()) {
        return failedValidation("Missing required argument: " + fieldName + " gateId or gateName.");
    }
    return requireGateId(netlist, resolveGateId(netlist, gateId, gateName), fieldName);
}

EditRequestValidation requireResolvedNet(const Netlist& netlist, int netId, const std::string& netName, const std::string& fieldName) {
    if (netId < 0 && netName.empty()) {
        return failedValidation("Missing required argument: " + fieldName + " netId or netName.");
    }
    return requireNetId(netlist, resolveNetId(netlist, netId, netName), fieldName);
}

EditRequestValidation requireFanoutLimit(int maxFanout, bool allowDefault) {
    if (maxFanout < 0 && allowDefault) {
        return okValidation();
    }
    if (maxFanout < 2) {
        return failedValidation("Invalid maxFanout: must be at least 2.");
    }
    return okValidation();
}

bool isKnownCombinationalGateType(GateType type) {
    switch (type) {
        case GateType::AND:
        case GateType::OR:
        case GateType::NAND:
        case GateType::NOR:
        case GateType::NOT:
        case GateType::BUF:
        case GateType::XOR:
        case GateType::XNOR:
            return true;
        default:
            return false;
    }
}

EditRequestValidation requireValidGateTypeList(
    const std::vector<GateType>& types,
    const std::string& fieldName,
    bool allowEmpty)
{
    if (types.empty()) {
        return allowEmpty ? okValidation() : failedValidation("Missing required argument: " + fieldName + ".");
    }
    for (GateType type : types) {
        if (!isKnownCombinationalGateType(type)) {
            return failedValidation("Invalid gate type in " + fieldName + ".");
        }
    }
    return okValidation();
}

EditRequestValidation requireScopeTarget(const Netlist& netlist, TargetScope scope, const std::string& scopeName) {
    switch (scope) {
        case TargetScope::WHOLE_NETLIST:
            return okValidation();
        case TargetScope::NET_FANIN:
        case TargetScope::NET_FANOUT:
            return requireNetName(netlist, scopeName, "scopeName");
        case TargetScope::GATE_FANIN:
        case TargetScope::GATE_FANOUT:
            return requireGateName(netlist, scopeName, "scopeName");
        default:
            return failedValidation("Invalid technology mapping scope.");
    }
}

EditRequestValidation validateEditApplyRequest(const Netlist& netlist, const EditApplyRequest& request) {
    if (isInternalLowLevelPrimitive(request.kind)) {
        return failedValidation(
            "Command is an internal low-level primitive and is not exposed through EditApply because functional equivalence is not guaranteed.");
    }

    switch (request.kind) {
        // Public rename commands.
        case EditCommandKind::RenameGate:
            if (request.oldName.empty()) return failedValidation("Missing required argument: oldName.");
            if (request.newName.empty()) return failedValidation("Missing required argument: newName.");
            if (!isActiveGateId(netlist, netlist.getGateId(request.oldName))) {
                return failedValidation("Gate not found or already removed: " + request.oldName + ".");
            }
            if (request.oldName != request.newName && netlist.getGateId(request.newName) >= 0) {
                return failedValidation("Gate rename target already exists: " + request.newName + ".");
            }
            return okValidation();

        case EditCommandKind::RenameNet:
            if (request.oldName.empty()) return failedValidation("Missing required argument: oldName.");
            if (request.newName.empty()) return failedValidation("Missing required argument: newName.");
            if (!isActiveNetId(netlist, netlist.getNetId(request.oldName))) {
                return failedValidation("Net not found or already removed: " + request.oldName + ".");
            }
            if (request.oldName != request.newName && netlist.getNetId(request.newName) >= 0) {
                return failedValidation("Net rename target already exists: " + request.newName + ".");
            }
            return okValidation();

        // Internal-only graph rewiring primitives. These are blocked before dispatch.
        case EditCommandKind::DisconnectGateInput:
        case EditCommandKind::ConnectGateInput: {
            EditRequestValidation gateCheck = requireGateName(netlist, request.gateName, "gateName");
            if (!gateCheck.ok) return gateCheck;
            EditRequestValidation netCheck = requireNetName(netlist, request.netName, "netName");
            if (!netCheck.ok) return netCheck;
            if (request.pinIndex < -1) return failedValidation("Invalid pinIndex: must be -1 or non-negative.");
            return okValidation();
        }

        case EditCommandKind::RemoveGate:
            return requireResolvedGate(netlist, request.gateId, request.gateName, "target");

        case EditCommandKind::ReplaceAllLoadsOfNet:
            if (request.oldNetId == request.newNetId) return failedValidation("oldNetId and newNetId must be different.");
            if (EditRequestValidation check = requireNetId(netlist, request.oldNetId, "oldNetId"); !check.ok) return check;
            return requireNetId(netlist, request.newNetId, "newNetId");

        // Public cleanup and local simplification commands.
        case EditCommandKind::CleanupBuffers:
        case EditCommandKind::CollapseDoubleInverter:
        case EditCommandKind::LocalSimplificationFixpoint:
        case EditCommandKind::SafeCleanupFixpoint:
        case EditCommandKind::TrimDeadLogic:
        case EditCommandKind::RemoveDanglingLogic:
        case EditCommandKind::RemoveUnusedNets:
        case EditCommandKind::MergeStructurallyEquivalentGates:
        case EditCommandKind::SimplifySameInput:
            return okValidation();

        case EditCommandKind::MergeFunctionallyEquivalentGates: {
            EditRequestValidation scopeCheck =
                requireScopeTarget(netlist, request.scope, request.scopeName);
            if (!scopeCheck.ok) return scopeCheck;
            if (request.gateType != GateType::UNKNOWN &&
                !isKnownCombinationalGateType(request.gateType)) {
                return failedValidation(
                    "gateType must be a supported combinational gate type or UNKNOWN.");
            }
            if (request.simulationPatternCount == 0 ||
                request.simulationPatternCount > 4096) {
                return failedValidation(
                    "simulationPatternCount must be in the range 1..4096.");
            }
            if (request.timeLimitSeconds <= 0.0) {
                return failedValidation("timeLimitSeconds must be positive.");
            }
            return okValidation();
        }

        case EditCommandKind::MergeEquivalentGates:
            return failedValidation(
                "MergeEquivalentGates is a legacy internal structural-merge alias. "
                "Use MergeStructurallyEquivalentGates; SAT-based functional merge "
                "is not implemented yet.");

        case EditCommandKind::SimplifyConstants:
            if (request.gateType != GateType::UNKNOWN &&
                !isKnownCombinationalGateType(request.gateType)) {
                return failedValidation("gateType must be a supported combinational gate type or UNKNOWN.");
            }
            if (request.constValue < -1 || request.constValue > 1) {
                return failedValidation("constValue must be -1, 0, or 1.");
            }
            if (request.inputCount != -1 && request.inputCount < 1) {
                return failedValidation("inputCount must be -1 or a positive integer.");
            }
            return okValidation();

        // Public buffer insertion commands.
        case EditCommandKind::InsertBuffersForFanout:
            return requireFanoutLimit(request.maxFanout, true);

        case EditCommandKind::InsertBuffersForSpecificNet: {
            EditRequestValidation netCheck = requireNetName(netlist, request.netName, "netName");
            if (!netCheck.ok) return netCheck;
            return requireFanoutLimit(request.maxFanout, true);
        }

        case EditCommandKind::InsertBuffersForDffControl:
            if (!request.processClock && !request.processReset) {
                return failedValidation("At least one of processClock or processReset must be true.");
            }
            return requireFanoutLimit(request.maxFanout, true);

        case EditCommandKind::InsertBuffersOnEachLoad:
        case EditCommandKind::InsertBufferAtDriver:
            return requireNetName(netlist, request.netName, "netName");

        case EditCommandKind::InsertBufferBeforeGate: {
            EditRequestValidation netCheck = requireNetName(netlist, request.netName, "netName");
            if (!netCheck.ok) return netCheck;
            return requireGateName(netlist, request.targetGateName, "targetGateName");
        }

        case EditCommandKind::InsertBuffersByGateType:
            if (request.gateType == GateType::UNKNOWN) {
                return failedValidation("Missing required argument: gateType.");
            }
            if (!request.bufferInputs && !request.bufferOutputs) {
                return failedValidation("At least one of bufferInputs or bufferOutputs must be true.");
            }
            return okValidation();

        // Public technology mapping commands.
        case EditCommandKind::ConvertToBasis: {
            EditRequestValidation scopeCheck = requireScopeTarget(netlist, request.scope, request.scopeName);
            if (!scopeCheck.ok) return scopeCheck;
            if (request.allowedTypes.empty() && request.bannedTypes.empty()) {
                return failedValidation("ConvertToBasis requires allowedTypes or bannedTypes.");
            }
            if (EditRequestValidation allowedCheck = requireValidGateTypeList(request.allowedTypes, "allowedTypes", true); !allowedCheck.ok) return allowedCheck;
            return requireValidGateTypeList(request.bannedTypes, "bannedTypes", true);
        }

        case EditCommandKind::ReplaceGateType: {
            EditRequestValidation scopeCheck = requireScopeTarget(netlist, request.scope, request.scopeName);
            if (!scopeCheck.ok) return scopeCheck;
            if (!isKnownCombinationalGateType(request.targetGateType)) {
                return failedValidation("ReplaceGateType requires targetGateType.");
            }
            return requireValidGateTypeList(request.allowedTypes, "allowedTypes", false);
        }

        // Internal-only Boolean/driver/net replacement primitives.
        case EditCommandKind::ReplaceGateWithNet:
        case EditCommandKind::ReplaceGateWithNotOfNet: {
            EditRequestValidation gateCheck = requireResolvedGate(netlist, request.gateId, request.gateName, "target");
            if (!gateCheck.ok) return gateCheck;
            return requireResolvedNet(netlist, request.sourceNetId, request.netName, "source");
        }

        case EditCommandKind::ReplaceGateWithConstant:
            if (EditRequestValidation gateCheck = requireResolvedGate(netlist, request.gateId, request.gateName, "target"); !gateCheck.ok) return gateCheck;
            if (!isActiveNetId(netlist, request.sourceNetId) || !netlist.getNet(request.sourceNetId).isConst) {
                return failedValidation("sourceNetId must reference an existing constant net.");
            }
            return okValidation();

        case EditCommandKind::ReplaceDriverOfNet:
            if (EditRequestValidation netCheck = requireNetId(netlist, request.targetNetId, "targetNetId"); !netCheck.ok) return netCheck;
            return requireGateId(netlist, request.newDriverGateId, "newDriverGateId");

        case EditCommandKind::RewireGateOutputToExistingNet:
            if (EditRequestValidation gateCheck = requireResolvedGate(netlist, request.gateId, request.gateName, "target"); !gateCheck.ok) return gateCheck;
            return requireNetId(netlist, request.newOutputNetId, "newOutputNetId");

        case EditCommandKind::ReplaceNetFunctionKeepingName:
            if (EditRequestValidation targetCheck = requireNetId(netlist, request.targetNetId, "targetNetId"); !targetCheck.ok) return targetCheck;
            return requireNetId(netlist, request.sourceNetId, "sourceNetId");

        case EditCommandKind::MergeNetIntoNet:
            if (request.oldNetId == request.newNetId) return failedValidation("oldNetId and newNetId must be different.");
            if (EditRequestValidation oldCheck = requireNetId(netlist, request.oldNetId, "oldNetId"); !oldCheck.ok) return oldCheck;
            return requireNetId(netlist, request.newNetId, "newNetId");

        case EditCommandKind::BypassNetKeepingPortSemantics:
            if (request.netId == request.replacementNetId) return failedValidation("netId and replacementNetId must be different.");
            if (EditRequestValidation removedCheck = requireNetId(netlist, request.netId, "netId"); !removedCheck.ok) return removedCheck;
            return requireNetId(netlist, request.replacementNetId, "replacementNetId");

        case EditCommandKind::RedirectAllLoads:
            if (request.oldNetId == request.newNetId) return failedValidation("oldNetId and newNetId must be different.");
            if (EditRequestValidation oldCheck = requireNetId(netlist, request.oldNetId, "oldNetId"); !oldCheck.ok) return oldCheck;
            return requireNetId(netlist, request.newNetId, "newNetId");

        case EditCommandKind::RemoveNetIfUnused:
            return requireResolvedNet(netlist, request.netId, request.netName, "target");

        default:
            return failedValidation("Unsupported edit command kind.");
    }
}

NetlistEditReport makeFailedEditApplyReport(
    const Netlist& netlist,
    EditCommandKind kind,
    const std::string& message)
{
    NetlistEditReport report;
    report.operationKind = isTechnologyMappingCommand(kind)
        ? NetlistEditOperationKind::TechnologyMapping
        : NetlistEditOperationKind::PrimitiveMutation;
    report.operationName = "edit_apply:" + editCommandKindName(kind);
    report.message = message;
    report.beforeStats = netlist.collectNetlistStats();
    report.afterStats = report.beforeStats;
    report.diff = Netlist::diffStats(report.beforeStats, report.afterStats);
    report.validation = Netlist::validateEditResult(netlist, netlist);
    report.success = false;
    report.changed = false;
    report.rolledBack = false;
    report.validation.messages.push_back(message);
    return report;
}

void finalizeEditApplyReport(NetlistEditReport& report, const EditApplyRequest& request) {
    report.operationName = "edit_apply:" + editCommandKindName(request.kind);

    if (request.validateEquivalence && !report.validation.equivalenceChecked) {
        report.addWarning("validateEquivalence requested, but this edit command has no equivalence certificate yet.");
    }
    if (!request.rollbackOnFailure) {
        report.addWarning("rollbackOnFailure=false is recorded, but current WithReport wrappers rollback on validation failure.");
    }
}

} // namespace

NetlistEditReport Netlist::runEditApply(const EditApplyRequest& request) {
    NetlistEditReport report;
    EditRequestValidation requestValidation = validateEditApplyRequest(*this, request);
    if (!requestValidation.ok) {
        return makeFailedEditApplyReport(*this, request.kind, requestValidation.message);
    }

    switch (request.kind) {
        // Rename commands preserve structure and only update names.
        case EditCommandKind::RenameGate:
            report = renameGateWithReport(request.oldName, request.newName);
            break;
        case EditCommandKind::RenameNet:
            report = renameNetWithReport(request.oldName, request.newName);
            break;
        case EditCommandKind::DisconnectGateInput:
            report = disconnectGateInputWithReport(request.gateName, request.netName);
            break;
        case EditCommandKind::ConnectGateInput:
            report = connectGateInputWithReport(request.gateName, request.netName, request.pinIndex);
            break;
        case EditCommandKind::RemoveGate:
            report = removeGateWithReport(resolveGateId(*this, request.gateId, request.gateName));
            break;
        case EditCommandKind::ReplaceAllLoadsOfNet:
            report = replaceAllLoadsOfNetWithReport(request.oldNetId, request.newNetId);
            break;

        // Cleanup and local simplification commands.
        case EditCommandKind::CleanupBuffers:
            report = cleanupAllRemovableBuffersWithReport();
            break;
        case EditCommandKind::CollapseDoubleInverter:
            report = collapseBackToBackInvertersWithReport();
            break;
        case EditCommandKind::LocalSimplificationFixpoint:
            report = runLocalSimplificationFixpointWithReport();
            break;
        case EditCommandKind::SafeCleanupFixpoint:
            report = runSafeCleanupFixpointWithReport();
            break;
        case EditCommandKind::TrimDeadLogic:
            report = trimDeadLogicWithReport();
            break;
        case EditCommandKind::RemoveDanglingLogic:
            report = removeDanglingLogicWithReport();
            break;
        case EditCommandKind::RemoveUnusedNets:
            report = removeUnusedNetsWithReport();
            break;
        case EditCommandKind::MergeEquivalentGates:
            report = mergeEquivalentGatesWithReport();
            break;
        case EditCommandKind::MergeStructurallyEquivalentGates:
            report = mergeStructurallyEquivalentGatesWithReport();
            break;
        case EditCommandKind::MergeFunctionallyEquivalentGates:
            report = mergeFunctionallyEquivalentGatesWithReport(
                request.scope,
                request.scopeName,
                request.gateType,
                request.simulationPatternCount,
                request.timeLimitSeconds);
            break;
        case EditCommandKind::SimplifyConstants:
            report = simplifyGatesWithConstantsWithReport(
                request.gateType,
                request.constValue,
                request.inputCount);
            break;
        case EditCommandKind::SimplifySameInput:
            report = simplifyAllSameInputGatesWithReport();
            break;

        // Buffer insertion commands.
        case EditCommandKind::InsertBuffersForFanout:
            report = insertBuffersForFanoutWithReport(fanoutLimitOrDefault(request.maxFanout));
            break;
        case EditCommandKind::InsertBuffersForSpecificNet:
            report = insertBuffersForSpecificNetWithReport(request.netName, fanoutLimitOrDefault(request.maxFanout));
            break;
        case EditCommandKind::InsertBuffersForDffControl:
            report = insertBuffersForDffControlWithReport(
                fanoutLimitOrDefault(request.maxFanout),
                request.processClock,
                request.processReset);
            break;
        case EditCommandKind::InsertBuffersOnEachLoad:
            report = insertBuffersOnEachLoadWithReport(request.netName);
            break;
        case EditCommandKind::InsertBufferAtDriver:
            report = insertBufferAtDriverWithReport(request.netName);
            break;
        case EditCommandKind::InsertBufferBeforeGate:
            report = insertBufferBeforeGateWithReport(request.netName, request.targetGateName);
            break;
        case EditCommandKind::InsertBuffersByGateType:
            report = insertBuffersByGateTypeWithReport(
                request.gateType,
                request.bufferInputs,
                request.bufferOutputs);
            break;

        // Technology mapping commands.
        case EditCommandKind::ConvertToBasis: {
            TechMapper mapper;
            report = mapper.convertToBasisWithReport(
                *this,
                request.scope,
                request.scopeName,
                request.allowedTypes,
                request.bannedTypes,
                request.verbose);
            break;
        }
        case EditCommandKind::ReplaceGateType: {
            TechMapper mapper;
            report = mapper.replaceGateTypeWithReport(
                *this,
                request.targetGateType,
                request.allowedTypes,
                request.scope,
                request.scopeName,
                request.verbose);
            break;
        }

        // Internal-only primitives should be rejected during validation.
        case EditCommandKind::ReplaceGateWithNet:
            report = replaceGateWithNetWithReport(
                resolveGateId(*this, request.gateId, request.gateName),
                resolveNetId(*this, request.sourceNetId, request.netName));
            break;
        case EditCommandKind::ReplaceGateWithConstant:
            report = replaceGateWithConstantWithReport(
                resolveGateId(*this, request.gateId, request.gateName),
                request.sourceNetId);
            break;
        case EditCommandKind::ReplaceGateWithNotOfNet:
            report = replaceGateWithNotOfNetWithReport(
                resolveGateId(*this, request.gateId, request.gateName),
                resolveNetId(*this, request.sourceNetId, request.netName));
            break;
        case EditCommandKind::ReplaceDriverOfNet:
            report = replaceDriverOfNetWithReport(request.targetNetId, request.newDriverGateId);
            break;
        case EditCommandKind::RewireGateOutputToExistingNet:
            report = rewireGateOutputToExistingNetWithReport(
                resolveGateId(*this, request.gateId, request.gateName),
                request.newOutputNetId);
            break;
        case EditCommandKind::ReplaceNetFunctionKeepingName:
            report = replaceNetFunctionWithNetKeepingNameWithReport(request.targetNetId, request.sourceNetId);
            break;
        case EditCommandKind::MergeNetIntoNet:
            report = mergeNetIntoNetWithReport(request.oldNetId, request.newNetId);
            break;
        case EditCommandKind::BypassNetKeepingPortSemantics:
            report = bypassNetKeepingPortSemanticsWithReport(request.netId, request.replacementNetId);
            break;
        case EditCommandKind::RedirectAllLoads:
            report = redirectAllLoadsWithReport(request.oldNetId, request.newNetId, request.allowDuplicateLoads);
            break;
        case EditCommandKind::RemoveNetIfUnused:
            report = removeNetIfUnusedWithReport(resolveNetId(*this, request.netId, request.netName));
            break;

        default:
            return makeFailedEditApplyReport(*this, request.kind, "Unsupported edit command kind.");
    }

    finalizeEditApplyReport(report, request);
    return report;
}
