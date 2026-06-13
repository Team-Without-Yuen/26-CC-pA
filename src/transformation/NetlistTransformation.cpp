#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/lib/cadical.hpp"
#include <string>
#include <functional>
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

        while ((int)nets[netIdx].loadGateIds.size() > maxFanout) {
            
            // 只保留 maxFanout - 1 個原有的 loads
            int keepCount = maxFanout - 1;

            // 把超載的負載擷取出來
            std::vector<int> overLoads(
                nets[netIdx].loadGateIds.begin() + keepCount,
                nets[netIdx].loadGateIds.end()
            );
            nets[netIdx].loadGateIds.resize(keepCount);

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
            
            nets[netIdx].loadGateIds.push_back(bufGateId); 

            // 雙向連接 Buffer Output
            gates[bufGateId].outputNetId = bufNetId;
            nets[bufNetId].driverGateId  = bufGateId;

            // 把超載的 load 改接到新的 bufNetId 上，並收集資訊給 Report
            for (int j = 0; j < (int)overLoads.size(); j++) {
                int loadGateId = overLoads[j];
                
                // 收集被這顆 Buffer 驅動的下游資訊
                record.drivenGateIds.push_back(loadGateId);
                record.drivenGateNames.push_back(gates[loadGateId].instName);

                // 修改下游 Gate 的接線 (就地修改，完美保護原 DFF inputPinNames 對應位置)
                for (int k = 0; k < (int)gates[loadGateId].inputNetIds.size(); k++) {
                    if (gates[loadGateId].inputNetIds[k] == netIdx) {
                        gates[loadGateId].inputNetIds[k] = bufNetId;
                    }
                }
                nets[bufNetId].loadGateIds.push_back(loadGateId);
            }

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

        // 如果這條線的負載超過限制，進行切割
        while ((int)nets[netIdx].loadGateIds.size() > maxFanout) {
            
            // 留下 maxFanout - 1 個原有的 loads
            // 剩下的 1 個名額，留給即將接上來的 Buffer
            int keepCount = maxFanout - 1;

            std::vector<int> overLoads(
                nets[netIdx].loadGateIds.begin() + keepCount,
                nets[netIdx].loadGateIds.end()
            );
            
            // 裁切原有的 load 名單
            nets[netIdx].loadGateIds.resize(keepCount);

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
            nets[netIdx].loadGateIds.push_back(bufGateId); // 把 Buffer 加進去，此時數量剛好等於 maxFanout！

            // 雙向連接 Buffer Output
            gates[bufGateId].outputNetId = bufNetId;
            nets[bufNetId].driverGateId  = bufGateId;

            // 把被切出來的超載 Loads，全部改接到新的 bufNetId 上
            for (int j = 0; j < (int)overLoads.size(); j++) {
                int loadGateId = overLoads[j];
                
                // 【報告紀錄】：收集下游 Gate 資訊
                record.drivenGateIds.push_back(loadGateId);
                record.drivenGateNames.push_back(gates[loadGateId].instName);

                // 就地修改接線，不會破壞 loadGate 的 inputPinNames 對應！
                for (int k = 0; k < (int)gates[loadGateId].inputNetIds.size(); k++) {
                    if (gates[loadGateId].inputNetIds[k] == netIdx) {
                        gates[loadGateId].inputNetIds[k] = bufNetId;
                    }
                }
                nets[bufNetId].loadGateIds.push_back(loadGateId);
            }

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

        // 如果這條線的負載超過限制，進行切割
        while ((int)nets[netIdx].loadGateIds.size() > maxFanout) {
            
            int keepCount = maxFanout - 1;

            std::vector<int> overLoads(
                nets[netIdx].loadGateIds.begin() + keepCount,
                nets[netIdx].loadGateIds.end()
            );
            
            nets[netIdx].loadGateIds.resize(keepCount);

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
            
            nets[netIdx].loadGateIds.push_back(bufGateId); 

            // 雙向連接 Buffer Output
            gates[bufGateId].outputNetId = bufNetId;
            nets[bufNetId].driverGateId  = bufGateId;

            // 將超載的負載轉移，並收集 Report 資訊
            for (int j = 0; j < (int)overLoads.size(); j++) {
                int loadGateId = overLoads[j];
                
                record.drivenGateIds.push_back(loadGateId);
                record.drivenGateNames.push_back(gates[loadGateId].instName);

                // 就地替換，不影響 DFF 的 inputPinNames
                for (size_t k = 0; k < gates[loadGateId].inputNetIds.size(); k++) {
                    if (gates[loadGateId].inputNetIds[k] == netIdx) {
                        gates[loadGateId].inputNetIds[k] = bufNetId;
                    }
                }
                nets[bufNetId].loadGateIds.push_back(loadGateId);
            }

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

        // 取得該條線的所有 loadGateIds，並去除重複的 Gate
        // 防呆：解決像 AND(n2, n2) 這種同一顆 Gate 吃同一條線兩次的問題
        std::vector<int> uniqueLoads = net.loadGateIds;
        std::sort(uniqueLoads.begin(), uniqueLoads.end());
        uniqueLoads.erase(std::unique(uniqueLoads.begin(), uniqueLoads.end()), uniqueLoads.end());

        // 清空原本 net 的 load 名單，準備全部換成 Buffer
        net.loadGateIds.clear();

        for (size_t i = 0; i < uniqueLoads.size(); i++) {
            int loadGateId = uniqueLoads[i];
            Gate& loadGate = gates[loadGateId];

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
            record.inputNetName   = net.name;
            record.driverGateId   = net.driverGateId;
            record.driverGateName = (net.driverGateId != -1) ? gates[net.driverGateId].instName : "PI";
            record.outputNetId    = bufOutNetId;
            record.outputNetName  = bufOutNetName;

            // 雙向連接 Buffer Input (接上原本的 netId)
            gates[bufGateId].inputNetIds.push_back(netId);
            // 為 BUF 補上 input pin name，維持與 inputNetIds 的一對一對齊
            gates[bufGateId].inputPinNames.push_back("A"); 
            net.loadGateIds.push_back(bufGateId); // 把新 buffer 註冊為原本 net 的負載

            // 雙向連接 Buffer Output (接上新的 bufOutNetId)
            gates[bufGateId].outputNetId = bufOutNetId;
            nets[bufOutNetId].driverGateId = bufGateId;
            nets[bufOutNetId].loadGateIds.push_back(loadGateId); // 新 net 的唯一負載是原本的 Gate

            record.drivenGateIds.push_back(loadGateId);
            record.drivenGateNames.push_back(loadGate.instName);
            record.fanoutCount = 1; // 專屬 Buffer 的 Fanout 永遠為 1

            // 將原本目標 Gate 的 Input Pin 從原本的 netId 替換成新的 bufOutNetId
            // 保護原 DFF inputPinNames 對應位置
            for (size_t k = 0; k < loadGate.inputNetIds.size(); k++) {
                if (loadGate.inputNetIds[k] == netId) {
                    loadGate.inputNetIds[k] = bufOutNetId;
                }
            }

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

// 遞迴比對引擎核心 (Backward Pattern Matching)
bool TechMapper::matchNet(Netlist& netlist, std::shared_ptr<PatternNode> pNode, int physNetId, MatchContext& ctx) {
    if (physNetId == -1) return false;

    // 處理 Leaf 節點 (外部輸入綁定)
    if (pNode->nodeType == NodeType::LEAF_A) {
        if (ctx.netA == -1) { ctx.netA = physNetId; return true; } // 第一次遇到 A，綁定！
        return ctx.netA == physNetId;                              // 之後遇到 A，必須是同一條線
    }
    if (pNode->nodeType == NodeType::LEAF_B) {
        if (ctx.netB == -1) { ctx.netB = physNetId; return true; }
        return ctx.netB == physNetId;
    }

    // 處理常數比對
    if (pNode->nodeType == NodeType::CONST_1 || pNode->nodeType == NodeType::CONST_0) {
        const Net& net = netlist.getNet(physNetId);
        if (!net.isConst) return false;
        std::string expectedName = (pNode->nodeType == NodeType::CONST_1) ? "1'b1" : "1'b0";
        return net.name == expectedName;
    }

    // 處理內部 Gate 節點
    const Net& net = netlist.getNet(physNetId);
    int driverGateId = net.driverGateId;
    if (driverGateId == -1) return false; // 這條線沒有驅動閘 (可能是 PI)

    // DAG 檢查：如果這個 PatternNode 之前比對過了，物理實體必須是同一個 Gate
    if (ctx.mappedNodes.count(pNode.get())) {
        return ctx.mappedNodes[pNode.get()] == driverGateId;
    }

    const Gate& physGate = netlist.getGate(driverGateId);
    if (physGate.type != pNode->gateType) return false; // 閘類型不符
    if (physGate.inputNetIds.size() != pNode->inputs.size()) return false; // 輸入數量不符

    // 註冊這個物理 Gate
    ctx.mappedNodes[pNode.get()] = driverGateId;
    ctx.matchedGates.insert(driverGateId);

    // 比對輸入線 (考慮 2-input 的交換律)
    if (pNode->inputs.size() == 1) {
        return matchNet(netlist, pNode->inputs[0], physGate.inputNetIds[0], ctx);
    } 
    else if (pNode->inputs.size() == 2) {
        MatchContext backupCtx = ctx; // 建立備份，以便回溯
        
        // 嘗試正向順序：(Input 0 == 0) && (Input 1 == 1)
        if (matchNet(netlist, pNode->inputs[0], physGate.inputNetIds[0], ctx) &&
            matchNet(netlist, pNode->inputs[1], physGate.inputNetIds[1], ctx)) {
            return true;
        }
        
        // 如果失敗，嘗試交換律：(Input 0 == 1) && (Input 1 == 0)
        ctx = backupCtx; // 恢復狀態
        if (matchNet(netlist, pNode->inputs[0], physGate.inputNetIds[1], ctx) &&
            matchNet(netlist, pNode->inputs[1], physGate.inputNetIds[0], ctx)) {
            return true;
        }
    }
    return false;
}

// 觸發比對的入口點
bool TechMapper::matchRootGate(Netlist& netlist, int physGateId, const TechMapRule& rule, MatchContext& ctx) {
    auto pRoot = rule.pattern;
    if (pRoot->nodeType != NodeType::GATE) return false;
    
    // 把目前的物理 Gate 當作 Root，它的 Output 不用比對，我們只比對它的 Input 往下長相
    const Gate& physGate = netlist.getGate(physGateId);
    if (physGate.type != pRoot->gateType) return false;
    if (physGate.inputNetIds.size() != pRoot->inputs.size()) return false;

    ctx.mappedNodes[pRoot.get()] = physGateId;
    ctx.matchedGates.insert(physGateId);

    // 進入遞迴
    if (pRoot->inputs.size() == 1) {
        return matchNet(netlist, pRoot->inputs[0], physGate.inputNetIds[0], ctx);
    } else if (pRoot->inputs.size() == 2) {
        MatchContext backup = ctx;
        if (matchNet(netlist, pRoot->inputs[0], physGate.inputNetIds[0], ctx) &&
            matchNet(netlist, pRoot->inputs[1], physGate.inputNetIds[1], ctx)) return true;
        
        ctx = backup;
        if (matchNet(netlist, pRoot->inputs[0], physGate.inputNetIds[1], ctx) &&
            matchNet(netlist, pRoot->inputs[1], physGate.inputNetIds[0], ctx)) return true;
    }
    return false;
}

// 驗證與替換邏輯
bool TechMapper::isValidSubgraph(Netlist& netlist, const MatchContext& ctx, int rootGateId) {
    // 遍歷子圖中的每一個 Gate
    for (int gateId : ctx.matchedGates) {
        if (gateId == rootGateId) continue; // Root Gate 的輸出本來就是要接給別人的，合法。

        const Gate& g = netlist.getGate(gateId);
        if (g.outputNetId == -1) continue;

        const Net& outNet = netlist.getNet(g.outputNetId);
        if (outNet.isPO) return false; // 不能把連到 Primary Output 的中介閘吃掉！

        // 【最精華的 Fan-out 檢查】
        // 中介閘的所有 Load (吃它訊號的人)，必須統統包含在這次匹配的子圖內！
        for (int loadGateId : outNet.loadGateIds) {
            if (ctx.matchedGates.find(loadGateId) == ctx.matchedGates.end()) {
                return false; // 發現有外部的 Gate 偷接這條線，拔掉會讓別人懸空，退件！
            }
        }
    }
    return true;
}

void TechMapper::replaceSubgraph(Netlist& netlist, const MatchContext& ctx, int rootGateId, const TechMapRule& rule) {
    const Gate& rootGate = netlist.getGate(rootGateId);
    int origOutNetId = rootGate.outputNetId;
    std::string newInstName = rootGate.instName + "_opt";

    // 先把它們標記為 UNKNOWN 並斷開
    for (int gateId : ctx.matchedGates) {
        netlist.removeGate(gateId); 
    }

    // 產生新的目標 Gate
    int newGateId = netlist.addGate(newInstName, rule.targetGate);

    // 接上 A 與 B 的輸入
    if (ctx.netA != -1) netlist.connectGateInput(newGateId, ctx.netA);
    if (ctx.netB != -1) netlist.connectGateInput(newGateId, ctx.netB);

    // 接回原本 Root 負責的輸出線
    if (origOutNetId != -1) {
        netlist.connectGateOutput(newGateId, origOutNetId);
    }
}

std::vector<TechMapRule> TechMapper::getValidRules(GateType targetGate, const std::vector<GateType>& allowedTypes) {
    std::vector<TechMapRule> validRules;
    
    // 將 allowedTypes 轉成 unordered_set 加速查詢
    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());

    for (const auto& rule : rules) {
        // 檢查目標 Gate 是否相符
        if (rule.targetGate != targetGate) continue;

        // 檢查這條 Rule 需要的 Gate 是否都在 allowedTypes 裡面
        bool isValid = true;
        for (GateType reqGate : rule.requiredGates) {
            if (allowedSet.find(reqGate) == allowedSet.end()) {
                isValid = false;
                break;
            }
        }

        // 如果全部吻合，就加入候選名單
        if (isValid) {
            validRules.push_back(rule);
        }
    }

    // 依照「新增 Gate 數量」由小到大排序
    std::sort(validRules.begin(), validRules.end(), 
        [](const TechMapRule& a, const TechMapRule& b) {
            return a.addedGateCount < b.addedGateCount;
        });

    return validRules;
}

int TechMapper::applyForwardMapping(Netlist& netlist, int targetGateId, const TechMapRule& rule) {
    // 取得目標 Gate 的原始資訊 
    const Gate& targetGate = netlist.getGate(targetGateId);
    std::string origInstName = targetGate.instName;
    
    int origNetA = targetGate.inputNetIds.size() > 0 ? targetGate.inputNetIds[0] : -1;
    int origNetB = targetGate.inputNetIds.size() > 1 ? targetGate.inputNetIds[1] : -1;
    int origOutNet = targetGate.outputNetId;

    // 準備 Memoization 表 (解決 Fan-out / DAG 問題)
    std::unordered_map<PatternNode*, int> visited;

    // 建立遞迴走訪的 Lambda 函式
    std::function<int(std::shared_ptr<PatternNode>, bool)> buildNode = 
        [&](std::shared_ptr<PatternNode> node, bool isRoot) -> int {
        
        // 如果節點已經生成過，直接回傳之前產生的 Net ID
        if (visited.count(node.get())) {
            return visited[node.get()];
        }

        int outNetId = -1;

        if (node->nodeType == NodeType::LEAF_A) {
            outNetId = origNetA;
        } 
        else if (node->nodeType == NodeType::LEAF_B) {
            outNetId = origNetB;
        } 
        else if (node->nodeType == NodeType::CONST_1 || node->nodeType == NodeType::CONST_0) {
            std::string constName = (node->nodeType == NodeType::CONST_1) ? "1'b1" : "1'b0";
            outNetId = netlist.getNetId(constName);
            
            if (outNetId == -1) {
                outNetId = netlist.addNet(constName);
                int constVal = (node->nodeType == NodeType::CONST_1) ? 1 : 0;
                netlist.setNetConst(outNetId, true, constVal);  
            }
        } 
        else if (node->nodeType == NodeType::GATE) {
            // A. 先遞迴產生所有的 Input 子樹
            std::vector<int> childNetIds;
            for (auto& child : node->inputs) {
                childNetIds.push_back(buildNode(child, false));
            }

            // B. 在 Netlist 中實體化這個新的 Gate
            std::string newInstName = origInstName + "_map_" + std::to_string(visited.size());
            int newGateId = netlist.addGate(newInstName, node->gateType);

            // C. 將前面取得的 Input Nets 接上
            for (int cNetId : childNetIds) {
                netlist.connectGateInput(newGateId, cNetId);
            }

            // D. 處理 Output Net
            if (isRoot) {
                netlist.connectGateOutput(newGateId, origOutNet);
                outNetId = origOutNet;
            } else {
                std::string newNetName = "net_" + newInstName;
                outNetId = netlist.addNet(newNetName);
                netlist.connectGateOutput(newGateId, outNetId);
            }
        }

        visited[node.get()] = outNetId;
        return outNetId;
    };

    // 啟動遞迴引擎
    buildNode(rule.pattern, true);

    // 透過 Netlist 的合法 API 完美懸空舊 Gate，並標記為 UNKNOWN
    netlist.removeGate(targetGateId);

    return rule.addedGateCount;
}

// 主迴圈 (Iterative Fixpoint Engine)
int TechMapper::applyBackwardMapping(Netlist& netlist) {
    int totalReplacedCount = 0;
    bool isChanged;

    // 將 rules 依照「包含的 Gate 數量 (addedGateCount)」由大到小排序
    // 越複雜的子圖如果能被匹配成功，省下的 Gate 越多！
    std::vector<TechMapRule> sortedRules = rules;
    std::sort(sortedRules.begin(), sortedRules.end(), [](const TechMapRule& a, const TechMapRule& b){
        return a.addedGateCount > b.addedGateCount;
    });

    // 啟動收斂引擎
    do {
        isChanged = false;
        
        // 收集現存的所有合法 Gate (Snapshot) 避免迴圈內增刪干擾 Iterator
        std::vector<int> candidates;
        for (size_t i = 0; i < netlist.getGateCount(); ++i) {
            // 假設你有 Tombstone 機制，這裡要過濾掉已刪除的 Gate
            if (netlist.getGate(i).type != GateType::UNKNOWN) { 
                candidates.push_back(i);
            }
        }

        for (int rootId : candidates) {
            // 如果這個 Gate 在此回合的稍早已經被拔掉了，跳過
            if (netlist.getGate(rootId).type == GateType::UNKNOWN) continue;

            for (const auto& rule : sortedRules) {
                MatchContext ctx;
                
                // 結構匹配
                if (matchRootGate(netlist, rootId, rule, ctx)) {
                    // Fan-out 合法性檢查
                    if (isValidSubgraph(netlist, ctx, rootId)) {
                        
                        // 執行替換
                        replaceSubgraph(netlist, ctx, rootId, rule);
                        
                        totalReplacedCount += (ctx.matchedGates.size() - 1); 
                        isChanged = true;
                        break; // 已經被替換了，不需要再為這個 rootId 測試其他 Rule
                    }
                }
            }
        }
    } while (isChanged); // 只要有改動，就再掃一輪，直到完美收斂為止

    return totalReplacedCount;
}

// targetTypes  : 使用者想要「拔除/替換掉」的 Gate 類型 (例如 {OR, AND})
// allowedTypes : 使用者允許「新增/使用」的 Gate 類型 (例如 {NAND, NOT})
// 回傳值       : 總共變動的 Gate 數量 (正數代表變多，負數代表變少)
// isOneToMany=true=展開(面積增加), false=濃縮(面積減少)
// scopeGates 是一個可選的參數，如果提供了，就只對這些 Gate 進行技術映射，其他 Gate 不受影響。
// 底層的實作引擎
int TechMapper::mapTechnologyCore(Netlist& netlist, 
                              const std::vector<GateType>& targetTypes, 
                              const std::vector<GateType>& allowedTypes,
                              bool isOneToMany,
                              const std::unordered_set<int>* scopeGates) {
    
    std::unordered_set<GateType> targetSet(targetTypes.begin(), targetTypes.end());
    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());

    int totalGateChange = 0;

    // 判斷邏輯：直接根據使用者明確傳入的指令執行
    if (!isOneToMany) {
        
        // ==========================================
        // 情境 A：多對一 (Backward Mapping / Subgraph Reduction)
        // 目標：減少面積
        // ==========================================
        std::vector<TechMapRule> backwardRules;
        for (const auto& rule : rules) {
            // 條件：縮減後的目標 Gate 必須是我們允許生成的 (allowedTypes)
            bool isValid = allowedSet.count(rule.targetGate);
            // 條件：被吃掉的 Pattern 內部 Gate 必須都是我們要拔除的 (targetTypes)
            for (GateType req : rule.requiredGates) {
                if (!targetSet.count(req)) { isValid = false; break; }
            }
            if (isValid) backwardRules.push_back(rule);
        }

        // 排序：優先匹配能消除最多 Gate 的大 Pattern
        std::sort(backwardRules.begin(), backwardRules.end(), [](const TechMapRule& a, const TechMapRule& b) {
            return a.addedGateCount > b.addedGateCount;
        });

        bool isChanged;
        do {
            isChanged = false;
            
            // Snapshot 收集候選人
            std::vector<int> candidates;
            // 判斷是否有給定範圍名單
            if (scopeGates != nullptr) {
                for (int gateId : *scopeGates) {
                    if (gateId >= 0 && gateId < netlist.getGateCount() && netlist.getGate(gateId).type != GateType::UNKNOWN) {
                        candidates.push_back(gateId);
                    }
                }
            } else {
                for (size_t i = 0; i < netlist.getGateCount(); ++i) {
                    if (netlist.getGate(i).type != GateType::UNKNOWN) {
                        candidates.push_back(i);
                    }
                }
            }

            for (int rootId : candidates) {
                if (netlist.getGate(rootId).type == GateType::UNKNOWN) continue;

                for (const auto& rule : backwardRules) {
                    MatchContext ctx;
                    if (matchRootGate(netlist, rootId, rule, ctx)) {
                        if (isValidSubgraph(netlist, ctx, rootId)) {
                            replaceSubgraph(netlist, ctx, rootId, rule);
                            totalGateChange -= (ctx.matchedGates.size() - 1); 
                            isChanged = true;
                            break; // 已經被替換了，跳出 Rule 迴圈
                        }
                    }
                }
            }
        } while (isChanged);

    } 
    else {
        // ==========================================
        // 情境 B：一對多 (Forward Mapping / Gate Expansion)
        // 目標：增加面積 (拆解複雜邏輯)
        // ==========================================
        std::vector<TechMapRule> forwardRules;
        for (const auto& rule : rules) {
            // 條件：被拔除的目標 Gate 必須是我們要拔除的 (targetTypes)
            bool isValid = targetSet.count(rule.targetGate);
            // 條件：展開後用到的 Gate 必須都是允許的 (allowedTypes)
            for (GateType req : rule.requiredGates) {
                if (!allowedSet.count(req)) { isValid = false; break; }
            }
            if (isValid) forwardRules.push_back(rule);
        }

        // 排序：優先使用展開後「新增 Gate 數量最少」的 Rule
        std::sort(forwardRules.begin(), forwardRules.end(), [](const TechMapRule& a, const TechMapRule& b) {
            return a.addedGateCount < b.addedGateCount;
        });

        // 建立快取：為每一個 Target Gate 挑選最優解
        std::unordered_map<GateType, const TechMapRule*> bestForwardRule;
        for (const auto& rule : forwardRules) {
            if (bestForwardRule.find(rule.targetGate) == bestForwardRule.end()) {
                bestForwardRule[rule.targetGate] = &rule;
            }
        }

        // Snapshot 收集需要展開的 Gate
        std::vector<int> forwardCandidates;
        // 判斷是否有給定範圍名單
        if (scopeGates != nullptr) {
            for (int gateId : *scopeGates) {
                if (gateId >= 0 && gateId < netlist.getGateCount()) {
                    const Gate& g = netlist.getGate(gateId);
                    if (g.type != GateType::UNKNOWN && targetSet.count(g.type)) {
                        forwardCandidates.push_back(gateId);
                    }
                }
            }
        } else {
            for (size_t i = 0; i < netlist.getGateCount(); ++i) {
                const Gate& g = netlist.getGate(i);
                if (g.type != GateType::UNKNOWN && targetSet.count(g.type)) {
                    forwardCandidates.push_back(i);
                }
            }
        }

        // 執行一對多替換
        for (int gateId : forwardCandidates) {
            GateType type = netlist.getGate(gateId).type;
            if (bestForwardRule.count(type)) {
                const TechMapRule* bestRule = bestForwardRule[type];
                applyForwardMapping(netlist, gateId, *bestRule);
                totalGateChange += (bestRule->addedGateCount - 1); 
            }
        }
    }

    return totalGateChange;
}

// 萬用 API 實作 (直接呼叫 Core，scopeGates 給 nullptr 代表全部)
int TechMapper::mapTechnology(Netlist& netlist, const std::vector<GateType>& targetTypes, const std::vector<GateType>& allowedTypes, bool isOneToMany) {
    return mapTechnologyCore(netlist, targetTypes, allowedTypes, isOneToMany, nullptr);
}

// 針對 Cone 的 API 實作 (使用 Netlist 內建的 getConeGateIds)
int TechMapper::mapTechnologyForCone(Netlist& netlist, 
                              const std::vector<GateType>& targetTypes, 
                              const std::vector<GateType>& allowedTypes, 
                              bool isOneToMany, 
                              const ConeResult& targetCone) {
                              
    // 呼叫 Netlist 原本就有的 Function 取得 Gate 陣列
    std::vector<int> coneGateVec = netlist.getConeGateIds(targetCone);
    
    // 將 vector 轉換為 unordered_set，讓後續引擎在查詢時擁有 O(1) 的效率
    std::unordered_set<int> scopeGates(coneGateVec.begin(), coneGateVec.end());
    
    // 呼叫核心引擎！
    return mapTechnologyCore(netlist, targetTypes, allowedTypes, isOneToMany, &scopeGates);
}

// 核心輔助函式：給定「允許使用的基礎閘」，自動把其他所有的組合邏輯閘拆解
// 核心輔助函式：根據 TargetScope 解析 Cone，並呼叫底層引擎
int TechMapper::convertToBasis(Netlist& netlist, const std::vector<GateType>& allowedTypes, TargetScope scope, const std::string& name) {
    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());
    std::vector<GateType> targetTypes;

    std::vector<GateType> allCombinational = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
        GateType::NOT, GateType::BUF, GateType::XOR, GateType::XNOR
    };

    for (GateType type : allCombinational) {
        if (allowedSet.find(type) == allowedSet.end()) {
            targetTypes.push_back(type);
        }
    }

    // 根據使用者的選項，動態產生 ConeResult
    // 如果是 WHOLE_NETLIST，就呼叫原本不需要 Cone 的 mapTechnology
    switch (scope) {
        case TargetScope::WHOLE_NETLIST:
            return mapTechnology(netlist, targetTypes, allowedTypes, true);
            
        case TargetScope::NET_FANIN:
            return mapTechnologyForCone(netlist, targetTypes, allowedTypes, true, netlist.getTransitiveFaninCone(name));
            
        case TargetScope::NET_FANOUT:
            return mapTechnologyForCone(netlist, targetTypes, allowedTypes, true, netlist.getTransitiveFanoutCone(name));
            
        case TargetScope::GATE_FANIN:
            return mapTechnologyForCone(netlist, targetTypes, allowedTypes, true, netlist.getGateTransitiveFaninCone(name));
            
        case TargetScope::GATE_FANOUT:
            return mapTechnologyForCone(netlist, targetTypes, allowedTypes, true, netlist.getGateTransitiveFanoutCone(name));
            
        default:
            return 0; // 防呆
    }
}

// 將整個 netlist 轉成 {AND, NOT} (AIG: And-Inverter Graph)
int TechMapper::convertToAndNot(Netlist& netlist, TargetScope scope, const std::string& name) {
    return convertToBasis(netlist, {GateType::AND, GateType::NOT}, scope, name);
}

// 將整個 netlist 轉成 {OR, NOT} (OIG: Or-Inverter Graph)
int TechMapper::convertToOrNot(Netlist& netlist, TargetScope scope, const std::string& name) {
    return convertToBasis(netlist, {GateType::OR, GateType::NOT}, scope, name);
}

// 將整個 netlist 轉成 {NAND} (純 NAND 網路)
// 說明：NAND 是 Universal Gate (萬用閘)。在早期 TTL 或現代 CMOS 中，NAND 的電晶體堆疊最少，
// 速度最快，這是一個非常符合物理特性的轉換。
int TechMapper::convertToNand(Netlist& netlist, TargetScope scope, const std::string& name) {
    return convertToBasis(netlist, {GateType::NAND}, scope, name);
}

// 將整個 netlist 轉成 {NOR} (純 NOR 網路)
// 說明：NOR 同樣是 Universal Gate，常用於一些特殊的記憶體周邊控制電路。
int TechMapper::convertToNor(Netlist& netlist, TargetScope scope, const std::string& name) {
    return convertToBasis(netlist, {GateType::NOR}, scope, name);
}

// 將整個 netlist 轉成 XAG (XOR-AND Graph)
// 說明：由 {XOR, AND, NOT} 組成。XAG 在現代 EDA 非常紅！
// 在全同態加密 (FHE) 與量子運算中，XOR 通常是 Free (不用成本) 的，
// 而 AND 需要消耗極大的資源，所以會特別使用 XAG 來做進一步的最佳化。
int TechMapper::convertToXag(Netlist& netlist, TargetScope scope, const std::string& name) {
    return convertToBasis(netlist, {GateType::XOR, GateType::AND, GateType::NOT}, scope, name);
}

// 將整個 netlist 轉成 {XOR, AND} (ANF: Algebraic Normal Form)
// 說明：又稱 Reed-Muller 展開。這是一種沒有 NOT 閘的代數結構！
// 引擎非常聰明，遇到 NOT 閘時，會自動使用查表裡的 A XOR 1 來替換，
int TechMapper::convertToAnf(Netlist& netlist, TargetScope scope, const std::string& name) {
    return convertToBasis(netlist, {GateType::XOR, GateType::AND}, scope, name);
}

// 將整個 netlist 轉成 {XOR, OR}
// 說明：這是另一種特化的代數基底映射。
int TechMapper::convertToXorOr(Netlist& netlist, TargetScope scope, const std::string& name) {
    return convertToBasis(netlist, {GateType::XOR, GateType::OR}, scope, name);
}

// 將整個 netlist 轉成 {XNOR, AND}
// 說明：XNOR 與 XOR 具有對稱性，在某些 Cell Library 中 XNOR 的面積更小。
// NOT 閘會被自動替換為 A XNOR 0。
int TechMapper::convertToXnorAnd(Netlist& netlist, TargetScope scope, const std::string& name) {
    return convertToBasis(netlist, {GateType::XNOR, GateType::AND}, scope, name);
}

// 將整個 netlist 轉成 {XNOR, OR}
// 說明：特化的邏輯合成基底。
int TechMapper::convertToXnorOr(Netlist& netlist, TargetScope scope, const std::string& name) {
    return convertToBasis(netlist, {GateType::XNOR, GateType::OR}, scope, name);
}

// 給使用者呼叫的任意修改 API 
int TechMapper::customMapTechnology(Netlist& netlist, 
                                    const std::vector<GateType>& targetTypes, 
                                    const std::vector<GateType>& allowedTypes, 
                                    bool isOneToMany, 
                                    TargetScope scope, 
                                    const std::string& name) {
    
    // 根據使用者指定的 Scope，自動產生對應的 Cone 並呼叫底層的 API
    switch (scope) {
        case TargetScope::WHOLE_NETLIST:
            return mapTechnology(netlist, targetTypes, allowedTypes, isOneToMany);
            
        case TargetScope::NET_FANIN:
            return mapTechnologyForCone(netlist, targetTypes, allowedTypes, isOneToMany, netlist.getTransitiveFaninCone(name));
            
        case TargetScope::NET_FANOUT:
            return mapTechnologyForCone(netlist, targetTypes, allowedTypes, isOneToMany, netlist.getTransitiveFanoutCone(name));
            
        case TargetScope::GATE_FANIN:
            return mapTechnologyForCone(netlist, targetTypes, allowedTypes, isOneToMany, netlist.getGateTransitiveFaninCone(name));
            
        case TargetScope::GATE_FANOUT:
            return mapTechnologyForCone(netlist, targetTypes, allowedTypes, isOneToMany, netlist.getGateTransitiveFanoutCone(name));
            
        default:
            return 0; // 防呆機制
    }
}