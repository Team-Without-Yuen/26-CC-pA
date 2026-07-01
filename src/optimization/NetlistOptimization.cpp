#include "include/core/Netlist.h"
#include "include/lib/cadical.hpp"
#include <string>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>
#include <iostream>

namespace {

void eraseOneLoad(std::vector<int>& loads, int gateId) {
    auto it = std::find(loads.begin(), loads.end(), gateId);
    if (it != loads.end()) loads.erase(it);
}

NetlistEditReport finalizeEditReport(
    Netlist& netlist,
    const Netlist& before,
    int changedCount,
    const std::string& operationName,
    NetlistEditOperationKind kind,
    const std::string& successMessage,
    const std::string& rollbackMessage,
    EquivalenceCheckMethod equivalenceMethod = EquivalenceCheckMethod::NotChecked,
    const std::string& equivalenceMessage = "")
{
    NetlistEditReport report = Netlist::buildEditReport(before, netlist, operationName, kind);
    report.changed = report.changed || changedCount > 0;
    report.depthChange = Netlist::buildDepthChangeReport(before, netlist);
    report.message = report.success ? successMessage : rollbackMessage;

    if (report.success && equivalenceMethod != EquivalenceCheckMethod::NotChecked) {
        Netlist::certifyEquivalence(report, equivalenceMethod, equivalenceMessage);
    }

    if (!report.success) {
        netlist.restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

NetlistEditReport finalizeBooleanPrimitiveReport(
    Netlist& netlist,
    const Netlist& before,
    bool operationSucceeded,
    const std::string& operationName,
    const std::string& successMessage,
    const std::string& failureMessage,
    const std::vector<std::string>& changedGateNames = {},
    const std::vector<std::string>& changedNetNames = {},
    const std::vector<int>& changedGateIds = {},
    const std::vector<int>& changedNetIds = {})
{
    NetlistEditReport report = Netlist::buildEditReport(
        before,
        netlist,
        operationName,
        NetlistEditOperationKind::PrimitiveMutation);

    report.changed = report.changed || operationSucceeded;
    report.changedGateNames = changedGateNames;
    report.changedNetNames = changedNetNames;
    report.changedGateIds = changedGateIds;
    report.changedNetIds = changedNetIds;

    if (!operationSucceeded) {
        if (report.changed) {
            netlist.restoreFrom(before);
            report.rolledBack = true;
        }
        report.success = false;
        report.message = failureMessage;
        return report;
    }

    report.message = report.success
        ? successMessage
        : operationName + " failed validation and was rolled back.";

    if (!report.success) {
        netlist.restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

}

// ─────────────────────────────────────────────────────────────────────────────
//  trimDeadLogic  【BUG FIX: 補上 DFF 作為 timing endpoint】
//  移除所有不影響任何 PO 或 DFF input 的 gate 和 net
//  回傳移除的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::trimDeadLogic() {
    std::unordered_set<int> usefulGates;
    std::unordered_set<int> usefulNets;
    std::queue<int> q; // 用於反向拓撲追查的 Net ID 佇列

    // 【雙重保險起點】同時檢查 nets 標記，並探測結構上的真實 PO
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].isPO) {
            usefulNets.insert(i);
            q.push(i);
        }
    }

    // 如果發現 usefulNets 是空的（預防 Reader 漏標記），強制鎖定事實上的 PO
    if (q.empty()) {
        std::vector<int> isUsedAsInput((int)nets.size(), 0);
        for (const auto& g : gates) {
            if (g.type == GateType::UNKNOWN) continue;
            for (int inId : g.inputNetIds) {
                if (inId >= 0) isUsedAsInput[inId] = 1;
            }
        }
        for (int i = 0; i < (int)gates.size(); i++) {
            if (gates[i].type == GateType::UNKNOWN) continue;
            int onet = gates[i].outputNetId;
            if (onet >= 0 && isUsedAsInput[onet] == 0) {
                nets[onet].isPO = true; 
                usefulNets.insert(onet);
                q.push(onet);
            }
        }
    }

    // 如果電路中有包含 DFF，DFF 的輸入端也是必須保留的反向 BFS 起點
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type == GateType::DFF) {
            usefulGates.insert(i);
            for (int inNetId : gates[i].inputNetIds) {
                if (inNetId >= 0 && !usefulNets.count(inNetId)) {
                    usefulNets.insert(inNetId);
                    q.push(inNetId);
                }
            }
        }
    }

    // 【反向 BFS 擴散】標記所有對輸出有實質貢獻的邏輯閘
    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        int dgid = nets[currNetId].driverGateId;
        if (dgid < 0) continue; // PI 或 Constant 沒有上游驅動閘

        if (gates[dgid].type != GateType::UNKNOWN && !usefulGates.count(dgid)) {
            usefulGates.insert(dgid); // 認定此閘有用，安全鎖定
            
            for (int inNetId : gates[dgid].inputNetIds) {
                if (inNetId >= 0 && !usefulNets.count(inNetId)) {
                    usefulNets.insert(inNetId);
                    q.push(inNetId);
                }
            }
        }
    }

    // 【安全刪除階段】只有沒被標記、且絕對不連向 PO 的閘才允許刪除
    int deadGateCount = 0;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type == GateType::UNKNOWN) continue;

        int outNetId = gates[i].outputNetId;
        
        // 【PO 絕對保護盾】
        if (outNetId >= 0 && (nets[outNetId].isPO || usefulNets.count(outNetId))) {
            continue; // 強制留住 PO 的驅動閘
        }

        if (!usefulGates.count(i)) {
            if (markGateRemoved(i)) {
                deadGateCount++;
            }
        }
    }

    return deadGateCount;
}

NetlistEditReport Netlist::trimDeadLogicWithReport() {
    Netlist before = cloneForRollback();
    int removed = trimDeadLogic();
    return finalizeEditReport(
        *this,
        before,
        removed,
        "trimDeadLogic",
        NetlistEditOperationKind::Cleanup,
        "Dead logic trimming completed.",
        "Dead logic trimming failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified by removing logic that is unreachable from PO and DFF input endpoints.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  collapseBackToBackInverters  【BUG FIX: 補上 PO guard】
//  找出所有 NOT → NOT 的連續對，把兩個 NOT 刪掉，直接把前面的 net 接到後面
//  例如：A → NOT1 → B → NOT2 → C  →  A → C
//  回傳移除的 inverter pair 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::collapseBackToBackInverters() {
    cleanupAllRemovableBuffers();

    int collapsed = 0;
    bool changed = true;

    while (changed) {
        changed = false;

        for (int i = 0; i < (int)gates.size(); i++) {
            Gate& g1 = gates[i];

            if (g1.type != GateType::NOT) continue;
            if (g1.outputNetId < 0) continue;
            if (g1.inputNetIds.empty()) continue;

            Net& midNet = nets[g1.outputNetId];
            if (midNet.loadGateIds.size() != 1) continue;
            if (midNet.isPO) continue;

            int g2id = midNet.loadGateIds[0];
            if (g2id < 0) continue;

            Gate& g2 = gates[g2id];
            if (g2.type != GateType::NOT) continue;

            if (g2.outputNetId < 0) continue;
            if (nets[g2.outputNetId].isPO) continue;

            int inNetId  = g1.inputNetIds[0];
            int outNetId = g2.outputNetId;

            Net& inNet = nets[inNetId];
            Net& outNet = nets[outNetId];

            // redirect fanout
            for (int loadGateId : outNet.loadGateIds) {
                Gate& lg = gates[loadGateId];

                for (int& pin : lg.inputNetIds) {
                    if (pin == outNetId) pin = inNetId;
                }

                inNet.loadGateIds.push_back(loadGateId);
            }

            // remove g1 from inNet fanout
            inNet.loadGateIds.erase(
                std::remove(inNet.loadGateIds.begin(),
                            inNet.loadGateIds.end(),
                            g1.id),
                inNet.loadGateIds.end()
            );

            // mark dead (NO structural cleanup here)
            g1.type = GateType::UNKNOWN;
            g1.inputNetIds.clear();
            g1.outputNetId = -1;

            g2.type = GateType::UNKNOWN;
            g2.inputNetIds.clear();
            g2.outputNetId = -1;

            midNet.loadGateIds.clear();
            midNet.driverGateId = -1;

            outNet.driverGateId = -1;
            outNet.loadGateIds.clear();

            collapsed++;
            changed = true;
            break;
        }
    }
    return collapsed;
}

NetlistEditReport Netlist::collapseBackToBackInvertersWithReport() {
    Netlist before = cloneForRollback();
    int collapsed = collapseBackToBackInverters();
    return finalizeEditReport(
        *this,
        before,
        collapsed,
        "collapseBackToBackInverters",
        NetlistEditOperationKind::Simplification,
        "Back-to-back inverter collapse completed.",
        "Back-to-back inverter collapse failed validation and was rolled back.",
        EquivalenceCheckMethod::LocalRewriteRule,
        "Equivalence certified by the local Boolean identity NOT(NOT(x)) = x.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  mergeEquivalentGates
//  合併結構等價的 gate（相同 type + 相同 input net 集合）
//  回傳合併的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::mergeEquivalentGates() {
    int merged = 0;

    std::map<std::pair<int, std::vector<int>>, int> seen;

    for (int i = 0; i < (int)gates.size(); i++) {
        Gate& g = gates[i];
        if (g.type == GateType::UNKNOWN) continue;
        if (g.outputNetId < 0) continue;

        // 只對可交換（Commutative）的邏輯閘排序，保護 DFF 的腳位順序
        std::vector<int> sigInputs = g.inputNetIds;
        if (g.type == GateType::AND || g.type == GateType::OR ||
            g.type == GateType::NAND || g.type == GateType::NOR ||
            g.type == GateType::XOR || g.type == GateType::XNOR) {
            std::sort(sigInputs.begin(), sigInputs.end());
        }
        std::pair<int, std::vector<int>> key = std::make_pair((int)g.type, sigInputs);

        auto it = seen.find(key);
        if (it == seen.end()) {
            seen[key] = i;
        } else {
            int keepGateId = it->second;
            int deadGateId = i;

            int keepOutNet = gates[keepGateId].outputNetId;
            int deadOutNet = gates[deadGateId].outputNetId;

            if (keepOutNet < 0 || deadOutNet < 0) continue;
            if (keepOutNet == deadOutNet) continue;

            for (int j = 0; j < (int)nets[deadOutNet].loadGateIds.size(); j++) {
                int loadGateId = nets[deadOutNet].loadGateIds[j];
                Gate& loadGate = gates[loadGateId];
                for (int k = 0; k < (int)loadGate.inputNetIds.size(); k++) {
                    if (loadGate.inputNetIds[k] == deadOutNet) {
                        loadGate.inputNetIds[k] = keepOutNet;
                        nets[keepOutNet].loadGateIds.push_back(loadGateId);
                    }
                }
            }

            if (nets[deadOutNet].isPO) {
                nets[keepOutNet].isPO = true;
                for (int pi = 0; pi < (int)primaryOutputs.size(); pi++) {
                    for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++) {
                        if (primaryOutputs[pi].netIds[pj] == deadOutNet)
                            primaryOutputs[pi].netIds[pj] = keepOutNet;
                    }
                }
            }

            nets[deadOutNet].loadGateIds.clear();
            nets[deadOutNet].driverGateId = -1;

            // 清除 deadGate 在 input nets 上的負載（避免 dangling pointer）
            for (int inNetId : gates[deadGateId].inputNetIds) {
                auto& loads = nets[inNetId].loadGateIds;
                loads.erase(std::remove(loads.begin(), loads.end(), deadGateId), loads.end());
            }

            gates[deadGateId].type = GateType::UNKNOWN;
            gates[deadGateId].outputNetId = -1;
            gates[deadGateId].inputNetIds.clear();

            merged++;
        }
    }

    if (merged > 0) trimDeadLogic();
    return merged;
}

NetlistEditReport Netlist::mergeEquivalentGatesWithReport() {
    Netlist before = cloneForRollback();
    int merged = mergeEquivalentGates();
    return finalizeEditReport(
        *this,
        before,
        merged,
        "mergeEquivalentGates",
        NetlistEditOperationKind::Simplification,
        "Equivalent gate merge completed.",
        "Equivalent gate merge failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified by merging gates with identical type and input structure.");
}

// =============================================================================
//  新增 function（對應 CLEANUP_SIMPLIFICATION_NOTES.md 各 section）
// =============================================================================

// ─────────────────────────────────────────────────────────────────────────────
//  Section 1.5: Gate removed flag helpers
// ─────────────────────────────────────────────────────────────────────────────
bool Netlist::isGateRemoved(int gateId) const {
    if (gateId < 0 || gateId >= (int)gates.size()) return true;
    return gates[gateId].type == GateType::UNKNOWN;
}

bool Netlist::markGateRemoved(int gateId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    if (gates[gateId].type == GateType::UNKNOWN) return false;

    int outNetId = gates[gateId].outputNetId;

    for (int inNetId : gates[gateId].inputNetIds) {
        if (inNetId < 0) continue;
        auto& loads = nets[inNetId].loadGateIds;
        loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
    }

    if (outNetId >= 0)
        nets[outNetId].driverGateId = -1;

    gates[gateId].type = GateType::UNKNOWN;
    gates[gateId].inputNetIds.clear();
    gates[gateId].outputNetId = -1;
    return true;
}

int Netlist::compactRemovedGates() {
    std::unordered_set<int> deadSet;
    for (int i = 0; i < (int)gates.size(); i++)
        if (gates[i].type == GateType::UNKNOWN) deadSet.insert(i);
    if (deadSet.empty()) return 0;

    for (int i = 0; i < (int)nets.size(); i++) {
        std::vector<int> newLoads;
        for (int lgid : nets[i].loadGateIds)
            if (!deadSet.count(lgid)) newLoads.push_back(lgid);
        nets[i].loadGateIds = newLoads;
    }

    std::vector<int> oldToNew(gates.size(), -1);
    std::vector<Gate> newGates;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!deadSet.count(i)) {
            int newId = (int)newGates.size();
            oldToNew[i] = newId;
            newGates.push_back(gates[i]);
            newGates.back().id = newId;
        }
    }

    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].driverGateId >= 0)
            nets[i].driverGateId = oldToNew[nets[i].driverGateId];
        for (int& lgid : nets[i].loadGateIds)
            lgid = oldToNew[lgid];
    }

    gateNameToId.clear();
    for (int i = 0; i < (int)newGates.size(); i++)
        gateNameToId[newGates[i].instName] = i;

    int removed = (int)gates.size() - (int)newGates.size();
    gates = newGates;
    return removed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 1.7: Validation / Rollback
// ─────────────────────────────────────────────────────────────────────────────
bool Netlist::validateStructure() const {
    bool ok = true;

    for (int gi = 0; gi < (int)gates.size(); gi++) {
        const Gate& g = gates[gi];
        if (g.type == GateType::UNKNOWN) continue;

        if (g.id != gi) {
            std::cerr << "[validateStructure] gate id mismatch: gate[" << gi << "].id=" << g.id << "\n";
            ok = false;
        }

        if (g.outputNetId >= 0) {
            if (g.outputNetId >= (int)nets.size()) {
                std::cerr << "[validateStructure] gate[" << gi << "] outputNetId out of range\n";
                ok = false;
            } else if (nets[g.outputNetId].driverGateId != gi) {
                std::cerr << "[validateStructure] gate[" << gi << "] outputNet["
                          << g.outputNetId << "].driverGateId="
                          << nets[g.outputNetId].driverGateId << " mismatch\n";
                ok = false;
            }
        }

        for (int inNetId : g.inputNetIds) {
            if (inNetId < 0) continue;
            if (inNetId >= (int)nets.size()) {
                std::cerr << "[validateStructure] gate[" << gi << "] inputNetId out of range\n";
                ok = false;
                continue;
            }
            bool found = false;
            for (int lgid : nets[inNetId].loadGateIds)
                if (lgid == gi) { found = true; break; }
            if (!found) {
                std::cerr << "[validateStructure] gate[" << gi << "] not in net["
                          << inNetId << "].loadGateIds\n";
                ok = false;
            }
        }
    }

    for (int ni = 0; ni < (int)nets.size(); ni++) {
        if (nets[ni].isRemoved) continue;
        int driverGateId = nets[ni].driverGateId;
        if (driverGateId >= 0) {
            if (driverGateId >= (int)gates.size()) {
                std::cerr << "[validateStructure] net[" << ni
                          << "] has invalid driverGateId=" << driverGateId << "\n";
                ok = false;
            } else if (gates[driverGateId].type == GateType::UNKNOWN) {
                std::cerr << "[validateStructure] net[" << ni
                          << "] is driven by removed gate[" << driverGateId << "]\n";
                ok = false;
            } else if (gates[driverGateId].outputNetId != ni) {
                std::cerr << "[validateStructure] net[" << ni
                          << "] driverGate[" << driverGateId
                          << "] does not output to this net\n";
                ok = false;
            }
        }

        for (int lgid : nets[ni].loadGateIds) {
            if (lgid < 0 || lgid >= (int)gates.size()) {
                std::cerr << "[validateStructure] net[" << ni << "] has invalid loadGateId=" << lgid << "\n";
                ok = false;
                continue;
            }
            bool found = false;
            for (int inNetId : gates[lgid].inputNetIds)
                if (inNetId == ni) { found = true; break; }
            if (!found) {
                std::cerr << "[validateStructure] net[" << ni << "] loadGate["
                          << lgid << "] does not have this net as input\n";
                ok = false;
            }
        }
    }

    return ok;
}

bool Netlist::validateProblemAConstraints() const {
    bool ok = true;

    for (int ni = 0; ni < (int)nets.size(); ni++) {
        if (nets[ni].isRemoved) continue;
        if (!nets[ni].isPO) continue;
        if (nets[ni].driverGateId < 0 && !nets[ni].isPI && !nets[ni].isConst) {
            std::cerr << "[validateProblemAConstraints] PO net[" << ni
                      << "] (" << nets[ni].name << ") has no driver\n";
            ok = false;
        }
    }

    for (int gi = 0; gi < (int)gates.size(); gi++) {
        if (gates[gi].type != GateType::DFF) continue;
        if (gates[gi].outputNetId < 0) {
            std::cerr << "[validateProblemAConstraints] DFF gate[" << gi
                      << "] (" << gates[gi].instName << ") has invalid outputNetId\n";
            ok = false;
        }
    }

    return ok;
}

Netlist Netlist::cloneForRollback() const {
    return *this;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.1: Buffer cleanup helpers
// ─────────────────────────────────────────────────────────────────────────────
bool Netlist::replaceAllLoadsOfNet(int oldNetId, int newNetId) {
    if (oldNetId < 0 || oldNetId >= (int)nets.size()) return false;
    if (newNetId < 0 || newNetId >= (int)nets.size()) return false;
    if (oldNetId == newNetId) return false;

    Net& oldNet = nets[oldNetId];
    Net& newNet = nets[newNetId];

    if (oldNet.loadGateIds.empty()) return false;

    for (int lgid : oldNet.loadGateIds) {
        if (lgid < 0 || lgid >= (int)gates.size()) continue;
        Gate& g = gates[lgid];
        for (int k = 0; k < (int)g.inputNetIds.size(); k++)
            if (g.inputNetIds[k] == oldNetId)
                g.inputNetIds[k] = newNetId;
        newNet.loadGateIds.push_back(lgid);
    }

    oldNet.loadGateIds.clear();
    return true;
}

NetlistEditReport Netlist::replaceAllLoadsOfNetWithReport(int oldNetId, int newNetId) {
    Netlist before = cloneForRollback();
    const std::string oldNetName =
        (oldNetId >= 0 && oldNetId < (int)nets.size()) ? nets[oldNetId].name : "";
    const std::string newNetName =
        (newNetId >= 0 && newNetId < (int)nets.size()) ? nets[newNetId].name : "";

    const bool ok = replaceAllLoadsOfNet(oldNetId, newNetId);
    NetlistEditReport report = Netlist::buildEditReport(
        before,
        *this,
        "replaceAllLoadsOfNet",
        NetlistEditOperationKind::PrimitiveMutation);
    report.changed = report.changed || ok;

    if (!oldNetName.empty()) {
        report.changedNetNames.push_back(oldNetName);
        report.changedNetIds.push_back(oldNetId);
    }
    if (!newNetName.empty()) {
        report.changedNetNames.push_back(newNetName);
        report.changedNetIds.push_back(newNetId);
    }

    if (!ok) {
        if (report.changed) {
            restoreFrom(before);
            report.rolledBack = true;
        }
        report.success = false;
        report.message = "Load replacement failed: net id was invalid, source and target were identical, or the source had no loads.";
        return report;
    }

    report.message = report.success
        ? "Load replacement completed."
        : "Load replacement failed validation and was rolled back.";

    if (!report.success) {
        restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

bool Netlist::bypassBufferGate(int bufGateId) {
    if (bufGateId < 0 || bufGateId >= (int)gates.size()) return false;
    Gate& g = gates[bufGateId];

    if (g.type != GateType::BUF) return false;
    if (g.inputNetIds.size() != 1) return false;

    int inNetId  = g.inputNetIds[0];
    int outNetId = g.outputNetId;

    if (inNetId < 0 || outNetId < 0) return false;
    if (nets[outNetId].isPO) return false;
    if (nets[outNetId].isConst) return false;
    if (inNetId == outNetId) return false;
    if (nets[outNetId].loadGateIds.empty()) return false;

    replaceAllLoadsOfNet(outNetId, inNetId);

    // 從 inNet 的 loadGateIds 移除 BUF gate 自身
    auto& loads = nets[inNetId].loadGateIds;
    loads.erase(std::remove(loads.begin(), loads.end(), bufGateId), loads.end());

    nets[outNetId].driverGateId = -1;
    g.type = GateType::UNKNOWN;
    g.inputNetIds.clear();
    g.outputNetId = -1;

    return true;
}

std::vector<int> Netlist::findAllRemovableBufferGates() const {
    std::vector<int> result;
    for (int i = 0; i < (int)gates.size(); i++) {
        const Gate& g = gates[i];
        if (g.type != GateType::BUF) continue;
        if (g.inputNetIds.size() != 1) continue;

        int inNetId  = g.inputNetIds[0];
        int outNetId = g.outputNetId;

        if (inNetId < 0 || outNetId < 0) continue;
        if (nets[outNetId].isPO) continue;
        if (nets[outNetId].isConst) continue;
        if (inNetId == outNetId) continue;
        if (nets[outNetId].loadGateIds.empty()) continue;

        result.push_back(i);
    }
    return result;
}

int Netlist::cleanupAllRemovableBuffers() {
    int removed = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        std::vector<int> candidates = findAllRemovableBufferGates();
        for (int bufGateId : candidates) {
            if (bypassBufferGate(bufGateId)) {
                removed++;
                changed = true;
            }
        }
    }

    if (removed > 0) compactRemovedGates();
    return removed;
}

NetlistEditReport Netlist::cleanupAllRemovableBuffersWithReport() {
    Netlist before = cloneForRollback();

    int removed = cleanupAllRemovableBuffers();
    NetlistEditReport report = buildEditReport(
        before,
        *this,
        "cleanupAllRemovableBuffers",
        NetlistEditOperationKind::Cleanup);

    report.changed = report.changed || removed > 0;
    report.depthChange = buildDepthChangeReport(before, *this);
    if (report.success) {
        certifyEquivalence(
            report,
            EquivalenceCheckMethod::LocalRewriteRule,
            "Equivalence certified by the local Boolean identity BUF(x) = x.");
    }
    report.message = report.success
        ? "Removable buffer cleanup completed."
        : "Removable buffer cleanup failed validation and was rolled back.";

    if (!report.success) {
        restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.3: Double inverter removal
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::pair<int,int>> Netlist::findDoubleInverterPairs() const {
    std::vector<std::pair<int,int>> result;
    for (int i = 0; i < (int)gates.size(); i++) {
        const Gate& g1 = gates[i];
        if (g1.type != GateType::NOT) continue;
        if (g1.outputNetId < 0) continue;
        if (g1.inputNetIds.empty() || g1.inputNetIds[0] < 0) continue;

        const Net& midNet = nets[g1.outputNetId];
        if (midNet.loadGateIds.size() != 1) continue;
        if (midNet.isPI || midNet.isConst) continue;

        int g2id = midNet.loadGateIds[0];
        if (g2id < 0 || g2id >= (int)gates.size()) continue;
        const Gate& g2 = gates[g2id];
        if (g2.type != GateType::NOT) continue;
        if (g2.outputNetId < 0) continue;
        if (nets[g2.outputNetId].isPO) continue;

        result.emplace_back(i, g2id);
    }
    return result;
}

bool Netlist::bypassDoubleInverter(int g1id, int g2id) {
    if (g1id < 0 || g1id >= (int)gates.size()) return false;
    if (g2id < 0 || g2id >= (int)gates.size()) return false;

    Gate& g1 = gates[g1id];
    Gate& g2 = gates[g2id];

    if (g1.type != GateType::NOT || g2.type != GateType::NOT) return false;
    if (g1.outputNetId < 0 || g2.outputNetId < 0) return false;
    if (g1.inputNetIds.empty() || g1.inputNetIds[0] < 0) return false;

    int inNetId  = g1.inputNetIds[0];
    int midNetId = g1.outputNetId;
    int outNetId = g2.outputNetId;

    if (nets[midNetId].loadGateIds.size() != 1) return false;
    if (nets[outNetId].isPO) return false;

    replaceAllLoadsOfNet(outNetId, inNetId);

    auto& loads = nets[inNetId].loadGateIds;
    loads.erase(std::remove(loads.begin(), loads.end(), g1id), loads.end());

    g1.type = GateType::UNKNOWN; g1.inputNetIds.clear(); g1.outputNetId = -1;
    g2.type = GateType::UNKNOWN; g2.inputNetIds.clear(); g2.outputNetId = -1;
    nets[midNetId].driverGateId = -1; nets[midNetId].loadGateIds.clear();
    nets[outNetId].driverGateId = -1; nets[outNetId].loadGateIds.clear();

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.4: Constant propagation helpers
// ─────────────────────────────────────────────────────────────────────────────
bool Netlist::isConst0Net(int netId) const {
    if (netId < 0 || netId >= (int)nets.size()) return false;
    return nets[netId].isConst && (nets[netId].constVal == 0);
}

bool Netlist::isConst1Net(int netId) const {
    if (netId < 0 || netId >= (int)nets.size()) return false;
    return nets[netId].isConst && (nets[netId].constVal == 1);
}

int Netlist::getConst0NetId() const {
    for (int i = 0; i < (int)nets.size(); i++)
        if (nets[i].isConst && nets[i].constVal == 0) return i;
    return -1;
}

int Netlist::getConst1NetId() const {
    for (int i = 0; i < (int)nets.size(); i++)
        if (nets[i].isConst && nets[i].constVal == 1) return i;
    return -1;
}

bool Netlist::simplifyGateWithConstant(int gateId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    Gate& g = gates[gateId];
    if (g.type == GateType::UNKNOWN || g.type == GateType::DFF) return false;
    if (g.outputNetId < 0) return false;
    if (nets[g.outputNetId].isPO) return false;

    int outNetId = g.outputNetId;
    int const0   = getConst0NetId();
    int const1   = getConst1NetId();

    auto replaceGateWithNet = [&](int srcNetId) -> bool {
        if (srcNetId < 0) return false;
        replaceAllLoadsOfNet(outNetId, srcNetId);
        for (int inNetId : g.inputNetIds) {
            if (inNetId < 0) continue;
            auto& loads = nets[inNetId].loadGateIds;
            loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
        }
        nets[outNetId].driverGateId = -1;
        g.type = GateType::UNKNOWN;
        g.inputNetIds.clear();
        g.outputNetId = -1;
        return true;
    };

    auto insertNotAndReplace = [&](int srcNetId) -> bool {
        if (srcNetId < 0) return false;
        static int notCounter = 0;
        notCounter++;
        for (int inNetId : g.inputNetIds) {
            if (inNetId < 0) continue;
            auto& loads = nets[inNetId].loadGateIds;
            loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
        }
        g.type = GateType::NOT;
        g.inputNetIds = { srcNetId };
        nets[srcNetId].loadGateIds.push_back(gateId);
        return true;
    };

    // commutative gate helper（和 mergeEquivalentGates 保持一致）
    auto isCommutative = [](GateType t) {
        return t == GateType::AND || t == GateType::OR  || t == GateType::XOR ||
               t == GateType::NAND || t == GateType::NOR || t == GateType::XNOR;
    };

    GateType t = g.type;

    if (t == GateType::NOT || t == GateType::BUF) {
        if (g.inputNetIds.size() != 1) return false;
        int a = g.inputNetIds[0];
        if (isConst0Net(a)) return replaceGateWithNet(t == GateType::NOT ? const1 : const0);
        if (isConst1Net(a)) return replaceGateWithNet(t == GateType::NOT ? const0 : const1);
        return false;
    }

    if (g.inputNetIds.size() < 2) return false;
    int a = g.inputNetIds[0];
    int b = g.inputNetIds[1];
    bool aIs0 = isConst0Net(a), aIs1 = isConst1Net(a);
    bool bIs0 = isConst0Net(b), bIs1 = isConst1Net(b);
    if (!aIs0 && !aIs1 && !bIs0 && !bIs1) return false;

    // 正規化：commutative gate 把常數放到 b
    if (isCommutative(t) && (aIs0 || aIs1)) {
        std::swap(a, b);
        std::swap(aIs0, bIs0);
        std::swap(aIs1, bIs1);
    }

    if      (t == GateType::AND)  { if (bIs1) return replaceGateWithNet(a);      if (bIs0) return replaceGateWithNet(const0); }
    else if (t == GateType::OR)   { if (bIs0) return replaceGateWithNet(a);      if (bIs1) return replaceGateWithNet(const1); }
    else if (t == GateType::XOR)  { if (bIs0) return replaceGateWithNet(a);      if (bIs1) return insertNotAndReplace(a); }
    else if (t == GateType::NAND) { if (bIs1) return insertNotAndReplace(a);     if (bIs0) return replaceGateWithNet(const1); }
    else if (t == GateType::NOR)  { if (bIs0) return insertNotAndReplace(a);     if (bIs1) return replaceGateWithNet(const0); }
    else if (t == GateType::XNOR) { if (bIs0) return insertNotAndReplace(a);     if (bIs1) return replaceGateWithNet(a); }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.5: Same-input simplification
// ─────────────────────────────────────────────────────────────────────────────
std::vector<int> Netlist::findSameInputGates() const {
    std::vector<int> result;
    for (int i = 0; i < (int)gates.size(); i++) {
        const Gate& g = gates[i];
        if (g.type == GateType::UNKNOWN || g.type == GateType::DFF) continue;
        if (g.inputNetIds.size() < 2) continue;
        if (g.inputNetIds[0] >= 0 && g.inputNetIds[0] == g.inputNetIds[1])
            result.push_back(i);
    }
    return result;
}

bool Netlist::simplifySameInputGate(int gateId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    Gate& g = gates[gateId];
    if (g.type == GateType::UNKNOWN) return false;
    if (g.inputNetIds.size() < 2) return false;
    if (g.inputNetIds[0] < 0 || g.inputNetIds[0] != g.inputNetIds[1]) return false;
    if (g.outputNetId < 0) return false;
    if (nets[g.outputNetId].isPO) return false;

    int a        = g.inputNetIds[0];
    int outNetId = g.outputNetId;
    int const0   = getConst0NetId();
    int const1   = getConst1NetId();
    GateType t   = g.type;

    auto replaceGateWithNet = [&](int srcNetId) -> bool {
        if (srcNetId < 0) return false;
        replaceAllLoadsOfNet(outNetId, srcNetId);
        // a == b，loadGateIds 裡有兩份 gateId，全部移除
        auto& loads = nets[a].loadGateIds;
        loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
        nets[outNetId].driverGateId = -1;
        g.type = GateType::UNKNOWN;
        g.inputNetIds.clear();
        g.outputNetId = -1;
        return true;
    };

    auto replaceGateWithNot = [&](int srcNetId) -> bool {
        if (srcNetId < 0) return false;
        // 移除 loadGateIds 裡的兩份 gateId，之後只保留一份（重新 connect）
        auto& loads = nets[a].loadGateIds;
        loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
        g.type = GateType::NOT;
        g.inputNetIds = { srcNetId };
        nets[srcNetId].loadGateIds.push_back(gateId);
        return true;
    };

    if      (t == GateType::AND)  return replaceGateWithNet(a);
    else if (t == GateType::OR)   return replaceGateWithNet(a);
    else if (t == GateType::XOR)  return replaceGateWithNet(const0);
    else if (t == GateType::XNOR) return replaceGateWithNet(const1);
    else if (t == GateType::NAND) return replaceGateWithNot(a);
    else if (t == GateType::NOR)  return replaceGateWithNot(a);

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.6: Dangling logic removal
// ─────────────────────────────────────────────────────────────────────────────
std::unordered_set<int> Netlist::getEssentialGateIdsForTimingEndpoints() const {
    std::unordered_set<int> usefulGates;
    std::unordered_set<int> visitedNets;
    std::queue<int> q;

    // PO seeds
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].isPO)
            visitedNets.insert(i), q.push(i);
    }

    // DFF seeds (inputs)
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type != GateType::DFF) continue;
        usefulGates.insert(i);
        for (int inNetId : gates[i].inputNetIds) {
            if (inNetId >= 0 && visitedNets.insert(inNetId).second)
                q.push(inNetId);
        }
    }

    // backward traversal
    while (!q.empty()) {
        int netId = q.front(); q.pop();
        const Net& net = nets[netId];

        if (net.driverGateId < 0 || net.driverGateId >= (int)gates.size())
            continue;

        int g = net.driverGateId;
        if (!usefulGates.insert(g).second) continue;

        for (int inNetId : gates[g].inputNetIds) {
            if (inNetId >= 0 && visitedNets.insert(inNetId).second)
                q.push(inNetId);
        }
    }

    return usefulGates;
}

std::vector<int> Netlist::findDanglingGateIds() const {
    std::unordered_set<int> essential = getEssentialGateIdsForTimingEndpoints();
    std::vector<int> dangling;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type == GateType::UNKNOWN) continue;
        if (!essential.count(i)) dangling.push_back(i);
    }
    return dangling;
}


// =========================================================================
// 2. 懸空邏輯清理 (Dangling Logic Removal) - 大賽安全防護版
// =========================================================================
int Netlist::removeDanglingLogic() {
    std::unordered_set<int> usefulGates;
    std::unordered_set<int> usefulNets;
    std::queue<int> q;

    // 【雙重保險起點】同時檢查 nets 裡標記為 isPO 的，以及找出最後一級可能驅動 PO 的 Gate
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].isPO) {
            usefulNets.insert(i);
            q.push(i);
        }
    }

    // 如果 usefulNets 竟然是空的（代表 Reader 標記漏掉或被洗掉），
    // 強制把所有「輸出端網線沒有被任何其他閘當作 input」的活閘輸出線，通通當成 PO 保護起來！
    if (q.empty()) {
        std::vector<int> isUsedAsInput((int)nets.size(), 0);
        for (const auto& g : gates) {
            if (g.type == GateType::UNKNOWN) continue;
            for (int inId : g.inputNetIds) {
                if (inId >= 0) isUsedAsInput[inId] = 1;
            }
        }
        for (int i = 0; i < (int)gates.size(); i++) {
            if (gates[i].type == GateType::UNKNOWN) continue;
            int onet = gates[i].outputNetId;
            if (onet >= 0 && isUsedAsInput[onet] == 0) {
                // 這個閘的輸出沒有任何人用，它在目前殘存結構中就是事實上的 PO！
                nets[onet].isPO = true; 
                usefulNets.insert(onet);
                q.push(onet);
            }
        }
    }

    // 反向 BFS 擴散標記
    while (!q.empty()) {
        int currNetId = q.front();
        q.pop();

        int dgid = nets[currNetId].driverGateId;
        if (dgid < 0) continue; 

        if (gates[dgid].type != GateType::UNKNOWN && !usefulGates.count(dgid)) {
            usefulGates.insert(dgid);
            for (int inNetId : gates[dgid].inputNetIds) {
                if (inNetId >= 0 && !usefulNets.count(inNetId)) {
                    usefulNets.insert(inNetId);
                    q.push(inNetId);
                }
            }
        }
    }

    // 最終刪除階段
    int removedCount = 0;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type == GateType::UNKNOWN) continue;

        int outNetId = gates[i].outputNetId;
        
        // 核心強制保護：如果它驅動的 Net 已經被我們確認是 usefulNets (包含所有PO路徑)，
        // 或者該 Net 標記為 isPO，絕對、無條件跳過，不准設為 UNKNOWN！
        if (outNetId >= 0 && (nets[outNetId].isPO || usefulNets.count(outNetId))) {
            // 如果上游被優化拔光了，為了讓它成為合法節點，強制轉成 BUF
            if (gates[i].type != GateType::BUF && gates[i].inputNetIds.empty()) {
                gates[i].type = GateType::BUF;
                // 嘗試接向常數或隨便一個 PI 訊號源，避免 dangling pin 錯誤
                for (int n = 0; n < (int)nets.size(); n++) {
                    if (nets[n].isPI || nets[n].isConst) {
                        gates[i].inputNetIds = { n };
                        nets[n].loadGateIds.push_back(i);
                        break;
                    }
                }
            }
            continue; 
        }

        // 真正沒用且跟輸出毫無關聯的閘，才執行刪除
        if (!usefulGates.count(i)) {
            if (markGateRemoved(i)) {
                removedCount++;
            }
        }
    }

    return removedCount;
}

NetlistEditReport Netlist::removeDanglingLogicWithReport() {
    Netlist before = cloneForRollback();
    int removed = removeDanglingLogic();
    return finalizeEditReport(
        *this,
        before,
        removed,
        "removeDanglingLogic",
        NetlistEditOperationKind::Cleanup,
        "Dangling logic removal completed.",
        "Dangling logic removal failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified by removing logic that does not drive any PO or DFF input endpoint.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Section 2.7: Structural hashing
// ─────────────────────────────────────────────────────────────────────────────
std::string Netlist::makeStructuralKey(int gateId) const {
    if (gateId < 0 || gateId >= (int)gates.size()) return "";
    const Gate& g = gates[gateId];
    if (g.type == GateType::UNKNOWN) return "";

    std::vector<int> inputs = g.inputNetIds;
    if (g.type == GateType::AND || g.type == GateType::OR  || g.type == GateType::XOR ||
        g.type == GateType::NAND || g.type == GateType::NOR || g.type == GateType::XNOR)
        std::sort(inputs.begin(), inputs.end());

    std::string key = std::to_string((int)g.type) + ":";
    for (int i = 0; i < (int)inputs.size(); i++) {
        if (i > 0) key += ",";
        key += std::to_string(inputs[i]);
    }
    return key;
}

std::vector<std::vector<int>> Netlist::findStructurallyEquivalentGateGroups() const {
    std::unordered_map<std::string, std::vector<int>> keyToGates;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type == GateType::UNKNOWN) continue;
        if (gates[i].outputNetId < 0) continue;
        std::string key = makeStructuralKey(i);
        if (!key.empty()) keyToGates[key].push_back(i);
    }

    std::vector<std::vector<int>> result;
    for (auto& kv : keyToGates)
        if (kv.second.size() >= 2)
            result.push_back(kv.second);
    return result;
}

int Netlist::mergeStructurallyEquivalentGates() {
    int merged = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        std::vector<std::vector<int>> groups = findStructurallyEquivalentGateGroups();
        for (auto& group : groups) {
            if (group.size() < 2) continue;
            int keepGateId = group[0];
            int keepOutNet = gates[keepGateId].outputNetId;
            if (keepOutNet < 0) continue;

            for (int j = 1; j < (int)group.size(); j++) {
                int deadGateId = group[j];
                int deadOutNet = gates[deadGateId].outputNetId;
                if (deadOutNet < 0 || deadOutNet == keepOutNet) continue;

                for (int loadGateId : nets[deadOutNet].loadGateIds) {
                    Gate& loadGate = gates[loadGateId];
                    for (int k = 0; k < (int)loadGate.inputNetIds.size(); k++) {
                        if (loadGate.inputNetIds[k] == deadOutNet) {
                            loadGate.inputNetIds[k] = keepOutNet;
                            nets[keepOutNet].loadGateIds.push_back(loadGateId);
                        }
                    }
                }

                if (nets[deadOutNet].isPO) {
                    nets[keepOutNet].isPO = true;
                    for (int pi = 0; pi < (int)primaryOutputs.size(); pi++)
                        for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++)
                            if (primaryOutputs[pi].netIds[pj] == deadOutNet)
                                primaryOutputs[pi].netIds[pj] = keepOutNet;
                }

                // 清除 deadGate 在 input nets 上的負載
                for (int inNetId : gates[deadGateId].inputNetIds) {
                    auto& loads = nets[inNetId].loadGateIds;
                    loads.erase(std::remove(loads.begin(), loads.end(), deadGateId), loads.end());
                }

                nets[deadOutNet].loadGateIds.clear();
                nets[deadOutNet].driverGateId = -1;
                gates[deadGateId].type = GateType::UNKNOWN;
                gates[deadGateId].outputNetId = -1;
                gates[deadGateId].inputNetIds.clear();

                merged++;
                changed = true;
            }
        }
    }

    if (merged > 0) trimDeadLogic();
    return merged;
}

NetlistEditReport Netlist::mergeStructurallyEquivalentGatesWithReport() {
    Netlist before = cloneForRollback();
    int merged = mergeStructurallyEquivalentGates();
    return finalizeEditReport(
        *this,
        before,
        merged,
        "mergeStructurallyEquivalentGates",
        NetlistEditOperationKind::Simplification,
        "Structurally equivalent gate merge completed.",
        "Structurally equivalent gate merge failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified by structural hashing over gate type and input nets.");
}

// =============================================================================
//  B-5: Unique name generator（P0）
//  【說明】產生不會撞名的 gate / net 名稱，避免 repeated pass 後命名衝突。
// =============================================================================

std::string Netlist::makeUniqueGateName(const std::string& prefix) const {
    int counter = 0;
    while (true) {
        std::string name = prefix + "_" + std::to_string(counter++);
        if (gateNameToId.find(name) == gateNameToId.end()) return name;
    }
}

std::string Netlist::makeUniqueNetName(const std::string& prefix) const {
    int counter = 0;
    while (true) {
        std::string name = prefix + "_" + std::to_string(counter++);
        if (netNameToId.find(name) == netNameToId.end()) return name;
    }
}

int Netlist::addGateWithUniqueName(const std::string& prefix, GateType type) {
    return addGate(makeUniqueGateName(prefix), type);
}

int Netlist::addNetWithUniqueName(const std::string& prefix) {
    return addNet(makeUniqueNetName(prefix));
}

// =============================================================================
//  B-4: Gate replacement primitives（P0）
//  【說明】把 gate 直接替換成 net / NOT(net) / constant，是 constant propagation
//          和 same-input 化簡的共同底層。
// =============================================================================

// 把 gate 的 output 改成直接由 sourceNet 驅動（gate 本身消失）
bool Netlist::replaceGateWithNet(int gateId, int sourceNetId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    if (sourceNetId < 0 || sourceNetId >= (int)nets.size()) return false;
    Gate& g = gates[gateId];
    if (g.type == GateType::UNKNOWN) return false;
    int outNetId = g.outputNetId;
    if (outNetId < 0) return false;
    if (nets[outNetId].isPO) return false; // PO-safe 版本請用 preservePortNetAndReplaceDriver

    replaceAllLoadsOfNet(outNetId, sourceNetId);
    for (int inNetId : g.inputNetIds) {
        if (inNetId < 0) continue;
        auto& loads = nets[inNetId].loadGateIds;
        loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
    }
    nets[outNetId].driverGateId = -1;
    g.type = GateType::UNKNOWN; g.inputNetIds.clear(); g.outputNetId = -1;
    return true;
}

NetlistEditReport Netlist::replaceGateWithNetWithReport(int gateId, int sourceNetId) {
    Netlist before = cloneForRollback();
    const std::string gateName =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].instName : "";
    const std::string sourceNetName =
        (sourceNetId >= 0 && sourceNetId < (int)nets.size()) ? nets[sourceNetId].name : "";
    const int outputNetId =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].outputNetId : -1;
    const std::string outputNetName =
        (outputNetId >= 0 && outputNetId < (int)nets.size()) ? nets[outputNetId].name : "";

    const bool ok = replaceGateWithNet(gateId, sourceNetId);
    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "replaceGateWithNet",
        "Gate replacement with net completed.",
        "Gate replacement with net failed: gate/source net was invalid, removed, or the output net is PO.",
        !gateName.empty() ? std::vector<std::string>{gateName} : std::vector<std::string>{},
        !sourceNetName.empty() || !outputNetName.empty()
            ? std::vector<std::string>{sourceNetName, outputNetName}
            : std::vector<std::string>{},
        gateId >= 0 ? std::vector<int>{gateId} : std::vector<int>{},
        sourceNetId >= 0 || outputNetId >= 0
            ? std::vector<int>{sourceNetId, outputNetId}
            : std::vector<int>{});
}

// 把 gate 替換成常數 net（直接改接所有 load）
bool Netlist::replaceGateWithConstant(int gateId, int constNetId) {
    return replaceGateWithNet(gateId, constNetId);
}

NetlistEditReport Netlist::replaceGateWithConstantWithReport(int gateId, int constNetId) {
    Netlist before = cloneForRollback();
    const std::string gateName =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].instName : "";
    const std::string constNetName =
        (constNetId >= 0 && constNetId < (int)nets.size()) ? nets[constNetId].name : "";

    const bool ok = replaceGateWithConstant(gateId, constNetId);
    NetlistEditReport report = finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "replaceGateWithConstant",
        "Gate replacement with constant completed.",
        "Gate replacement with constant failed: gate/constant net was invalid, removed, or the output net is PO.",
        !gateName.empty() ? std::vector<std::string>{gateName} : std::vector<std::string>{},
        !constNetName.empty() ? std::vector<std::string>{constNetName} : std::vector<std::string>{},
        gateId >= 0 ? std::vector<int>{gateId} : std::vector<int>{},
        constNetId >= 0 ? std::vector<int>{constNetId} : std::vector<int>{});
    if (constNetId >= 0 && constNetId < (int)nets.size() && !nets[constNetId].isConst) {
        report.addWarning("constNetId does not refer to a constant net.");
    }
    return report;
}

// 把 gate 替換成 NOT(sourceNet)（gate 被改寫成 NOT gate）
bool Netlist::replaceGateWithNotOfNet(int gateId, int sourceNetId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    if (sourceNetId < 0 || sourceNetId >= (int)nets.size()) return false;
    Gate& g = gates[gateId];
    if (g.type == GateType::UNKNOWN) return false;
    if (g.outputNetId < 0 || nets[g.outputNetId].isPO) return false;

    // 從所有原本的 input net 移除此 gate
    for (int inNetId : g.inputNetIds) {
        if (inNetId < 0) continue;
        auto& loads = nets[inNetId].loadGateIds;
        loads.erase(std::remove(loads.begin(), loads.end(), gateId), loads.end());
    }
    g.type = GateType::NOT;
    g.inputNetIds = { sourceNetId };
    nets[sourceNetId].loadGateIds.push_back(gateId);
    return true;
}

NetlistEditReport Netlist::replaceGateWithNotOfNetWithReport(int gateId, int sourceNetId) {
    Netlist before = cloneForRollback();
    const std::string gateName =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].instName : "";
    const std::string sourceNetName =
        (sourceNetId >= 0 && sourceNetId < (int)nets.size()) ? nets[sourceNetId].name : "";
    const int outputNetId =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].outputNetId : -1;
    const std::string outputNetName =
        (outputNetId >= 0 && outputNetId < (int)nets.size()) ? nets[outputNetId].name : "";

    const bool ok = replaceGateWithNotOfNet(gateId, sourceNetId);
    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "replaceGateWithNotOfNet",
        "Gate replacement with NOT(source net) completed.",
        "Gate replacement with NOT(source net) failed: gate/source net was invalid, removed, or the output net is PO.",
        !gateName.empty() ? std::vector<std::string>{gateName} : std::vector<std::string>{},
        !sourceNetName.empty() || !outputNetName.empty()
            ? std::vector<std::string>{sourceNetName, outputNetName}
            : std::vector<std::string>{},
        gateId >= 0 ? std::vector<int>{gateId} : std::vector<int>{},
        sourceNetId >= 0 || outputNetId >= 0
            ? std::vector<int>{sourceNetId, outputNetId}
            : std::vector<int>{});
}

// 新增一個 gate 並讓它驅動指定的 outputNet
// 回傳新 gate 的 ID；失敗回傳 -1
int Netlist::createGateDrivingNet(
    GateType type,
    const std::vector<int>& inputNetIds,
    int outputNetId,
    const std::string& nameHint)
{
    if (outputNetId < 0 || outputNetId >= (int)nets.size()) return -1;
    if (nets[outputNetId].isRemoved) return -1;

    int gateId = addGateWithUniqueName(nameHint.empty() ? "_cg" : nameHint, type);

    int oldDriver = nets[outputNetId].driverGateId;
    if (oldDriver >= 0 && oldDriver < (int)gates.size() && oldDriver != gateId) {
        if (gates[oldDriver].outputNetId == outputNetId) {
            gates[oldDriver].outputNetId = -1;
        }
    }

    gates[gateId].outputNetId = outputNetId;
    nets[outputNetId].driverGateId = gateId;

    for (int inNetId : inputNetIds) {
        if (inNetId < 0 || inNetId >= (int)nets.size()) continue;
        gates[gateId].inputNetIds.push_back(inNetId);
        nets[inNetId].loadGateIds.push_back(gateId);
    }
    return gateId;
}

// 把 gate 的所有 pin 斷開，標記為 UNKNOWN（不 compact）
bool Netlist::removeGateAndDetachPins(int gateId) {
    return markGateRemoved(gateId);
}

// =============================================================================
//  B-1: Pin-level 精準改線 primitive（P0）
//  【說明】讓 rewrite pass 可以精確改指定的 input pin，不影響其他 pin。
// =============================================================================

// 斷開指定 gate 的第 pinIndex 個 input pin
bool Netlist::disconnectGateInputPin(int gateId, int pinIndex) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    Gate& g = gates[gateId];
    if (pinIndex < 0 || pinIndex >= (int)g.inputNetIds.size()) return false;

    int oldNetId = g.inputNetIds[pinIndex];
    if (oldNetId >= 0 && oldNetId < (int)nets.size()) {
        auto& loads = nets[oldNetId].loadGateIds;
        eraseOneLoad(loads, gateId);
    }
    g.inputNetIds[pinIndex] = -1;
    return true;
}

// 把指定 gate 的第 pinIndex 個 input pin 接到 netId
bool Netlist::connectGateInputPin(int gateId, int pinIndex, int netId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    if (pinIndex < 0) return false;
    if (netId < 0 || netId >= (int)nets.size()) return false;
    if (nets[netId].isRemoved) return false;
    Gate& g = gates[gateId];

    // 如果 pinIndex 超出現有大小，先擴充
    if (pinIndex >= (int)g.inputNetIds.size())
        g.inputNetIds.resize(pinIndex + 1, -1);

    // 先斷開舊的
    int oldNetId = g.inputNetIds[pinIndex];
    if (oldNetId >= 0 && oldNetId < (int)nets.size()) {
        auto& loads = nets[oldNetId].loadGateIds;
        eraseOneLoad(loads, gateId);
    }

    g.inputNetIds[pinIndex] = netId;
    nets[netId].loadGateIds.push_back(gateId);
    return true;
}

// 把指定 gate 的第 pinIndex 個 input pin 改接到 newNetId（先斷舊再連新）
bool Netlist::reconnectGateInputPin(int gateId, int pinIndex, int newNetId) {
    return connectGateInputPin(gateId, pinIndex, newNetId);
}

// 用名稱版本：把指定 gate 的 pinName pin 改接到 newNetName
bool Netlist::reconnectGateInputPinByName(
    const std::string& gateName,
    const std::string& pinName,
    const std::string& newNetName)
{
    int gateId = getGateId(gateName);
    int newNetId = getNetId(newNetName);
    if (gateId < 0 || newNetId < 0) return false;

    Gate& g = gates[gateId];
    // 找 pinName 對應的 index
    for (int i = 0; i < (int)g.inputPinNames.size(); i++) {
        if (g.inputPinNames[i] == pinName) {
            return reconnectGateInputPin(gateId, i, newNetId);
        }
    }
    return false;
}

// =============================================================================
//  B-2: PO-safe output rewrite primitive（P1）
//  【說明】當 output net 是 PO 時，不能直接移掉 net，要保留 port name，改變 driver。
// =============================================================================

// 把 targetNet 的 driver 改成 newDriverGate
// targetNet 保留（含 PO flag / name），只換掉是誰在驅動它
bool Netlist::replaceDriverOfNet(int targetNetId, int newDriverGateId) {
    if (targetNetId < 0 || targetNetId >= (int)nets.size()) return false;
    if (newDriverGateId < 0 || newDriverGateId >= (int)gates.size()) return false;
    if (nets[targetNetId].isRemoved) return false;
    if (gates[newDriverGateId].type == GateType::UNKNOWN) return false;

    int oldDriver = nets[targetNetId].driverGateId;
    if (oldDriver >= 0 && oldDriver < (int)gates.size())
        gates[oldDriver].outputNetId = -1;

    int oldOutputNet = gates[newDriverGateId].outputNetId;
    if (oldOutputNet >= 0 && oldOutputNet < (int)nets.size() &&
        nets[oldOutputNet].driverGateId == newDriverGateId) {
        nets[oldOutputNet].driverGateId = -1;
    }

    nets[targetNetId].driverGateId = newDriverGateId;
    gates[newDriverGateId].outputNetId = targetNetId;
    return true;
}

NetlistEditReport Netlist::replaceDriverOfNetWithReport(int targetNetId, int newDriverGateId) {
    Netlist before = cloneForRollback();
    const std::string targetNetName =
        (targetNetId >= 0 && targetNetId < (int)nets.size()) ? nets[targetNetId].name : "";
    const int oldDriverGateId =
        (targetNetId >= 0 && targetNetId < (int)nets.size()) ? nets[targetNetId].driverGateId : -1;
    const std::string oldDriverGateName =
        (oldDriverGateId >= 0 && oldDriverGateId < (int)gates.size()) ? gates[oldDriverGateId].instName : "";
    const std::string newDriverGateName =
        (newDriverGateId >= 0 && newDriverGateId < (int)gates.size()) ? gates[newDriverGateId].instName : "";

    const bool ok = replaceDriverOfNet(targetNetId, newDriverGateId);

    std::vector<std::string> changedGateNames;
    if (!oldDriverGateName.empty()) changedGateNames.push_back(oldDriverGateName);
    if (!newDriverGateName.empty()) changedGateNames.push_back(newDriverGateName);
    std::vector<int> changedGateIds;
    if (oldDriverGateId >= 0) changedGateIds.push_back(oldDriverGateId);
    if (newDriverGateId >= 0) changedGateIds.push_back(newDriverGateId);

    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "replaceDriverOfNet",
        "Net driver replacement completed.",
        "Net driver replacement failed: target net or new driver gate was invalid or removed.",
        changedGateNames,
        !targetNetName.empty() ? std::vector<std::string>{targetNetName} : std::vector<std::string>{},
        changedGateIds,
        targetNetId >= 0 ? std::vector<int>{targetNetId} : std::vector<int>{});
}

// 把 gate 的 output 改接到 newOutputNetId（舊的 output net 不動，只斷開）
bool Netlist::rewireGateOutputToExistingNet(int gateId, int newOutputNetId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    if (newOutputNetId < 0 || newOutputNetId >= (int)nets.size()) return false;
    if (gates[gateId].type == GateType::UNKNOWN) return false;
    if (nets[newOutputNetId].isRemoved) return false;

    int oldOutNet = gates[gateId].outputNetId;
    if (oldOutNet >= 0 && oldOutNet < (int)nets.size())
        if (nets[oldOutNet].driverGateId == gateId)
            nets[oldOutNet].driverGateId = -1;

    int oldDriver = nets[newOutputNetId].driverGateId;
    if (oldDriver >= 0 && oldDriver < (int)gates.size() && oldDriver != gateId) {
        if (gates[oldDriver].outputNetId == newOutputNetId) {
            gates[oldDriver].outputNetId = -1;
        }
    }

    gates[gateId].outputNetId = newOutputNetId;
    nets[newOutputNetId].driverGateId = gateId;
    return true;
}

NetlistEditReport Netlist::rewireGateOutputToExistingNetWithReport(int gateId, int newOutputNetId) {
    Netlist before = cloneForRollback();
    const std::string gateName =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].instName : "";
    const int oldOutputNetId =
        (gateId >= 0 && gateId < (int)gates.size()) ? gates[gateId].outputNetId : -1;
    const std::string oldOutputNetName =
        (oldOutputNetId >= 0 && oldOutputNetId < (int)nets.size()) ? nets[oldOutputNetId].name : "";
    const std::string newOutputNetName =
        (newOutputNetId >= 0 && newOutputNetId < (int)nets.size()) ? nets[newOutputNetId].name : "";
    const int oldTargetDriverGateId =
        (newOutputNetId >= 0 && newOutputNetId < (int)nets.size()) ? nets[newOutputNetId].driverGateId : -1;
    const std::string oldTargetDriverGateName =
        (oldTargetDriverGateId >= 0 && oldTargetDriverGateId < (int)gates.size())
            ? gates[oldTargetDriverGateId].instName
            : "";

    const bool ok = rewireGateOutputToExistingNet(gateId, newOutputNetId);

    std::vector<std::string> changedGateNames;
    if (!gateName.empty()) changedGateNames.push_back(gateName);
    if (!oldTargetDriverGateName.empty()) changedGateNames.push_back(oldTargetDriverGateName);
    std::vector<std::string> changedNetNames;
    if (!oldOutputNetName.empty()) changedNetNames.push_back(oldOutputNetName);
    if (!newOutputNetName.empty()) changedNetNames.push_back(newOutputNetName);
    std::vector<int> changedGateIds;
    if (gateId >= 0) changedGateIds.push_back(gateId);
    if (oldTargetDriverGateId >= 0) changedGateIds.push_back(oldTargetDriverGateId);
    std::vector<int> changedNetIds;
    if (oldOutputNetId >= 0) changedNetIds.push_back(oldOutputNetId);
    if (newOutputNetId >= 0) changedNetIds.push_back(newOutputNetId);

    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "rewireGateOutputToExistingNet",
        "Gate output rewire completed.",
        "Gate output rewire failed: gate or target net was invalid or removed.",
        changedGateNames,
        changedNetNames,
        changedGateIds,
        changedNetIds);
}

// 保留 PO net（不改名、不移除），但插入一個新 gate 來驅動它
// 等於：新建 gate(type, inputNetIds) -> poNetId
bool Netlist::preservePortNetAndReplaceDriver(
    int poNetId,
    GateType newGateType,
    const std::vector<int>& inputNetIds)
{
    if (poNetId < 0 || poNetId >= (int)nets.size()) return false;

    // 移除舊 driver 的 outputNetId
    int oldDriver = nets[poNetId].driverGateId;
    if (oldDriver >= 0 && oldDriver < (int)gates.size())
        gates[oldDriver].outputNetId = -1;

    // 新增一個 gate 驅動這個 PO net
    int newGateId = createGateDrivingNet(newGateType, inputNetIds, poNetId, "_po_drv");
    return newGateId >= 0;
}

// 把 targetNet 的功能換成 sourceNet 的功能，但保留 targetNet 的名稱與 PO flag
// 做法：在 targetNet 前面插入一個 BUF，讓 sourceNet -> BUF -> targetNet
bool Netlist::replaceNetFunctionWithNetKeepingName(int targetNetId, int sourceNetId) {
    if (targetNetId < 0 || targetNetId >= (int)nets.size()) return false;
    if (sourceNetId < 0 || sourceNetId >= (int)nets.size()) return false;
    if (targetNetId == sourceNetId) return true;

    // 如果 targetNet 不是 PO，直接 replaceAllLoadsOfNet 即可
    if (!nets[targetNetId].isPO) {
        return replaceAllLoadsOfNet(targetNetId, sourceNetId);
    }

    // 如果是 PO，插入一個 BUF：sourceNet -> BUF -> targetNet
    return preservePortNetAndReplaceDriver(targetNetId, GateType::BUF, { sourceNetId });
}

NetlistEditReport Netlist::replaceNetFunctionWithNetKeepingNameWithReport(int targetNetId, int sourceNetId) {
    Netlist before = cloneForRollback();
    const std::string targetNetName =
        (targetNetId >= 0 && targetNetId < (int)nets.size()) ? nets[targetNetId].name : "";
    const std::string sourceNetName =
        (sourceNetId >= 0 && sourceNetId < (int)nets.size()) ? nets[sourceNetId].name : "";
    const int oldDriverGateId =
        (targetNetId >= 0 && targetNetId < (int)nets.size()) ? nets[targetNetId].driverGateId : -1;
    const std::string oldDriverGateName =
        (oldDriverGateId >= 0 && oldDriverGateId < (int)gates.size()) ? gates[oldDriverGateId].instName : "";

    const bool ok = replaceNetFunctionWithNetKeepingName(targetNetId, sourceNetId);
    NetlistEditReport report = Netlist::buildEditReport(
        before,
        *this,
        "replaceNetFunctionWithNetKeepingName",
        NetlistEditOperationKind::PrimitiveMutation);
    report.changed = report.changed || (ok && targetNetId != sourceNetId);

    if (!oldDriverGateName.empty()) {
        report.changedGateNames.push_back(oldDriverGateName);
        report.changedGateIds.push_back(oldDriverGateId);
    }
    const int newDriverGateId =
        (targetNetId >= 0 && targetNetId < (int)nets.size()) ? nets[targetNetId].driverGateId : -1;
    if (newDriverGateId >= 0 && newDriverGateId < (int)gates.size() &&
        newDriverGateId != oldDriverGateId) {
        report.changedGateNames.push_back(gates[newDriverGateId].instName);
        report.changedGateIds.push_back(newDriverGateId);
    }
    if (!targetNetName.empty()) {
        report.changedNetNames.push_back(targetNetName);
        report.changedNetIds.push_back(targetNetId);
    }
    if (!sourceNetName.empty()) {
        report.changedNetNames.push_back(sourceNetName);
        report.changedNetIds.push_back(sourceNetId);
    }

    if (!ok) {
        if (report.changed) {
            restoreFrom(before);
            report.rolledBack = true;
        }
        report.success = false;
        report.message = "Net function replacement failed: target/source net was invalid.";
        return report;
    }

    report.message = report.success
        ? "Net function replacement completed while keeping the target net name."
        : "Net function replacement failed validation and was rolled back.";

    if (!report.success) {
        restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

// =============================================================================
//  B-3: Net merge / net bypass primitive（P1）
//  【說明】完整處理 PI / PO / constant / driver / loads 的 net merge。
// =============================================================================

// 把 fromNet 完全合併到 toNet：fromNet 的所有 load 改接到 toNet，fromNet 清空
//把fromNet的東西繼承給toNet，刪除fromNet
bool Netlist::mergeNetIntoNet(int fromNetId, int toNetId) {
    if (fromNetId < 0 || fromNetId >= (int)nets.size()) return false;
    if (toNetId < 0 || toNetId >= (int)nets.size()) return false;
    if (fromNetId == toNetId) return true;

    // 如果 fromNet 是 PO，把 PO 語意也轉移到 toNet
    if (nets[fromNetId].isPO) {
        nets[toNetId].isPO = true;
        for (int pi = 0; pi < (int)primaryOutputs.size(); pi++)
            for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++)
                if (primaryOutputs[pi].netIds[pj] == fromNetId)
                    primaryOutputs[pi].netIds[pj] = toNetId;
        nets[fromNetId].isPO = false;
    }

    replaceAllLoadsOfNet(fromNetId, toNetId);

    // 清除 fromNet 的 driver 連線
    int drv = nets[fromNetId].driverGateId;
    if (drv >= 0 && drv < (int)gates.size())
        if (gates[drv].outputNetId == fromNetId)
            gates[drv].outputNetId = -1;
    nets[fromNetId].driverGateId = -1;

    return true;
}

NetlistEditReport Netlist::mergeNetIntoNetWithReport(int fromNetId, int toNetId) {
    Netlist before = cloneForRollback();
    const std::string fromNetName =
        (fromNetId >= 0 && fromNetId < (int)nets.size()) ? nets[fromNetId].name : "";
    const std::string toNetName =
        (toNetId >= 0 && toNetId < (int)nets.size()) ? nets[toNetId].name : "";
    const int fromDriverGateId =
        (fromNetId >= 0 && fromNetId < (int)nets.size()) ? nets[fromNetId].driverGateId : -1;
    const std::string fromDriverGateName =
        (fromDriverGateId >= 0 && fromDriverGateId < (int)gates.size())
            ? gates[fromDriverGateId].instName
            : "";

    const bool ok = mergeNetIntoNet(fromNetId, toNetId);
    NetlistEditReport report = finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "mergeNetIntoNet",
        "Net merge completed.",
        "Net merge failed: source/target net id was invalid.",
        !fromDriverGateName.empty() ? std::vector<std::string>{fromDriverGateName} : std::vector<std::string>{},
        !fromNetName.empty() || !toNetName.empty()
            ? std::vector<std::string>{fromNetName, toNetName}
            : std::vector<std::string>{},
        fromDriverGateId >= 0 ? std::vector<int>{fromDriverGateId} : std::vector<int>{},
        fromNetId >= 0 || toNetId >= 0
            ? std::vector<int>{fromNetId, toNetId}
            : std::vector<int>{});
    if (ok && fromNetId == toNetId) {
        report.changed = false;
        report.addWarning("fromNetId and toNetId are identical; merge is a no-op.");
    }
    return report;
}

// bypass removedNet，讓所有接到它的 gate 改接到 replacementNet
// 同時保留 PO 語意（若 removedNet 是 PO）
bool Netlist::bypassNetKeepingPortSemantics(int removedNetId, int replacementNetId) {
    return mergeNetIntoNet(removedNetId, replacementNetId);
}

NetlistEditReport Netlist::bypassNetKeepingPortSemanticsWithReport(int removedNetId, int replacementNetId) {
    Netlist before = cloneForRollback();
    const std::string removedNetName =
        (removedNetId >= 0 && removedNetId < (int)nets.size()) ? nets[removedNetId].name : "";
    const std::string replacementNetName =
        (replacementNetId >= 0 && replacementNetId < (int)nets.size()) ? nets[replacementNetId].name : "";
    const bool ok = bypassNetKeepingPortSemantics(removedNetId, replacementNetId);
    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "bypassNetKeepingPortSemantics",
        "Net bypass completed while preserving port semantics.",
        "Net bypass failed: removed/replacement net id was invalid.",
        {},
        !removedNetName.empty() || !replacementNetName.empty()
            ? std::vector<std::string>{removedNetName, replacementNetName}
            : std::vector<std::string>{},
        {},
        removedNetId >= 0 || replacementNetId >= 0
            ? std::vector<int>{removedNetId, replacementNetId}
            : std::vector<int>{});
}

// 把 oldNet 的所有 load 改接到 newNet（allowDuplicateLoads 控制是否允許重複）
bool Netlist::redirectAllLoads(int oldNetId, int newNetId, bool allowDuplicateLoads) {
    if (oldNetId < 0 || oldNetId >= (int)nets.size()) return false;
    if (newNetId < 0 || newNetId >= (int)nets.size()) return false;
    if (oldNetId == newNetId) return false;

    Net& oldNet = nets[oldNetId];
    Net& newNet = nets[newNetId];

    for (int lgid : oldNet.loadGateIds) {
        if (lgid < 0 || lgid >= (int)gates.size()) continue;
        Gate& g = gates[lgid];
        for (int k = 0; k < (int)g.inputNetIds.size(); k++)
            if (g.inputNetIds[k] == oldNetId) g.inputNetIds[k] = newNetId;

        if (!allowDuplicateLoads) {
            bool already = false;
            for (int id : newNet.loadGateIds) if (id == lgid) { already = true; break; }
            if (!already) newNet.loadGateIds.push_back(lgid);
        } else {
            newNet.loadGateIds.push_back(lgid);
        }
    }
    oldNet.loadGateIds.clear();
    return true;
}

NetlistEditReport Netlist::redirectAllLoadsWithReport(int oldNetId, int newNetId, bool allowDuplicateLoads) {
    Netlist before = cloneForRollback();
    const std::string oldNetName =
        (oldNetId >= 0 && oldNetId < (int)nets.size()) ? nets[oldNetId].name : "";
    const std::string newNetName =
        (newNetId >= 0 && newNetId < (int)nets.size()) ? nets[newNetId].name : "";
    const std::vector<int> oldLoadGateIds =
        (oldNetId >= 0 && oldNetId < (int)nets.size()) ? nets[oldNetId].loadGateIds : std::vector<int>{};

    const bool ok = redirectAllLoads(oldNetId, newNetId, allowDuplicateLoads);

    std::vector<int> changedGateIds;
    std::vector<std::string> changedGateNames;
    for (int gateId : oldLoadGateIds) {
        if (gateId < 0 || gateId >= (int)gates.size()) continue;
        changedGateIds.push_back(gateId);
        changedGateNames.push_back(gates[gateId].instName);
    }

    return finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "redirectAllLoads",
        "Load redirection completed.",
        "Load redirection failed: old/new net id was invalid or identical.",
        changedGateNames,
        !oldNetName.empty() || !newNetName.empty()
            ? std::vector<std::string>{oldNetName, newNetName}
            : std::vector<std::string>{},
        changedGateIds,
        oldNetId >= 0 || newNetId >= 0
            ? std::vector<int>{oldNetId, newNetId}
            : std::vector<int>{});
}

// 如果 net 沒有任何 load 且不是 PO / PI / const，把它從 netlist 移除
bool Netlist::removeNetIfUnused(int netId) {
    if (netId < 0 || netId >= (int)nets.size()) return false;
    const Net& n = nets[netId];
    if (n.isRemoved) return false;
    if (n.isPO || n.isPI || n.isConst) return false;
    if (!n.loadGateIds.empty()) return false;

    // 通知 driver gate
    if (n.driverGateId >= 0 && n.driverGateId < (int)gates.size())
        if (gates[n.driverGateId].outputNetId == netId)
            gates[n.driverGateId].outputNetId = -1;

    // 用 sentinel 標記（不真的 erase，避免 ID shift）
    nets[netId].driverGateId = -1;
    nets[netId].loadGateIds.clear();
    nets[netId].isRemoved = true;
    // 不 erase，保留 slot，讓 compact 時處理
    return true;
}

NetlistEditReport Netlist::removeNetIfUnusedWithReport(int netId) {
    Netlist before = cloneForRollback();
    const std::string netName =
        (netId >= 0 && netId < (int)nets.size()) ? nets[netId].name : "";
    const int driverGateId =
        (netId >= 0 && netId < (int)nets.size()) ? nets[netId].driverGateId : -1;
    const std::string driverGateName =
        (driverGateId >= 0 && driverGateId < (int)gates.size()) ? gates[driverGateId].instName : "";

    const bool ok = removeNetIfUnused(netId);
    NetlistEditReport report = finalizeBooleanPrimitiveReport(
        *this,
        before,
        ok,
        "removeNetIfUnused",
        "Unused net removal completed.",
        "Unused net removal failed: net was invalid, PI/PO/const, already removed, or still had loads.",
        !driverGateName.empty() ? std::vector<std::string>{driverGateName} : std::vector<std::string>{},
        !netName.empty() ? std::vector<std::string>{netName} : std::vector<std::string>{},
        driverGateId >= 0 ? std::vector<int>{driverGateId} : std::vector<int>{},
        netId >= 0 ? std::vector<int>{netId} : std::vector<int>{});
    if (report.success) {
        certifyEquivalence(
            report,
            EquivalenceCheckMethod::StructuralIdentity,
            "Equivalence certified by removing one internal net with no loads and no PI/PO/constant role.");
    }
    return report;
}

// 掃描所有 net，移除沒有 load 且非 PO/PI/const 的 net(計算出移除掉的net數量)
int Netlist::removeUnusedNets() {
    int removed = 0;
    for (int i = 0; i < (int)nets.size(); i++)
        if (removeNetIfUnused(i)) removed++;
    return removed;
}

NetlistEditReport Netlist::removeUnusedNetsWithReport() {
    Netlist before = cloneForRollback();
    std::vector<int> candidateNetIds;
    std::vector<std::string> candidateNetNames;
    for (int i = 0; i < (int)nets.size(); ++i) {
        if (nets[i].isRemoved || nets[i].isPO || nets[i].isPI || nets[i].isConst) continue;
        if (!nets[i].loadGateIds.empty()) continue;
        candidateNetIds.push_back(i);
        candidateNetNames.push_back(nets[i].name);
    }

    const int removed = removeUnusedNets();
    NetlistEditReport report = finalizeEditReport(
        *this,
        before,
        removed,
        "removeUnusedNets",
        NetlistEditOperationKind::Cleanup,
        "Unused net cleanup completed.",
        "Unused net cleanup failed validation and was rolled back.",
        EquivalenceCheckMethod::StructuralIdentity,
        "Equivalence certified by removing internal nets with no loads and no PI/PO/constant role.");
    report.changedNetIds = candidateNetIds;
    report.changedNetNames = candidateNetNames;
    if (removed == 0) {
        report.addWarning("No unused internal nets were removed.");
    }
    return report;
}

// =============================================================================
//  B-6: Transaction / rollback primitive（P0）
//  【說明】正式的 snapshot / restore API，讓 rewrite pass 失敗時可以乾淨還原。
// =============================================================================

bool Netlist::restoreFrom(const Netlist& backup) {
    *this = backup;
    return true;
}

bool Netlist::validateAfterMutation() const {
    return validateStructure() && validateProblemAConstraints();
}

// =============================================================================
//  B-7: Cleanup fixpoint runner（P1）
//  【說明】反覆執行 constant propagation / same-input 化簡，直到沒有新的機會。
// =============================================================================

// 對所有 gate 執行一輪 constant propagation，回傳化簡的 gate 數
int Netlist::simplifyAllGatesWithConstants() {
    int count = 0;
    for (int i = 0; i < (int)gates.size(); i++)
        if (simplifyGateWithConstant(i)) count++;
    return count;
}

NetlistEditReport Netlist::simplifyAllGatesWithConstantsWithReport() {
    Netlist before = cloneForRollback();
    int simplified = simplifyAllGatesWithConstants();
    return finalizeEditReport(
        *this,
        before,
        simplified,
        "simplifyAllGatesWithConstants",
        NetlistEditOperationKind::Simplification,
        "Constant simplification completed.",
        "Constant simplification failed validation and was rolled back.",
        EquivalenceCheckMethod::LocalRewriteRule,
        "Equivalence certified by local constant-folding Boolean identities.");
}

// 對所有 gate 執行一輪 same-input 化簡，回傳化簡的 gate 數
int Netlist::simplifyAllSameInputGates() {
    int count = 0;
    for (int i = 0; i < (int)gates.size(); i++)
        if (simplifySameInputGate(i)) count++;
    return count;
}

NetlistEditReport Netlist::simplifyAllSameInputGatesWithReport() {
    Netlist before = cloneForRollback();
    int simplified = simplifyAllSameInputGates();
    return finalizeEditReport(
        *this,
        before,
        simplified,
        "simplifyAllSameInputGates",
        NetlistEditOperationKind::Simplification,
        "Same-input simplification completed.",
        "Same-input simplification failed validation and was rolled back.",
        EquivalenceCheckMethod::LocalRewriteRule,
        "Equivalence certified by same-input Boolean identities such as AND(x,x)=x and XOR(x,x)=0.");
}

// 反覆執行所有 local simplification，直到 fixpoint（沒有任何改變）
// 回傳總共化簡的 gate 數
int Netlist::runLocalSimplificationFixpoint() {
    int total = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        int r1 = simplifyAllGatesWithConstants();
        int r2 = simplifyAllSameInputGates();
        int r3 = cleanupAllRemovableBuffers();
        int r4 = collapseBackToBackInverters();
        int r5 = removeDanglingLogic();
        int round = r1 + r2 + r3 + r4 + r5;
        total += round;
        if (round > 0) changed = true;
    }
    return total;
}

NetlistEditReport Netlist::runLocalSimplificationFixpointWithReport() {
    Netlist before = cloneForRollback();

    int changedCount = runLocalSimplificationFixpoint();
    return finalizeEditReport(
        *this,
        before,
        changedCount,
        "runLocalSimplificationFixpoint",
        NetlistEditOperationKind::Simplification,
        "Local simplification fixpoint completed.",
        "Local simplification fixpoint failed validation and was rolled back.",
        EquivalenceCheckMethod::LocalRewriteRule,
        "Equivalence certified by composing local cleanup and simplification rewrite rules.");
}

// =============================================================================
//  B-8: compactRemovedGatesWithIdMap（P2）
//  【說明】compaction 後回傳 oldToNew / newToOld ID map，讓外部能追蹤 gate ID 變化。
// =============================================================================

Netlist::CompactResult Netlist::compactRemovedGatesWithIdMap() {
    CompactResult result;
    result.removedGateCount = 0;

    std::unordered_set<int> deadSet;
    for (int i = 0; i < (int)gates.size(); i++)
        if (gates[i].type == GateType::UNKNOWN) deadSet.insert(i);

    if (deadSet.empty()) return result;

    for (int i = 0; i < (int)nets.size(); i++) {
        std::vector<int> newLoads;
        for (int lgid : nets[i].loadGateIds)
            if (!deadSet.count(lgid)) newLoads.push_back(lgid);
        nets[i].loadGateIds = newLoads;
    }

    std::vector<Gate> newGates;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!deadSet.count(i)) {
            int newId = (int)newGates.size();
            result.oldToNewGateId[i] = newId;
            result.newToOldGateId[newId] = i;
            newGates.push_back(gates[i]);
            newGates.back().id = newId;
        }
    }

    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].driverGateId >= 0) {
            auto it = result.oldToNewGateId.find(nets[i].driverGateId);
            nets[i].driverGateId = (it != result.oldToNewGateId.end()) ? it->second : -1;
        }
        for (int& lgid : nets[i].loadGateIds) {
            auto it = result.oldToNewGateId.find(lgid);
            lgid = (it != result.oldToNewGateId.end()) ? it->second : -1;
        }
    }

    gateNameToId.clear();
    for (int i = 0; i < (int)newGates.size(); i++)
        gateNameToId[newGates[i].instName] = i;

    result.removedGateCount = (int)gates.size() - (int)newGates.size();
    gates = newGates;
    return result;
}
