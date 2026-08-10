#include <chrono>
#include <initializer_list>
#include <iostream>
#include <string>

#include "include/SATEngine/Primitives.h"
#include "include/core/Netlist.h"

namespace {

struct TestReport {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name) {
        if (condition) {
            ++passed;
            std::cout << "[PASS] " << name << '\n';
        } else {
            ++failed;
            std::cout << "[FAIL] " << name << '\n';
        }
    }
};

eqeng::Primitives::Config quietConfig() {
    eqeng::Primitives::Config config;
    config.verbose_rebuild = false;
    return config;
}

int addUnary(Netlist& netlist,
             const std::string& gateName,
             GateType type,
             const std::string& input,
             const std::string& output) {
    const int outputId = netlist.addNet(output);
    const int gateId = netlist.addGate(gateName, type);
    netlist.connectGateInput(gateId, netlist.getNetId(input));
    netlist.connectGateOutput(gateId, outputId);
    return outputId;
}

int addBinary(Netlist& netlist,
              const std::string& gateName,
              GateType type,
              const std::string& inputA,
              const std::string& inputB,
              const std::string& output) {
    const int outputId = netlist.addNet(output);
    const int gateId = netlist.addGate(gateName, type);
    netlist.connectGateInput(gateId, netlist.getNetId(inputA));
    netlist.connectGateInput(gateId, netlist.getNetId(inputB));
    netlist.connectGateOutput(gateId, outputId);
    return outputId;
}

int addNary(Netlist& netlist,
            const std::string& gateName,
            GateType type,
            std::initializer_list<const char*> inputs,
            const std::string& output) {
    const int outputId = netlist.addNet(output);
    const int gateId = netlist.addGate(gateName, type);
    for (const char* input : inputs) {
        netlist.connectGateInput(gateId, netlist.getNetId(input));
    }
    netlist.connectGateOutput(gateId, outputId);
    return outputId;
}

Netlist makeCircuit() {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    addBinary(netlist, "g_and", GateType::AND, "a", "b", "n_and");
    addUnary(netlist, "g_not_a", GateType::NOT, "a", "n_not_a");
    addUnary(netlist, "g_not_b", GateType::NOT, "b", "n_not_b");
    addBinary(netlist, "g_demorgan", GateType::NOR,
              "n_not_a", "n_not_b", "n_demorgan");
    addBinary(netlist, "g_zero", GateType::XOR, "a", "a", "n_zero");
    return netlist;
}

void testPrimitiveProofs(TestReport& report) {
    Netlist netlist = makeCircuit();
    eqeng::Primitives primitives(netlist, {}, quietConfig());

    const eqeng::SigRef nAnd = primitives.resolve("n_and");
    const eqeng::SigRef nDemorgan = primitives.resolve("n_demorgan");
    const eqeng::SigRef nZero = primitives.resolve("n_zero");

    report.check(
        primitives.equiv_checked(nAnd, nDemorgan, 10.0) ==
            eqeng::EquivResult::Equal,
        "Phase-A equivalence proof works as an internal primitive");
    report.check(
        primitives.is_const_checked(nZero, false, 10.0) ==
            eqeng::EquivResult::Equal,
        "Phase-A constant proof works as an internal primitive");
}

void testCofactorCacheKey(TestReport& report) {
    Netlist netlist = makeCircuit();
    eqeng::Primitives primitives(netlist, {}, quietConfig());

    const eqeng::SigRef f = primitives.resolve("n_and");
    const eqeng::SigRef a = primitives.resolve("a");
    const eqeng::SigRef b = primitives.resolve("b");
    const eqeng::SigRef fA0 = primitives.cofactor(f, a, false);
    const eqeng::SigRef fA1 = primitives.cofactor(f, a, true);
    const eqeng::SigRef fB1 = primitives.cofactor(f, b, true);

    report.check(
        primitives.is_const_checked(fA0, false) == eqeng::EquivResult::Equal,
        "cofactor cache keeps value=false separate");
    report.check(
        primitives.equiv_checked(fA1, b) == eqeng::EquivResult::Equal,
        "cofactor cache keeps the selected variable in its key");
    report.check(
        primitives.equiv_checked(fB1, a) == eqeng::EquivResult::Equal,
        "cofactor cache returns the correct function for another variable");
}

void testPerInstanceFreshness(TestReport& report) {
    Netlist netlist = makeCircuit();
    eqeng::Primitives first(netlist, {}, quietConfig());
    eqeng::Primitives second(netlist, {}, quietConfig());

    const eqeng::SigRef staleA = first.resolve("a");
    (void)second.resolve("a");
    const uint32_t firstGeneration = first.generation();

    addUnary(netlist, "g_copy", GateType::BUF, "a", "n_copy");
    (void)second.resolve("n_copy");
    const eqeng::SigRef currentA = first.resolve("a");

    report.check(
        first.generation() == firstGeneration + 1,
        "each Primitives instance tracks the Netlist revision independently");

    bool staleRejected = false;
    try {
        (void)first.equiv_checked(staleA, currentA);
    } catch (const eqeng::StaleSignal&) {
        staleRejected = true;
    }
    report.check(staleRejected, "rebuild rejects a stale SigRef");
}

void testInterruptibleProof(TestReport& report) {
    Netlist netlist = makeCircuit();
    eqeng::Primitives primitives(netlist, {}, quietConfig());
    const eqeng::SigRef a = primitives.resolve("a");
    const eqeng::SigRef b = primitives.resolve("b");

    const auto startedAt = std::chrono::steady_clock::now();
    const eqeng::EquivResult timed =
        primitives.equiv_checked(a, b, 1e-12);
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startedAt).count();

    report.check(
        timed == eqeng::EquivResult::Unknown &&
            primitives.last_proof_timed_out() && elapsed < 0.5,
        "timed Phase-A proof returns Unknown promptly");
    report.check(
        primitives.equiv_checked(a, b, 10.0) ==
                eqeng::EquivResult::NotEqual &&
            !primitives.last_proof_timed_out(),
        "a later proof remains conclusive after timeout");
}

void testNaryGateSemantics(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryInput("c");

    addNary(netlist, "g_and3", GateType::AND, {"a", "b", "c"}, "and3");
    addBinary(netlist, "g_and_ab", GateType::AND, "a", "b", "and_ab");
    addBinary(netlist, "g_and_ref", GateType::AND, "and_ab", "c", "and_ref");
    addNary(netlist, "g_nand3", GateType::NAND, {"a", "b", "c"}, "nand3");
    addUnary(netlist, "g_nand_ref", GateType::NOT, "and_ref", "nand_ref");

    addNary(netlist, "g_or3", GateType::OR, {"a", "b", "c"}, "or3");
    addBinary(netlist, "g_or_ab", GateType::OR, "a", "b", "or_ab");
    addBinary(netlist, "g_or_ref", GateType::OR, "or_ab", "c", "or_ref");
    addNary(netlist, "g_nor3", GateType::NOR, {"a", "b", "c"}, "nor3");
    addUnary(netlist, "g_nor_ref", GateType::NOT, "or_ref", "nor_ref");

    addNary(netlist, "g_xor3", GateType::XOR, {"a", "b", "c"}, "xor3");
    addBinary(netlist, "g_xor_ab", GateType::XOR, "a", "b", "xor_ab");
    addBinary(netlist, "g_xor_ref", GateType::XOR, "xor_ab", "c", "xor_ref");
    addNary(netlist, "g_xnor3", GateType::XNOR, {"a", "b", "c"}, "xnor3");
    addUnary(netlist, "g_xnor_ref", GateType::NOT, "xor_ref", "xnor_ref");

    addNary(netlist, "g_tied", GateType::AND, {"a", "b", "a"}, "tied");
    addBinary(netlist, "g_tied_ref", GateType::AND, "a", "b", "tied_ref");

    eqeng::Primitives primitives(netlist, {}, quietConfig());
    report.check(primitives.model_health() == eqeng::ModelHealth::Sound,
                 "arbitrary fan-in circuit remains a sound AIG model");

    const auto equivalent = [&](const char* lhs, const char* rhs) {
        return primitives.equiv_checked(
                   primitives.resolve(lhs), primitives.resolve(rhs), 10.0) ==
               eqeng::EquivResult::Equal;
    };
    report.check(equivalent("and3", "and_ref"),
                 "three-input AND uses every input");
    report.check(equivalent("nand3", "nand_ref"),
                 "three-input NAND negates the complete fold");
    report.check(equivalent("or3", "or_ref"),
                 "three-input OR uses every input");
    report.check(equivalent("nor3", "nor_ref"),
                 "three-input NOR negates the complete fold");
    report.check(equivalent("xor3", "xor_ref"),
                 "three-input XOR uses every input");
    report.check(equivalent("xnor3", "xnor_ref"),
                 "three-input XNOR negates the complete fold");
    report.check(equivalent("tied", "tied_ref"),
                 "non-adjacent tied inputs do not break topological ordering");
}

void testNetlistValueLifecycle(TestReport& report) {
    Netlist original = makeCircuit();
    const uint64_t originalRevision = original.revision();

    Netlist copied = original;
    report.check(copied.revision() == originalRevision && copied.isDirty(),
                 "Netlist copy keeps graph revision but starts without a cache");
    addUnary(copied, "g_copy_only", GateType::BUF, "a", "copy_only");
    report.check(original.getNetId("copy_only") < 0 &&
                     copied.getNetId("copy_only") >= 0,
                 "Netlist copy owns independent named graph storage");

    Netlist moved = std::move(copied);
    report.check(moved.getNetId("copy_only") >= 0 && moved.isDirty(),
                 "Netlist move preserves the graph and discards bound caches");

    const uint64_t beforeRestore = moved.revision();
    moved.restoreFrom(original);
    report.check(moved.getNetId("copy_only") < 0 &&
                     moved.revision() > beforeRestore && moved.isDirty(),
                 "rollback restores graph data with a strictly newer revision");

    const Netlist& readOnly = moved;
    eqeng::Primitives primitives(readOnly, {}, quietConfig());
    report.check(primitives.model_health() == eqeng::ModelHealth::Sound,
                 "Boolean primitives bind to Netlist as a read-only backend");
}

} // namespace

int main() {
    TestReport report;
    testPrimitiveProofs(report);
    testCofactorCacheKey(report);
    testPerInstanceFreshness(report);
    testInterruptibleProof(report);
    testNaryGateSemantics(report);
    testNetlistValueLifecycle(report);

    std::cout << "AIG internal primitive regression: " << report.passed
              << " passed, " << report.failed << " failed\n";
    return report.failed == 0 ? 0 : 1;
}
