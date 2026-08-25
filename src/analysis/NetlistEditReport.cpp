#include "include/core/Netlist.h"
#include "include/core/OptimizerRequest.h"

#include <set>

namespace {

bool hasStatsChange(const NetlistDiff& diff) {
    if (diff.gateCountDelta != 0 ||
        diff.activeGateCountDelta != 0 ||
        diff.netCountDelta != 0 ||
        diff.activeNetCountDelta != 0 ||
        diff.dffCountDelta != 0 ||
        diff.combinationalGateCountDelta != 0) {
        return true;
    }

    for (const auto& item : diff.gateTypeCountDelta) {
        if (item.second != 0) return true;
    }

    return false;
}

bool hasStructureRegression(
    const Netlist::StructureViolations& before,
    const Netlist::StructureViolations& after)
{
    return
        after.gateIdMismatch > before.gateIdMismatch ||
        after.gateMissingOutput > before.gateMissingOutput ||
        after.gateOutputOutOfRange > before.gateOutputOutOfRange ||
        after.gateOutputNotDriver > before.gateOutputNotDriver ||
        after.gateInputOutOfRange > before.gateInputOutOfRange ||
        after.gateInputRemoved > before.gateInputRemoved ||
        after.netInvalidDriver > before.netInvalidDriver ||
        after.netDriverRemoved > before.netDriverRemoved ||
        after.netDriverOutputMismatch > before.netDriverOutputMismatch ||
        after.netInvalidLoad > before.netInvalidLoad ||
        after.netLoadMissingInput > before.netLoadMissingInput ||
        after.netLoadMultiplicity > before.netLoadMultiplicity;
}

}

NetlistStats Netlist::collectNetlistStats() const {
    NetlistStats stats;

    stats.gateCount = gates.size();
    stats.netCount = nets.size();
    stats.primaryInputCount = primaryInputs.size();
    stats.primaryOutputCount = primaryOutputs.size();

    for (const Gate& gate : gates) {
        if (gate.type == GateType::UNKNOWN) {
            stats.removedGateCount++;
            continue;
        }

        stats.activeGateCount++;
        stats.gateTypeCounts[gate.type]++;

        if (gate.type == GateType::DFF) {
            stats.dffCount++;
        } else {
            stats.combinationalGateCount++;
        }
    }

    for (const Net& net : nets) {
        if (net.isRemoved) {
            stats.removedNetCount++;
        } else {
            stats.activeNetCount++;
        }
    }

    return stats;
}

NetlistDiff Netlist::diffStats(const NetlistStats& before, const NetlistStats& after) {
    NetlistDiff diff;

    diff.gateCountDelta =
        static_cast<int>(after.gateCount) - static_cast<int>(before.gateCount);
    diff.activeGateCountDelta =
        static_cast<int>(after.activeGateCount) - static_cast<int>(before.activeGateCount);
    diff.netCountDelta =
        static_cast<int>(after.netCount) - static_cast<int>(before.netCount);
    diff.activeNetCountDelta =
        static_cast<int>(after.activeNetCount) - static_cast<int>(before.activeNetCount);
    diff.dffCountDelta =
        static_cast<int>(after.dffCount) - static_cast<int>(before.dffCount);
    diff.combinationalGateCountDelta =
        static_cast<int>(after.combinationalGateCount) -
        static_cast<int>(before.combinationalGateCount);

    for (const auto& item : before.gateTypeCounts) {
        diff.gateTypeCountDelta[item.first] -= item.second;
    }
    for (const auto& item : after.gateTypeCounts) {
        diff.gateTypeCountDelta[item.first] += item.second;
    }

    return diff;
}

EditValidationResult Netlist::validateEditResult(const Netlist& before, const Netlist& after) {
    EditValidationResult result;

    // ---- 結構驗證 ----
    // 只驗 after 會把「載入時就存在的問題」算到這次 edit 頭上。
    // 有些 benchmark 的來源檔案本身就有 multi-driver（同一條 net 被兩顆
    // gate 驅動），那會讓每一次 edit 都被 rollback。
    // 因此改成比較 before/after：edit 沒讓情況變糟就算通過。
    result.structureChecked = true;

    const StructureViolations beforeStructure =
        before.collectStructureViolations(false);   // baseline 不印訊息
    const StructureViolations afterStructure =
        after.collectStructureViolations(false);

    result.structureBaselineValid = beforeStructure.clean();
    result.structureValid         = afterStructure.clean();
    result.structureRegressed     = hasStructureRegression(beforeStructure, afterStructure);
    result.structureViolationCount         = afterStructure.total();
    result.baselineStructureViolationCount = beforeStructure.total();

    if (!result.structureBaselineValid && !result.structureRegressed) {
        result.messages.push_back(
            "The loaded design already had " +
            std::to_string(beforeStructure.total()) +
            " structural violation(s) (typically multi-driver nets in the source "
            "file); this edit introduced none.");
    }
    if (result.structureRegressed) {
        result.messages.push_back(
            "One or more structural-violation categories regressed (total before: " +
            std::to_string(beforeStructure.total()) + ", total after: " +
            std::to_string(afterStructure.total()) + ").");
    }

    // ---- Problem A 約束 ----
    result.problemAConstraintsChecked = true;
    auto collectProblemAViolations = [](const Netlist& netlist) {
        std::set<std::string> violations;
        for (const Net& net : netlist.nets) {
            if (net.isRemoved || !net.isPO) continue;
            if (net.driverGateId < 0 && !net.isPI && !net.isConst) {
                violations.insert("PO net " + net.name + " has no driver.");
            }
        }
        for (const Gate& gate : netlist.gates) {
            if (gate.type == GateType::DFF && gate.outputNetId < 0) {
                violations.insert("DFF gate " + gate.instName + " has invalid outputNetId.");
            }
        }
        return violations;
    };

    const std::set<std::string> beforeProblemA = collectProblemAViolations(before);
    const std::set<std::string> afterProblemA  = collectProblemAViolations(after);

    result.problemAConstraintsBaselineValid = beforeProblemA.empty();
    result.problemAConstraintsValid         = afterProblemA.empty();
    for (const std::string& violation : afterProblemA) {
        if (beforeProblemA.count(violation) == 0) {
            result.newProblemAConstraintViolations.push_back(violation);
        }
    }
    result.problemAConstraintsRegressed =
        !result.newProblemAConstraintViolations.empty();

    if (!result.problemAConstraintsBaselineValid &&
        !result.problemAConstraintsRegressed) {
        result.messages.push_back(
            "The loaded baseline already violates Problem A structural constraints; "
            "this edit introduced no new violations.");
    }

    // ---- 等價 ----
    result.equivalenceChecked = false;
    result.functionallyEquivalent = false;
    result.equivalenceMethod = EquivalenceCheckMethod::NotChecked;
    result.messages.push_back(
        "Whole-design equivalence is not checked by validateEditResult() yet.");

    return result;
}

CostChange Netlist::buildDepthCostChange(
    const Netlist& before,
    const Netlist& after,
    const std::string& endpointName,
    int targetDepth)
{
    CostChange change;
    change.metricName  = opt::toString(opt::CostMetric::GlobalMaxDepth);
    change.targetName  = endpointName;
    change.targetValue = targetDepth;

    if (!endpointName.empty()) {
        change.beforeValue = before.getMaxDepthToNet(endpointName);
        change.afterValue  = after.getMaxDepthToNet(endpointName);
    } else {
        const DepthReport beforeWorst = before.findGlobalCriticalPath();
        const DepthReport afterWorst  = after.findGlobalCriticalPath();
        change.targetName = beforeWorst.endpointName.empty()
            ? afterWorst.endpointName
            : beforeWorst.endpointName;
        change.beforeValue = beforeWorst.depth;
        change.afterValue  = afterWorst.depth;
    }

    change.improved =
        change.beforeValue >= 0 &&
        change.afterValue  >= 0 &&
        change.afterValue  <  change.beforeValue;
    change.meetsTarget =
        targetDepth >= 0 &&
        change.afterValue >= 0 &&
        change.afterValue <= targetDepth;

    return change;
}

NetlistEditReport Netlist::buildEditReport(
    const Netlist& before,
    const Netlist& after,
    const std::string& operationName,
    NetlistEditOperationKind kind)
{
    NetlistEditReport report;
    report.operationKind = kind;
    report.operationName = operationName;

    report.beforeStats = before.collectNetlistStats();
    report.afterStats = after.collectNetlistStats();
    report.diff = diffStats(report.beforeStats, report.afterStats);
    report.changed = hasStatsChange(report.diff);

    report.validation = validateEditResult(before, after);
    report.success =
        !report.validation.structureRegressed &&
        !report.validation.problemAConstraintsRegressed;

    if (report.success) {
        report.message = report.changed ? "Edit completed." : "Edit completed with no structural count change.";
    } else {
        report.message = "Edit completed but validation failed.";
        report.warnings.push_back("Result netlist failed validation; caller should rollback or inspect details.");
    }

    return report;
}

void Netlist::certifyEquivalence(
    NetlistEditReport& report,
    EquivalenceCheckMethod method,
    const std::string& message)
{
    if (method == EquivalenceCheckMethod::NotChecked) {
        return;
    }

    report.validation.equivalenceChecked = true;
    report.validation.functionallyEquivalent = true;
    report.validation.equivalenceMethod = method;
    report.validation.messages.push_back(message);
}
