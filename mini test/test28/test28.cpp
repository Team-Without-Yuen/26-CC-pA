#include "include/core/Netlist.h"
#include "include/core/FunctionalPatternEngine.h"
#include "include/core/RequestTimeBudget.h"
#include "include/SATEngine/Primitives.h"
#include "include/io/VerilogReader.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
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

const DffInputPatternReport* findDff(
    const SequentialPatternReportSet& report,
    const std::string& name) {
    for (const DffInputPatternReport& dff : report.reports) {
        if (dff.dffName == name) {
            return &dff;
        }
    }
    return nullptr;
}

const DffInputPattern* findConfirmedControl(
    const DffInputPatternReport* report,
    const std::string& controlName) {
    if (report == nullptr) {
        return nullptr;
    }
    for (const DffInputPattern& pattern : report->patterns) {
        if (pattern.confirmed && pattern.enableNetName == controlName) {
            return &pattern;
        }
    }
    return nullptr;
}

int addTestGate(
    Netlist& netlist,
    const std::string& name,
    GateType type,
    const std::vector<int>& inputs,
    int output) {
    const int gateId = netlist.addGate(name, type);
    for (int input : inputs) {
        netlist.connectGateInput(gateId, input);
    }
    netlist.connectGateOutput(gateId, output);
    return gateId;
}

FunctionalPatternSearchResult runFindAnyAfterUnsupportedCandidate() {
    Netlist netlist;
    netlist.addPrimaryInput("edge_bad");
    const int bad = netlist.getNetId("edge_bad");
    const int unknownGate = netlist.addGate("edge_unknown", GateType::UNKNOWN);
    netlist.connectGateOutput(unknownGate, bad);

    netlist.addPrimaryInput("edge_a");
    netlist.addPrimaryInput("edge_b");
    netlist.addPrimaryInput("edge_data");
    const int a = netlist.getNetId("edge_a");
    const int b = netlist.getNetId("edge_b");
    const int data = netlist.getNetId("edge_data");
    const int q = netlist.addNet("edge_q");
    const int select = netlist.addNet("edge_select");
    const int delta = netlist.addNet("edge_delta");
    const int masked = netlist.addNet("edge_masked");
    const int d = netlist.addNet("edge_d");

    addTestGate(netlist, "edge_select_xor", GateType::XOR, {a, b}, select);
    addTestGate(netlist, "edge_delta_xor", GateType::XOR, {q, data}, delta);
    addTestGate(netlist, "edge_mask", GateType::AND, {select, delta}, masked);
    addTestGate(netlist, "edge_mux", GateType::XOR, {q, masked}, d);
    const int dff = netlist.addGate("edge_ff", GateType::DFF);
    netlist.connectGateInput(dff, d, "D");
    netlist.connectGateOutput(dff, q);

    FunctionalPatternContext context =
        buildSequentialPatternContext(netlist, dff, d, q);
    // Keep an unrelated unsupported candidate ahead of the valid internal
    // selector while preserving the real D-input gate cone for SAT.
    context.coneNetIds = {d, q, bad, select};

    FunctionalPatternSearchOptions options;
    options.maxCandidates = 8;
    options.maxMatches = 1;
    options.findAllMatches = false;
    options.resolveDataNets = false;
    options.timeLimitSeconds = 2.0;

    FunctionalPatternEngine engine;
    eqeng::Primitives primitives(netlist);
    primitives.enable_phase_b(true);
    request_time_budget::RequestDeadline deadline(options.timeLimitSeconds);
    return engine.search(
        FunctionalPatternKind::MuxHold,
        netlist,
        primitives,
        context,
        options,
        deadline);
}

int runBenchmark(int argc, char** argv) {
    const std::string path = argv[2];
    const size_t sampleCount = argc > 3
        ? static_cast<size_t>(std::max(1, std::atoi(argv[3])))
        : 12;
    const double perDffSeconds = argc > 4
        ? std::max(0.001, std::atof(argv[4]))
        : 0.25;
    const double allDffSeconds = argc > 5
        ? std::max(0.001, std::atof(argv[5]))
        : 2.0;
    const size_t maxCandidates = argc > 6
        ? static_cast<size_t>(std::max(1, std::atoi(argv[6])))
        : 64;

    Netlist netlist;
    VerilogReader reader;
    const auto loadStart = std::chrono::steady_clock::now();
    if (!reader.read(path, netlist)) {
        std::cerr << "Failed to load: " << path << "\n";
        return 1;
    }
    const auto loadEnd = std::chrono::steady_clock::now();

    SequentialPatternQuery canonicalQuery;
    const auto canonicalStart = std::chrono::steady_clock::now();
    const SequentialPatternReportSet canonical =
        netlist.runSequentialPatternQuery(canonicalQuery);
    const auto canonicalEnd = std::chrono::steady_clock::now();

    std::vector<std::string> unmatched;
    for (const DffInputPatternReport& dff : canonical.reports) {
        if (!dff.matched && dff.patterns.empty()) {
            unmatched.push_back(dff.dffName);
        }
    }

    const size_t actualSamples = std::min(sampleCount, unmatched.size());
    size_t sampleMatches = 0;
    size_t sampleTimeouts = 0;
    size_t sampleComplete = 0;
    size_t sampleCandidates = 0;
    size_t sampleExamined = 0;
    size_t sampleSatChecks = 0;
    const auto sampleStart = std::chrono::steady_clock::now();
    for (size_t i = 0; i < actualSamples; ++i) {
        SequentialPatternQuery query;
        query.dffName = unmatched[i];
        query.includeAndGatedCandidates = false;
        query.enableFunctionalFallback = true;
        query.maxFunctionalCandidates = maxCandidates;
        query.maxFunctionalMatchesPerDff = 8;
        query.findAllFunctionalMatches = false;
        query.resolveFunctionalDataNets = false;
        query.functionalPerDffTimeLimitSeconds = perDffSeconds;
        query.functionalTimeLimitSeconds = perDffSeconds;
        const SequentialPatternReportSet report =
            netlist.runSequentialPatternQuery(query);
        sampleMatches += report.matchedDffCount;
        sampleTimeouts += report.timedOut ? 1 : 0;
        sampleComplete += report.complete ? 1 : 0;
        sampleCandidates += report.functionalCandidateCount;
        sampleExamined += report.functionalCandidatesExamined;
        sampleSatChecks += report.functionalSatCheckCount;
        std::cout << "sample dff=" << unmatched[i]
                  << " matched=" << report.matchedDffCount
                  << " complete=" << report.complete
                  << " timeout=" << report.timedOut
                  << " candidates=" << report.functionalCandidateCount
                  << " examined=" << report.functionalCandidatesExamined
                  << " sat_checks=" << report.functionalSatCheckCount
                  << " simulated=" << report.functionalSimulationCandidateCount
                  << " sim_rejected="
                  << report.functionalSimulationRejectedCandidateCount
                  << " elapsed=" << report.elapsedSeconds << "\n";
    }
    const auto sampleEnd = std::chrono::steady_clock::now();

    SequentialPatternQuery allQuery;
    allQuery.includeAndGatedCandidates = true;
    allQuery.enableFunctionalFallback = true;
    allQuery.maxFunctionalCandidates = maxCandidates;
    allQuery.maxFunctionalMatchesPerDff = 8;
    allQuery.findAllFunctionalMatches = false;
    allQuery.resolveFunctionalDataNets = false;
    allQuery.functionalPerDffTimeLimitSeconds = perDffSeconds;
    allQuery.functionalTimeLimitSeconds = allDffSeconds;
    const auto allStart = std::chrono::steady_clock::now();
    const SequentialPatternReportSet all =
        netlist.runSequentialPatternQuery(allQuery);
    const auto allEnd = std::chrono::steady_clock::now();

    std::vector<size_t> searchableCosts;
    std::vector<size_t> examinedCosts;
    std::vector<size_t> unexaminedCosts;
    std::vector<size_t> inconclusiveCosts;
    size_t budgetSkippedDffs = 0;
    for (const DffInputPatternReport& dff : all.reports) {
        if (!dff.functionalFallbackAttempted) {
            continue;
        }
        const bool skippedBeforeSearch =
            dff.functionalFallbackTimedOut &&
            !dff.functionalFallbackComplete &&
            dff.functionalCandidateCount == 0 &&
            dff.functionalSimulationCandidateCount == 0 &&
            dff.functionalSatCheckCount == 0;
        if (skippedBeforeSearch) {
            ++budgetSkippedDffs;
            continue;
        }
        searchableCosts.push_back(dff.functionalSearchableCandidateCount);
        examinedCosts.push_back(dff.functionalCandidatesExamined);
        unexaminedCosts.push_back(dff.functionalUnexaminedCandidateCount);
        inconclusiveCosts.push_back(dff.functionalInconclusiveCandidateCount);
    }

    const auto percentile = [](std::vector<size_t> values, size_t percent) {
        if (values.empty()) {
            return size_t{0};
        }
        std::sort(values.begin(), values.end());
        const size_t index =
            (percent * (values.size() - 1) + 99) / 100;
        return values[index];
    };
    const auto nonzeroCount = [](const std::vector<size_t>& values) {
        return static_cast<size_t>(std::count_if(
            values.begin(), values.end(), [](size_t value) { return value != 0; }));
    };

    const auto seconds = [](auto begin, auto end) {
        return std::chrono::duration<double>(end - begin).count();
    };
    std::cout << std::fixed << std::setprecision(6)
              << "benchmark_summary\n"
              << "load_seconds=" << seconds(loadStart, loadEnd) << "\n"
              << "canonical_seconds=" << seconds(canonicalStart, canonicalEnd) << "\n"
              << "total_dffs=" << canonical.totalDffCount << "\n"
              << "canonical_matched_dffs=" << canonical.matchedDffCount << "\n"
              << "unmatched_no_candidate_dffs=" << unmatched.size() << "\n"
              << "sample_count=" << actualSamples << "\n"
              << "sample_wall_seconds=" << seconds(sampleStart, sampleEnd) << "\n"
              << "sample_matched_dffs=" << sampleMatches << "\n"
              << "sample_complete_queries=" << sampleComplete << "\n"
              << "sample_timeouts=" << sampleTimeouts << "\n"
              << "sample_candidates=" << sampleCandidates << "\n"
              << "sample_examined=" << sampleExamined << "\n"
              << "sample_sat_checks=" << sampleSatChecks << "\n"
               << "all_budget_seconds=" << allDffSeconds << "\n"
               << "all_candidate_quota=" << maxCandidates << "\n"
              << "all_wall_seconds=" << seconds(allStart, allEnd) << "\n"
              << "all_status=" << all.status << "\n"
              << "all_complete=" << all.complete << "\n"
              << "all_timed_out=" << all.timedOut << "\n"
              << "all_matched_dffs=" << all.matchedDffCount << "\n"
              << "all_functional_matches=" << all.functionalMatchCount << "\n"
              << "all_candidates=" << all.functionalCandidateCount << "\n"
              << "all_examined=" << all.functionalCandidatesExamined << "\n"
              << "simulation_patterns=" << all.functionalSimulationPatternCount << "\n"
              << "simulation_seconds=" << all.functionalSimulationSeconds << "\n"
              << "all_simulated_candidates="
              << all.functionalSimulationCandidateCount << "\n"
               << "all_simulation_rejected="
               << all.functionalSimulationRejectedCandidateCount << "\n"
               << "all_sat_checks=" << all.functionalSatCheckCount << "\n"
               << "cost_search_started_dffs=" << searchableCosts.size() << "\n"
               << "cost_budget_skipped_dffs=" << budgetSkippedDffs << "\n"
               << "cost_searchable_nonzero_dffs=" << nonzeroCount(searchableCosts) << "\n"
               << "cost_searchable_p50=" << percentile(searchableCosts, 50) << "\n"
               << "cost_searchable_p90=" << percentile(searchableCosts, 90) << "\n"
               << "cost_searchable_p99=" << percentile(searchableCosts, 99) << "\n"
               << "cost_searchable_max=" << percentile(searchableCosts, 100) << "\n"
               << "cost_examined_p50=" << percentile(examinedCosts, 50) << "\n"
               << "cost_examined_p90=" << percentile(examinedCosts, 90) << "\n"
               << "cost_examined_p99=" << percentile(examinedCosts, 99) << "\n"
               << "cost_examined_max=" << percentile(examinedCosts, 100) << "\n"
               << "cost_unexamined_nonzero_dffs=" << nonzeroCount(unexaminedCosts) << "\n"
               << "cost_unexamined_p90=" << percentile(unexaminedCosts, 90) << "\n"
               << "cost_unexamined_p99=" << percentile(unexaminedCosts, 99) << "\n"
               << "cost_unexamined_max=" << percentile(unexaminedCosts, 100) << "\n"
               << "cost_inconclusive_nonzero_dffs=" << nonzeroCount(inconclusiveCosts) << "\n"
               << "cost_inconclusive_max=" << percentile(inconclusiveCosts, 100) << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--benchmark") {
        return runBenchmark(argc, argv);
    }

    TestReport test;
    Netlist netlist;
    VerilogReader reader;
    test.check(
        reader.read("mini test/test28/functional_pattern_circuit.v", netlist),
        "load functional pattern circuit");

    SequentialPatternQuery defaultQuery;
    const SequentialPatternReportSet defaultReport =
        netlist.runSequentialPatternQuery(defaultQuery);
    const DffInputPatternReport* defaultXor = findDff(defaultReport, "ff_xor");
    test.check(defaultReport.ok && defaultReport.complete &&
                   defaultXor != nullptr && !defaultXor->matched &&
                   !defaultXor->functionalFallbackAttempted,
               "functional fallback stays disabled by default");

    SequentialPatternQuery query;
    query.enableFunctionalFallback = true;
    query.functionalTimeLimitSeconds = 10.0;
    query.maxFunctionalCandidates = 64;
    query.maxFunctionalMatchesPerDff = 16;
    const SequentialPatternReportSet report = netlist.runSequentialPatternQuery(query);

    const DffInputPatternReport* xorReport = findDff(report, "ff_xor");
    const DffInputPattern* xorPattern = findConfirmedControl(xorReport, "en");
    test.check(xorPattern != nullptr &&
                   xorPattern->detectionMethod ==
                       SequentialPatternDetectionMethod::FunctionalCofactorSat &&
                   xorPattern->activeLevel == 1 && xorPattern->holdLevel == 0 &&
                   xorPattern->dataFunctionResolved &&
                   xorPattern->dataNetName == "data" &&
                   xorPattern->holdFunctionallyProven &&
                   xorPattern->loadFunctionallyProven,
               "XOR-restructured MUX is proven by functional cofactors");

    const DffInputPatternReport* internalReport = findDff(report, "ff_internal");
    const DffInputPattern* internalPattern =
        findConfirmedControl(internalReport, "internal_sel");
    test.check(internalPattern != nullptr && internalPattern->activeLevel == 1 &&
                   internalPattern->dataNetName == "data_b",
               "internal combinational selector is recognized");
    test.check(internalReport != nullptr && internalReport->patterns.size() >= 2,
               "nested enable interpretations remain separately reportable");

    const DffInputPatternReport* andReport = findDff(report, "ff_and");
    test.check(andReport != nullptr && !andReport->matched &&
                   andReport->patterns.size() == 1 &&
                   andReport->patterns.front().semanticsPending,
               "AND-only D input remains an unconfirmed candidate");

    const DffInputPatternReport* plainReport = findDff(report, "ff_plain");
    test.check(plainReport != nullptr && !plainReport->matched &&
                   plainReport->patterns.empty(),
               "plain D input remains a negative case");

    const DffInputPatternReport* canonicalReport = findDff(report, "ff_canonical");
    test.check(canonicalReport != nullptr && canonicalReport->matched &&
                   !canonicalReport->functionalFallbackAttempted &&
                   canonicalReport->patterns.front().holdLevel == 0 &&
                   canonicalReport->patterns.front().dataFunctionResolved,
               "canonical fast path remains preferred and reports complete roles");

    const DffInputPatternReport* unreachableReport =
        findDff(report, "ff_unreachable");
    test.check(unreachableReport != nullptr && !unreachableReport->matched,
               "unreachable control levels cannot create vacuous MUX matches");

    test.check(report.functionalSatCheckCount > 0 &&
                   report.functionalCandidatesExamined > 0 &&
                   report.functionalSimulationPatternCount == 256 &&
                   report.functionalSimulationCandidateCount > 0 &&
                   report.functionalSimulationRejectedCandidateCount > 0 &&
                   report.functionalMatchCount > 0 && report.elapsedSeconds > 0.0,
               "aggregate functional-search accounting is populated");

    size_t summedCandidates = 0;
    size_t summedSearchable = 0;
    size_t summedExamined = 0;
    size_t summedUnexamined = 0;
    size_t summedInconclusive = 0;
    bool perDffCostInvariant = true;
    for (const DffInputPatternReport& dff : report.reports) {
        summedCandidates += dff.functionalCandidateCount;
        summedSearchable += dff.functionalSearchableCandidateCount;
        summedExamined += dff.functionalCandidatesExamined;
        summedUnexamined += dff.functionalUnexaminedCandidateCount;
        summedInconclusive += dff.functionalInconclusiveCandidateCount;
        perDffCostInvariant = perDffCostInvariant &&
            dff.functionalCandidateCount ==
                dff.functionalSimulationRejectedCandidateCount +
                    dff.functionalSearchableCandidateCount &&
            dff.functionalCandidatesExamined <=
                dff.functionalSearchableCandidateCount &&
            dff.functionalUnexaminedCandidateCount ==
                dff.functionalSearchableCandidateCount -
                    dff.functionalCandidatesExamined &&
            dff.functionalInconclusiveCandidateCount <=
                dff.functionalCandidatesExamined;
    }
    test.check(perDffCostInvariant &&
                   report.functionalCandidateCount == summedCandidates &&
                   report.functionalSearchableCandidateCount == summedSearchable &&
                   report.functionalCandidatesExamined == summedExamined &&
                   report.functionalUnexaminedCandidateCount == summedUnexamined &&
                   report.functionalInconclusiveCandidateCount == summedInconclusive,
               "functional cost invariants and aggregate sums are exact");

    SequentialPatternQuery countOnly;
    countOnly.dffName = "ff_internal";
    countOnly.enableFunctionalFallback = true;
    countOnly.findAllFunctionalMatches = false;
    countOnly.resolveFunctionalDataNets = false;
    countOnly.functionalTimeLimitSeconds = 2.0;
    const SequentialPatternReportSet countOnlyReport =
        netlist.runSequentialPatternQuery(countOnly);
    const DffInputPatternReport* countOnlyDff =
        findDff(countOnlyReport, "ff_internal");
    test.check(countOnlyReport.complete && countOnlyReport.matchedDffCount == 1 &&
                   countOnlyReport.functionalMatchCount == 1 &&
                   countOnlyReport.functionalCandidatesExamined == 1 &&
                   countOnlyReport.functionalSimulationCandidateCount >= 1 &&
                   countOnlyReport.functionalSatCheckCount == 1 &&
                   countOnlyDff != nullptr && countOnlyDff->patterns.size() == 1 &&
                   !countOnlyDff->patterns.front().dataSearchAttempted &&
                   !countOnlyDff->patterns.front().dataSearchComplete,
               "FindAny count mode stops after one proof and skips named-data search");

    const FunctionalPatternSearchResult unsupportedThenMatch =
        runFindAnyAfterUnsupportedCandidate();
    test.check(unsupportedThenMatch.complete && !unsupportedThenMatch.timedOut &&
                   unsupportedThenMatch.matches.size() == 1 &&
                   unsupportedThenMatch.searchableCandidateCount == 2 &&
                   unsupportedThenMatch.candidatesExamined == 2 &&
                   unsupportedThenMatch.unexaminedCandidateCount == 0 &&
                   unsupportedThenMatch.inconclusiveCandidateCount == 1 &&
                   !unsupportedThenMatch.candidateLimitReached,
               "FindAny proof completes existence after an earlier unsupported candidate");

    SequentialPatternQuery ranked = countOnly;
    ranked.dffName = "ff_ranked";
    ranked.maxFunctionalCandidates = 1;
    const SequentialPatternReportSet rankedReport =
        netlist.runSequentialPatternQuery(ranked);
    const DffInputPatternReport* rankedDff = findDff(rankedReport, "ff_ranked");
    test.check(rankedReport.complete && rankedReport.matchedDffCount == 1 &&
                   rankedReport.functionalCandidatesExamined == 1 &&
                   rankedReport.functionalSimulationRejectedCandidateCount > 0 &&
                   rankedDff != nullptr &&
                   !rankedDff->functionalCandidateLimitReached &&
                   rankedDff->patterns.size() == 1 &&
                   rankedDff->patterns.front().enableNetName == "rank_sel",
               "simulation ranking prioritizes the internal hold control before PI decoys");

    SequentialPatternQuery safeReject = countOnly;
    safeReject.dffName = "ff_safe_reject";
    safeReject.findAllFunctionalMatches = true;
    safeReject.maxFunctionalCandidates = 1;
    const SequentialPatternReportSet safeRejectReport =
        netlist.runSequentialPatternQuery(safeReject);
    const DffInputPatternReport* safeRejectDff =
        findDff(safeRejectReport, "ff_safe_reject");
    test.check(safeRejectReport.complete && !safeRejectReport.timedOut &&
                   safeRejectReport.matchedDffCount == 0 &&
                   safeRejectReport.functionalCandidateCount > 1 &&
                   safeRejectReport.functionalSearchableCandidateCount == 0 &&
                   safeRejectReport.functionalCandidatesExamined == 0 &&
                   safeRejectReport.functionalUnexaminedCandidateCount == 0 &&
                   safeRejectReport.functionalInconclusiveCandidateCount == 0 &&
                   safeRejectReport.functionalSatCheckCount == 0 &&
                   safeRejectReport.functionalSimulationRejectedCandidateCount ==
                       safeRejectReport.functionalCandidateCount &&
                   safeRejectDff != nullptr &&
                   !safeRejectDff->functionalCandidateLimitReached &&
                   safeRejectDff->status == "NO_PATTERN",
               "simulation-safe Rejects complete without consuming candidate quota");

    SequentialPatternQuery noSimulation = countOnly;
    noSimulation.dffName = "ff_xor";
    noSimulation.enableFunctionalSimulationFilter = false;
    const SequentialPatternReportSet noSimulationReport =
        netlist.runSequentialPatternQuery(noSimulation);
    test.check(noSimulationReport.complete &&
                   noSimulationReport.matchedDffCount == 1 &&
                   noSimulationReport.functionalSimulationCandidateCount == 0 &&
                   noSimulationReport.functionalSatCheckCount >= 4,
               "disabling simulation preserves the SAT-proven result");

    SequentialPatternQuery boundedData;
    boundedData.dffName = "ff_internal";
    boundedData.enableFunctionalFallback = true;
    boundedData.findAllFunctionalMatches = false;
    boundedData.resolveFunctionalDataNets = true;
    boundedData.maxFunctionalDataCandidatesPerMatch = 1;
    boundedData.functionalTimeLimitSeconds = 2.0;
    const SequentialPatternReportSet boundedDataReport =
        netlist.runSequentialPatternQuery(boundedData);
    const DffInputPatternReport* boundedDataDff =
        findDff(boundedDataReport, "ff_internal");
    test.check(boundedDataReport.matchedDffCount == 1 &&
                   boundedDataDff != nullptr &&
                   boundedDataDff->patterns.front().dataSearchAttempted &&
                   boundedDataDff->patterns.front().dataCandidatesExamined <= 1,
               "named-data resolution obeys its per-match candidate bound");

    SequentialPatternQuery limited;
    limited.dffName = "ff_internal";
    limited.enableFunctionalFallback = true;
    limited.maxFunctionalCandidates = 1;
    limited.maxFunctionalMatchesPerDff = 8;
    limited.functionalTimeLimitSeconds = 5.0;
    const SequentialPatternReportSet limitedReport =
        netlist.runSequentialPatternQuery(limited);
    const DffInputPatternReport* limitedXor = findDff(limitedReport, "ff_internal");
    test.check(limitedReport.status == "PARTIAL" && !limitedReport.complete &&
                   limitedXor != nullptr &&
                   limitedXor->functionalCandidateLimitReached &&
                   limitedXor->functionalCandidatesExamined == 1 &&
                   limitedXor->functionalUnexaminedCandidateCount > 0 &&
                   limitedXor->functionalInconclusiveCandidateCount == 0,
               "candidate truncation is explicit instead of silently complete");

    SequentialPatternQuery timeout;
    timeout.dffName = "ff_xor";
    timeout.enableFunctionalFallback = true;
    timeout.functionalTimeLimitSeconds = 1e-12;
    const SequentialPatternReportSet timeoutReport =
        netlist.runSequentialPatternQuery(timeout);
    const bool timeoutCostConsistent =
        timeoutReport.functionalCandidatesExamined == 0
            ? timeoutReport.functionalInconclusiveCandidateCount == 0
            : timeoutReport.functionalInconclusiveCandidateCount > 0;
    test.check(!timeoutReport.ok && timeoutReport.timedOut &&
                   !timeoutReport.complete && timeoutReport.status == "PARTIAL" &&
                   timeoutCostConsistent,
               "query-wide functional time budget reports timeout conservatively");

    std::cout << "Summary: " << test.passed << " passed, "
              << test.failed << " failed.\n";
    return test.failed == 0 ? 0 : 1;
}
