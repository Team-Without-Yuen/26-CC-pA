#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "include/core/Netlist.h"
#include "include/io/VerilogReader.h"
#include "include/io/VerilogWriter.h"

namespace {

using Clock = std::chrono::steady_clock;

struct CaseSpec {
    std::string name;
    std::string inputPath;
    std::string outputPath;
    bool useDanglingCleanup = false;
};

struct CaseResult {
    std::string name;
    bool passed = false;
    size_t expectedMerged = 0;
    size_t reportedMerged = 0;
    int activeGateDelta = 0;
    double oracleSeconds = 0.0;
    double mergeSearchSeconds = 0.0;
    double mergeTotalSeconds = 0.0;
    double flowSeconds = 0.0;
    double writeReadSeconds = 0.0;
    std::vector<std::string> failures;
};

template <typename Function>
auto timed(Function&& function, double& seconds) {
    const auto start = Clock::now();
    auto result = function();
    seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return result;
}

void require(CaseResult& result, bool condition, const std::string& message) {
    if (!condition) {
        result.failures.push_back(message);
    }
}

bool usesOnlyAndNotDff(const Netlist& netlist) {
    for (size_t index = 0; index < netlist.getGateCount(); ++index) {
        const int gateId = static_cast<int>(index);
        if (!netlist.isValidGateId(gateId) || netlist.isGateRemoved(gateId)) {
            continue;
        }
        const GateType type = netlist.getGate(gateId).type;
        if (type != GateType::AND && type != GateType::NOT && type != GateType::DFF) {
            return false;
        }
    }
    return true;
}

NetlistEditReport runEdit(
    Netlist& netlist,
    EditCommandKind kind,
    double budgetSeconds,
    const std::vector<GateType>& allowedTypes = {}) {
    EditApplyRequest request;
    request.kind = kind;
    request.scope = TargetScope::WHOLE_NETLIST;
    request.allowedTypes = allowedTypes;
    request.timeLimitSeconds = budgetSeconds;
    request.simulationPatternCount = 256;
    request.validateEquivalence = true;
    request.rollbackOnFailure = true;
    return netlist.runEditApply(request);
}

CaseResult runCase(const CaseSpec& spec) {
    CaseResult result;
    result.name = spec.name;
    const auto flowStart = Clock::now();

    VerilogReader reader;
    Netlist original;
    require(result, reader.read(spec.inputPath, original), "failed to read official input");
    if (!result.failures.empty()) {
        return result;
    }

    Netlist current = original.cloneForRollback();
    const auto mapping = runEdit(
        current,
        EditCommandKind::ConvertToBasis,
        290.0,
        {GateType::AND, GateType::NOT});
    require(result, mapping.success && !mapping.rolledBack,
            "ConvertToBasis failed or rolled back");
    require(result, mapping.validation.structureChecked && mapping.validation.structureValid,
            "ConvertToBasis did not leave a valid structure");
    require(result, usesOnlyAndNotDff(current),
            "ConvertToBasis did not produce an AND/NOT-only combinational design");

    const EditCommandKind cleanupKind = spec.useDanglingCleanup
        ? EditCommandKind::RemoveDanglingLogic
        : EditCommandKind::TrimDeadLogic;
    const auto cleanup = runEdit(current, cleanupKind, 290.0);
    require(result, cleanup.success && !cleanup.rolledBack,
            "dead/dangling cleanup failed or rolled back");
    require(result, cleanup.validation.structureChecked && cleanup.validation.structureValid,
            "dead/dangling cleanup did not leave a valid structure");

    const auto collapse = runEdit(current, EditCommandKind::CollapseDoubleInverter, 290.0);
    require(result, collapse.success && !collapse.rolledBack,
            "CollapseDoubleInverter failed or rolled back");
    require(result, collapse.validation.structureChecked && collapse.validation.structureValid,
            "CollapseDoubleInverter did not leave a valid structure");

    Netlist oracleNetlist = current.cloneForRollback();
    FunctionSearchQuery oracleRequest;
    oracleRequest.type = FunctionSearchQueryType::EquivalentGatePairs;
    oracleRequest.mode = FunctionSearchMode::FindAll;
    oracleRequest.scope = FunctionSearchScope::WholeDesign;
    oracleRequest.simulationPatternCount = 256;
    oracleRequest.timeLimitSeconds = 290.0;
    const auto oracle = timed(
        [&]() { return oracleNetlist.runFunctionSearchQuery(oracleRequest); },
        result.oracleSeconds);
    require(result, oracle.ok && oracle.complete && !oracle.timedOut,
            "pre-merge Function Search oracle was incomplete");
    for (const auto& equivalentClass : oracle.equivalenceClasses) {
        if (equivalentClass.gateIds.size() > 1) {
            result.expectedMerged += equivalentClass.gateIds.size() - 1;
        }
    }

    const Netlist preMerge = current.cloneForRollback();
    const auto preMergeStats = current.collectNetlistStats();
    double mergeWallSeconds = 0.0;
    const auto merge = timed(
        [&]() {
            return runEdit(
                current,
                EditCommandKind::MergeFunctionallyEquivalentGates,
                290.0);
        },
        mergeWallSeconds);
    require(result, merge.success && !merge.rolledBack,
            "functional merge failed or rolled back");
    require(result, merge.validation.equivalenceChecked &&
                    merge.validation.functionallyEquivalent &&
                    merge.validation.equivalenceMethod == EquivalenceCheckMethod::CertifiedRewrite,
            "functional merge lacks its SAT-class rewrite certificate");
    require(result, merge.validation.structureChecked && merge.validation.structureValid,
            "functional merge did not leave a valid structure");
    require(result, merge.functionalMerge.has_value(),
            "functional merge report is missing its detailed summary");

    if (merge.functionalMerge) {
        result.reportedMerged = merge.functionalMerge->mergedGateCount;
        result.mergeSearchSeconds = merge.functionalMerge->searchElapsedSeconds;
        result.mergeTotalSeconds = merge.functionalMerge->totalElapsedSeconds;
        require(result, merge.functionalMerge->searchComplete &&
                        !merge.functionalMerge->searchTimedOut,
                "functional merge search was incomplete");
        require(result, !merge.functionalMerge->wholeDesignEquivalenceChecked &&
                        !merge.functionalMerge->wholeDesignEquivalent &&
                        !merge.functionalMerge->wholeDesignTimedOut,
                "functional merge unexpectedly ran final whole-design SAT");
        require(result, merge.functionalMerge->records.size() == result.reportedMerged,
                "merge record count differs from mergedGateCount");
    }
    require(result, result.reportedMerged == result.expectedMerged,
            "EditApply merged-gate count differs from the read-only oracle");

    const auto postMergeStats = current.collectNetlistStats();
    result.activeGateDelta = static_cast<int>(postMergeStats.activeGateCount) -
                             static_cast<int>(preMergeStats.activeGateCount);
    require(result, result.activeGateDelta == -static_cast<int>(result.reportedMerged),
            "active-gate delta differs from the reported merge count");
    require(result, merge.diff.activeGateCountDelta == result.activeGateDelta,
            "NetlistEditReport diff disagrees with the actual active-gate delta");

    const auto independentCec = current.checkWholeDesignEquivalence(preMerge, 290.0);
    require(result, independentCec.ok && independentCec.equivalent &&
                    !independentCec.timeBudgetExceeded,
            "independent post-merge whole-design equivalence failed");
    require(result, usesOnlyAndNotDff(current),
            "functional merge violated the AND/NOT-only requirement");

    VerilogWriter writer;
    const auto ioStart = Clock::now();
    require(result, writer.write(spec.outputPath, current),
            "failed to write merged design");
    Netlist reloaded;
    require(result, reader.read(spec.outputPath, reloaded),
            "failed to read the written merged design");
    result.writeReadSeconds =
        std::chrono::duration<double>(Clock::now() - ioStart).count();
    if (result.failures.empty()) {
        require(result, reloaded.collectNetlistStats().activeGateCount ==
                        current.collectNetlistStats().activeGateCount,
                "write/readback changed the active gate count");
        require(result, usesOnlyAndNotDff(reloaded),
                "write/readback violated the AND/NOT-only requirement");
        const auto readbackCec = reloaded.checkWholeDesignEquivalence(original, 290.0);
        require(result, readbackCec.ok && readbackCec.equivalent &&
                        !readbackCec.timeBudgetExceeded,
                "written design is not equivalent to the official input");
    }

    result.flowSeconds =
        std::chrono::duration<double>(Clock::now() - flowStart).count();
    result.passed = result.failures.empty();
    std::cout << spec.name
              << " oracle=" << result.expectedMerged
              << " merged=" << result.reportedMerged
              << " search_s=" << result.mergeSearchSeconds
              << " merge_total_s=" << result.mergeTotalSeconds
              << " wall_s=" << mergeWallSeconds
              << " flow_s=" << result.flowSeconds
              << " status=" << (result.passed ? "PASS" : "FAIL") << '\n';
    for (const std::string& failure : result.failures) {
        std::cout << "  - " << failure << '\n';
    }
    return result;
}

} // namespace

int main() {
    const std::vector<CaseSpec> cases = {
        {"test29", "NewTestCase/test29/test29.v", "mini test/test45/test29_merged.v", false},
        {"test30", "NewTestCase/test30/test30.v", "mini test/test45/test30_merged.v", true}
    };

    std::vector<CaseResult> results;
    for (const CaseSpec& spec : cases) {
        results.push_back(runCase(spec));
    }

    std::ofstream summary("mini test/test45/results.tsv");
    summary << "case\tpassed\texpected_merged\treported_merged\tactive_gate_delta"
               "\toracle_seconds\tmerge_search_seconds\tmerge_total_seconds"
               "\tflow_seconds\twrite_read_seconds\tfailures\n";
    summary << std::fixed << std::setprecision(6);
    bool allPassed = true;
    for (const CaseResult& result : results) {
        allPassed = allPassed && result.passed;
        summary << result.name << '\t'
                << (result.passed ? 1 : 0) << '\t'
                << result.expectedMerged << '\t'
                << result.reportedMerged << '\t'
                << result.activeGateDelta << '\t'
                << result.oracleSeconds << '\t'
                << result.mergeSearchSeconds << '\t'
                << result.mergeTotalSeconds << '\t'
                << result.flowSeconds << '\t'
                << result.writeReadSeconds << '\t';
        for (size_t index = 0; index < result.failures.size(); ++index) {
            if (index != 0) summary << " | ";
            summary << result.failures[index];
        }
        summary << '\n';
    }
    return allPassed ? 0 : 1;
}
