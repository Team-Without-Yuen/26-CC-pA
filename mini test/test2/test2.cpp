#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct TestReport {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name) {
        if (condition) {
            ++passed;
            std::cout << "[PASS] " << name << "\n";
        } else {
            ++failed;
            std::cout << "[FAIL] " << name << "\n";
        }
    }
};

int mapCount(const std::map<GateType, int>& values, GateType type) {
    const auto it = values.find(type);
    return it == values.end() ? 0 : it->second;
}

bool containsString(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

bool containsPrefix(const std::vector<std::string>& values, const std::string& prefix) {
    for (const std::string& value : values) {
        if (value.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool containsWarningText(const std::vector<std::string>& warnings, const std::string& text) {
    for (const std::string& warning : warnings) {
        if (warning.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasActiveGate(const Netlist& netlist, const std::string& gateName) {
    const int gateId = netlist.getGateId(gateName);
    return gateId >= 0 && !netlist.isGateRemoved(gateId);
}

bool hasActiveNet(const Netlist& netlist, const std::string& netName) {
    const int netId = netlist.getNetId(netName);
    return netId >= 0 && !netlist.getNet(netId).isRemoved;
}

bool isPoNet(const Netlist& netlist, const std::string& netName) {
    const int netId = netlist.getNetId(netName);
    return netId >= 0 && netlist.getNet(netId).isPO;
}

bool loadCircuit(const std::string& path, Netlist& netlist) {
    VerilogReader reader;
    return reader.read(path, netlist);
}

Netlist makeSimpleAndNetlist() {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryOutput("y");

    const int a = netlist.getNetId("a");
    const int b = netlist.getNetId("b");
    const int y = netlist.getNetId("y");
    const int g = netlist.addGate("g_and", GateType::AND);
    netlist.connectGateInput(g, a);
    netlist.connectGateInput(g, b);
    netlist.connectGateOutput(g, y);
    return netlist;
}

void testRenameNet(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::RenameNet;
    request.oldName = "y";
    request.newName = "renamed_y";
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:rename_net" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 netlist.getNetId("renamed_y") >= 0 &&
                 netlist.getNetId("y") == -1 &&
                 containsString(editReport.changedNetNames, "y") &&
                 containsString(editReport.changedNetNames, "renamed_y") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply rename_net");
}

void testRenameGate(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::RenameGate;
    request.oldName = "g_keep";
    request.newName = "g_keep_renamed";
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:rename_gate" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 netlist.getGateId("g_keep") == -1 &&
                 hasActiveGate(netlist, "g_keep_renamed") &&
                 containsString(editReport.changedGateNames, "g_keep") &&
                 containsString(editReport.changedGateNames, "g_keep_renamed") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply rename_gate");
}

void testCleanupBuffers(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::CleanupBuffers;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:cleanup_buffers" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 !hasActiveGate(netlist, "g_buf0") &&
                 !hasActiveGate(netlist, "g_aux_buf") &&
                 hasActiveGate(netlist, "g_buf1") &&
                 hasActiveGate(netlist, "g_keep") &&
                 isPoNet(netlist, "y") &&
                 isPoNet(netlist, "keep") &&
                 editReport.diff.activeGateCountDelta <= -2 &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply cleanup_buffers");
}

void testCollapseDoubleInverter(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::CollapseDoubleInverter;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:collapse_double_inverter" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 editReport.diff.activeGateCountDelta < 0 &&
                 !hasActiveGate(netlist, "g_inv0") &&
                 !hasActiveGate(netlist, "g_inv1") &&
                 hasActiveGate(netlist, "g_buf1") &&
                 isPoNet(netlist, "y") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply collapse_double_inverter");
}

void testSimplifyConstants(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::SimplifyConstants;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:simplify_constants" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 !hasActiveGate(netlist, "g_const_and") &&
                 hasActiveGate(netlist, "g_const_buf") &&
                 isPoNet(netlist, "const_y") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply simplify_constants");
}

void testSimplifySameInput(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::SimplifySameInput;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:simplify_same_input" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 !hasActiveGate(netlist, "g_same_or") &&
                 hasActiveGate(netlist, "g_same_buf") &&
                 isPoNet(netlist, "same_y") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply simplify_same_input");
}

void testMergeStructurallyEquivalentGates(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::MergeStructurallyEquivalentGates;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:merge_structurally_equivalent_gates" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 !hasActiveGate(netlist, "g_live_and1_dup") &&
                 !hasActiveGate(netlist, "g_live_cd1_dup") &&
                 hasActiveGate(netlist, "g_live_and0") &&
                 hasActiveGate(netlist, "g_live_cd0") &&
                 isPoNet(netlist, "y") &&
                 isPoNet(netlist, "aux") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply merge_structurally_equivalent_gates");
}

void testTrimDeadLogic(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::TrimDeadLogic;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:trim_dead_logic" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 !hasActiveGate(netlist, "g_dead_and") &&
                 !hasActiveGate(netlist, "g_dead_not") &&
                 !hasActiveGate(netlist, "g_dead_nand") &&
                 !hasActiveGate(netlist, "g_dead_nor") &&
                 !hasActiveGate(netlist, "g_floating_buf") &&
                 hasActiveGate(netlist, "g_live_and1_dup") &&
                 isPoNet(netlist, "y") &&
                 isPoNet(netlist, "aux") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply trim_dead_logic");
}

void testRemoveDanglingLogic(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::RemoveDanglingLogic;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:remove_dangling_logic" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 !hasActiveGate(netlist, "g_dead_and") &&
                 !hasActiveGate(netlist, "g_dead_not") &&
                 !hasActiveGate(netlist, "g_dead_nand") &&
                 !hasActiveGate(netlist, "g_dead_nor") &&
                 !hasActiveGate(netlist, "g_floating_buf") &&
                 hasActiveGate(netlist, "g_keep") &&
                 isPoNet(netlist, "keep") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply remove_dangling_logic");
}

void testSafeCleanupFixpointChanges(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();
    const NetlistStats beforeStats = netlist.collectNetlistStats();

    report.check(beforeStats.activeGateCount == 22 &&
                 beforeStats.primaryInputCount == 5 &&
                 beforeStats.primaryOutputCount == 5 &&
                 hasActiveGate(netlist, "g_live_and1_dup") &&
                 hasActiveGate(netlist, "g_const_and") &&
                 hasActiveGate(netlist, "g_dead_nor") &&
                 hasActiveGate(netlist, "g_floating_buf") &&
                 hasActiveNet(netlist, "y") &&
                 hasActiveNet(netlist, "keep") &&
                 hasActiveNet(netlist, "aux"),
                 "test2 edit_circuit has expected edit opportunities");

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::SafeCleanupFixpoint;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:safe_cleanup_fixpoint" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 editReport.diff.activeGateCountDelta < 0 &&
                 editReport.diff.activeNetCountDelta < 0 &&
                 editReport.beforeStats.activeGateCount == beforeStats.activeGateCount &&
                 editReport.afterStats.activeGateCount < editReport.beforeStats.activeGateCount &&
                 !hasActiveGate(netlist, "g_live_and1_dup") &&
                 !hasActiveGate(netlist, "g_same_or") &&
                 !hasActiveGate(netlist, "g_const_and") &&
                 !hasActiveGate(netlist, "g_dead_and") &&
                 !hasActiveGate(netlist, "g_dead_not") &&
                 !hasActiveGate(netlist, "g_dead_nand") &&
                 !hasActiveGate(netlist, "g_dead_nor") &&
                 !hasActiveGate(netlist, "g_floating_buf") &&
                 hasActiveGate(netlist, "g_keep") &&
                 isPoNet(netlist, "y") &&
                 isPoNet(netlist, "keep") &&
                 isPoNet(netlist, "aux") &&
                 isPoNet(netlist, "same_y") &&
                 isPoNet(netlist, "const_y") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply safe_cleanup_fixpoint complex cleanup");
}

void testSafeCleanupFixpointNoop(TestReport& report) {
    Netlist netlist = makeSimpleAndNetlist();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::SafeCleanupFixpoint;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 !editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:safe_cleanup_fixpoint" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 containsWarningText(editReport.warnings, "No safe cleanup opportunities") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply safe_cleanup_fixpoint noop");
}

void testLocalSimplificationFixpoint(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::LocalSimplificationFixpoint;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:local_simplification_fixpoint" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 !hasActiveGate(netlist, "g_inv0") &&
                 !hasActiveGate(netlist, "g_inv1") &&
                 !hasActiveGate(netlist, "g_same_or") &&
                 !hasActiveGate(netlist, "g_const_and") &&
                 hasActiveGate(netlist, "g_keep") &&
                 isPoNet(netlist, "y") &&
                 isPoNet(netlist, "same_y") &&
                 isPoNet(netlist, "const_y") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply local_simplification_fixpoint");
}

void testMergeEquivalentGates(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::MergeEquivalentGates;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(!editReport.success &&
                 !editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:merge_equivalent_gates" &&
                 editReport.message.find("legacy internal structural-merge alias") != std::string::npos &&
                 !editReport.validation.equivalenceChecked &&
                 !editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::NotChecked &&
                 hasActiveGate(netlist, "g_live_and1_dup") &&
                 hasActiveGate(netlist, "g_live_cd1_dup") &&
                 hasActiveGate(netlist, "g_live_and0") &&
                 hasActiveGate(netlist, "g_live_cd0") &&
                 netlist.validateAfterMutation(),
                 "test2 rejects legacy merge_equivalent_gates high-level request");
}

void testRemoveUnusedNets(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::RemoveUnusedNets;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:remove_unused_nets" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 editReport.diff.activeNetCountDelta < 0 &&
                 !hasActiveNet(netlist, "dead_out") &&
                 !hasActiveNet(netlist, "floating_buf_out") &&
                 hasActiveNet(netlist, "y") &&
                 hasActiveNet(netlist, "aux") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply remove_unused_nets");
}

void testRemoveNetIfUnused(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();
    const int manualUnused = netlist.addNet("manual_unused_net");

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::RemoveNetIfUnused;
    request.netId = manualUnused;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:remove_net_if_unused" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::StructuralIdentity &&
                 netlist.getNet(manualUnused).isRemoved &&
                 containsString(editReport.changedNetNames, "manual_unused_net") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply remove_net_if_unused");
}

void testInsertBuffersForSpecificNet(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();
    const GlobalFanoutReport before = netlist.getGlobalFanoutReport(3);

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::InsertBuffersForSpecificNet;
    request.netName = "src";
    request.maxFanout = 3;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:insert_buffers_for_specific_net" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 editReport.fanoutChange.has_value() &&
                 editReport.fanoutChange->beforeMaxFanout == static_cast<int>(before.maxFanout) &&
                 editReport.fanoutChange->afterMaxFanout <= 3 &&
                 editReport.fanoutChange->meetsConstraint &&
                 !editReport.changedGateNames.empty() &&
                 netlist.satisfiesFanoutLimit(3) &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply insert_buffers_for_specific_net");
}

void testInsertBuffersForFanout(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::InsertBuffersForFanout;
    request.maxFanout = 4;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:insert_buffers_for_fanout" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 editReport.fanoutChange.has_value() &&
                 editReport.fanoutChange->beforeMaxFanout > 4 &&
                 editReport.fanoutChange->afterMaxFanout <= 4 &&
                 editReport.fanoutChange->meetsConstraint &&
                 netlist.satisfiesFanoutLimit(4) &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply insert_buffers_for_fanout");
}

void testInsertBuffersOnEachLoad(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();
    const int srcNetId = netlist.getNetId("src");
    const size_t beforeLoads = srcNetId >= 0 ? netlist.getNet(srcNetId).loadGateIds.size() : 0;

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::InsertBuffersOnEachLoad;
    request.netName = "src";
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    bool sourceLoadsAreBuffers = srcNetId >= 0;
    if (sourceLoadsAreBuffers) {
        for (int loadGateId : netlist.getNet(srcNetId).loadGateIds) {
            sourceLoadsAreBuffers =
                sourceLoadsAreBuffers &&
                netlist.isValidGateId(loadGateId) &&
                netlist.getGate(loadGateId).type == GateType::BUF;
        }
    }

    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:insert_buffers_on_each_load" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 beforeLoads == 8 &&
                 editReport.changedGateNames.size() == beforeLoads &&
                 netlist.getNet(srcNetId).loadGateIds.size() == beforeLoads &&
                 sourceLoadsAreBuffers &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply insert_buffers_on_each_load");
}

void testInsertBufferAtDriver(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();
    const int beforeYDriver = netlist.getNet(netlist.getNetId("y0")).driverGateId;

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::InsertBufferAtDriver;
    request.netName = "y0";
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    const int afterYDriver = netlist.getNet(netlist.getNetId("y0")).driverGateId;
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:insert_buffer_at_driver" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 beforeYDriver >= 0 &&
                 afterYDriver >= 0 &&
                 afterYDriver != beforeYDriver &&
                 netlist.getGate(afterYDriver).type == GateType::BUF &&
                 editReport.changedGateNames.size() == 1 &&
                 isPoNet(netlist, "y0") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply insert_buffer_at_driver");
}

void testInsertBufferBeforeGate(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();
    const int srcNetId = netlist.getNetId("src");
    const int g0Id = netlist.getGateId("g0");

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::InsertBufferBeforeGate;
    request.netName = "src";
    request.targetGateName = "g0";
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    bool g0NoLongerReadsSrc = true;
    for (int inputNetId : netlist.getGate(g0Id).inputNetIds) {
        if (inputNetId == srcNetId) {
            g0NoLongerReadsSrc = false;
        }
    }

    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:insert_buffer_before_gate" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 g0Id >= 0 &&
                 g0NoLongerReadsSrc &&
                 editReport.changedGateNames.size() == 1 &&
                 containsString(editReport.changedGateNames, "src_to_g0_buf") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply insert_buffer_before_gate");
}

void testInsertBuffersByGateType(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();
    const size_t beforeBuf = netlist.getGateCountByType(GateType::BUF);

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::InsertBuffersByGateType;
    request.gateType = GateType::AND;
    request.bufferInputs = true;
    request.bufferOutputs = true;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:insert_buffers_by_gate_type" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 netlist.getGateCountByType(GateType::BUF) > beforeBuf &&
                 !editReport.changedGateNames.empty() &&
                 hasActiveGate(netlist, "g0_inbuf_0") &&
                 hasActiveGate(netlist, "g0_inbuf_1") &&
                 hasActiveGate(netlist, "g0_outbuf") &&
                 isPoNet(netlist, "y0") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply insert_buffers_by_gate_type");
}

void testInsertBuffersForDffControl(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();
    const size_t beforeBuf = netlist.getGateCountByType(GateType::BUF);

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::InsertBuffersForDffControl;
    request.maxFanout = 2;
    request.processClock = true;
    request.processReset = true;
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:insert_buffers_for_dff_control" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 netlist.getGateCountByType(GateType::BUF) > beforeBuf &&
                 !editReport.changedGateNames.empty() &&
                 containsPrefix(editReport.changedGateNames, "dff_ctrl_buf_") &&
                 isPoNet(netlist, "q0") &&
                 isPoNet(netlist, "q4") &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply insert_buffers_for_dff_control");
}

void testReplaceGateType(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();
    const size_t beforeXor = netlist.getGateCountByType(GateType::XOR);

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::ReplaceGateType;
    request.targetGateType = GateType::XOR;
    request.allowedTypes = {GateType::NAND};
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:replace_type" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 editReport.mappingDelta.has_value() &&
                 beforeXor > 0 &&
                 netlist.getGateCountByType(GateType::XOR) == 0 &&
                 mapCount(editReport.mappingDelta->removedCountByType, GateType::XOR) >= static_cast<int>(beforeXor) &&
                 mapCount(editReport.mappingDelta->addedCountByType, GateType::NAND) > 0 &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply replace_gate_type");
}

void testConvertToBasis(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();
    const size_t beforeOr = netlist.getGateCountByType(GateType::OR);

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::ConvertToBasis;
    request.allowedTypes = {GateType::AND, GateType::NOT};
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:convert_basis" &&
                 editReport.validation.equivalenceChecked &&
                 editReport.validation.functionallyEquivalent &&
                 editReport.validation.equivalenceMethod == Netlist::EquivalenceCheckMethod::LocalRewriteRule &&
                 editReport.mappingDelta.has_value() &&
                 beforeOr > 0 &&
                 netlist.getGateCountByType(GateType::OR) == 0 &&
                 netlist.getGateCountByType(GateType::NAND) == 0 &&
                 netlist.getGateCountByType(GateType::NOR) == 0 &&
                 netlist.getGateCountByType(GateType::XOR) == 0 &&
                 netlist.getGateCountByType(GateType::XNOR) == 0 &&
                 mapCount(editReport.mappingDelta->finalGateCountByType, GateType::AND) > 0 &&
                 mapCount(editReport.mappingDelta->finalGateCountByType, GateType::NOT) > 0 &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply convert_to_basis");
}

void testInternalPrimitiveBlocked(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::ReplaceGateWithNet;
    request.gateName = "g_live_and0";
    request.netName = "a";

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(!editReport.success &&
                 !editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:replace_gate_with_net" &&
                 editReport.message == "Command is an internal low-level primitive and is not exposed through EditApply because functional equivalence is not guaranteed." &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply blocks internal primitive");
}

void testMissingArgumentValidation(TestReport& report, const Netlist& original) {
    Netlist netlist = original.cloneForRollback();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::RenameNet;
    request.oldName = "y";

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    report.check(!editReport.success &&
                 !editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.operationName == "edit_apply:rename_net" &&
                 editReport.message == "Missing required argument: newName." &&
                 netlist.getNetId("y") >= 0 &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply validation missing newName");
}

} // namespace

int main() {
    TestReport report;

    Netlist editCircuit;
    Netlist fanoutCircuit;
    Netlist techmapCircuit;
    Netlist dffControlCircuit;

    report.check(loadCircuit("mini test/test2/edit_circuit.v", editCircuit), "test2 load edit_circuit.v");
    report.check(loadCircuit("mini test/test2/fanout_circuit.v", fanoutCircuit), "test2 load fanout_circuit.v");
    report.check(loadCircuit("mini test/test2/techmap_circuit.v", techmapCircuit), "test2 load techmap_circuit.v");
    report.check(loadCircuit("mini test/test2/dff_control_circuit.v", dffControlCircuit), "test2 load dff_control_circuit.v");
    if (report.failed != 0) {
        std::cout << "\nCannot continue because one or more test circuits could not be read.\n";
        return report.failed;
    }

    testRenameNet(report, editCircuit);
    testRenameGate(report, editCircuit);
    testCleanupBuffers(report, editCircuit);
    testCollapseDoubleInverter(report, editCircuit);
    testSimplifyConstants(report, editCircuit);
    testSimplifySameInput(report, editCircuit);
    testLocalSimplificationFixpoint(report, editCircuit);
    testMergeEquivalentGates(report, editCircuit);
    testMergeStructurallyEquivalentGates(report, editCircuit);
    testTrimDeadLogic(report, editCircuit);
    testRemoveDanglingLogic(report, editCircuit);
    testRemoveUnusedNets(report, editCircuit);
    testRemoveNetIfUnused(report, editCircuit);
    testSafeCleanupFixpointChanges(report, editCircuit);
    testSafeCleanupFixpointNoop(report);
    testInsertBuffersForSpecificNet(report, fanoutCircuit);
    testInsertBuffersForFanout(report, fanoutCircuit);
    testInsertBuffersOnEachLoad(report, fanoutCircuit);
    testInsertBufferAtDriver(report, fanoutCircuit);
    testInsertBufferBeforeGate(report, fanoutCircuit);
    testInsertBuffersByGateType(report, fanoutCircuit);
    testInsertBuffersForDffControl(report, dffControlCircuit);
    testReplaceGateType(report, techmapCircuit);
    testConvertToBasis(report, techmapCircuit);
    testInternalPrimitiveBlocked(report, editCircuit);
    testMissingArgumentValidation(report, editCircuit);

    std::cout << "\nSummary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
