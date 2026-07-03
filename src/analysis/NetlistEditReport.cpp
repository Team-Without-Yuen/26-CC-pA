#include "include/core/Netlist.h"

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
    (void)before;

    EditValidationResult result;
    result.structureChecked = true;
    result.structureValid = after.validateStructure();

    result.problemAConstraintsChecked = true;
    result.problemAConstraintsValid = after.validateProblemAConstraints();

    result.equivalenceChecked = false;
    result.functionallyEquivalent = false;
    result.equivalenceMethod = EquivalenceCheckMethod::NotChecked;
    result.messages.push_back("Whole-design equivalence is not checked by validateEditResult() yet.");

    return result;
}

DepthChange Netlist::buildDepthChangeReport(
    const Netlist& before,
    const Netlist& after,
    const std::string& endpointName,
    int targetDepth)
{
    DepthChange change;
    change.endpointName = endpointName;
    change.targetDepth = targetDepth;

    if (!endpointName.empty()) {
        change.beforeDepth = before.getMaxDepthToNet(endpointName);
        change.afterDepth = after.getMaxDepthToNet(endpointName);
    } else {
        const DepthReport beforeWorst = before.findGlobalCriticalPath();
        const DepthReport afterWorst = after.findGlobalCriticalPath();
        change.endpointName = beforeWorst.endpointName;
        if (change.endpointName.empty()) {
            change.endpointName = afterWorst.endpointName;
        }
        change.beforeDepth = beforeWorst.depth;
        change.afterDepth = afterWorst.depth;
    }

    change.improved =
        change.beforeDepth >= 0 &&
        change.afterDepth >= 0 &&
        change.afterDepth < change.beforeDepth;
    change.meetsTarget =
        targetDepth >= 0 &&
        change.afterDepth >= 0 &&
        change.afterDepth <= targetDepth;

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
        report.validation.structureValid &&
        report.validation.problemAConstraintsValid;

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
