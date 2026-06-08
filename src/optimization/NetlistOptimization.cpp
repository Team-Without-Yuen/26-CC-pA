#include "include/core/Netlist.h"
#include "include/lib/cadical.hpp"
#include <string>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>

// ─────────────────────────────────────────────────────────────────────────────
//  trimDeadLogic
//  移除所有不影響任何 PO 的 gate 和 net
//  策略：從所有 PO 往回 BFS，找出有用的 net 和 gate
//        沒被訪問到的 gate 就從 netlist 中移除
//  回傳移除的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::trimDeadLogic() {
    // Step 1: 從所有 PO 往回 BFS，找出所有有用的 net 和 gate
    std::unordered_set<int> usefulNets;
    std::unordered_set<int> usefulGates;
    std::queue<int> q;
 
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].isPO || nets[i].isPI || nets[i].isConst) {
            usefulNets.insert(i);
            if (nets[i].isPO) q.push(i);
        }
    }
 
    while (!q.empty()) {
        int netId = q.front(); q.pop();
        const Net& net = nets[netId];
        if (net.driverGateId >= 0) {
            const Gate& gate = gates[net.driverGateId];
            if (usefulGates.insert(gate.id).second) {
                for (int i = 0; i < (int)gate.inputNetIds.size(); i++) {
                    int inNetId = gate.inputNetIds[i];
                    if (usefulNets.insert(inNetId).second) {
                        q.push(inNetId);
                    }
                }
            }
        }
    }
 
    // Step 2: 找出要刪除的 gate
    std::unordered_set<int> deadGateSet;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!usefulGates.count(i)) deadGateSet.insert(i);
    }
    if (deadGateSet.empty()) return 0;
 
    // Step 3: 從 net 的 loadGateIds 移除 dead gate
    for (int i = 0; i < (int)nets.size(); i++) {
        std::vector<int> newLoads;
        for (int j = 0; j < (int)nets[i].loadGateIds.size(); j++) {
            if (!deadGateSet.count(nets[i].loadGateIds[j]))
                newLoads.push_back(nets[i].loadGateIds[j]);
        }
        nets[i].loadGateIds = newLoads;
    }
 
    // Step 4: 重建 gates vector，移除 dead gate，更新 ID
    std::vector<int> oldToNew(gates.size(), -1);
    std::vector<Gate> newGates;
    for (int i = 0; i < (int)gates.size(); i++) {
        if (!deadGateSet.count(i)) {
            int newId = newGates.size();
            oldToNew[i] = newId;
            newGates.push_back(gates[i]);
            newGates.back().id = newId;
        }
    }
 
    // Step 5: 更新 net 的 driverGateId 和 loadGateIds
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].driverGateId >= 0)
            nets[i].driverGateId = oldToNew[nets[i].driverGateId];
        for (int j = 0; j < (int)nets[i].loadGateIds.size(); j++)
            nets[i].loadGateIds[j] = oldToNew[nets[i].loadGateIds[j]];
    }
 
    // Step 6: 更新 gateNameToId
    gateNameToId.clear();
    for (int i = 0; i < (int)newGates.size(); i++)
        gateNameToId[newGates[i].instName] = i;
 
    int removed = (int)gates.size() - (int)newGates.size();
    gates = newGates;
    return removed;
}
 
// ─────────────────────────────────────────────────────────────────────────────
//  collapseBackToBackInverters
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
 
            Net& midNet = nets[g1.outputNetId];
            if (midNet.loadGateIds.size() != 1) continue;
            if (midNet.isPO) continue;
 
            int g2id = midNet.loadGateIds[0];
            Gate& g2 = gates[g2id];
            if (g2.type != GateType::NOT) continue;
            if (g2.outputNetId < 0) continue;
 
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
 
            // 如果 outNet 是 PO，把 inNet 也標記為 PO
            if (outNet.isPO) {
                inNet.isPO = true;
                for (int pi = 0; pi < (int)primaryOutputs.size(); pi++) {
                    for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++) {
                        if (primaryOutputs[pi].netIds[pj] == outNetId)
                            primaryOutputs[pi].netIds[pj] = inNetId;
                    }
                }
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
//  mergeEquivalentGates
//  合併結構等價的 gate（相同 type + 相同 input net 集合）
//  回傳合併的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::mergeEquivalentGates() {
    int merged = 0;

    // key: (type, signature input net ids) → 代表 gate 的 ID
    std::map<std::pair<int, std::vector<int>>, int> seen;

    // 【效能優化】改為 One-pass 單次掃描到底
    for (int i = 0; i < (int)gates.size(); i++) {
        Gate& g = gates[i];
        if (g.type == GateType::UNKNOWN) continue;
        if (g.outputNetId < 0) continue;

        // 【防呆】建立 key：只對可交換 (Commutative) 的邏輯閘進行排序
        // 這能保護 DFF 原本已經建好的精確腳位順序不被打亂
        std::vector<int> sigInputs = g.inputNetIds;
        if (g.type == GateType::AND || g.type == GateType::OR || 
            g.type == GateType::NAND || g.type == GateType::NOR || 
            g.type == GateType::XOR || g.type == GateType::XNOR) {
            std::sort(sigInputs.begin(), sigInputs.end());
        }
        std::pair<int, std::vector<int>> key = std::make_pair((int)g.type, sigInputs);

        std::map<std::pair<int, std::vector<int>>, int>::iterator it = seen.find(key);
        if (it == seen.end()) {
            // 第一次看到的結構，記錄下來
            seen[key] = i;
        } else {
            // 找到等價 gate，把 i 的 outNet 改接到 it->second 的 outNet
            int keepGateId = it->second;
            int deadGateId = i;

            int keepOutNet = gates[keepGateId].outputNetId;
            int deadOutNet = gates[deadGateId].outputNetId;

            if (keepOutNet < 0 || deadOutNet < 0) continue;
            if (keepOutNet == deadOutNet) continue;

            // 把所有接到 deadOutNet 的 gate 改接到 keepOutNet
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

            // 如果 deadOutNet 是 PO，把 keepOutNet 也標記為 PO
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

            // 清除 deadGate 在 Input Nets 上的負載 (避免 Dangling Pointer)
            // 務必在清除 gates[deadGateId].inputNetIds 之前執行這段
            for (int inNetId : gates[deadGateId].inputNetIds) {
                auto& loads = nets[inNetId].loadGateIds;
                loads.erase(std::remove(loads.begin(), loads.end(), deadGateId), loads.end());
            }

            // 把 dead gate 標記為 UNKNOWN
            gates[deadGateId].type = GateType::UNKNOWN;
            gates[deadGateId].outputNetId = -1;
            gates[deadGateId].inputNetIds.clear();

            merged++;
        }
    }

    if (merged > 0) trimDeadLogic();
    return merged;
}