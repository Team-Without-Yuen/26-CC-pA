#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
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

void testCollapseDoubleInverterSafety(TestReport& report) {
    auto makeInternalPair = []() {
        Netlist netlist;
        netlist.addPrimaryInput("a");
        netlist.addPrimaryInput("other");
        netlist.addPrimaryOutput("y");
        const int a = netlist.getNetId("a");
        const int mid = netlist.addNet("mid");
        const int out = netlist.addNet("out");
        const int g1 = netlist.addGate("g1", GateType::NOT);
        const int g2 = netlist.addGate("g2", GateType::NOT);
        const int sink = netlist.addGate("sink", GateType::BUF);
        netlist.connectGateInput(g1, a);
        netlist.connectGateOutput(g1, mid);
        netlist.connectGateInput(g2, mid);
        netlist.connectGateOutput(g2, out);
        netlist.connectGateInput(sink, out);
        netlist.connectGateOutput(sink, netlist.getNetId("y"));
        return netlist;
    };

    Netlist poPair;
    poPair.addPrimaryInput("a");
    poPair.addPrimaryOutput("y");
    const int poG1 = poPair.addGate("po_g1", GateType::NOT);
    const int poG2 = poPair.addGate("po_g2", GateType::NOT);
    const int poMid = poPair.addNet("po_mid");
    poPair.connectGateInput(poG1, poPair.getNetId("a"));
    poPair.connectGateOutput(poG1, poMid);
    poPair.connectGateInput(poG2, poMid);
    poPair.connectGateOutput(poG2, poPair.getNetId("y"));
    const bool poGuard = poPair.findDoubleInverterPairs().empty() &&
                         !poPair.bypassDoubleInverter(poG1, poG2) &&
                         poPair.collapseBackToBackInverters() == 0 &&
                         poPair.getGate(poG1).type == GateType::NOT &&
                         poPair.getGate(poG2).type == GateType::NOT &&
                         poPair.validateAfterMutation();

    Netlist staleDriver = makeInternalPair();
    const int staleDriverG1 = staleDriver.getGateId("g1");
    const int staleDriverG2 = staleDriver.getGateId("g2");
    const int staleDriverMid = staleDriver.getNetId("mid");
    staleDriver.getNetMutable(staleDriverMid).driverGateId = -1;
    const bool rejectsStaleDriver =
        staleDriver.findDoubleInverterPairs().empty() &&
        !staleDriver.bypassDoubleInverter(staleDriverG1, staleDriverG2) &&
        staleDriver.collapseBackToBackInverters() == 0 &&
        staleDriver.getGate(staleDriverG1).type == GateType::NOT &&
        staleDriver.getGate(staleDriverG2).type == GateType::NOT;

    Netlist staleLoad = makeInternalPair();
    const int staleLoadG1 = staleLoad.getGateId("g1");
    const int staleLoadG2 = staleLoad.getGateId("g2");
    const int staleLoadMid = staleLoad.getNetId("mid");
    const int unrelatedOut = staleLoad.addNet("unrelated_out");
    const int unrelated = staleLoad.addGate("unrelated", GateType::NOT);
    staleLoad.connectGateInput(unrelated, staleLoad.getNetId("other"));
    staleLoad.connectGateOutput(unrelated, unrelatedOut);
    staleLoad.getNetMutable(staleLoadMid).loadGateIds[0] = unrelated;
    const bool rejectsStaleLoad =
        staleLoad.findDoubleInverterPairs().empty() &&
        staleLoad.collapseBackToBackInverters() == 0 &&
        staleLoad.getGate(staleLoadG1).type == GateType::NOT &&
        staleLoad.getGate(staleLoadG2).type == GateType::NOT &&
        staleLoad.getGate(unrelated).type == GateType::NOT;

    Netlist invalidIds = makeInternalPair();
    const int invalidG1 = invalidIds.getGateId("g1");
    const int originalOutput = invalidIds.getGate(invalidG1).outputNetId;
    invalidIds.getGateMutable(invalidG1).outputNetId =
        static_cast<int>(invalidIds.getNetCount()) + 7;
    const bool rejectsInvalidOutput =
        invalidIds.findDoubleInverterPairs().empty() &&
        invalidIds.collapseBackToBackInverters() == 0 &&
        invalidIds.getGate(invalidG1).type == GateType::NOT;
    invalidIds.getGateMutable(invalidG1).outputNetId = originalOutput;

    Netlist invalidLoad = makeInternalPair();
    const int invalidLoadG1 = invalidLoad.getGateId("g1");
    const int invalidLoadG2 = invalidLoad.getGateId("g2");
    const int invalidMid = invalidLoad.getNetId("mid");
    invalidLoad.getNetMutable(invalidMid).loadGateIds[0] =
        static_cast<int>(invalidLoad.getGateCount()) + 9;
    const bool rejectsInvalidLoad =
        invalidLoad.findDoubleInverterPairs().empty() &&
        invalidLoad.collapseBackToBackInverters() == 0 &&
        invalidLoad.getGate(invalidLoadG1).type == GateType::NOT &&
        invalidLoad.getGate(invalidLoadG2).type == GateType::NOT;

    Netlist tombstone = makeInternalPair();
    const int tombstoneG1 = tombstone.getGateId("g1");
    const int tombstoneG2 = tombstone.getGateId("g2");
    tombstone.getGateMutable(tombstoneG2).type = GateType::UNKNOWN;
    const bool rejectsTombstone =
        tombstone.findDoubleInverterPairs().empty() &&
        tombstone.collapseBackToBackInverters() == 0 &&
        tombstone.getGate(tombstoneG1).type == GateType::NOT &&
        tombstone.getGate(tombstoneG2).type == GateType::UNKNOWN;

    report.check(poGuard &&
                 rejectsStaleDriver &&
                 rejectsStaleLoad &&
                 rejectsInvalidOutput &&
                 rejectsInvalidLoad &&
                 rejectsTombstone,
                 "test2 double inverter rejects PO/stale/tombstone/invalid candidates");
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

void testStructuralMergeSafetyAndScope(TestReport& report) {
    Netlist scoped;
    scoped.addPrimaryInput("a");
    scoped.addPrimaryInput("b");
    scoped.addPrimaryInput("c");
    scoped.addPrimaryOutput("y");
    const int a = scoped.getNetId("a");
    const int b = scoped.getNetId("b");
    const int c = scoped.getNetId("c");
    const int keepOut = scoped.addNet("keep_out");
    const int deadOut = scoped.addNet("dead_out");
    const int keep = scoped.addGate("keep", GateType::AND);
    const int dead = scoped.addGate("dead", GateType::AND);
    scoped.connectGateInput(keep, a);
    scoped.connectGateInput(keep, b);
    scoped.connectGateOutput(keep, keepOut);
    scoped.connectGateInput(dead, a);
    scoped.connectGateInput(dead, b);
    scoped.connectGateOutput(dead, deadOut);
    const int sink = scoped.addGate("sink", GateType::OR);
    scoped.connectGateInput(sink, deadOut);
    scoped.connectGateInput(sink, deadOut);
    scoped.connectGateOutput(sink, scoped.getNetId("y"));
    const int danglingOut = scoped.addNet("dangling_out");
    const int dangling = scoped.addGate("unrelated_dangling", GateType::NOT);
    scoped.connectGateInput(dangling, c);
    scoped.connectGateOutput(dangling, danglingOut);

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::MergeStructurallyEquivalentGates;
    request.validateEquivalence = true;
    const Netlist::NetlistEditReport scopedReport = scoped.runEditApply(request);
    const Gate& rewiredSink = scoped.getGate(sink);
    const bool scopedMerge = scopedReport.success &&
                             scopedReport.changed &&
                             !scopedReport.rolledBack &&
                             scopedReport.diff.activeGateCountDelta == -1 &&
                             scoped.getGate(dead).type == GateType::UNKNOWN &&
                             scoped.getGate(dangling).type == GateType::NOT &&
                             rewiredSink.inputNetIds.size() == 2 &&
                             rewiredSink.inputNetIds[0] == keepOut &&
                             rewiredSink.inputNetIds[1] == keepOut &&
                             scoped.getNet(keepOut).loadGateIds.size() == 2 &&
                             scoped.validateAfterMutation();

    Netlist poPair;
    poPair.addPrimaryInput("a");
    poPair.addPrimaryInput("b");
    poPair.addPrimaryOutput("y0");
    poPair.addPrimaryOutput("y1");
    const int poA = poPair.getNetId("a");
    const int poB = poPair.getNetId("b");
    const int po0 = poPair.addGate("po0", GateType::AND);
    const int po1 = poPair.addGate("po1", GateType::AND);
    poPair.connectGateInput(po0, poA);
    poPair.connectGateInput(po0, poB);
    poPair.connectGateOutput(po0, poPair.getNetId("y0"));
    poPair.connectGateInput(po1, poA);
    poPair.connectGateInput(po1, poB);
    poPair.connectGateOutput(po1, poPair.getNetId("y1"));
    const Netlist::NetlistEditReport poReport = poPair.runEditApply(request);
    const bool preservesNamedPos = poReport.success &&
                                   !poReport.changed &&
                                   !poReport.rolledBack &&
                                   poPair.getGate(po0).type == GateType::AND &&
                                   poPair.getGate(po1).type == GateType::AND &&
                                   poPair.validateAfterMutation();

    Netlist poRepresentative;
    poRepresentative.addPrimaryInput("a");
    poRepresentative.addPrimaryInput("b");
    poRepresentative.addPrimaryOutput("y");
    const int repA = poRepresentative.getNetId("a");
    const int repB = poRepresentative.getNetId("b");
    const int internalOut = poRepresentative.addNet("internal_out");
    const int internalGate = poRepresentative.addGate("internal", GateType::AND);
    const int poGate = poRepresentative.addGate("po_gate", GateType::AND);
    poRepresentative.connectGateInput(internalGate, repA);
    poRepresentative.connectGateInput(internalGate, repB);
    poRepresentative.connectGateOutput(internalGate, internalOut);
    poRepresentative.connectGateInput(poGate, repA);
    poRepresentative.connectGateInput(poGate, repB);
    poRepresentative.connectGateOutput(poGate, poRepresentative.getNetId("y"));
    const Netlist::NetlistEditReport representativeReport =
        poRepresentative.runEditApply(request);
    const bool selectsPoRepresentative = representativeReport.success &&
                                         representativeReport.changed &&
                                         !representativeReport.rolledBack &&
                                         poRepresentative.getGate(internalGate).type == GateType::UNKNOWN &&
                                         poRepresentative.getGate(poGate).type == GateType::AND &&
                                         poRepresentative.validateAfterMutation();

    Netlist invalidLoad;
    invalidLoad.addPrimaryInput("a");
    invalidLoad.addPrimaryInput("b");
    const int invalidA = invalidLoad.getNetId("a");
    const int invalidB = invalidLoad.getNetId("b");
    const int invalidOut0 = invalidLoad.addNet("out0");
    const int invalidOut1 = invalidLoad.addNet("out1");
    const int invalidGate0 = invalidLoad.addGate("g0", GateType::AND);
    const int invalidGate1 = invalidLoad.addGate("g1", GateType::AND);
    invalidLoad.connectGateInput(invalidGate0, invalidA);
    invalidLoad.connectGateInput(invalidGate0, invalidB);
    invalidLoad.connectGateOutput(invalidGate0, invalidOut0);
    invalidLoad.connectGateInput(invalidGate1, invalidA);
    invalidLoad.connectGateInput(invalidGate1, invalidB);
    invalidLoad.connectGateOutput(invalidGate1, invalidOut1);
    invalidLoad.getNetMutable(invalidOut1).loadGateIds.push_back(
        static_cast<int>(invalidLoad.getGateCount()) + 11);
    const bool rejectsInvalidLoad =
        invalidLoad.mergeStructurallyEquivalentGates() == 0 &&
        invalidLoad.getGate(invalidGate0).type == GateType::AND &&
        invalidLoad.getGate(invalidGate1).type == GateType::AND;

    report.check(scopedMerge &&
                 preservesNamedPos &&
                 selectsPoRepresentative &&
                 rejectsInvalidLoad,
                 "test2 structural merge preserves scope/PO/pin-level safety");
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

size_t countActiveConstantNets(const Netlist& netlist, int value) {
    size_t count = 0;
    for (size_t netIndex = 0; netIndex < netlist.getNetCount(); ++netIndex) {
        const Net& net = netlist.getNet(static_cast<int>(netIndex));
        if (!net.isRemoved && net.isConst && net.constVal == value) {
            ++count;
        }
    }
    return count;
}

void testFanoutInsertionWithTiedInputs(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("source");
    netlist.addPrimaryOutput("y");

    const int sourceNetId = netlist.getNetId("source");
    const int outputNetId = netlist.getNetId("y");
    const int gateId = netlist.addGate("tied_and", GateType::AND);
    netlist.connectGateInput(gateId, sourceNetId);
    netlist.connectGateInput(gateId, sourceNetId);
    netlist.connectGateOutput(gateId, outputNetId);

    const FanoutLoadReport before = netlist.getFanoutLoadReport(sourceNetId);

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::InsertBuffersOnEachLoad;
    request.netName = "source";
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    const Gate& tiedAnd = netlist.getGate(gateId);
    bool bothInputsHaveDistinctBufferDrivers = tiedAnd.inputNetIds.size() == 2 &&
                                               tiedAnd.inputNetIds[0] != tiedAnd.inputNetIds[1];
    if (bothInputsHaveDistinctBufferDrivers) {
        for (int inputNetId : tiedAnd.inputNetIds) {
            if (!netlist.isValidNetId(inputNetId)) {
                bothInputsHaveDistinctBufferDrivers = false;
                break;
            }
            const int driverGateId = netlist.getNet(inputNetId).driverGateId;
            if (!netlist.isValidGateId(driverGateId) ||
                netlist.getGate(driverGateId).type != GateType::BUF) {
                bothInputsHaveDistinctBufferDrivers = false;
                break;
            }
        }
    }

    report.check(before.ok &&
                 before.totalLoadCount == 2 &&
                 editReport.success &&
                 editReport.changed &&
                 !editReport.rolledBack &&
                 editReport.changedGateNames.size() == 2 &&
                 netlist.getFanoutLoadReport(sourceNetId).totalLoadCount == 2 &&
                 bothInputsHaveDistinctBufferDrivers &&
                 netlist.validateAfterMutation(),
                 "test2 fanout insertion handles tied input pins exactly once");
}

void testReplaceAllLoadsPreservesPinMultiplicity(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("old_source");
    netlist.addPrimaryInput("new_source");
    netlist.addPrimaryOutput("y");

    const int oldNetId = netlist.getNetId("old_source");
    const int newNetId = netlist.getNetId("new_source");
    const int gateId = netlist.addGate("tied_and", GateType::AND);
    netlist.connectGateInput(gateId, oldNetId);
    netlist.connectGateInput(gateId, oldNetId);
    netlist.connectGateOutput(gateId, netlist.getNetId("y"));

    const bool replaced = netlist.replaceAllLoadsOfNet(oldNetId, newNetId);
    const Gate& gate = netlist.getGate(gateId);
    const Net& oldNet = netlist.getNet(oldNetId);
    const Net& newNet = netlist.getNet(newNetId);
    report.check(replaced &&
                 gate.inputNetIds.size() == 2 &&
                 gate.inputNetIds[0] == newNetId &&
                 gate.inputNetIds[1] == newNetId &&
                 oldNet.loadGateIds.empty() &&
                 newNet.loadGateIds.size() == 2 &&
                 newNet.loadGateIds[0] == gateId &&
                 newNet.loadGateIds[1] == gateId &&
                 netlist.validateAfterMutation(),
                 "test2 replace_all_loads preserves pin-level multiplicity");
}

void testConstantNetDeduplication(TestReport& report) {
    Netlist directNetlist;
    const int const0First = directNetlist.addNet("1'b0");
    const int const0Second = directNetlist.addNet("1'b0");
    const int const1First = directNetlist.addNet("1'b1");
    const int const1Second = directNetlist.addNet("1'b1");
    report.check(const0First == const0Second &&
                 const1First == const1Second &&
                 directNetlist.getNetId("1'b0") == const0First &&
                 directNetlist.getNetId("1'b1") == const1First &&
                 directNetlist.getNetCount() == 2 &&
                 countActiveConstantNets(directNetlist, 0) == 1 &&
                 countActiveConstantNets(directNetlist, 1) == 1,
                 "test2 addNet deduplicates canonical constant nets");

    Netlist mappedNetlist;
    mappedNetlist.addPrimaryInput("a");
    const int inputNetId = mappedNetlist.getNetId("a");
    constexpr int kGateCountPerType = 8;
    for (int i = 0; i < kGateCountPerType; ++i) {
        const std::string notOutputName = "not_y" + std::to_string(i);
        mappedNetlist.addPrimaryOutput(notOutputName);
        const int notGateId = mappedNetlist.addGate(
            "not_g" + std::to_string(i), GateType::NOT);
        mappedNetlist.connectGateInput(notGateId, inputNetId);
        mappedNetlist.connectGateOutput(
            notGateId, mappedNetlist.getNetId(notOutputName));

        const std::string bufOutputName = "buf_y" + std::to_string(i);
        mappedNetlist.addPrimaryOutput(bufOutputName);
        const int bufGateId = mappedNetlist.addGate(
            "buf_g" + std::to_string(i), GateType::BUF);
        mappedNetlist.connectGateInput(bufGateId, inputNetId);
        mappedNetlist.connectGateOutput(
            bufGateId, mappedNetlist.getNetId(bufOutputName));
    }

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::ConvertToBasis;
    request.scope = TargetScope::WHOLE_NETLIST;
    request.allowedTypes = {GateType::XOR};
    request.validateEquivalence = true;

    const Netlist::NetlistEditReport firstReport =
        mappedNetlist.runEditApply(request);
    const size_t netCountAfterFirstMapping = mappedNetlist.getNetCount();
    const Netlist::NetlistEditReport secondReport =
        mappedNetlist.runEditApply(request);

    report.check(firstReport.success &&
                 firstReport.changed &&
                 !firstReport.rolledBack &&
                 secondReport.success &&
                 !secondReport.rolledBack &&
                 mappedNetlist.getNetId("1'b0") >= 0 &&
                 mappedNetlist.getNetId("1'b1") >= 0 &&
                 countActiveConstantNets(mappedNetlist, 0) == 1 &&
                 countActiveConstantNets(mappedNetlist, 1) == 1 &&
                 mappedNetlist.getNetCount() == netCountAfterFirstMapping &&
                 mappedNetlist.validateAfterMutation(),
                 "test2 repeated technology mapping reuses constant nets");
}

void testConstantNetRenameRejected(TestReport& report) {
    Netlist directNetlist;
    const int directConst0 = directNetlist.addNet("1'b0");
    const int directSignal = directNetlist.addNet("ordinary_signal");
    const bool directRenameResult =
        directNetlist.renameNet("1'b0", "renamed_zero");
    const bool renameToLiteralResult =
        directNetlist.renameNet("ordinary_signal", "1'b1");
    report.check(!directRenameResult &&
                 !renameToLiteralResult &&
                 directNetlist.getNetId("1'b0") == directConst0 &&
                 directNetlist.getNetId("renamed_zero") == -1 &&
                 directNetlist.getNetId("ordinary_signal") == directSignal &&
                 directNetlist.getNetId("1'b1") == -1 &&
                 directNetlist.getNet(directConst0).name == "1'b0",
                 "test2 low-level renameNet rejects constant literal");

    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryOutput("y");
    const int a = netlist.getNetId("a");
    const int y = netlist.getNetId("y");
    const int const0 = netlist.addNet("1'b0");
    const int gateId = netlist.addGate("g_and_const", GateType::AND);
    netlist.connectGateInput(gateId, a);
    netlist.connectGateInput(gateId, const0);
    netlist.connectGateOutput(gateId, y);

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::RenameNet;
    request.oldName = "1'b0";
    request.newName = "renamed_zero";
    request.validateEquivalence = true;
    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);

    const std::string outputPath =
        "mini test/test2/constant_rename_rejection_output.v";
    VerilogWriter writer;
    const bool writeOk = writer.write(outputPath, netlist);
    std::ifstream input(outputPath);
    std::stringstream buffer;
    buffer << input.rdbuf();
    input.close();
    std::remove(outputPath.c_str());
    const std::string writtenVerilog = buffer.str();

    report.check(!editReport.success &&
                 !editReport.changed &&
                 !editReport.rolledBack &&
                 netlist.getNetId("1'b0") == const0 &&
                 netlist.getNetId("renamed_zero") == -1 &&
                 writeOk &&
                 writtenVerilog.find("1'b0") != std::string::npos &&
                 writtenVerilog.find("renamed_zero") == std::string::npos &&
                 netlist.validateAfterMutation(),
                 "test2 edit_apply rejects constant rename and writer keeps literal");
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
    testCollapseDoubleInverterSafety(report);
    testSimplifyConstants(report, editCircuit);
    testSimplifySameInput(report, editCircuit);
    testLocalSimplificationFixpoint(report, editCircuit);
    testMergeEquivalentGates(report, editCircuit);
    testMergeStructurallyEquivalentGates(report, editCircuit);
    testStructuralMergeSafetyAndScope(report);
    testTrimDeadLogic(report, editCircuit);
    testRemoveDanglingLogic(report, editCircuit);
    testRemoveUnusedNets(report, editCircuit);
    testRemoveNetIfUnused(report, editCircuit);
    testSafeCleanupFixpointChanges(report, editCircuit);
    testSafeCleanupFixpointNoop(report);
    testInsertBuffersForSpecificNet(report, fanoutCircuit);
    testInsertBuffersForFanout(report, fanoutCircuit);
    testInsertBuffersOnEachLoad(report, fanoutCircuit);
    testFanoutInsertionWithTiedInputs(report);
    testReplaceAllLoadsPreservesPinMultiplicity(report);
    testConstantNetDeduplication(report);
    testConstantNetRenameRejected(report);
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
