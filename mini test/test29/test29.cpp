#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <iostream>
#include <string>

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

void testDffOnlyDeadLogicCleanup(
    TestReport& report,
    const std::string& circuitPath) {
    Netlist netlist;
    VerilogReader reader;
    const bool loaded = reader.read(circuitPath, netlist);
    report.check(loaded, "DFF-only cleanup circuit loads");
    if (!loaded) return;

    const int liveGate = netlist.getGateId("g_live");
    const int deadGate = netlist.getGateId("g_dead");
    const int dff = netlist.getGateId("ff0");

    const int removed = netlist.trimDeadLogic();
    report.check(
        removed == 1 &&
        netlist.getGate(liveGate).type == GateType::AND &&
        netlist.getGate(deadGate).type == GateType::UNKNOWN &&
        netlist.getGate(dff).type == GateType::DFF &&
        netlist.validateStructure(),
        "DFF-only design preserves D-input logic and removes dead logic");
}

void testNoEndpointCleanupIsConservative(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    const int a = netlist.getNetId("a");
    const int danglingNet = netlist.addNet("n_dangling");
    const int danglingGate = netlist.addGate("g_dangling", GateType::BUF);
    netlist.connectGateInput(danglingGate, a);
    netlist.connectGateOutput(danglingGate, danglingNet);

    const int removed = netlist.trimDeadLogic();
    report.check(
        removed == 0 &&
        netlist.getGate(danglingGate).type == GateType::BUF,
        "design without observable endpoints is not guessed or trimmed");
}

} // namespace

int main(int argc, char** argv) {
    const std::string circuitPath =
        argc > 1 ? argv[1] : "mini test/test29/dff_only_cleanup_circuit.v";

    TestReport report;
    testDffOnlyDeadLogicCleanup(report, circuitPath);
    testNoEndpointCleanupIsConservative(report);

    std::cout << "Summary: " << report.passed
              << " passed, " << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
