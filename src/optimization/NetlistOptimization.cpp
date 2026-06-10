#include "include/core/Netlist.h"
#include "include/lib/cadical.hpp"
#include <string>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
//  trimDeadLogic  【BUG FIX: 補上 DFF 作為 timing endpoint】
//  移除所有不影響任何 PO 或 DFF input 的 gate 和 net
//  回傳移除的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::trimDeadLogic() {
    std::unordered_set<int> usefulNets;
    std::unordered_set<int> usefulGates;
    std::queue<int> q;

    // Step 1a: 從所有 PO 往回 BFS
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].isPO || nets[i].isPI || nets[i].isConst) {
            usefulNets.insert(i);
            if (nets[i].isPO) q.push(i);
        }
    }

    // Step 1b: 【新增】把所有 DFF 的所有 input pin net 也加入 BFS 起點
    //          保守策略：DFF 所有 input pin（D / CK / RN / SN）的 cone 都保留
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type != GateType::DFF) continue;
        usefulGates.insert(i); // DFF gate 本身是 essential
        for (int inNetId : gates[i].inputNetIds) {
            if (inNetId < 0) continue;
            if (usefulNets.insert(inNetId).second)
                q.push(inNetId);
        }
    }

    // Step 2: BFS 往回追 fanin
    while (!q.empty()) {
        int netId = q.front(); q.pop();
        const Net& net = nets[netId];
        if (net.driverGateId >= 0) {
            const Gate& gate = gates[net.driverGateId];
            if (usefulGates.insert(gate.id).second) {
                for (int i = 0; i < (int)gate.inputNetIds.size(); i++) {
                    int inNetId = gate.inputNetIds[i];
                    if (inNetId < 0) continue;
                    if (usefulNets.insert(inNetId).second)
                        q.push(inNetId);
                }
            }
        }
    }

    // Step 3: 找出要刪除的 gate
    std::unordered_set<int> deadGateSet;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type == GateType::UNKNOWN) continue;
        if (!usefulGates.count(i)) deadGateSet.insert(i);
    }
    if (deadGateSet.empty()) return 0;

    // Step 4: 從 net 的 loadGateIds 移除 dead gate
    for (int i = 0; i < (int)nets.size(); i++) {
        std::vector<int> newLoads;
        for (int j = 0; j < (int)nets[i].loadGateIds.size(); j++) {
            if (!deadGateSet.count(nets[i].loadGateIds[j]))
                newLoads.push_back(nets[i].loadGateIds[j]);
        }
        nets[i].loadGateIds = newLoads;
    }

    // Step 5: 重建 gates vector，移除 dead gate，更新 ID
    std::vector<int> oldToNew(gates.size(), -1);
    std::vector<Gate> newGates;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!deadGateSet.count(i)) {
            int newId = (int)newGates.size();
            oldToNew[i] = newId;
            newGates.push_back(gates[i]);
            newGates.back().id = newId;
        }
    }

    // Step 6: 更新 net 的 driverGateId 和 loadGateIds
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].driverGateId >= 0)
            nets[i].driverGateId = oldToNew[nets[i].driverGateId];
        for (int j = 0; j < (int)nets[i].loadGateIds.size(); j++)
            nets[i].loadGateIds[j] = oldToNew[nets[i].loadGateIds[j]];
    }

    // Step 7: 更新 gateNameToId
    gateNameToId.clear();
    for (int i = 0; i < (int)newGates.size(); i++)
        gateNameToId[newGates[i].instName] = i;

    int removed = (int)gates.size() - (int)newGates.size();
    gates = newGates;
    return removed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  collapseBackToBackInverters  【BUG FIX: 補上 PO guard】
//  找出所有 NOT → NOT 的連續對，把兩個 NOT 刪掉，直接把前面的 net 接到後面
//  例如：A → NOT1 → B → NOT2 → C  →  A → C
//  回傳移除的 inverter pair 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::collapseBackToBackInverters() {
    int collapsed = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        for (int i = 0; i < (int)gates.size(); i++) {
            Gate& g1 = gates[i];
            if (g1.type != GateType::NOT) continue;
            if (g1.outputNetId < 0) continue;
            if (g1.inputNetIds.empty() || g1.inputNetIds[0] < 0) continue;

            Net& midNet = nets[g1.outputNetId];
            if (midNet.loadGateIds.size() != 1) continue;
            if (midNet.isPO) continue;

            int g2id = midNet.loadGateIds[0];
            if (g2id < 0) continue;
            Gate& g2 = gates[g2id];
            if (g2.type != GateType::NOT) continue;
            if (g2.outputNetId < 0) continue;

            // 【新增】PO guard：第二個 NOT 的 output 是 PO 時跳過
            if (nets[g2.outputNetId].isPO) continue;

            int inNetId  = g1.inputNetIds[0];
            int outNetId = g2.outputNetId;
            Net& inNet  = nets[inNetId];
            Net& outNet = nets[outNetId];

            // 把所有接到 outNet 的 gate 改接到 inNet
            for (int j = 0; j < (int)outNet.loadGateIds.size(); j++) {
                int loadGateId = outNet.loadGateIds[j];
                Gate& loadGate = gates[loadGateId];
                for (int k = 0; k < (int)loadGate.inputNetIds.size(); k++) {
                    if (loadGate.inputNetIds[k] == outNetId)
                        loadGate.inputNetIds[k] = inNetId;
                }
                inNet.loadGateIds.push_back(loadGateId);
            }

            // 從 inNet 的 loadGateIds 移除 g1
            std::vector<int> newLoads;
            for (int j = 0; j < (int)inNet.loadGateIds.size(); j++) {
                if (inNet.loadGateIds[j] != g1.id)
                    newLoads.push_back(inNet.loadGateIds[j]);
            }
            inNet.loadGateIds = newLoads;

            // 把 g1 和 g2 標記為 dead
            g1.type = GateType::UNKNOWN;
            g1.outputNetId = -1;
            g1.inputNetIds.clear();
            g2.type = GateType::UNKNOWN;
            g2.outputNetId = -1;
            g2.inputNetIds.clear();
            midNet.driverGateId = -1;
            midNet.loadGateIds.clear();
            outNet.driverGateId = -1;
            outNet.loadGateIds.clear();

            collapsed++;
            changed = true;
            break;
        }
    }

    if (collapsed > 0) trimDeadLogic();
    return collapsed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  insertBuffersForFanout
//  對 fanout > maxFanout 的 net 插入 buffer
//  採用 Cascaded Buffer（串聯緩衝樹）結構
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::insertBuffersForFanout(int maxFanout) {
    // 防呆：Fanout 必須至少為 2，否則無法插入 Buffer（因為 Buffer 本身就會佔用 1 個 Fanout）
    if (maxFanout < 2) return 0;

    int inserted = 0;
    int bufCounter = 0;

    // 注意：這裡使用動態的 nets.size()，讓新生成的 bufNet 也能被迴圈檢查到！
    for (int netIdx = 0; netIdx < (int)nets.size(); netIdx++) {
        if (nets[netIdx].isConst) continue;

        while ((int)nets[netIdx].loadGateIds.size() > maxFanout) {

            // 只保留 maxFanout - 1 個原有的 loads
            // 空出 1 個名額，用來連接即將新增的 Buffer
            int keepCount = maxFanout - 1;

            std::vector<int> overLoads(
                nets[netIdx].loadGateIds.begin() + keepCount,
                nets[netIdx].loadGateIds.end()
            );
            nets[netIdx].loadGateIds.resize(keepCount);

            std::string bufName    = "_ins_buf_"     + std::to_string(bufCounter);
            std::string bufNetName = "_ins_buf_net_" + std::to_string(bufCounter);
            bufCounter++;

            int bufGateId = addGate(bufName, GateType::BUF);
            int bufNetId  = addNet(bufNetName);

            // 雙向連接 Buffer Input
            gates[bufGateId].inputNetIds.push_back(netIdx);
            nets[netIdx].loadGateIds.push_back(bufGateId);
            // 此時 nets[netIdx].loadGateIds.size() 剛好等於 keepCount + 1 = maxFanout
            // 完美符合規格，while 迴圈也會順利終止！

            // 雙向連接 Buffer Output
            gates[bufGateId].outputNetId = bufNetId;
            nets[bufNetId].driverGateId  = bufGateId;

            // 把超載的 load 改接到新的 bufNetId 上
            for (int j = 0; j < (int)overLoads.size(); j++) {
                int loadGateId = overLoads[j];
                for (int k = 0; k < (int)gates[loadGateId].inputNetIds.size(); k++) {
                    if (gates[loadGateId].inputNetIds[k] == netIdx)
                        gates[loadGateId].inputNetIds[k] = bufNetId;
                }
                nets[bufNetId].loadGateIds.push_back(loadGateId);
            }

            inserted++;
        }
    }

    return inserted;
}

// ─────────────────────────────────────────────────────────────────────────────
//  mergeEquivalentGates  【保留組員版：One-pass + 清除 dead gate 在 input nets 上的負載】
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

    for (int i = 0; i < (int)nets.size(); i++)
        if (nets[i].isPO && visitedNets.insert(i).second) q.push(i);

    for (int i = 0; i < (int)gates.size(); i++) {
        if (gates[i].type != GateType::DFF) continue;
        usefulGates.insert(i);
        for (int inNetId : gates[i].inputNetIds) {
            if (inNetId < 0) continue;
            if (visitedNets.insert(inNetId).second) q.push(inNetId);
        }
    }

    while (!q.empty()) {
        int netId = q.front(); q.pop();
        const Net& net = nets[netId];
        if (net.driverGateId < 0) continue;
        int drvGate = net.driverGateId;
        if (drvGate < 0 || drvGate >= (int)gates.size()) continue;
        if (usefulGates.insert(drvGate).second) {
            for (int inNetId : gates[drvGate].inputNetIds) {
                if (inNetId < 0) continue;
                if (visitedNets.insert(inNetId).second) q.push(inNetId);
            }
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

int Netlist::removeDanglingLogic() {
    std::vector<int> dangling = findDanglingGateIds();
    if (dangling.empty()) return 0;
    for (int gid : dangling) markGateRemoved(gid);
    return compactRemovedGates();
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