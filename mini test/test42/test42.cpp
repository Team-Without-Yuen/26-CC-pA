#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "include/SATEngine/Primitives.h"
#include "include/SATEngine/Fraig.h"
#include "include/SATEngine/SatEngine.h"
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
    config.fraig_auto_sweep_threshold = 0;
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

Netlist makeCircuit() {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryInput("c");

    addBinary(netlist, "g_and", GateType::AND, "a", "b", "n_and");
    addUnary(netlist, "g_not_a", GateType::NOT, "a", "n_not_a");

    addBinary(netlist, "g_or_ab", GateType::OR, "a", "b", "n_or_ab");
    addBinary(netlist, "g_absorb", GateType::AND, "a", "n_or_ab", "n_absorb");

    addBinary(netlist, "g_and_ab", GateType::AND, "a", "b", "n_and_ab");
    addBinary(netlist, "g_and_ac", GateType::AND, "a", "c", "n_and_ac");
    addBinary(netlist, "g_expr", GateType::OR, "n_and_ab", "n_and_ac", "n_expr");
    addBinary(netlist, "g_or_bc", GateType::OR, "b", "c", "n_or_bc");
    return netlist;
}

void testIncrementalQueries(TestReport& report) {
    Netlist netlist = makeCircuit();
    eqeng::Primitives primitives(netlist, {}, quietConfig());
    primitives.enable_phase_b(true);

    const auto a = primitives.resolve("a");
    const auto b = primitives.resolve("b");
    const auto nAnd = primitives.resolve("n_and");
    const auto nNotA = primitives.resolve("n_not_a");
    const auto zero = primitives.constant(false);

    report.check(primitives.phase_b_ready(),
                 "Phase B creates the incremental SatEngine lazily");
    report.check(primitives.equiv_checked(a, !nNotA) == eqeng::EquivResult::Equal,
                 "complement parity is preserved by incremental equivalence");
    report.check(primitives.equiv_checked(a, nNotA) == eqeng::EquivResult::NotEqual,
                 "opposite signals are not reported equivalent");

    report.check(
        primitives.equiv_under(nAnd, zero, {{a, false}}) ==
            eqeng::EquivResult::Equal,
        "conditional equivalence is proved under a=false");
    report.check(
        primitives.equiv_under(nAnd, zero, {{a, true}}) ==
            eqeng::EquivResult::NotEqual,
        "the next query does not retain the previous assumption");
    report.check(
        primitives.equiv_under(nAnd, zero, {{a, false}, {a, true}}) ==
            eqeng::EquivResult::Unknown,
        "contradictory assumptions return Unknown instead of a vacuous proof");
    report.check(primitives.equiv_checked(nAnd, zero) == eqeng::EquivResult::NotEqual,
                 "an unconditional query remains sound after conditional queries");

    const auto nExpr = primitives.resolve("n_expr");
    const auto nOrBc = primitives.resolve("n_or_bc");
    const auto restricted = primitives.cofactor(nExpr, a, true);
    report.check(primitives.equiv_checked(restricted, nOrBc) ==
                     eqeng::EquivResult::Equal,
                 "SatEngine syncs after cofactor grows the AIG");

    eqeng::SatEngine direct(primitives.aig());
    const auto rawAnd = primitives.raw(nAnd);
    const auto rawZero = primitives.raw(zero);
    const auto rawA = primitives.raw(a);
    report.check(direct.are_equal_under(rawAnd, rawZero, {{rawA, false}}) ==
                     eqeng::EquivResult::Equal,
                 "direct SatEngine proves a conditional equality");
    report.check(!direct.assert_equal(rawAnd, rawZero),
                 "assert_equal re-proves and rejects a conditional-only fact");
    report.check(direct.are_equal(rawAnd, rawZero) == eqeng::EquivResult::NotEqual &&
                     direct.last_counterexample().valid,
                 "unconditional mismatch provides a counterexample");
    report.check(direct.are_equal(rawAnd, rawAnd) == eqeng::EquivResult::Equal &&
                     !direct.last_counterexample().valid,
                 "a later structural result clears the old counterexample");

    (void)b;
}

void testMutationLifecycle(TestReport& report) {
    Netlist netlist = makeCircuit();
    eqeng::Primitives primitives(netlist, {}, quietConfig());
    primitives.enable_phase_b(true);

    const auto staleA = primitives.resolve("a");
    const uint32_t generation = primitives.generation();
    addUnary(netlist, "g_copy", GateType::BUF, "a", "n_copy");

    const auto copy = primitives.resolve("n_copy");
    const auto currentA = primitives.resolve("a");
    report.check(primitives.phase_b_ready() &&
                     primitives.generation() == generation + 1,
                 "mutation rebuilds both the AIG and Phase B engines");
    report.check(primitives.equiv_checked(copy, currentA) ==
                     eqeng::EquivResult::Equal,
                 "the rebuilt Phase B engine answers on the new revision");

    bool staleRejected = false;
    try {
        (void)primitives.equiv_checked(staleA, currentA);
    } catch (const eqeng::StaleSignal&) {
        staleRejected = true;
    }
    report.check(staleRejected,
                 "mutation rejects SigRef objects from the previous generation");
}

void testVerifiedFraig(TestReport& report) {
    Netlist netlist = makeCircuit();
    eqeng::Primitives primitives(netlist, {}, quietConfig());
    primitives.enable_phase_b(true);
    (void)primitives.resolve("n_absorb");

    report.check(primitives.request_fraig_sweep(true),
                 "verified FRAIG sweep completes on a small circuit");
    report.check(primitives.fraig_verified() && primitives.fraig_complete(),
                 "verified FRAIG reports a complete result");

    const auto classes = primitives.equivalence_report(2);
    bool foundAbsorptionClass = false;
    for (const auto& group : classes.equivalence_classes) {
        const bool hasA = std::find(group.begin(), group.end(), "a") != group.end();
        const bool hasAbsorb =
            std::find(group.begin(), group.end(), "n_absorb") != group.end();
        if (hasA && hasAbsorb) {
            foundAbsorptionClass = true;
            break;
        }
    }
    report.check(classes.is_complete && foundAbsorptionClass,
                 "equivalence_report returns the SAT-verified absorption class");
}

void testFraigMemoryLimitTermination(TestReport& report) {
    Netlist netlist;
    std::string previous;
    for (int i = 0; i < 16; ++i) {
        const std::string name = "x" + std::to_string(i);
        netlist.addPrimaryInput(name);
        if (i == 0) {
            previous = name;
        } else {
            const std::string output = "rare" + std::to_string(i);
            addBinary(netlist, "g_rare" + std::to_string(i), GateType::AND,
                      previous, name, output);
            previous = output;
        }
    }

    eqeng::Primitives primitives(netlist, {}, quietConfig());
    primitives.enable_phase_b(true);
    (void)primitives.resolve(previous);

    eqeng::SatEngine sat(primitives.aig());
    eqeng::Fraig::Config config;
    config.sim_words_init = 1;
    config.sim_words_max = 1;
    config.adaptive_sim = false;
    config.total_time_budget = 1.0;

    eqeng::Fraig fraig(primitives.aig(), sat, config);
    const auto start = std::chrono::steady_clock::now();
    fraig.sweep();
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    report.check(fraig.is_swept() && elapsed < 0.5,
                 "FRAIG terminates when no simulation word remains for a counterexample");
    report.check(fraig.stats().gave_up > 0,
                 "unrecyclable counterexamples are reported as gave-up instead of retried");
}

void testUntrustedEquivalenceReport(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryOutput("y");
    const int gate = netlist.addGate("broken_and", GateType::AND);
    netlist.connectGateOutput(gate, netlist.getNetId("y"));

    eqeng::Primitives primitives(netlist, {}, quietConfig());
    primitives.enable_phase_b(true);
    const auto classes = primitives.equivalence_report(1);

    report.check(classes.excluded_untrusted > 0 && !classes.is_complete,
                 "excluding untrusted nets marks the equivalence report incomplete");
}

void testPhaseADifferential(TestReport& report) {
    Netlist netlist;
    std::vector<std::string> signals;
    for (int i = 0; i < 6; ++i) {
        const std::string name = "p" + std::to_string(i);
        netlist.addPrimaryInput(name);
        signals.push_back(name);
    }

    std::mt19937 rng(0x5a17u);
    const std::vector<GateType> binaryTypes = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
        GateType::XOR, GateType::XNOR};
    for (int i = 0; i < 96; ++i) {
        const std::string output = "r" + std::to_string(i);
        const std::string a = signals[rng() % signals.size()];
        if ((rng() % 5u) == 0u) {
            const GateType type = (rng() & 1u) ? GateType::NOT : GateType::BUF;
            addUnary(netlist, "g_r" + std::to_string(i), type, a, output);
        } else {
            const std::string b = signals[rng() % signals.size()];
            const GateType type = binaryTypes[rng() % binaryTypes.size()];
            addBinary(netlist, "g_r" + std::to_string(i), type, a, b, output);
        }
        signals.push_back(output);
    }

    struct QueryResult {
        std::size_t a;
        std::size_t b;
        eqeng::EquivResult equivalent;
        eqeng::EquivResult constZero;
    };
    std::vector<QueryResult> golden;
    golden.reserve(160);

    eqeng::Primitives primitives(netlist, {}, quietConfig());
    for (int i = 0; i < 160; ++i) {
        const std::size_t a = rng() % signals.size();
        const std::size_t b = rng() % signals.size();
        const auto sa = primitives.resolve(signals[a]);
        const auto sb = primitives.resolve(signals[b]);
        golden.push_back({a, b,
                          primitives.equiv_checked(sa, sb),
                          primitives.is_const_checked(sa, false)});
    }

    primitives.enable_phase_b(true);
    (void)primitives.request_fraig_sweep(false);
    std::size_t mismatches = 0;
    for (const auto& query : golden) {
        const auto sa = primitives.resolve(signals[query.a]);
        const auto sb = primitives.resolve(signals[query.b]);
        if (primitives.equiv_checked(sa, sb) != query.equivalent) ++mismatches;
        if (primitives.is_const_checked(sa, false) != query.constZero) ++mismatches;
    }

    report.check(mismatches == 0,
                 "Phase B matches Phase A on 320 fixed-random DAG queries");
}

} // namespace

int main() {
    TestReport report;
    testIncrementalQueries(report);
    testMutationLifecycle(report);
    testVerifiedFraig(report);
    testFraigMemoryLimitTermination(report);
    testUntrustedEquivalenceReport(report);
    testPhaseADifferential(report);

    std::cout << "Phase B focused regression: " << report.passed
              << " passed, " << report.failed << " failed\n";
    return report.failed == 0 ? 0 : 1;
}
