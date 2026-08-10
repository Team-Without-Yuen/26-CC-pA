#include "include/core/Netlist.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    int gateCount = 100000;
    if (argc >= 2) {
        gateCount = std::atoi(argv[1]);
    }
    if (gateCount <= 0) {
        std::cerr << "gate count must be positive\n";
        return 2;
    }

    const auto buildStart = std::chrono::steady_clock::now();
    Netlist netlist;
    netlist.addPrimaryInput("a");
    const int a = netlist.getNetId("a");
    const int const0 = netlist.addNet("1'b0");
    for (int i = 0; i < gateCount; ++i) {
        const int outputNet = netlist.addNet("out_" + std::to_string(i));
        const int gateId = netlist.addGate(
            "const_and_" + std::to_string(i), GateType::AND);
        netlist.connectGateInput(gateId, a);
        netlist.connectGateInput(gateId, const0);
        netlist.connectGateOutput(gateId, outputNet);
    }
    const auto buildEnd = std::chrono::steady_clock::now();

    if (argc >= 3 && std::string(argv[2]) == "profile") {
        const auto cloneStart = std::chrono::steady_clock::now();
        const Netlist before = netlist.cloneForRollback();
        const auto cloneEnd = std::chrono::steady_clock::now();
        const auto simplifyStart = std::chrono::steady_clock::now();
        const int simplified = netlist.simplifyGatesWithConstants(
            GateType::AND, 0, 2);
        const auto simplifyEnd = std::chrono::steady_clock::now();
        const auto reportStart = std::chrono::steady_clock::now();
        const auto report = Netlist::buildEditReport(
            before, netlist, "profile", Netlist::NetlistEditOperationKind::Simplification);
        const auto reportEnd = std::chrono::steady_clock::now();
        const auto depthStart = std::chrono::steady_clock::now();
        const auto depth = Netlist::buildDepthChangeReport(before, netlist);
        const auto depthEnd = std::chrono::steady_clock::now();
        std::cout << "clone_seconds="
                  << std::chrono::duration<double>(cloneEnd - cloneStart).count() << "\n"
                  << "simplify_seconds="
                  << std::chrono::duration<double>(simplifyEnd - simplifyStart).count() << "\n"
                  << "report_seconds="
                  << std::chrono::duration<double>(reportEnd - reportStart).count() << "\n"
                  << "depth_seconds="
                  << std::chrono::duration<double>(depthEnd - depthStart).count() << "\n"
                  << "simplified_count=" << simplified << "\n"
                  << "report_success=" << (report.success ? "true" : "false") << "\n"
                  << "before_depth=" << depth.beforeDepth << "\n";
        return simplified == gateCount && report.success ? 0 : 1;
    }

    Netlist::EditApplyRequest request;
    request.kind = Netlist::EditCommandKind::SimplifyConstants;
    request.gateType = GateType::AND;
    request.constValue = 0;
    request.inputCount = 2;
    request.validateEquivalence = true;

    const auto applyStart = std::chrono::steady_clock::now();
    const Netlist::NetlistEditReport editReport = netlist.runEditApply(request);
    const auto applyEnd = std::chrono::steady_clock::now();

    const double buildSeconds =
        std::chrono::duration<double>(buildEnd - buildStart).count();
    const double applySeconds =
        std::chrono::duration<double>(applyEnd - applyStart).count();
    const size_t simplifiedCount = editReport.constantSimplification.has_value()
        ? editReport.constantSimplification->simplifiedCount
        : 0;
    const bool passed = editReport.success &&
                        !editReport.rolledBack &&
                        simplifiedCount == static_cast<size_t>(gateCount) &&
                        netlist.getNetId("1'b0") == const0 &&
                        netlist.validateAfterMutation();

    std::cout << "gate_count=" << gateCount << "\n"
              << "build_seconds=" << buildSeconds << "\n"
              << "apply_seconds=" << applySeconds << "\n"
              << "simplified_count=" << simplifiedCount << "\n"
              << "success=" << (editReport.success ? "true" : "false") << "\n"
              << "rolled_back=" << (editReport.rolledBack ? "true" : "false") << "\n"
              << "structure_valid=" << (netlist.validateAfterMutation() ? "true" : "false") << "\n"
              << (passed ? "[PASS]" : "[FAIL]")
              << " constant simplification scalability\n";
    return passed ? 0 : 1;
}
