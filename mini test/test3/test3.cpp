#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <algorithm>
#include <iostream>
#include <string>
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

bool containsString(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

bool loadCircuit(const std::string& path, Netlist& netlist) {
    VerilogReader reader;
    return reader.read(path, netlist);
}

void testEquivalentRewrite(TestReport& report, const Netlist& original, const Netlist& same) {
    const Netlist::WholeDesignEquivalenceReport eq =
        same.checkWholeDesignEquivalence(original);
    report.check(eq.ok &&
                 eq.equivalent &&
                 eq.method == Netlist::EquivalenceCheckMethod::WholeDesignSat &&
                 eq.comparedOutputCount == 2 &&
                 containsString(eq.matchedOutputNames, "y") &&
                 containsString(eq.matchedOutputNames, "z") &&
                 eq.mismatchedOutputNames.empty() &&
                 eq.missingInputNames.empty() &&
                 eq.extraInputNames.empty() &&
                 eq.missingOutputNames.empty() &&
                 eq.extraOutputNames.empty(),
                 "test3 whole_design_equivalence equivalent rewrite");
}

void testMismatchedOutput(TestReport& report, const Netlist& original, const Netlist& wrong) {
    const Netlist::WholeDesignEquivalenceReport eq =
        wrong.checkWholeDesignEquivalence(original);
    report.check(eq.ok &&
                 !eq.equivalent &&
                 eq.comparedOutputCount == 2 &&
                 containsString(eq.matchedOutputNames, "y") &&
                 containsString(eq.mismatchedOutputNames, "z") &&
                 eq.missingOutputNames.empty() &&
                 eq.extraOutputNames.empty(),
                 "test3 whole_design_equivalence detects mismatched PO");
}

void testMissingOutput(TestReport& report, const Netlist& original, const Netlist& missing) {
    const Netlist::WholeDesignEquivalenceReport eq =
        missing.checkWholeDesignEquivalence(original);
    report.check(!eq.ok &&
                 !eq.equivalent &&
                 eq.comparedOutputCount == 0 &&
                 containsString(eq.missingOutputNames, "z") &&
                 eq.extraOutputNames.empty(),
                 "test3 whole_design_equivalence reports missing PO");
}

void testExtraOutput(TestReport& report, const Netlist& original, const Netlist& extra) {
    const Netlist::WholeDesignEquivalenceReport eq =
        extra.checkWholeDesignEquivalence(original);
    report.check(!eq.ok &&
                 !eq.equivalent &&
                 eq.comparedOutputCount == 0 &&
                 eq.missingOutputNames.empty() &&
                 containsString(eq.extraOutputNames, "extra"),
                 "test3 whole_design_equivalence reports extra PO");
}

void testUndrivenOutputSelfEquivalence(TestReport& report, const Netlist& original) {
    const Netlist::WholeDesignEquivalenceReport eq =
        original.checkWholeDesignEquivalence(original);
    report.check(eq.ok &&
                 eq.equivalent &&
                 eq.comparedOutputCount == 2 &&
                 containsString(eq.matchedOutputNames, "floating") &&
                 eq.mismatchedOutputNames.empty(),
                 "test3 whole_design_equivalence shares same-name undriven leaves");
}

} // namespace

int main() {
    TestReport report;

    Netlist original;
    Netlist same;
    Netlist wrong;
    Netlist missing;
    Netlist extra;
    Netlist undriven;

    report.check(loadCircuit("mini test/test3/equiv_original.v", original), "test3 load equiv_original.v");
    report.check(loadCircuit("mini test/test3/equiv_same.v", same), "test3 load equiv_same.v");
    report.check(loadCircuit("mini test/test3/equiv_wrong.v", wrong), "test3 load equiv_wrong.v");
    report.check(loadCircuit("mini test/test3/equiv_missing_output.v", missing), "test3 load equiv_missing_output.v");
    report.check(loadCircuit("mini test/test3/equiv_extra_output.v", extra), "test3 load equiv_extra_output.v");
    report.check(loadCircuit("mini test/test3/equiv_undriven_output.v", undriven), "test3 load equiv_undriven_output.v");
    if (report.failed != 0) {
        std::cout << "\nCannot continue because one or more equivalence test circuits could not be read.\n";
        return report.failed;
    }

    testEquivalentRewrite(report, original, same);
    testMismatchedOutput(report, original, wrong);
    testMissingOutput(report, original, missing);
    testExtraOutput(report, original, extra);
    testUndrivenOutputSelfEquivalence(report, undriven);

    std::cout << "\nSummary: " << report.passed << " passed, "
              << report.failed << " failed.\n";
    return report.failed == 0 ? 0 : 1;
}
