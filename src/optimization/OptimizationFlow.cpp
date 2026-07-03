#include "include/core/Netlist.h"

namespace {

std::string optPassKindName(OptPassKind passKind) {
    switch (passKind) {
        case OptPassKind::CleanupBufferChain:
            return "cleanup_buffer_chain";
        case OptPassKind::CollapseDoubleInverter:
            return "collapse_double_inverter";
        case OptPassKind::LocalSimplificationFixpoint:
            return "local_simplification_fixpoint";
        default:
            return "unknown";
    }
}

OptCandidate makeGateCandidate(
    int id,
    OptPassKind passKind,
    const std::string& reason,
    const std::vector<int>& gateIds,
    const Netlist& netlist,
    int estimatedGateDelta,
    int estimatedNetDelta)
{
    OptCandidate candidate;
    candidate.id = id;
    candidate.passKind = passKind;
    candidate.reason = reason;
    candidate.gateIds = gateIds;
    candidate.estimatedGateDelta = estimatedGateDelta;
    candidate.estimatedNetDelta = estimatedNetDelta;

    for (int gateId : gateIds) {
        if (!netlist.isValidGateId(gateId)) continue;
        const Gate& gate = netlist.getGate(gateId);
        candidate.gateNames.push_back(gate.instName);
        if (gate.outputNetId >= 0 && netlist.isValidNetId(gate.outputNetId)) {
            candidate.netIds.push_back(gate.outputNetId);
            candidate.netNames.push_back(netlist.getNet(gate.outputNetId).name);
        }
        for (int inputNetId : gate.inputNetIds) {
            if (inputNetId < 0 || !netlist.isValidNetId(inputNetId)) continue;
            candidate.netIds.push_back(inputNetId);
            candidate.netNames.push_back(netlist.getNet(inputNetId).name);
        }
    }

    return candidate;
}

NetlistEditReport makeFailedOptApplyReport(
    const Netlist& netlist,
    OptPassKind passKind,
    const std::string& message)
{
    NetlistEditReport report;
    report.operationKind = NetlistEditOperationKind::CustomRewrite;
    report.operationName = "opt_apply:" + optPassKindName(passKind);
    report.message = message;
    report.beforeStats = netlist.collectNetlistStats();
    report.afterStats = report.beforeStats;
    report.diff = Netlist::diffStats(report.beforeStats, report.afterStats);
    report.validation = Netlist::validateEditResult(netlist, netlist);
    report.success = false;
    report.changed = false;
    report.rolledBack = false;
    return report;
}

} // namespace

OptQueryReport Netlist::runOptQuery(const OptQueryRequest& request) const {
    OptQueryReport report;
    report.passKind = request.passKind;
    report.scopeName = request.scopeName;

    int candidateId = 0;
    switch (request.passKind) {
        case OptPassKind::CleanupBufferChain: {
            const std::vector<int> bufferGateIds = findAllRemovableBufferGates();
            for (int gateId : bufferGateIds) {
                report.candidates.push_back(makeGateCandidate(
                    candidateId++,
                    request.passKind,
                    "Removable BUF gate can be bypassed safely.",
                    {gateId},
                    *this,
                    -1,
                    0));
            }
            report.ok = true;
            report.message = "Found " + std::to_string(report.candidates.size()) +
                             " cleanup_buffer_chain candidate(s).";
            break;
        }
        case OptPassKind::CollapseDoubleInverter: {
            const std::vector<std::pair<int, int>> pairs = findDoubleInverterPairs();
            for (const auto& pair : pairs) {
                report.candidates.push_back(makeGateCandidate(
                    candidateId++,
                    request.passKind,
                    "Back-to-back NOT gates can be collapsed.",
                    {pair.first, pair.second},
                    *this,
                    -2,
                    0));
            }
            report.ok = true;
            report.message = "Found " + std::to_string(report.candidates.size()) +
                             " collapse_double_inverter candidate(s).";
            break;
        }
        case OptPassKind::LocalSimplificationFixpoint: {
            OptCandidate candidate;
            candidate.id = candidateId++;
            candidate.passKind = request.passKind;
            candidate.reason = "Run constant and same-input simplification until no local changes remain.";
            candidate.estimatedGateDelta = 0;
            candidate.estimatedNetDelta = 0;
            report.candidates.push_back(candidate);
            report.ok = true;
            report.message = "Local simplification fixpoint is available as a pass-level candidate.";
            report.warnings.push_back("This first opt_query version does not enumerate every sub-simplification candidate.");
            break;
        }
        default:
            report.ok = false;
            report.message = "Unsupported optimization pass kind.";
            break;
    }

    if (report.ok && !request.scopeName.empty()) {
        report.warnings.push_back("scopeName is recorded but not enforced in the first OPT flow skeleton.");
    }
    return report;
}

NetlistEditReport Netlist::runOptApply(const OptApplyRequest& request) {
    NetlistEditReport report;

    switch (request.passKind) {
        case OptPassKind::CleanupBufferChain:
            report = cleanupAllRemovableBuffersWithReport();
            report.operationName = "opt_apply:cleanup_buffer_chain";
            break;
        case OptPassKind::CollapseDoubleInverter:
            report = collapseBackToBackInvertersWithReport();
            report.operationName = "opt_apply:collapse_double_inverter";
            break;
        case OptPassKind::LocalSimplificationFixpoint:
            report = runLocalSimplificationFixpointWithReport();
            report.operationName = "opt_apply:local_simplification_fixpoint";
            break;
        default:
            return makeFailedOptApplyReport(*this, request.passKind, "Unsupported optimization pass kind.");
    }

    if (!request.candidateIds.empty()) {
        report.addWarning("candidateIds are accepted for API compatibility, but first-version opt_apply applies the whole pass.");
    }
    if (request.validateEquivalence) {
        report.addWarning("validateEquivalence requested, but whole-design equivalence is not integrated yet.");
    }
    if (!request.rollbackOnFailure) {
        report.addWarning("rollbackOnFailure=false is recorded, but current WithReport pass wrappers always rollback on validation failure.");
    }

    return report;
}

