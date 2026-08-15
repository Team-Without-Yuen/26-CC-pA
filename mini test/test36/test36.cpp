#include <iostream>
#include <string>

#include "include/SATEngine/Primitives.h"
#include "include/core/Netlist.h"

using eqeng::EquivResult;
using eqeng::ModelHealth;
using eqeng::Primitives;
using eqeng::UnsoundModel;

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

Primitives::Config quietConfig() {
    Primitives::Config config;
    config.verbose_rebuild = false;
    return config;
}

Netlist makeAndCircuit() {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryOutput("y");
    const int gate = netlist.addGate("g_and", GateType::AND);
    netlist.connectGateInput(gate, netlist.getNetId("a"));
    netlist.connectGateInput(gate, netlist.getNetId("b"));
    netlist.connectGateOutput(gate, netlist.getNetId("y"));
    return netlist;
}

void testSoundDag(TestReport& report) {
    Netlist netlist = makeAndCircuit();
    Primitives prim(netlist, {}, quietConfig());
    const auto y = prim.resolve("y");

    report.check(prim.model_health() == ModelHealth::Sound,
                 "sound DAG is classified as Sound");
    report.check(prim.model().can_prove(),
                 "sound DAG permits Boolean proof");
    report.check(prim.is_const_checked(y, false) == EquivResult::NotEqual,
                 "sound DAG keeps normal constant analysis");
}

void testFloatingGateInput(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryOutput("y");
    const int floating = netlist.addNet("floating");
    const int gate = netlist.addGate("g_and", GateType::AND);
    netlist.connectGateInput(gate, netlist.getNetId("a"));
    netlist.connectGateInput(gate, floating);
    netlist.connectGateOutput(gate, netlist.getNetId("y"));

    Primitives prim(netlist, {}, quietConfig());
    const auto f = prim.resolve("floating");
    const auto y = prim.resolve("y");

    report.check(prim.model_health() == ModelHealth::Conservative,
                 "floating gate input is classified as Conservative");
    report.check(prim.model().stats().num_free_pis == 1 && prim.is_free_var(f),
                 "floating gate input becomes one free PI");
    report.check(prim.is_const_checked(y, false) == EquivResult::NotEqual,
                 "floating gate input is not silently tied to zero");
}

void testFloatingPrimaryOutput(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryOutput("y");

    Primitives prim(netlist, {}, quietConfig());
    const auto y = prim.resolve("y");

    report.check(prim.model_health() == ModelHealth::Conservative,
                 "PO-only floating net is classified as Conservative");
    report.check(prim.model().stats().num_free_pis == 1 && prim.is_free_var(y),
                 "PO-only floating net becomes a free PI");
    report.check(prim.is_const_checked(y, false) == EquivResult::NotEqual,
                 "PO-only floating net is not silently tied to zero");
}

void testMissingGateInput(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryOutput("y");
    const int gate = netlist.addGate("broken_and", GateType::AND);
    netlist.connectGateOutput(gate, netlist.getNetId("y"));

    Primitives::Config config = quietConfig();
    config.unknown_policy = Primitives::UnknownPolicy::AsEqual;
    Primitives prim(netlist, {}, config);
    const auto y = prim.resolve("y");

    report.check(prim.model_health() == ModelHealth::Invalid,
                 "zero-input combinational gate makes the model Invalid");
    report.check(prim.model_health_message().find("expected at least one") !=
                     std::string::npos,
                 "invalid model reports the n-ary gate arity problem");
    report.check(prim.is_const_checked(y, false) == EquivResult::Unknown,
                 "checked constant query returns Unknown on invalid model");
    report.check(y.tainted() && (!y).tainted(),
                 "signal complement preserves the untrusted taint flag");

    bool boolRejected = false;
    try {
        (void)prim.is_const0(y);
    } catch (const UnsoundModel&) {
        boolRejected = true;
    }
    report.check(boolRejected,
                 "AsEqual cannot turn an invalid model into a true bool result");

    bool cutRejected = false;
    try {
        (void)prim.enumerate_cuts(y);
    } catch (const UnsoundModel&) {
        cutRejected = true;
    }
    report.check(cutRejected,
                 "cut analysis rejects an invalid model");
}

void testInvalidUnaryArity(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryInput("a");
    netlist.addPrimaryInput("b");
    netlist.addPrimaryOutput("y");
    const int gate = netlist.addGate("broken_not", GateType::NOT);
    netlist.connectGateInput(gate, netlist.getNetId("a"));
    netlist.connectGateInput(gate, netlist.getNetId("b"));
    netlist.connectGateOutput(gate, netlist.getNetId("y"));

    Primitives prim(netlist, {}, quietConfig());
    report.check(prim.model_health() == ModelHealth::Invalid,
                 "multi-input NOT makes the model Invalid");
    report.check(prim.model_health_message().find("expected exactly one") !=
                     std::string::npos,
                 "invalid model reports the unary gate arity problem");
}

void testDffInputs(TestReport& report) {
    Netlist valid;
    valid.addPrimaryInput("d");
    valid.addPrimaryOutput("q");
    const int validDff = valid.addGate("ff_ok", GateType::DFF);
    valid.connectGateInput(validDff, valid.getNetId("d"), "D");
    valid.connectGateOutput(validDff, valid.getNetId("q"));

    Primitives validPrim(valid, {}, quietConfig());
    report.check(validPrim.model_health() == ModelHealth::Sound,
                 "unconnected RN/SN are legal inactive DFF controls");

    Netlist invalid;
    invalid.addPrimaryOutput("q");
    const int invalidDff = invalid.addGate("ff_missing_d", GateType::DFF);
    invalid.connectGateOutput(invalidDff, invalid.getNetId("q"));

    Primitives invalidPrim(invalid, {}, quietConfig());
    report.check(invalidPrim.model_health() == ModelHealth::Invalid,
                 "missing DFF D input makes the model Invalid");
    report.check(invalidPrim.model_health_message().find("D input") !=
                     std::string::npos,
                 "invalid DFF model reports the D input problem");
}

void testCombinationalLoopAndCec(TestReport& report) {
    Netlist netlist;
    netlist.addPrimaryOutput("y");
    const int n2 = netlist.addNet("n2");
    const int g1 = netlist.addGate("loop_1", GateType::NOT);
    const int g2 = netlist.addGate("loop_2", GateType::NOT);
    netlist.connectGateInput(g1, n2);
    netlist.connectGateOutput(g1, netlist.getNetId("y"));
    netlist.connectGateInput(g2, netlist.getNetId("y"));
    netlist.connectGateOutput(g2, n2);

    Primitives prim(netlist, {}, quietConfig());
    report.check(prim.model_health() == ModelHealth::Invalid &&
                     prim.model().stats().num_topo_dropped == 2,
                 "combinational loop makes the model Invalid");

    auto before = prim.snapshot();
    const auto cec = prim.equiv_to_snapshot(before);
    report.check(before.valid() && !cec.ok() &&
                     cec.status == EquivResult::Unknown &&
                     cec.untrusted_outputs.size() == 1 &&
                     cec.untrusted_outputs.front() == "y",
                 "CEC localizes an invalid cone and returns Unknown");
}

} // namespace

int main() {
    TestReport report;
    testSoundDag(report);
    testFloatingGateInput(report);
    testFloatingPrimaryOutput(report);
    testMissingGateInput(report);
    testInvalidUnaryArity(report);
    testDffInputs(report);
    testCombinationalLoopAndCec(report);

    std::cout << "AIG health regression: " << report.passed << " passed, "
              << report.failed << " failed\n";
    return report.failed == 0 ? 0 : 1;
}
