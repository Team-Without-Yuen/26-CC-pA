#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"

#include <chrono>
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

const DffInputPatternReport* findDff(const SequentialPatternReportSet& report,
                                    const std::string& name) {
    for (const auto& dff : report.reports) {
        if (dff.dffName == name) {
            return &dff;
        }
    }
    return nullptr;
}

const DffInputPattern* firstConfirmed(const DffInputPatternReport* report) {
    if (report == nullptr) {
        return nullptr;
    }
    for (const auto& pattern : report->patterns) {
        if (pattern.confirmed) {
            return &pattern;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        Netlist netlist;
        VerilogReader reader;
        const auto loadStart = std::chrono::steady_clock::now();
        if (!reader.read(argv[1], netlist)) {
            std::cerr << "Failed to load: " << argv[1] << "\n";
            return 1;
        }
        const auto queryStart = std::chrono::steady_clock::now();
        SequentialPatternQuery query;
        const SequentialPatternReportSet report = netlist.runSequentialPatternQuery(query);
        const auto end = std::chrono::steady_clock::now();

        const double loadSeconds =
            std::chrono::duration<double>(queryStart - loadStart).count();
        const double querySeconds =
            std::chrono::duration<double>(end - queryStart).count();
        std::cout << "status=" << report.status << "\n"
                  << "total_dffs=" << report.totalDffCount << "\n"
                  << "analyzed_dffs=" << report.analyzedDffCount << "\n"
                  << "matched_dffs=" << report.matchedDffCount << "\n"
                  << "candidate_dffs=" << report.candidateDffCount << "\n"
                  << "load_seconds=" << loadSeconds << "\n"
                  << "query_seconds=" << querySeconds << "\n";
        return report.ok ? 0 : 1;
    }

    TestReport tests;
    Netlist netlist;
    VerilogReader reader;
    tests.check(
        reader.read("mini test/test19/sequential_pattern_circuit.v", netlist),
        "load sequential pattern circuit");

    SequentialPatternQuery query;
    query.type = SequentialPatternQueryType::DffEnableHold;
    const SequentialPatternReportSet all = netlist.runSequentialPatternQuery(query);

    tests.check(all.ok && all.status == "OK", "batch query succeeds");
    tests.check(all.totalDffCount == 8 && all.analyzedDffCount == 8,
                "all eight DFFs are analyzed");
    tests.check(all.matchedDffCount == 6,
                "six canonical feedback MUX DFFs are counted once");
    tests.check(all.candidateDffCount == 6,
                "AND-only DFF is not counted as an enable/hold candidate");

    const DffInputPattern* high = firstConfirmed(findDff(all, "ff_high"));
    tests.check(high != nullptr && high->enableNetName == "en" &&
                    high->dataNetName == "data" && high->activeLevel == 1,
                "active-high OR/AND MUX is recognized");

    const DffInputPattern* low = firstConfirmed(findDff(all, "ff_low"));
    tests.check(low != nullptr && low->enableNetName == "en_n" &&
                    low->activeLevel == 0,
                "active-low OR/AND MUX is recognized");

    const DffInputPattern* nandPattern = firstConfirmed(findDff(all, "ff_nand"));
    tests.check(nandPattern != nullptr && nandPattern->enableNetName == "en" &&
                    nandPattern->activeLevel == 1,
                "NAND/NAND canonical MUX is recognized");

    const DffInputPattern* norPattern = firstConfirmed(findDff(all, "ff_nor"));
    tests.check(norPattern != nullptr && norPattern->enableNetName == "en" &&
                    norPattern->activeLevel == 1,
                "NOR/NOR canonical MUX is recognized");

    const DffInputPattern* posPattern = firstConfirmed(findDff(all, "ff_pos"));
    tests.check(posPattern != nullptr && posPattern->enableNetName == "en" &&
                    posPattern->activeLevel == 1 &&
                    findDff(all, "ff_pos")->patterns.size() == 1,
                "AND/OR canonical POS MUX is recognized without an AND-only duplicate");

    const DffInputPattern* invertedData = firstConfirmed(findDff(all, "ff_nand_inv"));
    tests.check(invertedData != nullptr && invertedData->dataNetName == "data" &&
                    invertedData->dataBranchNetName == "not_data" &&
                    invertedData->dataInverted,
                "inverted data literal preserves base and branch net semantics");

    const DffInputPatternReport* andReport = findDff(all, "ff_and");
    tests.check(andReport != nullptr && !andReport->matched &&
                    andReport->status == "DATA_GATING_WITHOUT_HOLD_FEEDBACK" &&
                    andReport->patterns.size() == 1 &&
                    andReport->patterns.front().kind ==
                        DffInputPatternKind::DataGatingWithoutHoldFeedback &&
                    !andReport->patterns.front().semanticsPending &&
                    !andReport->patterns.front().confirmed,
                "AND-only structure is a non-match data-gating diagnostic");

    const DffInputPatternReport* plainReport = findDff(all, "ff_plain");
    tests.check(plainReport != nullptr && plainReport->ok &&
                    plainReport->status == "NO_PATTERN" &&
                    plainReport->patterns.empty(),
                "plain D input is not misclassified");

    SequentialPatternQuery one;
    one.dffName = "ff_nand_inv";
    one.verifyCanonicalMatchesWithSat = true;
    const SequentialPatternReportSet verified = netlist.runSequentialPatternQuery(one);
    const DffInputPattern* verifiedPattern = firstConfirmed(findDff(verified, "ff_nand_inv"));
    tests.check(verified.ok && verified.matchedDffCount == 1 &&
                    verifiedPattern != nullptr &&
                    verifiedPattern->holdFunctionallyProven &&
                    verifiedPattern->loadFunctionallyProven &&
                    verifiedPattern->solverStatus == "PROVEN",
                "optional conditional SAT verifies hold and load cofactors");

    SequentialPatternQuery missing;
    missing.dffName = "missing_ff";
    const SequentialPatternReportSet invalid = netlist.runSequentialPatternQuery(missing);
    tests.check(!invalid.ok && invalid.status == "DFF_NOT_FOUND",
                "missing DFF is reported explicitly");

    std::cout << "Summary: " << tests.passed << " passed, "
              << tests.failed << " failed.\n";
    return tests.failed == 0 ? 0 : 1;
}
