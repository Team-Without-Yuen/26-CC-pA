#include "include/core/DepthOptimizer.h"
#include "include/core/Netlist.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

struct DepthOptimizerTestAccess {
    static int eliminateDoubleInverters(DepthOptimizer& optimizer, Netlist& netlist) {
        return optimizer.eliminateDoubleInverters(netlist);
    }
};

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

struct ChainFixture {
    Netlist netlist;
    int firstInputNetId = -1;
    int outputNetId = -1;
};

ChainFixture makeReverseIdNotChain(int notCount, bool bufferBeforePrimaryOutput) {
    ChainFixture fixture;
    fixture.netlist.addPrimaryInput("a");
    fixture.netlist.addPrimaryOutput("y");
    fixture.firstInputNetId = fixture.netlist.getNetId("a");
    fixture.outputNetId = fixture.netlist.getNetId("y");

    std::vector<int> chainNets(static_cast<std::size_t>(notCount) + 1, -1);
    chainNets[0] = fixture.firstInputNetId;
    for (int index = 1; index <= notCount; ++index) {
        const bool usePrimaryOutput = !bufferBeforePrimaryOutput && index == notCount;
        chainNets[static_cast<std::size_t>(index)] = usePrimaryOutput
            ? fixture.outputNetId
            : fixture.netlist.addNet("chain_" + std::to_string(index));
    }

    // Create downstream gates first. Their IDs therefore run opposite to the
    // logical direction, which is the old repeated-full-scan worst case.
    for (int index = notCount - 1; index >= 0; --index) {
        const int gateId = fixture.netlist.addGate(
            "inv_" + std::to_string(index), GateType::NOT);
        fixture.netlist.connectGateInput(
            gateId, chainNets[static_cast<std::size_t>(index)]);
        fixture.netlist.connectGateOutput(
            gateId, chainNets[static_cast<std::size_t>(index + 1)]);
    }

    if (bufferBeforePrimaryOutput) {
        const int bufferId = fixture.netlist.addGate("out_buf", GateType::BUF);
        fixture.netlist.connectGateInput(
            bufferId, chainNets[static_cast<std::size_t>(notCount)]);
        fixture.netlist.connectGateOutput(bufferId, fixture.outputNetId);
    }

    return fixture;
}

void testReverseIdChainCorrectness(TestReport& report) {
    ChainFixture fixture = makeReverseIdNotChain(4, true);
    DepthOptimizer optimizer;

    const int removed = DepthOptimizerTestAccess::eliminateDoubleInverters(
        optimizer, fixture.netlist);

    report.check(
        removed == 3 &&
        fixture.netlist.getBooleanExpression("y") == "a" &&
        fixture.netlist.validateStructure(),
        "double-inverter worklist preserves even-chain function and structure");
}

void testPrimaryOutputDriverProtection(TestReport& report) {
    ChainFixture fixture = makeReverseIdNotChain(2, false);
    DepthOptimizer optimizer;
    const int originalDriver = fixture.netlist.getNet(fixture.outputNetId).driverGateId;

    const int removed = DepthOptimizerTestAccess::eliminateDoubleInverters(
        optimizer, fixture.netlist);

    report.check(
        removed == 0 &&
        fixture.netlist.getNet(fixture.outputNetId).driverGateId == originalDriver &&
        fixture.netlist.getGate(originalDriver).type == GateType::NOT &&
        fixture.netlist.validateStructure(),
        "double-inverter worklist preserves a primary-output driver");
}

void testReverseIdChainScalability(TestReport& report) {
    constexpr int kNotCount = 200000;
    ChainFixture fixture = makeReverseIdNotChain(kNotCount, true);
    DepthOptimizer optimizer;

    const auto start = std::chrono::steady_clock::now();
    const int removed = DepthOptimizerTestAccess::eliminateDoubleInverters(
        optimizer, fixture.netlist);
    const double elapsedSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "[INFO] reverse-id NOT chain: " << kNotCount
              << " gates, removed=" << removed
              << ", elapsed=" << elapsedSeconds << "s\n";
    report.check(
        removed == kNotCount - 1 &&
        fixture.netlist.getBooleanExpression("y") == "a" &&
        fixture.netlist.validateStructure() &&
        elapsedSeconds < 10.0,
        "double-inverter worklist scales linearly on reverse-id chain");
}

} // namespace

int main() {
    TestReport report;
    testReverseIdChainCorrectness(report);
    testPrimaryOutputDriverProtection(report);
    testReverseIdChainScalability(report);

    std::cout << "Summary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
