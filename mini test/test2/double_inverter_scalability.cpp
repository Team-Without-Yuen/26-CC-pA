#include "include/core/Netlist.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    int inverterCount = 100000;
    if (argc >= 2) {
        inverterCount = std::atoi(argv[1]);
    }
    if (inverterCount <= 0) {
        std::cerr << "inverter count must be positive\n";
        return 2;
    }

    const auto buildStart = std::chrono::steady_clock::now();
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryOutput("y");
    int previousNetId = netlist.getNetId("a");
    for (int i = 0; i < inverterCount; ++i) {
        const int outputNetId = netlist.addNet("n" + std::to_string(i));
        const int gateId = netlist.addGate("inv" + std::to_string(i), GateType::NOT);
        netlist.connectGateInput(gateId, previousNetId);
        netlist.connectGateOutput(gateId, outputNetId);
        previousNetId = outputNetId;
    }
    const int sinkGateId = netlist.addGate("sink", GateType::BUF);
    netlist.connectGateInput(sinkGateId, previousNetId);
    netlist.connectGateOutput(sinkGateId, netlist.getNetId("y"));
    const auto buildEnd = std::chrono::steady_clock::now();

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::CollapseDoubleInverter;
    request.validateEquivalence = true;

    const auto applyStart = std::chrono::steady_clock::now();
    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    const auto applyEnd = std::chrono::steady_clock::now();

    size_t activeNotCount = 0;
    for (size_t i = 0; i < netlist.getGateCount(); ++i) {
        if (netlist.getGate(static_cast<int>(i)).type == GateType::NOT) {
            ++activeNotCount;
        }
    }
    const int expectedPairs = inverterCount / 2;
    const size_t expectedRemainingNot = static_cast<size_t>(inverterCount % 2);
    const bool passed = editReport.success &&
                        !editReport.rolledBack &&
                        editReport.diff.activeGateCountDelta == -2 * expectedPairs &&
                        activeNotCount == expectedRemainingNot &&
                        netlist.validateAfterMutation();

    std::cout << "inverter_count=" << inverterCount << "\n"
              << "build_seconds="
              << std::chrono::duration<double>(buildEnd - buildStart).count() << "\n"
              << "apply_seconds="
              << std::chrono::duration<double>(applyEnd - applyStart).count() << "\n"
              << "collapsed_pairs=" << expectedPairs << "\n"
              << "active_not_count=" << activeNotCount << "\n"
              << "success=" << (editReport.success ? "true" : "false") << "\n"
              << "rolled_back=" << (editReport.rolledBack ? "true" : "false") << "\n"
              << "structure_valid="
              << (netlist.validateAfterMutation() ? "true" : "false") << "\n"
              << (passed ? "[PASS]" : "[FAIL]")
              << " double inverter scalability\n";
    return passed ? 0 : 1;
}
