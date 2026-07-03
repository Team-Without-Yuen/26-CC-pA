#include "include/core/Netlist.h"

namespace {

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
        case EditCommandKind::TrimDeadLogic: return "trim_dead_logic";
        case EditCommandKind::RemoveDanglingLogic: return "remove_dangling_logic";
        case EditCommandKind::RemoveUnusedNets: return "remove_unused_nets";
        case EditCommandKind::MergeEquivalentGates: return "merge_equivalent_gates";
        case EditCommandKind::MergeStructurallyEquivalentGates: return "merge_structurally_equivalent_gates";
        case EditCommandKind::SimplifyConstants: return "simplify_constants";
        case EditCommandKind::SimplifySameInput: return "simplify_same_input";
        case EditCommandKind::InsertBuffersForFanout: return "insert_buffers_for_fanout";
        case EditCommandKind::InsertBuffersForSpecificNet: return "insert_buffers_for_specific_net";
        case EditCommandKind::InsertBuffersForDffControl: return "insert_buffers_for_dff_control";
        case EditCommandKind::InsertBuffersOnEachLoad: return "insert_buffers_on_each_load";
        case EditCommandKind::InsertBufferAtDriver: return "insert_buffer_at_driver";
        case EditCommandKind::InsertBufferBeforeGate: return "insert_buffer_before_gate";
        case EditCommandKind::InsertBuffersByGateType: return "insert_buffers_by_gate_type";
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

NetlistEditReport makeFailedEditApplyReport(
    const Netlist& netlist,
    EditCommandKind kind,
    const std::string& message)
{
    NetlistEditReport report;
    report.operationKind = NetlistEditOperationKind::PrimitiveMutation;
    report.operationName = "edit_apply:" + editCommandKindName(kind);
    report.message = message;
    report.beforeStats = netlist.collectNetlistStats();
    report.afterStats = report.beforeStats;
    report.diff = Netlist::diffStats(report.beforeStats, report.afterStats);
    report.validation = Netlist::validateEditResult(netlist, netlist);
    report.success = false;
    report.changed = false;
    report.rolledBack = false;
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

    switch (request.kind) {
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

        case EditCommandKind::CleanupBuffers:
            report = cleanupAllRemovableBuffersWithReport();
            break;
        case EditCommandKind::CollapseDoubleInverter:
            report = collapseBackToBackInvertersWithReport();
            break;
        case EditCommandKind::LocalSimplificationFixpoint:
            report = runLocalSimplificationFixpointWithReport();
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
        case EditCommandKind::SimplifyConstants:
            report = simplifyAllGatesWithConstantsWithReport();
            break;
        case EditCommandKind::SimplifySameInput:
            report = simplifyAllSameInputGatesWithReport();
            break;

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
