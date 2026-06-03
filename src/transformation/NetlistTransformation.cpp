#include "include/core/Netlist.h"
#include "include/lib/cadical.hpp"
#include <string>
#include <algorithm>
#include <vector>

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
