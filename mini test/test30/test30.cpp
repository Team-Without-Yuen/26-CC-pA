#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/io/VerilogReader.h"

#include <iostream>
#include <string>
#include <unordered_set>
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

bool coneContainsOnly(
    const Netlist& netlist,
    const ConeResult& cone,
    const std::unordered_set<GateType>& allowed)
{
    for (int gateId : netlist.getConeGateIds(cone)) {
        if (!netlist.isValidGateId(gateId) || netlist.isGateRemoved(gateId)) continue;
        const GateType type = netlist.getGate(gateId).type;
        if (type != GateType::DFF && allowed.find(type) == allowed.end()) {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::string circuitPath =
        argc > 1 ? argv[1] : "mini test/test30/rewrite_scope_circuit.v";

    TestReport report;
    Netlist netlist;
    VerilogReader reader;
    const bool loaded = reader.read(circuitPath, netlist);
    report.check(loaded, "rewrite-scope circuit loads");
    if (!loaded) return 1;

    const RewriteScopeResolution qScope =
        resolveRewriteScope(netlist, TargetScope::NET_FANIN, "q");
    report.check(
        qScope.ok &&
        !qScope.resolvedThroughDffDataPin &&
        qScope.resolvedRootNetName == "q" &&
        qScope.cone.rootNetIds.size() == 1 &&
        qScope.cone.rootNetIds.front() == netlist.getNetId("q"),
        "DFF.Q net fanin rewrite stops at the sequential boundary");
    report.check(
        netlist.getConeGateCount(qScope.cone) == 0,
        "DFF.Q net fanin cone contains no combinational gates");

    const RewriteScopeResolution ffScope =
        resolveRewriteScope(netlist, TargetScope::GATE_FANIN, "ff0");
    report.check(
        ffScope.ok &&
        ffScope.resolvedThroughDffDataPin &&
        ffScope.resolvedRootNetName == "d" &&
        ffScope.cone.rootNetIds.size() == 1 &&
        ffScope.cone.rootNetIds.front() == netlist.getNetId("d"),
        "explicit DFF gate fanin rewrite resolves to the D-pin data cone");
    report.check(
        netlist.getConeGateCount(ffScope.cone) == 3,
        "explicit DFF gate fanin cone contains the D-input combinational logic");

    const RewriteScopeResolution yScope =
        resolveRewriteScope(netlist, TargetScope::NET_FANIN, "y");
    report.check(
        yScope.ok &&
        !yScope.resolvedThroughDffDataPin &&
        yScope.resolvedRootNetName == "y" &&
        netlist.getConeGateCount(yScope.cone) == 1,
        "ordinary net fanin rewrite keeps its original root");

    const RewriteScopeResolution missingScope =
        resolveRewriteScope(netlist, TargetScope::NET_FANIN, "missing");
    report.check(
        !missingScope.ok && !missingScope.message.empty(),
        "missing rewrite target returns an explicit failure");

    TechMapper mapper;
    const TechMapReport mapping = mapper.convertToBasis(
        netlist,
        TargetScope::NET_FANIN,
        "q",
        {GateType::NOR, GateType::NOT});
    const RewriteScopeResolution mappedScope =
        resolveRewriteScope(netlist, TargetScope::NET_FANIN, "q");
    report.check(
        mapping.status == TechMapStatus::SUCCESS &&
        !mapping.changed &&
        mappedScope.ok &&
        netlist.getConeGateCount(mappedScope.cone) == 0 &&
        coneContainsOnly(
            netlist,
            mappedScope.cone,
            {GateType::NOR, GateType::NOT}),
        "basis conversion over a DFF.Q net fanin cone is a safe no-op");

    const TechMapReport dffGateMapping = mapper.convertToBasis(
        netlist,
        TargetScope::GATE_FANIN,
        "ff0",
        {GateType::NOR, GateType::NOT});
    const RewriteScopeResolution mappedGateScope =
        resolveRewriteScope(netlist, TargetScope::GATE_FANIN, "ff0");
    report.check(
        dffGateMapping.status == TechMapStatus::SUCCESS &&
        mappedGateScope.ok &&
        coneContainsOnly(
            netlist,
            mappedGateScope.cone,
            {GateType::NOR, GateType::NOT}),
        "basis conversion over an explicit DFF gate fanin cone rewrites the D-input logic");
    report.check(
        netlist.getGate(netlist.getGateId("ff0")).type == GateType::DFF &&
        netlist.validateStructure(),
        "DFF boundary and graph structure remain valid after mapping");

    std::cout << "Summary: " << report.passed
              << " passed, " << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
