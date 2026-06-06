#include "include/core/Netlist.h"
#include "include/lib/cadical.hpp"
#include <string>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>

// 重新命名 Gate
bool Netlist::renameGate(const std::string& oldName, const std::string& newName) {
    // 如果名字根本沒變，直接當作成功
    if (oldName == newName) return true;

    // 檢查舊名字是否存在
    auto it = gateNameToId.find(oldName);
    if (it == gateNameToId.end()) {
        return false; // 找不到要改名的 Gate
    }

    // 檢查新名字是否有命名衝突
    if (gateNameToId.count(newName) > 0) {
        return false; // 新名字已存在，拒絕修改以維持網表合法性
    }

    // 正式修改資料
    int gateId = it->second;
    gates[gateId].instName = newName;

    // 更新 Hash Map
    gateNameToId.erase(it);        // 刪除舊的 Key
    gateNameToId[newName] = gateId; // 建立新的 Key 指向同一個 ID

    return true;
}

// 重新命名 Net
bool Netlist::renameNet(const std::string& oldName, const std::string& newName) {
    // 防呆：名字沒變
    if (oldName == newName) return true;

    // 檢查舊名字是否存在
    auto it = netNameToId.find(oldName);
    if (it == netNameToId.end()) {
        return false; // 找不到該 Net
    }

    // 檢查新名字是否衝突
    if (netNameToId.count(newName) > 0) {
        return false; // 命名衝突，拒絕修改
    }

    // 正式修改
    int netId = it->second;
    nets[netId].name = newName;

    // 更新 Hash Map
    netNameToId.erase(it);
    netNameToId[newName] = netId;

    return true;
}

// 斷開連線
bool Netlist::disconnectGateInput(const std::string& gateName, const std::string& netName) {
    int gateId = getGateId(gateName);
    int netId = getNetId(netName);
    if (gateId < 0 || netId < 0) return false;

    Gate& gate = gates[gateId];
    Net& net = nets[netId];

    // 把陣列裡的 netId 改成 -1 (懸空)，但不能 erase。
    // 尤其 DFF 的 inputNetIds 與 inputPinNames 是同位置對應：
    // inputNetIds[i] 對應 inputPinNames[i]，例如 D/CK/RN/SN。
    // 若 erase 會讓後面的 pin 往前遞補，造成 named pin 查詢錯位。
    auto gateInputIt = std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), netId);
    if (gateInputIt != gate.inputNetIds.end()) {
        *gateInputIt = -1; // 保留位子，但拔掉訊號
    } else {
        return false; 
    }

    // Net 那邊的 loadGateIds 直接 erase
    auto netLoadIt = std::find(net.loadGateIds.begin(), net.loadGateIds.end(), gateId);
    if (netLoadIt != net.loadGateIds.end()) {
        net.loadGateIds.erase(netLoadIt);
    }

    return true;
}

// 建立連線
bool Netlist::connectGateInput(const std::string& gateName, const std::string& netName, int pinIndex) {
    int gateId = getGateId(gateName);
    int netId = getNetId(netName);
    
    if (gateId < 0 || netId < 0) return false;

    Gate& gate = gates[gateId];
    Net& net = nets[netId];

    // 自動分配模式 (pinIndex == -1) 
    if (pinIndex == -1) {
        // 尋找有沒有之前斷開留下的空位 (數值為 -1)
        auto emptyIt = std::find(gate.inputNetIds.begin(), gate.inputNetIds.end(), -1);
        
        if (emptyIt != gate.inputNetIds.end()) {
            // 找到空位，直接填入
            pinIndex = std::distance(gate.inputNetIds.begin(), emptyIt);
        } else {
            // 沒有空位，代表要新增在最後面
            pinIndex = gate.inputNetIds.size(); 
        }
    }

    // 空間擴充與硬體防呆
    if (pinIndex >= gate.inputNetIds.size()) {
        // 擋下 DFF 的非法擴充
        if (gate.type == GateType::DFF) return false; 
        gate.inputNetIds.resize(pinIndex + 1, -1);
    }

    // 佔用防呆
    // 如果這個洞不是空的，而且插的也不是我們現在要接的這條線
    if (gate.inputNetIds[pinIndex] != -1 && gate.inputNetIds[pinIndex] != netId) {
        return false; // 強制要求使用者必須先呼叫 disconnectGateInput 才能換線
    }

    // 避免 Net 負載清單產生重複 ID 
    // 只有在這個 Gate 還不在 Net 的負載清單時，才 push_back
    if (std::find(net.loadGateIds.begin(), net.loadGateIds.end(), gateId) == net.loadGateIds.end()) {
        net.loadGateIds.push_back(gateId);
    }

    // 正式連線
    gate.inputNetIds[pinIndex] = netId;

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  reconstructToAndNot
//  將整個 netlist 重新建構成只使用 AND 和 NOT gates
//  策略：先備份所有需要替換的 gate info，再做替換
//  De Morgan 定理：
//  - OR(a,b)   → NOT(AND(NOT(a), NOT(b)))
//  - NAND(a,b) → NOT(AND(a,b))
//  - NOR(a,b)  → AND(NOT(a), NOT(b))
//  - XOR(a,b)  → NOT(AND(NOT(AND(a,NOT(b))), NOT(AND(NOT(a),b))))
//  - XNOR(a,b) → NOT(XOR(a,b))
//  - BUF       → 直接連線（移除 gate）
//  - DFF       → 保留不動
//  回傳新增的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::reconstructToAndNot() {
    int newGateCount = 0;
    int synCounter = 0;
 
    // Step 1: 備份所有需要替換的 gate info
    struct GateInfo {
        int id;
        GateType type;
        std::vector<int> inputNetIds;
        int outputNetId;
    };
 
    std::vector<GateInfo> toReplace;
    for (int gi = 0; gi < (int)gates.size(); gi++) {
        GateType t = gates[gi].type;
        if (t == GateType::OR  || t == GateType::NAND || t == GateType::NOR  ||
            t == GateType::XOR || t == GateType::XNOR || t == GateType::BUF) {
            GateInfo info;
            info.id          = gi;
            info.type        = t;
            info.inputNetIds = gates[gi].inputNetIds;
            info.outputNetId = gates[gi].outputNetId;
            toReplace.push_back(info);
        }
    }
 
    // Step 2: 替換每個 gate
    for (int ri = 0; ri < (int)toReplace.size(); ri++) {
        GateInfo& info = toReplace[ri];
        int gi       = info.id;
        int outNetId = info.outputNetId;
        int a        = (info.inputNetIds.size() > 0) ? info.inputNetIds[0] : -1;
        int b        = (info.inputNetIds.size() > 1) ? info.inputNetIds[1] : -1;
 
        // 從 input net 的 loadGateIds 移除舊 gate
        for (int inNetId : info.inputNetIds) {
            std::vector<int> newLoads;
            for (int x : nets[inNetId].loadGateIds)
                if (x != gi) newLoads.push_back(x);
            nets[inNetId].loadGateIds = newLoads;
        }
        nets[outNetId].driverGateId = -1;
 
        // 清空舊 gate
        gates[gi].type = GateType::UNKNOWN;
        gates[gi].inputNetIds.clear();
        gates[gi].outputNetId = -1;
 
        std::string sid = std::to_string(synCounter++);
 
        if (info.type == GateType::BUF) {
            // BUF: 把所有接到 outNet 的 gate 改接到 a
            for (int loadGateId : nets[outNetId].loadGateIds) {
                for (int k = 0; k < (int)gates[loadGateId].inputNetIds.size(); k++) {
                    if (gates[loadGateId].inputNetIds[k] == outNetId) {
                        gates[loadGateId].inputNetIds[k] = a;
                        nets[a].loadGateIds.push_back(loadGateId);
                    }
                }
            }
            if (nets[outNetId].isPO) {
                nets[a].isPO = true;
                for (int pi = 0; pi < (int)primaryOutputs.size(); pi++)
                    for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++)
                        if (primaryOutputs[pi].netIds[pj] == outNetId)
                            primaryOutputs[pi].netIds[pj] = a;
            }
            nets[outNetId].loadGateIds.clear();
 
        } else if (info.type == GateType::OR) {
            // OR(a,b) = NOT(AND(NOT(a), NOT(b)))
            int notA_net = addNet("_na_" + sid);
            int notB_net = addNet("_nb_" + sid);
            int and_net  = addNet("_and_" + sid);
            int notA_g = addGate("_notA_" + sid, GateType::NOT);
            int notB_g = addGate("_notB_" + sid, GateType::NOT);
            int and_g  = addGate("_and_"  + sid, GateType::AND);
            int notO_g = addGate("_notO_" + sid, GateType::NOT);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(and_g, notA_net);  connectGateInput(and_g, notB_net);
            connectGateOutput(and_g, and_net);
            connectGateInput(notO_g, and_net);  connectGateOutput(notO_g, outNetId);
            nets[outNetId].driverGateId = notO_g;
            newGateCount += 4;
 
        } else if (info.type == GateType::NAND) {
            // NAND(a,b) = NOT(AND(a,b))
            int and_net = addNet("_and_" + sid);
            int and_g   = addGate("_and_" + sid, GateType::AND);
            int not_g   = addGate("_not_" + sid, GateType::NOT);
            connectGateInput(and_g, a);     connectGateInput(and_g, b);
            connectGateOutput(and_g, and_net);
            connectGateInput(not_g, and_net); connectGateOutput(not_g, outNetId);
            nets[outNetId].driverGateId = not_g;
            newGateCount += 2;
 
        } else if (info.type == GateType::NOR) {
            // NOR(a,b) = AND(NOT(a), NOT(b))
            int notA_net = addNet("_na_" + sid);
            int notB_net = addNet("_nb_" + sid);
            int notA_g = addGate("_notA_" + sid, GateType::NOT);
            int notB_g = addGate("_notB_" + sid, GateType::NOT);
            int and_g  = addGate("_and_"  + sid, GateType::AND);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(and_g, notA_net);  connectGateInput(and_g, notB_net);
            connectGateOutput(and_g, outNetId);
            nets[outNetId].driverGateId = and_g;
            newGateCount += 3;
 
        } else if (info.type == GateType::XOR) {
            // XOR(a,b) = NOT(AND(NOT(AND(a,NOT(b))), NOT(AND(NOT(a),b))))
            std::string s2  = std::to_string(synCounter++);
            int notB_net  = addNet("_nb_"  + sid);
            int notA_net  = addNet("_na_"  + sid);
            int and1_net  = addNet("_a1_"  + sid);
            int and2_net  = addNet("_a2_"  + sid);
            int notX_net  = addNet("_nx_"  + s2);
            int notY_net  = addNet("_ny_"  + s2);
            int and3_net  = addNet("_a3_"  + s2);
            int notB_g  = addGate("_notB_"  + sid, GateType::NOT);
            int notA_g  = addGate("_notA_"  + sid, GateType::NOT);
            int and1_g  = addGate("_and1_"  + sid, GateType::AND);
            int and2_g  = addGate("_and2_"  + sid, GateType::AND);
            int notX_g  = addGate("_notX_"  + s2,  GateType::NOT);
            int notY_g  = addGate("_notY_"  + s2,  GateType::NOT);
            int and3_g  = addGate("_and3_"  + s2,  GateType::AND);
            int notF_g  = addGate("_notF_"  + s2,  GateType::NOT);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(and1_g, a);        connectGateInput(and1_g, notB_net);
            connectGateOutput(and1_g, and1_net);
            connectGateInput(and2_g, notA_net); connectGateInput(and2_g, b);
            connectGateOutput(and2_g, and2_net);
            connectGateInput(notX_g, and1_net); connectGateOutput(notX_g, notX_net);
            connectGateInput(notY_g, and2_net); connectGateOutput(notY_g, notY_net);
            connectGateInput(and3_g, notX_net); connectGateInput(and3_g, notY_net);
            connectGateOutput(and3_g, and3_net);
            connectGateInput(notF_g, and3_net); connectGateOutput(notF_g, outNetId);
            nets[outNetId].driverGateId = notF_g;
            newGateCount += 8;
 
        } else if (info.type == GateType::XNOR) {
            // XNOR(a,b) = NOT(XOR(a,b)) → 先展開 XOR 再加 NOT
            std::string s2  = std::to_string(synCounter++);
            std::string s3  = std::to_string(synCounter++);
            int notB_net  = addNet("_nb_"  + sid);
            int notA_net  = addNet("_na_"  + sid);
            int and1_net  = addNet("_a1_"  + sid);
            int and2_net  = addNet("_a2_"  + sid);
            int notX_net  = addNet("_nx_"  + s2);
            int notY_net  = addNet("_ny_"  + s2);
            int and3_net  = addNet("_a3_"  + s2);
            int xor_net   = addNet("_xr_"  + s3);
            int notB_g  = addGate("_notB_"  + sid, GateType::NOT);
            int notA_g  = addGate("_notA_"  + sid, GateType::NOT);
            int and1_g  = addGate("_and1_"  + sid, GateType::AND);
            int and2_g  = addGate("_and2_"  + sid, GateType::AND);
            int notX_g  = addGate("_notX_"  + s2,  GateType::NOT);
            int notY_g  = addGate("_notY_"  + s2,  GateType::NOT);
            int and3_g  = addGate("_and3_"  + s2,  GateType::AND);
            int notF_g  = addGate("_notF_"  + s2,  GateType::NOT);
            int notXor_g = addGate("_nxor_" + s3,  GateType::NOT);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(and1_g, a);        connectGateInput(and1_g, notB_net);
            connectGateOutput(and1_g, and1_net);
            connectGateInput(and2_g, notA_net); connectGateInput(and2_g, b);
            connectGateOutput(and2_g, and2_net);
            connectGateInput(notX_g, and1_net); connectGateOutput(notX_g, notX_net);
            connectGateInput(notY_g, and2_net); connectGateOutput(notY_g, notY_net);
            connectGateInput(and3_g, notX_net); connectGateInput(and3_g, notY_net);
            connectGateOutput(and3_g, and3_net);
            connectGateInput(notF_g, and3_net); connectGateOutput(notF_g, xor_net);
            connectGateInput(notXor_g, xor_net); connectGateOutput(notXor_g, outNetId);
            nets[outNetId].driverGateId = notXor_g;
            newGateCount += 9;
        }
    }
 
    trimDeadLogic();
    return newGateCount;
}