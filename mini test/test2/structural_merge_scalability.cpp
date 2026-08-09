#include "include/core/Netlist.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void buildSameGroup(Netlist& netlist, int gateCount) {
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    const int a = netlist.getNetId("a");
    const int b = netlist.getNetId("b");
    for (int i = 0; i < gateCount; ++i) {
        const int out = netlist.addNet("same_out_" + std::to_string(i));
        const int gate = netlist.addGate("same_and_" + std::to_string(i), GateType::AND);
        netlist.connectGateInput(gate, a);
        netlist.connectGateInput(gate, b);
        netlist.connectGateOutput(gate, out);
    }
}

void buildCascade(Netlist& netlist, int levelCount) {
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryInput("c");
    int left = netlist.getNetId("a");
    int right = netlist.getNetId("b");
    const int c = netlist.getNetId("c");
    for (int i = 0; i < levelCount; ++i) {
        const int leftOut = netlist.addNet("left_out_" + std::to_string(i));
        const int rightOut = netlist.addNet("right_out_" + std::to_string(i));
        const int leftGate = netlist.addGate("left_and_" + std::to_string(i), GateType::AND);
        const int rightGate = netlist.addGate("right_and_" + std::to_string(i), GateType::AND);
        netlist.connectGateInput(leftGate, left);
        netlist.connectGateInput(leftGate, i == 0 ? right : c);
        netlist.connectGateOutput(leftGate, leftOut);
        netlist.connectGateInput(rightGate, right);
        netlist.connectGateInput(rightGate, i == 0 ? left : c);
        netlist.connectGateOutput(rightGate, rightOut);
        left = leftOut;
        right = rightOut;
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc >= 2 ? argv[1] : "same_group";
    int size = argc >= 3 ? std::atoi(argv[2]) : 10000;
    if ((mode != "same_group" && mode != "cascade") || size <= 0) {
        std::cerr << "usage: structural_merge_scalability <same_group|cascade> <size>\n";
        return 2;
    }

    const auto buildStart = std::chrono::steady_clock::now();
    Netlist netlist;
    if (mode == "same_group") {
        buildSameGroup(netlist, size);
    } else {
        buildCascade(netlist, size);
    }
    const auto buildEnd = std::chrono::steady_clock::now();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::MergeStructurallyEquivalentGates;
    request.validateEquivalence = true;
    const auto applyStart = std::chrono::steady_clock::now();
    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    const auto applyEnd = std::chrono::steady_clock::now();

    const int expectedMerged = mode == "same_group" ? size - 1 : size;
    const bool passed = editReport.success &&
                        !editReport.rolledBack &&
                        editReport.diff.activeGateCountDelta == -expectedMerged &&
                        netlist.validateAfterMutation();
    std::cout << "mode=" << mode << "\n"
              << "size=" << size << "\n"
              << "build_seconds="
              << std::chrono::duration<double>(buildEnd - buildStart).count() << "\n"
              << "apply_seconds="
              << std::chrono::duration<double>(applyEnd - applyStart).count() << "\n"
              << "expected_merged=" << expectedMerged << "\n"
              << "actual_active_delta=" << editReport.diff.activeGateCountDelta << "\n"
              << "success=" << (editReport.success ? "true" : "false") << "\n"
              << "rolled_back=" << (editReport.rolledBack ? "true" : "false") << "\n"
              << "structure_valid="
              << (netlist.validateAfterMutation() ? "true" : "false") << "\n"
              << (passed ? "[PASS]" : "[FAIL]")
              << " structural merge scalability\n";
    return passed ? 0 : 1;
}
