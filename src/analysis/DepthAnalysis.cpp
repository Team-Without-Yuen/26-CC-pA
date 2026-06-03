#include "include/core/Netlist.h"

#include <algorithm>
#include <queue>
#include <vector>

namespace {

// 建立單一 endpoint 的 DepthReport，集中填入 endpoint metadata 與 critical path。
Netlist::DepthReport makeDepthReportForNet(
    const Netlist& netlist,
    const std::string& netName,
    Netlist::DepthEndpointType endpointType,
    const std::string& endpointName) {
    Netlist::DepthReport report;
    const int netId = netlist.getNetId(netName);
    if (netId < 0 || netId >= static_cast<int>(netlist.getNetCount())) {
        return report;
    }

    report.endpointType = endpointType;
    report.endpointName = endpointName;
    report.endpointNetId = netId;
    report.depth = netlist.getMaxDepthToNet(netName);
    if (report.depth >= 0) {
        report.criticalPath = netlist.findCriticalPathToNet(netName);
    }
    return report;
}

} // namespace

// 計算每個 net 的 combinational level；PI、constant、DFF.Q 視為 level 0。
std::vector<int> Netlist::computeNetLevels() const {
    std::vector<int> netLevels(nets.size(), -1);
    std::vector<unsigned char> processedGates(gates.size(), 0);
    std::queue<int> readyNets;

    auto markSourceNet = [&](int netId) {
        if (netId < 0 || netId >= static_cast<int>(nets.size())) {
            return;
        }
        if (netLevels[netId] == -1) {
            netLevels[netId] = 0;
            readyNets.push(netId);
        }
    };

    for (const Net& net : nets) {
        if (net.isPI || net.isConst) {
            markSourceNet(net.id);
        }
    }

    for (const Gate& gate : gates) {
        if (gate.type == GateType::DFF) {
            markSourceNet(gate.outputNetId);
        }
    }

    while (!readyNets.empty()) {
        const int currentNetId = readyNets.front();
        readyNets.pop();

        if (currentNetId < 0 || currentNetId >= static_cast<int>(nets.size())) {
            continue;
        }

        const Net& currentNet = nets[currentNetId];
        for (int gateId : currentNet.loadGateIds) {
            if (gateId < 0 || gateId >= static_cast<int>(gates.size()) ||
                processedGates[gateId] != 0) {
                continue;
            }

            const Gate& gate = gates[gateId];
            if (gate.type == GateType::DFF ||
                gate.outputNetId < 0 ||
                gate.outputNetId >= static_cast<int>(nets.size()) ||
                gate.inputNetIds.empty()) {
                continue;
            }

            bool allInputsKnown = true;
            int maxInputLevel = -1;
            for (int inputNetId : gate.inputNetIds) {
                if (inputNetId < 0 ||
                    inputNetId >= static_cast<int>(nets.size()) ||
                    netLevels[inputNetId] < 0) {
                    allInputsKnown = false;
                    break;
                }
                maxInputLevel = std::max(maxInputLevel, netLevels[inputNetId]);
            }

            if (!allInputsKnown) {
                continue;
            }

            const int outputLevel = maxInputLevel + 1;
            processedGates[gateId] = 1;
            if (netLevels[gate.outputNetId] < outputLevel) {
                netLevels[gate.outputNetId] = outputLevel;
                readyNets.push(gate.outputNetId);
            }
        }
    }

    return netLevels;
}

// 計算每個 gate output 的 combinational level；DFF 或無法計算者回傳 -1。
std::vector<int> Netlist::computeGateLevels() const {
    const std::vector<int> netLevels = computeNetLevels();
    std::vector<int> gateLevels(gates.size(), -1);

    for (const Gate& gate : gates) {
        if (gate.type == GateType::DFF ||
            gate.outputNetId < 0 ||
            gate.outputNetId >= static_cast<int>(nets.size()) ||
            gate.inputNetIds.empty()) {
            continue;
        }

        bool allInputsKnown = true;
        int maxInputLevel = -1;
        for (int inputNetId : gate.inputNetIds) {
            if (inputNetId < 0 ||
                inputNetId >= static_cast<int>(nets.size()) ||
                netLevels[inputNetId] < 0) {
                allInputsKnown = false;
                break;
            }
            maxInputLevel = std::max(maxInputLevel, netLevels[inputNetId]);
        }

        if (allInputsKnown) {
            gateLevels[gate.id] = maxInputLevel + 1;
        }
    }

    return gateLevels;
}

// 查詢指定 net 的最大 fanin depth；找不到或無法計算時回傳 -1。
int Netlist::getMaxDepthToNet(const std::string& netName) const {
    const int netId = getNetId(netName);
    if (netId < 0 || netId >= static_cast<int>(nets.size())) {
        return -1;
    }

    const std::vector<int> netLevels = computeNetLevels();
    return netLevels[netId];
}

// 找到到指定 net 的一條 critical path；不存在或無法計算時回傳空路徑。
Netlist::CombinationalPath Netlist::findCriticalPathToNet(const std::string& netName) const {
    CombinationalPath path;
    const int targetNetId = getNetId(netName);
    if (targetNetId < 0 || targetNetId >= static_cast<int>(nets.size())) {
        return path;
    }

    const std::vector<int> netLevels = computeNetLevels();
    if (netLevels[targetNetId] < 0) {
        return path;
    }

    int currentNetId = targetNetId;
    path.netIds.push_back(currentNetId);

    while (currentNetId >= 0 && currentNetId < static_cast<int>(nets.size())) {
        const Net& currentNet = nets[currentNetId];
        const int driverGateId = currentNet.driverGateId;
        if (driverGateId < 0 || driverGateId >= static_cast<int>(gates.size())) {
            break;
        }

        const Gate& driverGate = gates[driverGateId];
        if (driverGate.type == GateType::DFF || driverGate.inputNetIds.empty()) {
            break;
        }

        int bestInputNetId = -1;
        int bestInputLevel = -1;
        for (int inputNetId : driverGate.inputNetIds) {
            if (inputNetId < 0 ||
                inputNetId >= static_cast<int>(nets.size()) ||
                netLevels[inputNetId] < 0) {
                continue;
            }
            if (netLevels[inputNetId] > bestInputLevel) {
                bestInputLevel = netLevels[inputNetId];
                bestInputNetId = inputNetId;
            }
        }

        if (bestInputNetId < 0) {
            break;
        }

        path.gateIds.push_back(driverGateId);
        path.netIds.push_back(bestInputNetId);
        currentNetId = bestInputNetId;
    }

    std::reverse(path.netIds.begin(), path.netIds.end());
    std::reverse(path.gateIds.begin(), path.gateIds.end());
    return path;
}

// 分析指定 net 的 depth、endpoint net ID 與 critical path。
Netlist::DepthReport Netlist::analyzeDepthToNet(const std::string& netName) const {
    return makeDepthReportForNet(*this, netName, DepthEndpointType::SpecificNet, netName);
}

// 分析所有 primary output net 的 depth；bus output 會展開成每一個 bit net。
std::vector<Netlist::DepthReport> Netlist::analyzePrimaryOutputDepths() const {
    std::vector<DepthReport> reports;
    const std::vector<int> outputNetIds = getPrimaryOutputNetIds();
    reports.reserve(outputNetIds.size());

    for (int netId : outputNetIds) {
        if (netId < 0 || netId >= static_cast<int>(nets.size())) {
            continue;
        }
        reports.push_back(makeDepthReportForNet(*this, nets[netId].name,
                                                DepthEndpointType::PrimaryOutput,
                                                nets[netId].name));
    }
    return reports;
}

// 列出所有 depth 大於 limit 的 primary output 名稱；bus bit 會使用展開後的 net 名稱。
std::vector<std::string> Netlist::getPrimaryOutputsWithDepthGreaterThan(int limit) const {
    std::vector<std::string> outputNames;
    const std::vector<DepthReport> reports = analyzePrimaryOutputDepths();

    for (const DepthReport& report : reports) {
        if (report.endpointNetId >= 0 &&
            report.endpointNetId < static_cast<int>(nets.size()) &&
            report.depth > limit) {
            outputNames.push_back(nets[report.endpointNetId].name);
        }
    }
    return outputNames;
}

// 分析所有 DFF D-pin 的 depth；D pin 未連接或無法計算時保留 depth = -1。
std::vector<Netlist::DepthReport> Netlist::analyzeDffDDepths() const {
    std::vector<DepthReport> reports;

    for (const Gate& gate : gates) {
        if (gate.type != GateType::DFF) {
            continue;
        }

        DepthReport report;
        const int dNetId = getGateInputNetId(gate.id, "D");
        if (dNetId >= 0 && dNetId < static_cast<int>(nets.size())) {
            report = makeDepthReportForNet(*this, nets[dNetId].name,
                                           DepthEndpointType::DffD,
                                           gate.instName + ".D");
        }
        reports.push_back(report);
    }
    return reports;
}

// 列出所有 D-pin depth 大於 limit 的 DFF instance name。
std::vector<std::string> Netlist::getDffsWithDDepthGreaterThan(int limit) const {
    std::vector<std::string> dffNames;

    for (const Gate& gate : gates) {
        if (gate.type != GateType::DFF) {
            continue;
        }

        const int dNetId = getGateInputNetId(gate.id, "D");
        if (dNetId < 0 || dNetId >= static_cast<int>(nets.size())) {
            continue;
        }

        const int depth = getMaxDepthToNet(nets[dNetId].name);
        if (depth > limit) {
            dffNames.push_back(gate.instName);
        }
    }
    return dffNames;
}

// 在所有 primary output 與 DFF D-pin endpoints 中，找出 depth 最大的一條 critical path。
Netlist::DepthReport Netlist::findGlobalCriticalPath() const {
    DepthReport bestReport;

    auto considerReport = [&](const DepthReport& report) {
        if (report.depth > bestReport.depth) {
            bestReport = report;
        }
    };

    for (const DepthReport& report : analyzePrimaryOutputDepths()) {
        considerReport(report);
    }

    for (const DepthReport& report : analyzeDffDDepths()) {
        considerReport(report);
    }

    return bestReport;
}

// 找出所有 depth 大於 maxDepth 的 timing endpoints；endpoint 範圍包含 PO 與 DFF.D。
std::vector<Netlist::DepthReport> Netlist::findEndpointsExceedingDepth(int maxDepth) const {
    std::vector<DepthReport> reports;

    auto appendIfExceeded = [&](const DepthReport& report) {
        if (report.depth > maxDepth) {
            reports.push_back(report);
        }
    };

    for (const DepthReport& report : analyzePrimaryOutputDepths()) {
        appendIfExceeded(report);
    }

    for (const DepthReport& report : analyzeDffDDepths()) {
        appendIfExceeded(report);
    }

    return reports;
}
