#include "include/core/Netlist.h"

#include <iostream>

int main() {
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
    bool inputsDrivenByDistinctBuffers = tiedAnd.inputNetIds.size() == 2 &&
                                        tiedAnd.inputNetIds[0] != tiedAnd.inputNetIds[1];
    if (inputsDrivenByDistinctBuffers) {
        for (int inputNetId : tiedAnd.inputNetIds) {
            if (!netlist.isValidNetId(inputNetId)) {
                inputsDrivenByDistinctBuffers = false;
                break;
            }
            const int driverGateId = netlist.getNet(inputNetId).driverGateId;
            if (!netlist.isValidGateId(driverGateId) ||
                netlist.getGate(driverGateId).type != GateType::BUF) {
                inputsDrivenByDistinctBuffers = false;
                break;
            }
        }
    }

    const size_t afterSourceLoads =
        netlist.getFanoutLoadReport(sourceNetId).totalLoadCount;
    const bool passed = before.ok &&
                        before.totalLoadCount == 2 &&
                        editReport.success &&
                        editReport.changed &&
                        !editReport.rolledBack &&
                        editReport.changedGateNames.size() == 2 &&
                        afterSourceLoads == 2 &&
                        inputsDrivenByDistinctBuffers &&
                        netlist.validateAfterMutation();

    std::cout << "before_fanout=" << before.totalLoadCount << "\n"
              << "success=" << (editReport.success ? "true" : "false") << "\n"
              << "rolled_back=" << (editReport.rolledBack ? "true" : "false") << "\n"
              << "inserted_buffers=" << editReport.changedGateNames.size() << "\n"
              << "after_source_fanout=" << afterSourceLoads << "\n"
              << "structure_valid=" << (netlist.validateAfterMutation() ? "true" : "false") << "\n"
              << (passed ? "[PASS]" : "[FAIL]")
              << " tied-input fanout insertion\n";
    return passed ? 0 : 1;
}
