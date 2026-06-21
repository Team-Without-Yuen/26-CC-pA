#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/lib/cadical.hpp"
#include <string>
#include <functional>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>

namespace {

struct FanoutSinkPin {
    int gateId = -1;
    int pinIndex = -1;
};

// 收集指定 net 實際驅動的每一個 gate input pin。
// 這裡的單位是 pin，不是 gate；同一顆 DFF 的 RN/SN 都接同一條 net 時會回傳兩筆。
std::vector<FanoutSinkPin> collectFanoutSinkPins(const std::vector<Gate>& gates, int netId) {
    std::vector<FanoutSinkPin> sinks;
    for (const Gate& gate : gates) {
        for (int pinIndex = 0; pinIndex < (int)gate.inputNetIds.size(); ++pinIndex) {
            if (gate.inputNetIds[pinIndex] == netId) {
                sinks.push_back({gate.id, pinIndex});
            }
        }
    }
    return sinks;
}

// 依照 Problem A QA fanout load 定義計算 net fanout。
// total = gate/DFF input sink pins + primary output connection。
int getQaFanoutLoadCount(const std::vector<Gate>& gates,
                         const std::vector<Net>& nets,
                         int netId) {
    if (netId < 0 || netId >= (int)nets.size()) {
        return 0;
    }
    int total = (int)collectFanoutSinkPins(gates, netId).size();
    if (nets[netId].isPO) {
        ++total;
    }
    return total;
}

// 依照 kept/moved pin lists 重建 source net 與 buffer output net 的 loadGateIds，
// 並只改接 moved 裡指定的 pin。這可避免同一顆 DFF 多個 pin 被整顆一起搬走。
void moveFanoutSinkPinsToBuffer(std::vector<Gate>& gates,
                                std::vector<Net>& nets,
                                int sourceNetId,
                                int bufGateId,
                                int bufNetId,
                                const std::vector<FanoutSinkPin>& keptSinks,
                                const std::vector<FanoutSinkPin>& movedSinks) {
    nets[sourceNetId].loadGateIds.clear();
    for (const FanoutSinkPin& sink : keptSinks) {
        nets[sourceNetId].loadGateIds.push_back(sink.gateId);
    }
    nets[sourceNetId].loadGateIds.push_back(bufGateId);

    nets[bufNetId].loadGateIds.clear();
    for (const FanoutSinkPin& sink : movedSinks) {
        gates[sink.gateId].inputNetIds[sink.pinIndex] = bufNetId;
        nets[bufNetId].loadGateIds.push_back(sink.gateId);
    }
}

// source net 要接上一顆 buffer input；如果 source net 同時是 PO，要替 PO 預留一個 load 名額。
int computeKeepSinkCount(int maxFanout, const Net& sourceNet) {
    const int primaryOutputLoad = sourceNet.isPO ? 1 : 0;
    return std::max(0, maxFanout - 1 - primaryOutputLoad);
}

} // namespace

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

bool Netlist::disconnectAllPins(int gateId) {
    if (gateId < 0 || gateId >= gates.size()) return false;
    Gate& gate = gates[gateId];

    // 處理 Input Nets：從所有連接的 Net 的 loadGateIds 中移除自己
    for (size_t i = 0; i < gate.inputNetIds.size(); ++i) {
        int netId = gate.inputNetIds[i];
        
        if (netId != -1 && netId < (int)nets.size()) {
            Net& net = nets[netId];
            
            // 【Net 端】Fan-out 列表沒有順序問題，直接 erase 是安全的
            auto it = std::remove(net.loadGateIds.begin(), net.loadGateIds.end(), gateId);
            net.loadGateIds.erase(it, net.loadGateIds.end());
            
            // 【Gate 端】填入 -1 以維持 DFF/Macro 的腳位對應
            gate.inputNetIds[i] = -1;
        }
    }

    // 處理 Output Net：把輸出線的 driverGateId 設為懸空 (-1)
    if (gate.outputNetId != -1 && gate.outputNetId < (int)nets.size()) {
        nets[gate.outputNetId].driverGateId = -1;
        gate.outputNetId = -1;
    }
    
    return true;
}

bool Netlist::removeGate(int gateId) {
    if (gateId < 0 || gateId >= (int)gates.size()) return false;
    
    // 斷開所有腳位連線 (保留 DFF pin index)
    disconnectAllPins(gateId);
    
    // 將 GateType 標記為 UNKNOWN (Tombstone 機制)
    gates[gateId].type = GateType::UNKNOWN;
    
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  insertBuffersForFanout
//  對 fanout > maxFanout 的 net 插入 buffer，採用 Cascaded Buffer 結構
//  回傳值: BufferInsertionReport (包含所有新增 Buffer 的詳細細節與驅動/負載資訊)
// ─────────────────────────────────────────────────────────────────────────────
BufferInsertionReport Netlist::insertBuffersForFanout(int maxFanout) {
    BufferInsertionReport report;

    // 防呆：Fanout 必須至少為 2，否則無法插入 Buffer
    if (maxFanout < 2) return report; 

    int bufCounter = 0;

    // 使用動態邊界，讓新生成的 bufNet 也能被迴圈檢查到
    for (int netIdx = 0; netIdx < (int)nets.size(); netIdx++) {
        if (nets[netIdx].isConst) continue;

        while (getQaFanoutLoadCount(gates, nets, netIdx) > maxFanout) {
            
            // 只保留能安全留在 source net 上的 sink pins。
            // 若 source net 是 PO，要額外替 PO connection 保留一個 load 名額。
            int keepCount = computeKeepSinkCount(maxFanout, nets[netIdx]);
            std::vector<FanoutSinkPin> sinkPins = collectFanoutSinkPins(gates, netIdx);

            // 把超載的負載擷取出來
            std::vector<FanoutSinkPin> keptSinks(
                sinkPins.begin(),
                sinkPins.begin() + std::min(keepCount, (int)sinkPins.size())
            );
            std::vector<FanoutSinkPin> overLoads(
                sinkPins.begin() + std::min(keepCount, (int)sinkPins.size()),
                sinkPins.end()
            );

            // 產生新元件與線路名稱
            std::string bufName    = "_ins_buf_"    + std::to_string(bufCounter);
            std::string bufNetName = "_ins_buf_net_" + std::to_string(bufCounter);
            bufCounter++;

            int bufGateId = addGate(bufName, GateType::BUF);
            int bufNetId  = addNet(bufNetName);

            // 在接線被改動前，先紀錄下這顆 Buffer 的驅動源頭
            BufInsertRecord record;
            record.bufGateId     = bufGateId;
            record.bufGateName   = bufName;
            record.inputNetId    = netIdx;
            record.inputNetName  = nets[netIdx].name;
            record.driverGateId  = nets[netIdx].driverGateId;
            record.driverGateName = (nets[netIdx].driverGateId != -1) ? gates[nets[netIdx].driverGateId].instName : "PI";
            record.outputNetId   = bufNetId;
            record.outputNetName = bufNetName;

            // 雙向連接 Buffer Input
            gates[bufGateId].inputNetIds.push_back(netIdx);
            gates[bufGateId].inputPinNames.push_back("A"); 

            // 雙向連接 Buffer Output
            gates[bufGateId].outputNetId = bufNetId;
            nets[bufNetId].driverGateId  = bufGateId;

            // 把超載的 sink pins 改接到新的 bufNetId 上，並收集資訊給 Report
            for (int j = 0; j < (int)overLoads.size(); j++) {
                int loadGateId = overLoads[j].gateId;
                
                // 收集被這顆 Buffer 驅動的下游資訊
                record.drivenGateIds.push_back(loadGateId);
                record.drivenGateNames.push_back(gates[loadGateId].instName);
            }
            moveFanoutSinkPinsToBuffer(gates, nets, netIdx, bufGateId, bufNetId,
                                       keptSinks, overLoads);

            // 填入負載總數並將單筆紀錄推入報告中
            record.fanoutCount = record.drivenGateIds.size();
            report.records.push_back(record);
        }
    }

    return report;
}

//  針對特定的 Net 或 Bus，限制最大 Fanout，並採用 Cascaded Buffer (串聯緩衝樹) 結構
//  回傳值: BufferInsertionReport (包含所有新增 Buffer 的詳細細節)
BufferInsertionReport Netlist::insertBuffersForSpecificNet(const std::string& wireName, int maxFanout) {
    BufferInsertionReport report;

    // 防呆：Fanout 必須至少為 2
    if (maxFanout < 2) return report;

    // 展開 Bus，取得所有目標 Net ID
    std::vector<int> targetNets = expandNetToBits(wireName);
    if (targetNets.empty()) return report;

    int bufCounter = 0; // 用於命名

    // 使用 Queue 來動態追蹤：因為新產生的 Buffer Net 如果也超載，必須再被處理一次！
    std::queue<int> netsToProcess;
    for (int netId : targetNets) {
        netsToProcess.push(netId);
    }

    // 只要還有線路需要檢查，就繼續處理
    while (!netsToProcess.empty()) {
        int netIdx = netsToProcess.front();
        netsToProcess.pop();

        if (netIdx < 0 || netIdx >= (int)nets.size() || nets[netIdx].isConst) {
            continue;
        }

        // 如果這條線的 QA fanout load 超過限制，進行切割
        while (getQaFanoutLoadCount(gates, nets, netIdx) > maxFanout) {
            
            // 留下可安全保留在 source net 上的 sink pins。
            // 剩下的 1 個名額留給即將接上來的 Buffer；若 source net 是 PO，也要預留 PO 名額。
            int keepCount = computeKeepSinkCount(maxFanout, nets[netIdx]);
            std::vector<FanoutSinkPin> sinkPins = collectFanoutSinkPins(gates, netIdx);

            std::vector<FanoutSinkPin> keptSinks(
                sinkPins.begin(),
                sinkPins.begin() + std::min(keepCount, (int)sinkPins.size())
            );
            std::vector<FanoutSinkPin> overLoads(
                sinkPins.begin() + std::min(keepCount, (int)sinkPins.size()),
                sinkPins.end()
            );

            // 建立專屬的新 Buffer 與新線路
            std::string bufName    = wireName + "_fanout_buf_" + std::to_string(bufCounter);
            std::string bufNetName = wireName + "_fanout_net_" + std::to_string(bufCounter);
            bufCounter++;

            int bufGateId = addGate(bufName, GateType::BUF);
            int bufNetId  = addNet(bufNetName);

            // 報告紀錄準備
            BufInsertRecord record;
            record.bufGateId     = bufGateId;
            record.bufGateName   = bufName;
            record.inputNetId    = netIdx;
            record.inputNetName  = nets[netIdx].name;
            record.driverGateId  = nets[netIdx].driverGateId;
            record.driverGateName = (nets[netIdx].driverGateId != -1) ? gates[nets[netIdx].driverGateId].instName : "PI";
            record.outputNetId   = bufNetId;
            record.outputNetName = bufNetName;

            // 雙向連接 Buffer Input (接在當前的 netIdx 上)
            gates[bufGateId].inputNetIds.push_back(netIdx);
            // 為 BUF 補上 input pin name，維持與 inputNetIds 的一對一對齊，保護資料結構
            gates[bufGateId].inputPinNames.push_back("I"); 

            // 雙向連接 Buffer Output
            gates[bufGateId].outputNetId = bufNetId;
            nets[bufNetId].driverGateId  = bufGateId;

            // 把被切出來的超載 sink pins，全部改接到新的 bufNetId 上
            for (int j = 0; j < (int)overLoads.size(); j++) {
                int loadGateId = overLoads[j].gateId;
                
                // 【報告紀錄】：收集下游 Gate 資訊
                record.drivenGateIds.push_back(loadGateId);
                record.drivenGateNames.push_back(gates[loadGateId].instName);
            }
            moveFanoutSinkPinsToBuffer(gates, nets, netIdx, bufGateId, bufNetId,
                                       keptSinks, overLoads);

            // 結算這顆 Buffer 的資訊，並存入 Report
            record.fanoutCount = record.drivenGateIds.size();
            report.records.push_back(record);

            // 將這條「新的 Buffer 輸出線」也推入 Queue！
            // 這樣如果 overLoads 的數量依然 > maxFanout，下一輪它就會再被切出另一顆 Buffer！
            netsToProcess.push(bufNetId);
        }
    }

    return report;
}

// 解決 D-FF 的 Clock 與 Reset High-Fanout 問題，建立 Cascaded Buffer Tree
// maxFanout: 限制的最大推動力 (通常 Clock Tree 會設在 16~32 左右)
// processClock: 是否處理時脈網路
// processReset: 是否處理重置網路
// 回傳值: BufferInsertionReport (包含所有新增 Buffer 的詳細紀錄)
BufferInsertionReport Netlist::insertBuffersForDffControl(int maxFanout, bool processClock, bool processReset) {
    BufferInsertionReport report;

    // 防呆：Fanout 必須至少為 2，且至少要選擇處理一種訊號
    if (maxFanout < 2 || (!processClock && !processReset)) return report;

    // 掃描全電路，自動收集所有扮演 Clock 或 Reset 角色的 Net ID
    // 使用 unordered_set 來避免同一條 Clock 線被重複加入
    std::unordered_set<int> targetNets;

    for (size_t i = 0; i < gates.size(); i++) {
        const Gate& gate = gates[i];
        
        if (gate.type == GateType::DFF) {
            for (size_t k = 0; k < gate.inputPinNames.size(); k++) {
                const std::string& pinName = gate.inputPinNames[k];
                int netId = gate.inputNetIds[k];
                
                if (netId == -1 || nets[netId].isConst) continue;

                bool isClk = (pinName == "CK");
                bool isRst = (pinName == "RN" || pinName == "SN");

                if ((processClock && isClk) || (processReset && isRst)) {
                    targetNets.insert(netId);
                }
            }
        }
    }

    // 針對收集到的 High-Fanout Nets 進行 Cascaded Buffer 插入 (與特定 Net 邏輯相同)
    int bufCounter = 0;
    std::queue<int> netsToProcess;
    
    for (int netId : targetNets) {
        netsToProcess.push(netId);
    }

    while (!netsToProcess.empty()) {
        int netIdx = netsToProcess.front();
        netsToProcess.pop();

        if (netIdx < 0 || netIdx >= (int)nets.size() || nets[netIdx].isConst) {
            continue;
        }

        // 如果這條線的 QA fanout load 超過限制，進行切割
        while (getQaFanoutLoadCount(gates, nets, netIdx) > maxFanout) {
            
            int keepCount = computeKeepSinkCount(maxFanout, nets[netIdx]);
            std::vector<FanoutSinkPin> sinkPins = collectFanoutSinkPins(gates, netIdx);

            std::vector<FanoutSinkPin> keptSinks(
                sinkPins.begin(),
                sinkPins.begin() + std::min(keepCount, (int)sinkPins.size())
            );
            std::vector<FanoutSinkPin> overLoads(
                sinkPins.begin() + std::min(keepCount, (int)sinkPins.size()),
                sinkPins.end()
            );

            // 命名標記為 dff_ctrl_buf，方便在 Report 中一眼認出是 Clock/Reset 的 Buffer
            std::string bufName    = "dff_ctrl_buf_" + std::to_string(bufCounter);
            std::string bufNetName = "dff_ctrl_net_" + std::to_string(bufCounter);
            bufCounter++;

            int bufGateId = addGate(bufName, GateType::BUF);
            int bufNetId  = addNet(bufNetName);

            // 報告紀錄準備
            BufInsertRecord record;
            record.bufGateId      = bufGateId;
            record.bufGateName    = bufName;
            record.inputNetId     = netIdx;
            record.inputNetName   = nets[netIdx].name;
            record.driverGateId   = nets[netIdx].driverGateId;
            record.driverGateName = (nets[netIdx].driverGateId != -1) ? gates[nets[netIdx].driverGateId].instName : "PI";
            record.outputNetId    = bufNetId;
            record.outputNetName  = bufNetName;

            // 雙向連接 Buffer Input
            gates[bufGateId].inputNetIds.push_back(netIdx);
            
            // 為 BUF 補上 input pin name，維持 DFF Pin 對齊
            gates[bufGateId].inputPinNames.push_back("I"); 

            // 雙向連接 Buffer Output
            gates[bufGateId].outputNetId = bufNetId;
            nets[bufNetId].driverGateId  = bufGateId;

            // 將超載的負載轉移，並收集 Report 資訊
            for (int j = 0; j < (int)overLoads.size(); j++) {
                int loadGateId = overLoads[j].gateId;
                
                record.drivenGateIds.push_back(loadGateId);
                record.drivenGateNames.push_back(gates[loadGateId].instName);
            }
            moveFanoutSinkPinsToBuffer(gates, nets, netIdx, bufGateId, bufNetId,
                                       keptSinks, overLoads);

            record.fanoutCount = record.drivenGateIds.size();
            report.records.push_back(record);

            // 將新生成的 Buffer Net 推入 Queue，遞迴檢查是否依然超載
            netsToProcess.push(bufNetId);
        }
    }

    return report;
}

//  為每個負載加上獨立 Buffer (走訪 wire 的 fanout list，為每一個連接的 Gate 建立專屬的 Buffer)
//  回傳值: BufferInsertionReport (包含所有新增 Buffer 的詳細細節)
BufferInsertionReport Netlist::insertBuffersOnEachLoad(const std::string& wireName) {
    BufferInsertionReport report;

    // 展開 Bus，取得所有目標 Net ID (如果是一般線，回傳陣列只會有一個元素)
    std::vector<int> targetNets = expandNetToBits(wireName);
    if (targetNets.empty()) return report;

    for (int netId : targetNets) {
        Net& net = nets[netId];
        
        // 通常不會對常數線插 Buffer，若有特殊需求可移除此行
        if (net.isConst) continue; 

        // 取得該條線的所有 sink pins；同一顆 gate 多個 input pin 接同一條 net 時會保留多筆。
        std::vector<FanoutSinkPin> sinkPins = collectFanoutSinkPins(gates, netId);

        // 清空原本 net 的 load 名單，準備全部換成 Buffer
        nets[netId].loadGateIds.clear();

        for (size_t i = 0; i < sinkPins.size(); i++) {
            const FanoutSinkPin sink = sinkPins[i];
            int loadGateId = sink.gateId;

            // 建立專屬 Buffer 與其輸出的新 Net
            std::string bufName = wireName + "_loadbuf_" + std::to_string(netId) + "_" + std::to_string(i);
            std::string bufOutNetName = wireName + "_loadbuf_net_" + std::to_string(netId) + "_" + std::to_string(i);

            int bufGateId = addGate(bufName, GateType::BUF);
            int bufOutNetId = addNet(bufOutNetName);

            // 報告紀錄準備
            BufInsertRecord record;
            record.bufGateId      = bufGateId;
            record.bufGateName    = bufName;
            record.inputNetId     = netId;
            record.inputNetName   = nets[netId].name;
            record.driverGateId   = nets[netId].driverGateId;
            record.driverGateName = (nets[netId].driverGateId != -1) ? gates[nets[netId].driverGateId].instName : "PI";
            record.outputNetId    = bufOutNetId;
            record.outputNetName  = bufOutNetName;

            // 雙向連接 Buffer Input (接上原本的 netId)
            gates[bufGateId].inputNetIds.push_back(netId);
            // 為 BUF 補上 input pin name，維持與 inputNetIds 的一對一對齊
            gates[bufGateId].inputPinNames.push_back("A"); 
            nets[netId].loadGateIds.push_back(bufGateId); // 把新 buffer 註冊為原本 net 的負載

            // 雙向連接 Buffer Output (接上新的 bufOutNetId)
            gates[bufGateId].outputNetId = bufOutNetId;
            nets[bufOutNetId].driverGateId = bufGateId;
            nets[bufOutNetId].loadGateIds.push_back(loadGateId); // 新 net 的唯一負載是原本的 Gate

            record.drivenGateIds.push_back(loadGateId);
            record.drivenGateNames.push_back(gates[loadGateId].instName);
            record.fanoutCount = 1; // 專屬 Buffer 的 Fanout 永遠為 1

            // 只改接這一個 sink pin，避免同 gate 多 pin 被一起搬走。
            gates[loadGateId].inputNetIds[sink.pinIndex] = bufOutNetId;

            // 單筆紀錄完成，推入報告中
            report.records.push_back(record);
        }
    }
    return report;
}

//  在訊號的驅動端加上單一 Buffer
//  若為內部線或 PO，在 Driver Gate 與 Net 之間打斷並插入 Buffer。
//  若為 PI，將原本的 Net 保留給 PI，並將所有 Load 移至 Buffer 後方的新 Net。
//  回傳值: BufferInsertionReport
BufferInsertionReport Netlist::insertBufferAtDriver(const std::string& wireName) {
    BufferInsertionReport report;

    std::vector<int> targetNets = expandNetToBits(wireName);
    if (targetNets.empty()) return report;

    for (int netId : targetNets) {
        Net& net = nets[netId];
        if (net.isConst) continue;

        // 【情境 A】：這條線有實體的驅動閘 (Driver Gate)，適用於內部線或 PO
        if (net.driverGateId != -1) {
            int driverGateId = net.driverGateId;
            Gate& driverGate = gates[driverGateId];

            // 建立 Buffer 與 中繼 Net (夾在原 Driver 與 Buffer 之間)
            std::string bufName = wireName + "_drvbuf_" + std::to_string(netId);
            std::string midNetName = wireName + "_mid_net_" + std::to_string(netId);

            int bufGateId = addGate(bufName, GateType::BUF);
            int midNetId = addNet(midNetName);

            // 打斷原 Driver：將其輸出從原本的 netId 改接到 midNetId
            driverGate.outputNetId = midNetId;
            nets[midNetId].driverGateId = driverGateId;
            nets[midNetId].loadGateIds.push_back(bufGateId); // 中繼線的 Load 是新 Buffer

            // 連接新 Buffer：輸入吃中繼線，輸出推原本的 netId
            gates[bufGateId].inputNetIds.push_back(midNetId);
            // 為 BUF 補上 input pin name
            gates[bufGateId].inputPinNames.push_back("A"); 
            
            gates[bufGateId].outputNetId = netId;
            net.driverGateId = bufGateId; // 原本的線現在改由新 Buffer 驅動

            // 報告紀錄：情境 A
            // 下游 Gate 依然接在 netId 上，直接從 net.loadGateIds 讀取即可
            BufInsertRecord record;
            record.bufGateId      = bufGateId;
            record.bufGateName    = bufName;
            record.inputNetId     = midNetId;
            record.inputNetName   = midNetName;
            record.driverGateId   = driverGateId;
            record.driverGateName = driverGate.instName;
            record.outputNetId    = netId;
            record.outputNetName  = net.name;

            for (int loadId : net.loadGateIds) {
                record.drivenGateIds.push_back(loadId);
                record.drivenGateNames.push_back(gates[loadId].instName);
            }
            record.fanoutCount = record.drivenGateIds.size();
            report.records.push_back(record);
        }
        // 【情境 B】：這條線是 Primary Input (PI)，沒有驅動閘
        else {
            // 因為 PI 綁死了這條原始 netId，我們必須把 Buffer 加在 netId 「之後」
            std::string bufName = wireName + "_pibuf_" + std::to_string(netId);
            std::string bufOutNetName = wireName + "_pi_out_net_" + std::to_string(netId);

            int bufGateId = addGate(bufName, GateType::BUF);
            int bufOutNetId = addNet(bufOutNetName);

            // 將原始 netId 上面的所有 Loads 移交給新的 bufOutNetId
            std::vector<int> oldLoads = net.loadGateIds;
            net.loadGateIds.clear(); // 清空 PI 的負載

            nets[bufOutNetId].driverGateId = bufGateId;
            nets[bufOutNetId].loadGateIds = oldLoads;

            // 把新 Buffer 接到 PI 的 netId 上
            gates[bufGateId].inputNetIds.push_back(netId);
            // 為 BUF 補上 input pin name
            gates[bufGateId].inputPinNames.push_back("A"); 
            net.loadGateIds.push_back(bufGateId);
            
            gates[bufGateId].outputNetId = bufOutNetId;

            // 報告紀錄準備：情境 B
            BufInsertRecord record;
            record.bufGateId      = bufGateId;
            record.bufGateName    = bufName;
            record.inputNetId     = netId;
            record.inputNetName   = net.name;
            record.driverGateId   = -1;
            record.driverGateName = "PI";
            record.outputNetId    = bufOutNetId;
            record.outputNetName  = bufOutNetName;

            // 走訪所有被搬移的 Loads，將它們的 Input Pin 從原本的 netId 換成 bufOutNetId
            for (int loadGateId : oldLoads) {
                Gate& loadGate = gates[loadGateId];
                
                // 收集被這顆 Buffer 驅動的下游資訊
                record.drivenGateIds.push_back(loadGateId);
                record.drivenGateNames.push_back(loadGate.instName);

                // 就地修改，完美保護原 DFF inputPinNames 對應位置
                for (size_t k = 0; k < loadGate.inputNetIds.size(); k++) {
                    if (loadGate.inputNetIds[k] == netId) {
                        loadGate.inputNetIds[k] = bufOutNetId;
                    }
                }
            }
            
            record.fanoutCount = record.drivenGateIds.size();
            report.records.push_back(record);
        }
    }
    return report;
}

//  在特定的 Gate 前面增加 Buffer (只阻斷指定的 wire 到該 Gate 的連線)
//  回傳值: BufferInsertionReport (包含新增 Buffer 的詳細紀錄)
BufferInsertionReport Netlist::insertBufferBeforeGate(const std::string& wireName, const std::string& targetGateName) {
    BufferInsertionReport report;

    std::vector<int> targetNets = expandNetToBits(wireName);
    if (targetNets.empty()) return report;

    int targetGateId = getGateId(targetGateName);
    if (targetGateId == -1) return report; // 找不到指定的 Gate

    for (int netId : targetNets) {
        Net& net = nets[netId];
        if (net.isConst) continue;

        // 步驟 A：檢查並清除目標 Gate 在原本 net 中的負載紀錄
        // 防呆：解決像 AND(n2, n2) 這種同一顆 Gate 佔用兩個負載空位的情況
        int matchCount = 0;
        for (size_t i = 0; i < net.loadGateIds.size(); ) {
            if (net.loadGateIds[i] == targetGateId) {
                matchCount++;
                // 從原本的 net 拔除這個 Gate
                net.loadGateIds.erase(net.loadGateIds.begin() + i); 
            } else {
                i++;
            }
        }
        
        // 如果這個 Gate 根本沒有接在這條線上，跳過
        if (matchCount == 0) continue; 

        // 步驟 B：建立專屬 Buffer 與輸出的新 Net
        std::string bufName = wireName + "_to_" + targetGateName + "_buf";
        std::string bufOutNetName = wireName + "_to_" + targetGateName + "_net";

        int bufGateId = addGate(bufName, GateType::BUF);
        int bufOutNetId = addNet(bufOutNetName);

        // 報告紀錄準備
        BufInsertRecord record;
        record.bufGateId      = bufGateId;
        record.bufGateName    = bufName;
        record.inputNetId     = netId;
        record.inputNetName   = net.name;
        record.driverGateId   = net.driverGateId;
        record.driverGateName = (net.driverGateId != -1) ? gates[net.driverGateId].instName : "PI";
        record.outputNetId    = bufOutNetId;
        record.outputNetName  = bufOutNetName;

        // 步驟 C：雙向連接 Buffer Input
        gates[bufGateId].inputNetIds.push_back(netId);
        // 為 BUF 補上 input pin name，維持與 inputNetIds 的一對一對齊
        gates[bufGateId].inputPinNames.push_back("I"); 
        
        net.loadGateIds.push_back(bufGateId); // 把 Buffer (只加一次) 放入原本 net 的負載中

        // 步驟 D：雙向連接 Buffer Output
        gates[bufGateId].outputNetId = bufOutNetId;
        nets[bufOutNetId].driverGateId = bufGateId;
        
        // 如果原本接了兩次，新的 net 也要負責推動兩次
        for (int m = 0; m < matchCount; m++) {
            nets[bufOutNetId].loadGateIds.push_back(targetGateId);
        }

        // 步驟 E：將目標 Gate 裡面所有為 netId 的腳位，替換成新的 bufOutNetId
        // (就地修改，完美保護目標 Gate 的 inputPinNames 對應，沒有任何破壞)
        Gate& targetGate = gates[targetGateId];
        for (size_t k = 0; k < targetGate.inputNetIds.size(); k++) {
            if (targetGate.inputNetIds[k] == netId) {
                targetGate.inputNetIds[k] = bufOutNetId;
            }
        }

        // 【報告紀錄】：補上推動的下游目標 (必定只有一個目標 Gate)
        record.drivenGateIds.push_back(targetGateId);
        record.drivenGateNames.push_back(targetGateName);
        record.fanoutCount = 1;

        report.records.push_back(record);
    }
    return report;
}

//  針對某種類型的 Gate，讓他的輸入或輸出都接上 Buffer
//  回傳值: BufferInsertionReport (包含所有新增 Buffer 的詳細紀錄)
BufferInsertionReport Netlist::insertBuffersByGateType(GateType type, bool bufferInputs, bool bufferOutputs) {
    BufferInsertionReport report;

    // 取得當前的 Gate 總數 (Snapshot)，避免掃描到我們在此迴圈內剛新增的 Buffer
    int originalGateCount = gates.size();

    for (int i = 0; i < originalGateCount; i++) {
        if (gates[i].type != type || gates[i].type == GateType::UNKNOWN) continue;

        // 在所有的 Input 腳位前面加上 Buffer
        if (bufferInputs) {
            int numInputs = gates[i].inputNetIds.size();
            for (int k = 0; k < numInputs; k++) {
                int inNetId = gates[i].inputNetIds[k];
                if (inNetId == -1 || nets[inNetId].isConst) continue;

                // 建立 Buffer 與中繼線
                std::string bufName = gates[i].instName + "_inbuf_" + std::to_string(k);
                std::string bufNetName = gates[i].instName + "_innet_" + std::to_string(k);

                int bufGateId = addGate(bufName, GateType::BUF);
                int bufNetId = addNet(bufNetName);

                // 報告紀錄準備：Input Buffer 
                BufInsertRecord record;
                record.bufGateId      = bufGateId;
                record.bufGateName    = bufName;
                record.inputNetId     = inNetId;
                record.inputNetName   = nets[inNetId].name;
                record.driverGateId   = nets[inNetId].driverGateId;
                record.driverGateName = (nets[inNetId].driverGateId != -1) ? gates[nets[inNetId].driverGateId].instName : "PI";
                record.outputNetId    = bufNetId;
                record.outputNetName  = bufNetName;

                // 接上 Buffer 的 Input
                gates[bufGateId].inputNetIds.push_back(inNetId);
                // 為 BUF 補上 input pin name，維持與 inputNetIds 的一對一對齊
                gates[bufGateId].inputPinNames.push_back("I");

                // 在原線的負載中，找到 "1 個" 這個 Gate 的 ID 並替換成 bufGateId
                Net& inNet = nets[inNetId];
                for (size_t loadIdx = 0; loadIdx < inNet.loadGateIds.size(); loadIdx++) {
                    if (inNet.loadGateIds[loadIdx] == i) {
                        inNet.loadGateIds[loadIdx] = bufGateId;
                        break; // 只換一個，解決 AND(A, A) 被掃描兩次的問題
                    }
                }

                // 接上 Buffer 的 Output
                gates[bufGateId].outputNetId = bufNetId;
                nets[bufNetId].driverGateId = bufGateId;
                nets[bufNetId].loadGateIds.push_back(i);

                // 更新 Gate 的第 k 隻腳
                gates[i].inputNetIds[k] = bufNetId;

                // 填入報告：因為是專屬加在 Input 前的 Buffer，Fanout 必定為 1
                record.drivenGateIds.push_back(i);
                record.drivenGateNames.push_back(gates[i].instName);
                record.fanoutCount = 1;
                report.records.push_back(record);
            }
        }

        // 在 Output 腳位後面加上 Buffer
        if (bufferOutputs) {
            int outNetId = gates[i].outputNetId;
            if (outNetId != -1) {
                // 建立 Buffer 與中繼線
                std::string bufName = gates[i].instName + "_outbuf";
                std::string midNetName = gates[i].instName + "_outnet_mid";

                int bufGateId = addGate(bufName, GateType::BUF);
                int midNetId = addNet(midNetName);

                // 報告紀錄準備：Output Buffer 
                BufInsertRecord record;
                record.bufGateId      = bufGateId;
                record.bufGateName    = bufName;
                record.inputNetId     = midNetId;
                record.inputNetName   = midNetName;
                record.driverGateId   = i; // 驅動這顆 Buffer 的正是當前這顆 Gate
                record.driverGateName = gates[i].instName;
                record.outputNetId    = outNetId;
                record.outputNetName  = nets[outNetId].name;

                // 收集原本掛在 outNet 上的所有下游 Gate
                for (int loadId : nets[outNetId].loadGateIds) {
                    record.drivenGateIds.push_back(loadId);
                    record.drivenGateNames.push_back(gates[loadId].instName);
                }
                record.fanoutCount = record.drivenGateIds.size();

                // 打斷原本的連線：Gate 輸出改接 midNet
                gates[i].outputNetId = midNetId;
                nets[midNetId].driverGateId = i;
                nets[midNetId].loadGateIds.push_back(bufGateId); // midNet 負責推 Buffer

                // 接上 Buffer：Buffer 輸出負責推原本的 outNet
                gates[bufGateId].inputNetIds.push_back(midNetId);
                // 為 BUF 補上 input pin name
                gates[bufGateId].inputPinNames.push_back("I");

                gates[bufGateId].outputNetId = outNetId;
                nets[outNetId].driverGateId = bufGateId; // outNet 的新主人是 Buffer

                report.records.push_back(record);
            }
        }
    }
    
    return report;
}
