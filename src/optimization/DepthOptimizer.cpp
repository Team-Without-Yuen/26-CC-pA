#include "include/core/DepthOptimizer.h"
#include "include/lib/cadical.hpp"
#include <string>
#include <iostream>
#include <functional>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>

DepthOptimizer::DepthOptimizer(const DepthOptimizerConfig& config) 
    : config(config) {} // 使用初始化列表進行高效賦值

// 輔助函式：針對特定的 Fanin Cone 執行 Buffer 與 連續 NOT 的消除
// 回傳值：是否有對電路進行任何修改
bool DepthOptimizer::applyBufferAndNotBypass(Netlist& netlist, const OptimizationCandidate& candidate) {
    bool globalChanged = false;
    bool passChanged;

    if (!config.enableBufferAndNotBypass) return false;

    // 利用 API 取得 Cone 內的所有 Gate ID
    std::vector<int> coneGateList = netlist.getConeGateIds(candidate.faninCone);
    std::unordered_set<int> coneGateSet(coneGateList.begin(), coneGateList.end());

    do {
        passChanged = false;

        for (int gateId : coneGateSet) {
            // 安全檢查：可能在上一個迴圈被設為 UNKNOWN 或移除了
            if (netlist.isGateRemoved(gateId) || !netlist.isValidGateId(gateId)) continue;
            
            const Gate& gate = netlist.getGate(gateId);
            GateType type = gate.type;

            // Case 1: 移除 BUF
            if (type == GateType::BUF) {
                if (gate.inputNetIds.empty()) continue; // 防呆
                int inNetId = gate.inputNetIds[0];
                int outNetId = gate.outputNetId;

                if (outNetId != -1) {
                    bool isOutPO = netlist.isPrimaryOutputNet(outNetId);
                    bool isInPI = netlist.isPrimaryInputNet(inNetId);

                    // 如果輸入是 PI，且輸出是 PO，這顆 BUF 不能拔！
                    if (isOutPO && isInPI) {
                        continue; // 在沒有任何修改前安全放棄
                    }

                    // 將原本接在 outNetId 的所有負載，改接到 inNetId
                    netlist.replaceAllLoadsOfNet(outNetId, inNetId);

                    // 如果被拔掉的輸出是 PO，把 inNetId 升級成 PO
                    if (isOutPO) {
                        std::string oldPoName = netlist.getNet(outNetId).name;
                        std::string inNetName = netlist.getNet(inNetId).name;

                        std::string trashName = oldPoName + "_dead_" + std::to_string(gateId);
                        netlist.renameNet(oldPoName, trashName);
                        netlist.renameNet(inNetName, oldPoName);

                        // 屬性繼承與列表更新
                        netlist.swapPrimaryOutputNet(outNetId, inNetId);
                    }
                }

                netlist.removeGate(gateId);
                passChanged = true;
                break; // 結構改變，重新啟動 do-while
            }

            // Case 2: 處理連續的 NOT -> NOT 
            else if (type == GateType::NOT) {
                if (gate.inputNetIds.empty()) continue;
                int midNetId = gate.inputNetIds[0];
                
                // 透過 API 找到驅動它的上一級 Gate ID
                int driverGateId = netlist.getNetDriverGateId(midNetId);

                if (driverGateId != -1 && netlist.isValidGateId(driverGateId)) {
                    const Gate& driverGate = netlist.getGate(driverGateId);
                    
                    if (driverGate.type == GateType::NOT) {
                        // 確保上一級也是我們這次優化範圍內的 Gate
                        if (coneGateSet.count(driverGateId) == 0) continue;

                        if (driverGate.inputNetIds.empty()) continue;
                        int inNetId = driverGate.inputNetIds[0]; // 第一顆 NOT 的輸入
                        int outNetId = gate.outputNetId;         // 第二顆 NOT 的輸出

                        if (outNetId != -1) {
                            bool isOutPO = netlist.isPrimaryOutputNet(outNetId);
                            bool isInPI = netlist.isPrimaryInputNet(inNetId);

                            // 如果輸入是 PI，且輸出是 PO，這組 NOT 不能拔！
                            if (isOutPO && isInPI) {
                                continue; // 在沒有任何修改前安全放棄
                            }

                            netlist.replaceAllLoadsOfNet(outNetId, inNetId);

                            // 判斷被移除的輸出是否為 PO
                            if (isOutPO) {
                                std::string oldPoName = netlist.getNet(outNetId).name;
                                std::string inNetName = netlist.getNet(inNetId).name;

                                std::string trashName = oldPoName + "_dead_" + std::to_string(gateId);
                                netlist.renameNet(oldPoName, trashName);
                                netlist.renameNet(inNetName, oldPoName);
                                netlist.swapPrimaryOutputNet(outNetId, inNetId);
                            }
                        }

                        // 只移除後面的 NOT，前面的留給 DCE 處理
                        netlist.removeGate(gateId);
                        passChanged = true;
                        break; // 結構改變，重新啟動 do-while
                    }
                }
            }
        }

        globalChanged |= passChanged;

    } while (passChanged);

    // Phase 3: 針對 Fanin Cone 做局部 DCE (孤兒閘清除)
    if (globalChanged) {
        bool dceChanged;
        do {
            dceChanged = false;
            for (int gateId : coneGateSet) {
                if (netlist.isGateRemoved(gateId) || !netlist.isValidGateId(gateId)) continue;
                
                const Gate& gate = netlist.getGate(gateId);
                int outNetId = gate.outputNetId;
                
                bool isDrivingPO = false;
                size_t fanoutCount = 0;

                if (outNetId != -1) {
                    isDrivingPO = netlist.isPrimaryOutputNet(outNetId);
                    // 取得 fanout count (透過 API 計算負載閘總數)
                    fanoutCount = netlist.getNetLoadGateCount(netlist.getNet(outNetId).name);
                }

                // 如果輸出沒有 Fanout 且不是 PO，則安全刪除
                if (fanoutCount == 0 && !isDrivingPO) {
                    netlist.removeGate(gateId);
                    dceChanged = true;
                }
            }
        } while (dceChanged);
    }

    return globalChanged;
}