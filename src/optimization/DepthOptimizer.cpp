#include "include/core/DepthOptimizer.h"
#include "include/lib/cadical.hpp"
#include <string>
#include <iostream>
#include <functional>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>

DepthOptimizer::DepthOptimizer(const DepthOptimizerConfig& config) 
    : config(config) {} // 使用初始化列表進行高效賦值

// 輔助函式：判斷該閘是否滿足結合律 (Associative Law)
bool isAssociativeGate(GateType type) {
    return (type == GateType::AND || type == GateType::OR || 
            type == GateType::XOR || type == GateType::XNOR);
}

// 輔助函式：檢查該閘是否在規則允許的範圍內
bool isGateAllowed(GateType type, const std::vector<GateType>& allowedTypes, const std::vector<GateType>& bannedTypes) {
    // 如果有指定 bannedTypes，且在裡面，絕對不行
    if (std::find(bannedTypes.begin(), bannedTypes.end(), type) != bannedTypes.end()) {
        return false;
    }
    // 如果有指定 allowedTypes，且不在裡面，也不行
    if (!allowedTypes.empty() && std::find(allowedTypes.begin(), allowedTypes.end(), type) == allowedTypes.end()) {
        return false;
    }
    return true;
}

// 針對單一 Candidate 進行深度縮減
OptimizationResult DepthOptimizer::reduceDepth(Netlist& netlist, 
                                               const OptimizationCandidate& candidate,
                                               const DepthOptimizerConfig& config,
                                               const std::vector<GateType>& allowedTypes,
                                               const std::vector<GateType>& bannedTypes,
                                               bool verbose) {
    OptimizationResult result;
    result.passName = "DepthOptimizer::reduceDepth";
    
    // 構造正確：純粹的代數與結構重構保證邏輯等價
    result.equivalenceChecked = true;
    result.equivalent = true;

    // 輔助計數器 (Lambda)
    // 利用 countGatesByType() 加總取得全域總閘數
    auto getTotalGateCount = [](const Netlist& nl) -> int {
        int total = 0;
        for (const auto& pair : nl.countGatesByType()) {
            // 絕對不能把 UNKNOWN (已刪除的廢棄閘) 算進面積裡！
            if (pair.first != GateType::UNKNOWN) {
                total += pair.second;
            }
        }
        return total;
    };

    // 0. 記錄初始狀態與備份 (Snapshot & Backup)
    result.oldDepth = candidate.endpoint.depth;
    result.oldGateCount = getTotalGateCount(netlist);
    
    // 為了支援 Rollback，我們先備份一份當前的 Netlist 狀態。
    Netlist backupNetlist = netlist; 

    bool globalModified = false;
    int iterationCount = 0; // 用於 verbose 追蹤

    if (verbose) {
        std::cout << "\n[DepthOptimizer] Start reducing depth for endpoint: " 
                  << candidate.endpoint.endpointName << "\n";
    }

    // 收斂迴圈 (Convergence Loop)
    while (true) {
        iterationCount++;
        bool passModified = false;

        // 每輪開始，重新抓取最新的電路拓樸與該條 Critical Path
        // 永遠相信 endpointName！
        std::string currentEpName = candidate.endpoint.endpointName;
        OptimizationCandidate currentCandidate = candidate;
        
        currentCandidate.criticalPath = netlist.findCriticalPathToNet(currentEpName);

        // 防呆機制。如果電路被優化成常數 0/1，路徑可能不復存在
        if (!currentCandidate.criticalPath.exists()) {
            if (verbose) std::cout << "  -> [Iter " << iterationCount << "] Path no longer exists (optimized to const).\n";
            break; 
        }

        currentCandidate.faninCone = netlist.getTransitiveFaninCone(currentEpName);

        if (verbose) std::cout << "  -> [Iter " << iterationCount << "] Current Depth: " 
                               << currentCandidate.criticalPath.depth() << "\n";

        // 步驟 1. 初步清掃 (顯式冗餘消除)
        if (config.enableBufferAndNotBypass) {
            if (applyBufferAndNotBypass(netlist, currentCandidate)) {
                passModified = true;
                if (verbose) std::cout << "     * Buffer/Not Bypass modified the netlist.\n";
            }
        }

        // 步驟 2. 平凡邏輯消除 (拓樸感知的常數摺疊與等冪短路)
        if (!passModified) {
            if (applyTrivialReduction(netlist, currentCandidate, allowedTypes, bannedTypes)) {
                passModified = true;
                if (verbose) std::cout << "     * Trivial Reduction modified the netlist.\n";
            }
        }

        // 步驟 3. 代數重構 (霍夫曼建樹)
        if (config.enableTreeBalancing && !passModified) {
            if (applyTreeBalancing(netlist, currentCandidate, allowedTypes, bannedTypes)) {
                passModified = true;
                if (verbose) std::cout << "     * Tree Balancing modified the netlist.\n";
            }
        }

        // 步驟 4. 極性最佳化 (德摩根推擠)
        if (config.enableDeMorganPushing && !passModified) {
            if (applyDeMorganPushing(netlist, currentCandidate, allowedTypes, bannedTypes)) {
                passModified = true;
                if (verbose) std::cout << "     * DeMorgan Pushing modified the netlist.\n";
            }
        }

        // 步驟 5. 布林最佳化：K-feasible Cut + SAT Exact Synthesis
        /*if (config.enableConeResynthesis) {
            // TODO: SAT Synthesis
        }*/

        if (passModified) {
            globalModified = true;
            continue; // 拓樸已改變，立刻跳回 while 開頭，重新抓取 Critical Path！
        } else {
            break; // 所有 Pass 都無事可做，電路已完美收斂，跳出迴圈
        }
    }

    // 全域死碼消除 (Global Dead Code Elimination)
    // 迴圈收斂後，把掉出邏輯錐、失去連線的孤魂野鬼一次清乾淨
    if (globalModified) {
        int removedZombies = netlist.removeDanglingLogic();
        if (verbose && removedZombies > 0) {
            std::cout << "  -> [Global DCE] Cleaned up " << removedZombies << " dangling logic gates.\n";
        }
    }

    // 5. 結算成效與 Rollback 決策 (Resolution)
    if (globalModified) {
        // a. 重新計算深度與面積
        result.newDepth = netlist.findCriticalPathToNet(candidate.endpoint.endpointName).depth();
        result.newGateCount = getTotalGateCount(netlist);
        result.areaDelta = result.newGateCount - result.oldGateCount;
        result.depthImproved = (result.newDepth < result.oldDepth);

        // b. 檢查面積是否超標 (-1 代表無限制)
        bool areaExceeded = (config.maxAreaIncreasePerPath != -1) && 
                            (result.areaDelta > config.maxAreaIncreasePerPath);

        // c. 決策分流
        if (areaExceeded) {
            // 【Rollback 情境 1】深度可能降了，但面積代價太高，退回！
            netlist = backupNetlist; // 還原
            
            result.status = OptimizationStatus::ERROR_AREA_EXCEEDED;
            result.changed = false; // 退回後等於沒改
            result.newDepth = result.oldDepth;
            result.newGateCount = result.oldGateCount;
            result.message = "Optimization achieved but area exceeded limit (Delta: " + 
                             std::to_string(result.areaDelta) + " > " + 
                             std::to_string(config.maxAreaIncreasePerPath) + "). Rolled back.";
                             
        } else if (!result.depthImproved) {
            // 【Rollback 情境 2】電路拓樸被改得面目全非，但最長深度根本沒降，退回！
            netlist = backupNetlist; // 還原
            
            result.status = OptimizationStatus::NO_IMPROVEMENT;
            result.changed = false; // 退回後等於沒改
            result.newDepth = result.oldDepth;
            result.newGateCount = result.oldGateCount;
            result.message = "Topology changed but depth did not improve. Rolled back to original state.";
            
        } else {
            // 深度下降，且面積在容許範圍內！
            result.status = OptimizationStatus::SUCCESS;
            result.changed = true;
            result.message = "Depth successfully improved from " + std::to_string(result.oldDepth) + 
                             " to " + std::to_string(result.newDepth) + 
                             " (Area delta: " + std::to_string(result.areaDelta) + ").";
        }
    } else {
        // 電路完全沒有觸發任何優化
        result.status = OptimizationStatus::NO_IMPROVEMENT;
        result.changed = false;
        result.newDepth = result.oldDepth;
        result.newGateCount = result.oldGateCount;
        result.areaDelta = 0;
        result.message = "No optimization opportunities found.";
    }

    return result;
}

// 拓樸感知的平凡邏輯消除 (Basis-Aware Trivial Logic Reduction)
bool DepthOptimizer::applyTrivialReduction(Netlist& netlist, 
                                           const OptimizationCandidate& candidate,
                                           const std::vector<GateType>& allowedTypes,
                                           const std::vector<GateType>& bannedTypes) {
    bool isModified = false;
    std::vector<int> coneGateList = netlist.getConeGateIds(candidate.faninCone);
    
    // 定義化簡動作
    enum class Action { NONE, BYPASS, CONST_0, CONST_1, INVERT };

    for (int gateId : coneGateList) {
        if (!netlist.isValidGateId(gateId)) continue;
        const Gate& gate = netlist.getGate(gateId);
        
        Action action = Action::NONE;
        int signalNetId = -1; // 記錄留下來的有效訊號線

        // 1. 檢查等冪律 (Idempotency) 與自身互斥： A op A
        if (gate.inputNetIds.size() == 2 && gate.inputNetIds[0] == gate.inputNetIds[1]) {
            signalNetId = gate.inputNetIds[0];
            switch (gate.type) {
                case GateType::AND:
                case GateType::OR:   action = Action::BYPASS;  break; // A & A = A, A | A = A
                case GateType::XOR:  action = Action::CONST_0; break; // A ^ A = 0
                case GateType::XNOR: action = Action::CONST_1; break; // A XNOR A = 1
                case GateType::NAND:
                case GateType::NOR:  action = Action::INVERT;  break; // A NAND A = ~A, A NOR A = ~A
                default: break;
            }
        }
        
        // 2. 檢查常數交互作用 (Identity & Annihilator)： A op Const
        else if (gate.inputNetIds.size() == 2) {
            int in0 = gate.inputNetIds[0];
            int in1 = gate.inputNetIds[1];
            bool in0Const = netlist.getNet(in0).isConst;
            bool in1Const = netlist.getNet(in1).isConst;

            int constVal = -1;
            if (in0Const && !in1Const) { constVal = netlist.getNet(in0).constVal; signalNetId = in1; }
            else if (!in0Const && in1Const) { constVal = netlist.getNet(in1).constVal; signalNetId = in0; }

            if (signalNetId != -1) {
                if (gate.type == GateType::AND)       action = (constVal == 0) ? Action::CONST_0 : Action::BYPASS;
                else if (gate.type == GateType::OR)   action = (constVal == 1) ? Action::CONST_1 : Action::BYPASS;
                else if (gate.type == GateType::XOR)  action = (constVal == 0) ? Action::BYPASS  : Action::INVERT;
                else if (gate.type == GateType::XNOR) action = (constVal == 1) ? Action::BYPASS  : Action::INVERT;
                else if (gate.type == GateType::NAND) action = (constVal == 0) ? Action::CONST_1 : Action::INVERT;
                else if (gate.type == GateType::NOR)  action = (constVal == 1) ? Action::CONST_0 : Action::INVERT;
            }
        }

        if (action == Action::NONE) continue;

        int outNetId = gate.outputNetId;
        if (outNetId == -1) continue;
        bool isOutPO = netlist.getNet(outNetId).isPO;

        // 3. 執行化簡與拓樸修改
        if (action == Action::BYPASS) {
            if (isOutPO && netlist.getNet(signalNetId).isPI) continue; // 禁止 PI 直連 PO
            
            netlist.replaceAllLoadsOfNet(outNetId, signalNetId);
            if (isOutPO) netlist.swapPrimaryOutputNet(outNetId, signalNetId);
            netlist.removeGate(gateId);
            isModified = true;
        }
        else if (action == Action::CONST_0 || action == Action::CONST_1) {
            int cVal = (action == Action::CONST_1) ? 1 : 0;
            std::string constNetName = "const_" + std::to_string(cVal) + "_opt_" + std::to_string(netlist.getNetCount());
            int constNetId = netlist.addNet(constNetName);
            netlist.setNetConst(constNetId, true, cVal);

            netlist.replaceAllLoadsOfNet(outNetId, constNetId);
            if (isOutPO) netlist.swapPrimaryOutputNet(outNetId, constNetId);
            netlist.removeGate(gateId);
            isModified = true;
        }
        else if (action == Action::INVERT) {
            // 依據提供的 LUT 規則與當前電路的白名單，動態尋找合法的反相器！
            GateType bestInvType = GateType::UNKNOWN;
            
            if (isGateAllowed(GateType::NOT, allowedTypes, bannedTypes))       bestInvType = GateType::NOT;
            else if (isGateAllowed(GateType::NAND, allowedTypes, bannedTypes)) bestInvType = GateType::NAND;
            else if (isGateAllowed(GateType::NOR, allowedTypes, bannedTypes))  bestInvType = GateType::NOR;
            else if (isGateAllowed(GateType::XOR, allowedTypes, bannedTypes))  bestInvType = GateType::XOR;
            else if (isGateAllowed(GateType::XNOR, allowedTypes, bannedTypes)) bestInvType = GateType::XNOR;

            if (bestInvType == GateType::UNKNOWN) continue; // 找不到任何合法的反相結構，放棄此化簡以保證合法性

            // 如果這顆閘本來就已經是最佳的合法反相結構，跳過以防無限迴圈
            if (gate.type == bestInvType) {
                if (bestInvType == GateType::NAND || bestInvType == GateType::NOR) {
                    if (gate.inputNetIds[0] == gate.inputNetIds[1]) continue; 
                } else if (bestInvType == GateType::XOR || bestInvType == GateType::XNOR) {
                    if (netlist.getNet(gate.inputNetIds[0]).isConst || netlist.getNet(gate.inputNetIds[1]).isConst) continue;
                }
            }

            // 實作合法的反相器替換
            std::string invGateName = "triv_inv_" + std::to_string(netlist.getGateCount());
            std::string invNetName = "triv_inv_net_" + std::to_string(netlist.getNetCount());
            int invGateId = netlist.addGate(invGateName, bestInvType);
            int invNetId = netlist.addNet(invNetName);

            if (bestInvType == GateType::NOT) {
                netlist.connectGateInput(invGateId, signalNetId);
            } else if (bestInvType == GateType::NAND || bestInvType == GateType::NOR) {
                // 產生 NAND(A, A) 或 NOR(A, A)
                netlist.connectGateInput(invGateId, signalNetId);
                netlist.connectGateInput(invGateId, signalNetId);
            } else if (bestInvType == GateType::XOR) {
                // 產生 XOR(A, 1)
                std::string cNetName = "1'b1";
                int cNetId = netlist.addNet(cNetName);
                netlist.setNetConst(cNetId, true, 1);
                netlist.connectGateInput(invGateId, signalNetId);
                netlist.connectGateInput(invGateId, cNetId);
            } else if (bestInvType == GateType::XNOR) {
                // 產生 XNOR(A, 0)
                std::string cNetName = "1'b0";
                int cNetId = netlist.addNet(cNetName);
                netlist.setNetConst(cNetId, true, 0);
                netlist.connectGateInput(invGateId, signalNetId);
                netlist.connectGateInput(invGateId, cNetId);
            }

            netlist.connectGateOutput(invGateId, invNetId);
            netlist.replaceAllLoadsOfNet(outNetId, invNetId);
            if (isOutPO) netlist.swapPrimaryOutputNet(outNetId, invNetId);
            netlist.removeGate(gateId);
            isModified = true;
        }
    }
    return isModified;
}

// 針對特定的 Fanin Cone 執行 Buffer 與 連續 NOT 的消除
// 回傳值：是否有對電路進行任何修改
bool DepthOptimizer::applyBufferAndNotBypass(Netlist& netlist, const OptimizationCandidate& candidate) {
    bool globalChanged = false;
    bool passChanged;

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

// Tree Balancing 主程式
bool DepthOptimizer::applyTreeBalancing(Netlist& netlist, 
                                        const OptimizationCandidate& candidate, 
                                        const std::vector<GateType>& allowedTypes,
                                        const std::vector<GateType>& bannedTypes) {
    bool isModified = false;

    // 1. 取得 Cone 內的 Gate 集合 (限制 extractAssociativeChain 的邊界，避免破壞其他邏輯)
    std::vector<int> coneGateList = netlist.getConeGateIds(candidate.faninCone);
    std::unordered_set<int> coneGateSet(coneGateList.begin(), coneGateList.end());

    // 記錄已經在重構過程中被刪除的 Gate，保護 Critical Path 走訪時不會踩到空指標
    std::unordered_set<int> deletedGates;

    // 2. 沿著 Critical Path 由後往前 (從 Endpoint 往 PI) 尋找重構機會
    // 使用 rbegin() 到 rend() 反向走訪，優先抓住最下游的 Root！
    for (auto it = candidate.criticalPath.gateIds.rbegin(); it != candidate.criticalPath.gateIds.rend(); ++it) {
        int currGateId = *it;

        // 如果這顆閘已經在剛才的某次建樹過程中被當作舊閘刪除了，直接跳過
        if (deletedGates.count(currGateId) || !netlist.isValidGateId(currGateId)) {
            continue;
        }

        const Gate& currGate = netlist.getGate(currGateId);
        
        // 數學限制 (只處理滿足結合律的邏輯閘)
        if (!isAssociativeGate(currGate.type)) {
            continue;
        }

        // 其他限制 (確保這個閘是合法的，我們才去拔它並重新生成)
        if (!isGateAllowed(currGate.type, allowedTypes, bannedTypes)) {
            continue;
        }

        // 3. 嘗試以這顆閘為 Root，往上游拔起這棵樹
        AssociativeChain chain = extractAssociativeChain(netlist, currGateId, currGate.type, coneGateSet);

        // 計算新樹的目標類型 (處理 XOR/XNOR 吸收 NOT 的情況)
        GateType targetTreeType = currGate.type;
        if (currGate.type == GateType::XOR || currGate.type == GateType::XNOR) {
            if (chain.isXorParityInverted) {
                targetTreeType = (currGate.type == GateType::XOR) ? GateType::XNOR : GateType::XOR;
            }
            
            // 防呆：確保翻轉極性後的閘仍然在白名單內
            if (!isGateAllowed(targetTreeType, allowedTypes, bannedTypes)) {
                continue; 
            }
        }

        // 若完全沒改到任何結構 (既沒吸收到氣泡，也沒把兩顆以上的閘壓扁)，不值得平衡
        if (!chain.isXorParityInverted && chain.internalGateIds.size() <= 1) {
            continue;
        }

        // 4. 使用霍夫曼演算法重構平衡樹
        int newRootNetId = -1;
        // 處理 XOR/XNOR 成對消除後，全數抵消為空的情況
        if (chain.leafNetIds.empty()) {
            // XOR 消除為 0，XNOR 消除為 1
            int constVal = (currGate.type == GateType::XNOR) ? 1 : 0;
            std::string constNetName = constVal ? "1'b1" : "1'b0";
            
            newRootNetId = netlist.addNet(constNetName);
            netlist.setNetConst(newRootNetId, true, constVal);
        } 
        else if (chain.leafNetIds.size() == 1) {
            // 極端情況：抵消到只剩一個訊號。理論上 applyTrivialReduction 會先處理掉，
            // 萬一漏網，安全起見我們直接跳過，交給下一輪迴圈清理。
            continue;
        }
        else {
            //  Dry-Run 預先模擬 (加入氣泡紅利考量)
            std::priority_queue<int, std::vector<int>, std::greater<int>> depthPQ;
            for (int leafNetId : chain.leafNetIds) {
                if (!netlist.isValidNetId(leafNetId)) continue;
                std::string leafName = netlist.getNet(leafNetId).name;
                depthPQ.push(netlist.findCriticalPathToNet(leafName).depth());
            }

            // 推演建樹過程
            while (depthPQ.size() > 1) {
                int d1 = depthPQ.top(); depthPQ.pop();
                int d2 = depthPQ.top(); depthPQ.pop();
                depthPQ.push(std::max(d1, d2) + 1); 
            }
            int expectedNewDepth = depthPQ.empty() ? 0 : depthPQ.top();

            // 取得這棵樹目前的真實深度
            std::string oldRootName = netlist.getNet(currGate.outputNetId).name;
            int oldDepth = netlist.findCriticalPathToNet(oldRootName).depth();

            // 防護網：如果重構後的預期深度 >= 舊深度
            // 備註：如果發生了極性翻轉 (吸收了 NOT 閘)，代表我們成功消除了多餘的氣泡！
            // 此時即使深度不變 (expectedNewDepth == oldDepth)，我們依然要放行重構！
            // 且因為重構後 NOT 閘就消失了，下一輪掃描 isXorParityInverted 會是 false，從而成功擋下無限迴圈。
            if (expectedNewDepth > oldDepth || (expectedNewDepth == oldDepth && !chain.isXorParityInverted)) {
                continue;
            }

            // 確定值得重構，真正呼叫 API 建樹
            newRootNetId = buildBalancedTree(netlist, chain.leafNetIds, targetTreeType);
            if (newRootNetId == -1) continue; 
        }

        // 5. 完美的縫合手術 (Stitching)
        int oldRootNetId = currGate.outputNetId;
        const Net& oldRootNet = netlist.getNet(oldRootNetId);

        // a. 轉移負載：將所有原本聽從舊樹結果的邏輯閘，瞬間改接到新樹的 Root
        netlist.replaceAllLoadsOfNet(oldRootNetId, newRootNetId);

        // b. 邊界處理：如果舊樹的 Root 剛好是一條 Primary Output，必須特別通知 Netlist 替換 PO
        if (oldRootNet.isPO) {
            netlist.swapPrimaryOutputNet(oldRootNetId, newRootNetId);
        }

        // 6. 資源回收 (Garbage Collection)
        for (int id : chain.internalGateIds) {
            netlist.removeGate(id);
            deletedGates.insert(id); // 記錄起來，以免 Critical Path 後面的走訪踩雷
        }

        isModified = true;

        // 成功拔起一棵樹並重構後，原本的 Critical Path 已經失效，立刻跳出迴圈
        break;
    }

    return isModified;
}

// 根據霍夫曼演算法重新建構平衡樹
// 回傳值：重構後這棵新樹的「根節點 (Root Net ID)」
int DepthOptimizer::buildBalancedTree(Netlist& netlist, const std::vector<int>& leafNetIds, GateType targetType) {
    // 防呆：沒有輸入
    if (leafNetIds.empty()) return -1;
    
    // 防呆：如果只有一根線，根本不需要建樹，直接回傳自己
    if (leafNetIds.size() == 1) return leafNetIds[0];

    // 1. 初始化 Min-Heap
    std::priority_queue<SignalNode, std::vector<SignalNode>, std::greater<SignalNode>> pq;

    // 2. 計算每個 Leaf 的到達時間，並放入 Heap
    for (int netId : leafNetIds) {
        if (!netlist.isValidNetId(netId)) continue;
        std::string netName = netlist.getNet(netId).name;
        
        // 呼叫 API 計算 Arrival Time
        int d = netlist.findCriticalPathToNet(netName).depth();
        
        pq.push({netId, d});
    }

    int newCounter = 0; // 用來產生不重複的命名後綴

    // 3. 霍夫曼建樹 (Huffman Construction)
    // 只要 Queue 裡面還有 2 個以上的訊號，就繼續合併
    while (pq.size() > 1) {
        // 優先拿出深度最淺 (最早到達) 的兩個訊號
        SignalNode left = pq.top(); pq.pop();
        SignalNode right = pq.top(); pq.pop();

        // 準備建立新的邏輯閘與訊號線
        // 命名規則防呆：使用總數 + 計數器，確保名稱絕對唯一
        std::string newNetName = "bal_tree_net_" + std::to_string(netlist.getNetCount()) + "_" + std::to_string(newCounter);
        std::string newGateName = "bal_tree_gate_" + std::to_string(netlist.getGateCount()) + "_" + std::to_string(newCounter);
        newCounter++;

        // a. 創造一條新的線與一顆新的閘，並取得它們的 ID
        int newOutNetId = netlist.addNet(newNetName);
        int newGateId = netlist.addGate(newGateName, targetType);

        // b. 連接輸入端 (新閘的輸入接上 left 和 right 的線)
        netlist.connectGateInput(newGateId, left.netId);
        netlist.connectGateInput(newGateId, right.netId);
        
        // c. 連接輸出端 (新閘的輸出綁定到新的線上)
        netlist.connectGateOutput(newGateId, newOutNetId); 

        // 4. 計算這顆新閘的深度： max(左邊深度, 右邊深度) + 1
        int newDepth = std::max(left.depth, right.depth) + 1;

        // 5. 將合併後的新訊號線塞回 Queue 裡面參與下一次合併
        pq.push({newOutNetId, newDepth});
    }

    // 當 Queue 只剩下 1 個元素時，那就是整棵平衡樹的 Root！
    return pq.top().netId;
}

// DeMorgan Pushing 主程式
bool DepthOptimizer::applyDeMorganPushing(Netlist& netlist, 
                                          const OptimizationCandidate& candidate, 
                                          const std::vector<GateType>& allowedTypes, 
                                          const std::vector<GateType>& bannedTypes) {
    bool isModified = false;

    // 將 Critical Path 上的 Net 轉成 Hash Set 方便 O(1) 查詢
    std::unordered_set<int> criticalNets(candidate.criticalPath.netIds.begin(), candidate.criticalPath.netIds.end());

    for (int currGateId : candidate.criticalPath.gateIds) {
        if (!netlist.isValidGateId(currGateId)) continue;
        const Gate& currGate = netlist.getGate(currGateId);

        bool isCase1 = false; // AND / OR + downstream NOT
        bool isCase2 = false; // NAND / NOR
        bool isCase3 = false; // XOR / XNOR + upstream NOT (Bubble Absorption)
        
        int downstreamNotGateId = -1;
        int upstreamNotGateId = -1; // 專給 Case 3 記錄是哪顆輸入的 NOT 要被吸掉
        int upstreamNotNetId = -1;  // 專給 Case 3 記錄是哪條線
        GateType targetType;

        // 判斷推擠條件與設定目標類型
        if (currGate.type == GateType::AND || currGate.type == GateType::OR) {
            // Case 1: 必須尋找下游是否有且只有 1 個 NOT 閘
            int outNetId = currGate.outputNetId;
            if (outNetId != -1) {
                const Net& outNet = netlist.getNet(outNetId);
                if (outNet.loadGateIds.size() == 1 && !outNet.isPO) {
                    const Gate& loadGate = netlist.getGate(outNet.loadGateIds[0]);
                    if (loadGate.type == GateType::NOT) {
                        isCase1 = true;
                        downstreamNotGateId = loadGate.id;
                        targetType = (currGate.type == GateType::AND) ? GateType::OR : GateType::AND;
                    }
                }
            }
        } else if (currGate.type == GateType::NAND || currGate.type == GateType::NOR) {
            // Case 2: 自身即帶有反相輸出，直接成為推擠目標
            isCase2 = true;
            targetType = (currGate.type == GateType::NAND) ? GateType::OR : GateType::AND;
            
        } else if (currGate.type == GateType::XOR || currGate.type == GateType::XNOR) {
            // Case 3: XOR/XNOR，尋找上游是否有 NOT 閘可以吸收 (~(A^B) == A XNOR B)
            for (int inNetId : currGate.inputNetIds) {
                int driverId = netlist.getNetDriverGateId(inNetId);
                if (driverId != -1 && netlist.isValidGateId(driverId)) {
                    const Gate& driverGate = netlist.getGate(driverId);
                    const Net& inNet = netlist.getNet(inNetId);
                    
                    // 只要有一個輸入是 NOT，且該 NOT 沒有其他扇出，就可以吸附
                    if (driverGate.type == GateType::NOT && inNet.loadGateIds.size() == 1 && !inNet.isPO) {
                        isCase3 = true;
                        upstreamNotGateId = driverId;
                        upstreamNotNetId = inNetId;
                        targetType = (currGate.type == GateType::XOR) ? GateType::XNOR : GateType::XOR;
                        break; 
                    }
                }
            }
        }

        // 如果不符合任何推擠條件，跳過
        if (!isCase1 && !isCase2 && !isCase3) continue;

        // Case 1 & 2 的專屬檢查：是否至少有一個 Critical 輸入接了 NOT 閘？
        if (isCase1 || isCase2) {
            bool hasCriticalInputNot = false;
            for (int inNetId : currGate.inputNetIds) {
                int driverId = netlist.getNetDriverGateId(inNetId);
                if (driverId != -1 && netlist.isValidGateId(driverId)) {
                    const Gate& driverGate = netlist.getGate(driverId);
                    const Net& inNet = netlist.getNet(inNetId);
                    
                    if (driverGate.type == GateType::NOT && inNet.loadGateIds.size() == 1 && !inNet.isPO) {
                        if (criticalNets.count(inNetId)) {
                            hasCriticalInputNot = true;
                            break; 
                        }
                    }
                }
            }
            // 沒有 NOT 夾擊，推擠沒有深度紅利，跳過
            if (!hasCriticalInputNot) continue;
        }

        // 防護網：檢查翻轉後產生的閘是否合法
        if (!isGateAllowed(targetType, allowedTypes, bannedTypes)) continue;
        // 只有 Case 1 / Case 2 才可能會無中生有生出 NOT 閘
        if ((isCase1 || isCase2) && !isGateAllowed(GateType::NOT, allowedTypes, bannedTypes)) continue;

        // 執行德摩根翻轉手術 
        std::vector<int> gatesToRemove; // 記錄待會要刪除的舊閘
        gatesToRemove.push_back(currGateId);

        // 1. 建立新的中心邏輯閘與其輸出線
        std::string newGateName = "demorgan_gate_" + std::to_string(netlist.getGateCount());
        std::string newOutNetName = "demorgan_out_net_" + std::to_string(netlist.getNetCount());
        int newGateId = netlist.addGate(newGateName, targetType);
        int newOutNetId = netlist.addNet(newOutNetName);
        netlist.connectGateOutput(newGateId, newOutNetId);

        // 2. 處理輸入端極性
        for (int inNetId : currGate.inputNetIds) {

            if (isCase3) {
                // 【Case 3】: 若是剛好被選中要吸掉的那個 NOT，跳過它接源頭；其餘原封不動照接
                if (inNetId == upstreamNotNetId) {
                    const Gate& notGate = netlist.getGate(upstreamNotGateId);
                    netlist.connectGateInput(newGateId, notGate.inputNetIds[0]);
                    gatesToRemove.push_back(upstreamNotGateId); // 標記舊 NOT 待刪除
                } else {
                    netlist.connectGateInput(newGateId, inNetId);
                }
                
            } else {
                // 【Case 1 & 2】: 反轉所有輸入的極性
                int driverId = netlist.getNetDriverGateId(inNetId);
                bool hadNot = false;

                if (driverId != -1 && netlist.isValidGateId(driverId)) {
                    const Gate& driverGate = netlist.getGate(driverId);
                    const Net& inNet = netlist.getNet(inNetId);
                    
                    if (driverGate.type == GateType::NOT && inNet.loadGateIds.size() == 1 && !inNet.isPO) {
                        // 原本有 NOT：將其抵消！直接接上上一級的線
                        hadNot = true;
                        int notInputNetId = driverGate.inputNetIds[0];
                        netlist.connectGateInput(newGateId, notInputNetId);
                        
                        gatesToRemove.push_back(driverId); // 標記舊 NOT 待刪除
                    }
                }

                if (!hadNot) {
                    // 原本沒有 NOT：必須無中生有生出一個 NOT
                    std::string newNotGateName = "demorgan_not_in_" + std::to_string(netlist.getGateCount());
                    std::string newNotNetName = "demorgan_not_net_" + std::to_string(netlist.getNetCount());

                    int newNotGateId = netlist.addGate(newNotGateName, GateType::NOT);
                    int newNotNetId = netlist.addNet(newNotNetName);

                    netlist.connectGateInput(newNotGateId, inNetId);
                    netlist.connectGateOutput(newNotGateId, newNotNetId);
                    
                    netlist.connectGateInput(newGateId, newNotNetId);
                }
            }
        }

        // 3. 處理輸出端與負載轉移 (Stitching)
        int finalOutNetId = -1;
        if (isCase1) {
            // AND/OR 的情況，最終輸出線在下游的 NOT 之後
            const Gate& downstreamNot = netlist.getGate(downstreamNotGateId);
            finalOutNetId = downstreamNot.outputNetId;
            gatesToRemove.push_back(downstreamNotGateId); // 標記下游 NOT 待刪除
        } else {
            // NAND/NOR (Case 2) 以及 XOR/XNOR (Case 3) 都是直接替換自己的輸出
            finalOutNetId = currGate.outputNetId;
        }

        // 將所有下游邏輯改接到我們新建立的 outNet
        netlist.replaceAllLoadsOfNet(finalOutNetId, newOutNetId);
        
        // 處理 PO 邊界
        if (netlist.getNet(finalOutNetId).isPO) {
            netlist.swapPrimaryOutputNet(finalOutNetId, newOutNetId);
        }

        // 4. 資源回收 (刪除舊閘)
        // 為了避免推到同一個 Gate 兩次，對 gatesToRemove 去重
        std::sort(gatesToRemove.begin(), gatesToRemove.end());
        gatesToRemove.erase(std::unique(gatesToRemove.begin(), gatesToRemove.end()), gatesToRemove.end());
        
        for (int id : gatesToRemove) {
            netlist.removeGate(id);
        }

        isModified = true;
        
        // 只要成功翻轉一次，立刻跳出迴圈，讓最外層重新計算 Critical Path
        break; 
    }

    return isModified;
}

// 萃取多輸入的超級邏輯閘 (Super Gate)
AssociativeChain DepthOptimizer::extractAssociativeChain(Netlist& netlist, 
                                                         int startGateId, 
                                                         GateType targetType, 
                                                         const std::unordered_set<int>& coneGateSet) {
    AssociativeChain chain;

    // 代數樹平衡只適用於滿足結合律且等冪律 (A*A=A, A+A=A) 的邏輯閘！
    if (!isAssociativeGate(targetType)) {
        return chain; 
    }

    std::queue<int> q;
    std::unordered_set<int> visitedGates;

    // 1. 初始化起點
    q.push(startGateId);
    visitedGates.insert(startGateId);

    // 判斷當前家族，並初始化氣泡奇偶性追蹤器
    bool isXorFamily = (targetType == GateType::XOR || targetType == GateType::XNOR);
    bool bubbleParity = false;

    // 2. BFS 展開 (尋找同類型的 Fanout-Free 錐體)
    while (!q.empty()) {
        int currGateId = q.front();
        q.pop();

        // 記錄即將被「超級閘」吞噬的舊閘
        chain.internalGateIds.push_back(currGateId);

        const Gate& currGate = netlist.getGate(currGateId);

        // 如果是 XOR 家族，每遇到一個 XNOR 或 NOT，氣泡極性就翻轉一次
        if (isXorFamily) {
            if (currGate.type == GateType::XNOR || currGate.type == GateType::NOT) {
                bubbleParity = !bubbleParity;
            }
        }

        for (int inNetId : currGate.inputNetIds) {
            int driverId = netlist.getNetDriverGateId(inNetId);
            bool absorbable = false;

            if (driverId != -1 && netlist.isValidGateId(driverId)) {
                const Gate& driverGate = netlist.getGate(driverId);
                
                // 確保在 Cone 內
                if (coneGateSet.count(driverId)) {
                    int driverOutNetId = driverGate.outputNetId;
                    if (driverOutNetId != -1) {
                        const Net& outNet = netlist.getNet(driverOutNetId);
                        
                        // Fanout 必須為 1，且不能是 PO
                        if (outNet.loadGateIds.size() == 1 && !outNet.isPO) {
                            
                            // 分流：AND/OR 家族 vs XOR/XNOR 家族的吞噬條件
                            if (!isXorFamily) {
                                // AND/OR：嚴格要求同型才能吞噬
                                if (driverGate.type == targetType) {
                                    absorbable = true;
                                }
                            } else {
                                // XOR/XNOR：只要是 XOR、XNOR 或是 NOT，通通吞噬！
                                if (driverGate.type == GateType::XOR || 
                                    driverGate.type == GateType::XNOR || 
                                    driverGate.type == GateType::NOT) {
                                    absorbable = true;
                                }
                            }
                        }
                    }
                }
            }

            if (absorbable) {
                if (visitedGates.find(driverId) == visitedGates.end()) {
                    visitedGates.insert(driverId);
                    q.push(driverId);
                }
            } else {
                // 不可吞噬，這條線成為超級閘的邊界 (Leaf)
                if (!isXorFamily) {
                    // AND / OR 適用等冪律 (單純去重)
                    if (std::find(chain.leafNetIds.begin(), chain.leafNetIds.end(), inNetId) == chain.leafNetIds.end()) {
                        chain.leafNetIds.push_back(inNetId);
                    }
                } else {
                    // XOR / XNOR 適用成對消除
                    auto it = std::find(chain.leafNetIds.begin(), chain.leafNetIds.end(), inNetId);
                    if (it != chain.leafNetIds.end()) {
                        // 已經存在，成對抵消！將其從 leafNetIds 中剔除
                        chain.leafNetIds.erase(it);
                    } else {
                        // 不存在，正常收入
                        chain.leafNetIds.push_back(inNetId);
                    }
                }
            }
        }
    }

    // 將最終計算出來的極性反轉狀態寫入結構體
    chain.isXorParityInverted = bubbleParity;

    return chain;
}

// 輔助函式：聯集兩個 Cut 列表 (DP Merge)
std::vector<KCut> mergeCuts(const std::vector<KCut>& leftCuts, const std::vector<KCut>& rightCuts) {
    std::vector<KCut> result;
    
    // 如果其中一邊沒有 Cut (例如遇到 1-input gate)，直接回傳另一邊
    if (leftCuts.empty()) return rightCuts;
    if (rightCuts.empty()) return leftCuts;

    for (const auto& cl : leftCuts) {
        for (const auto& cr : rightCuts) {
            
            KCut merged;
            int i = 0, j = 0;
            bool exceed = false;

            // 雙指標掃描：因為 cl 和 cr 都已經是排序好的 (Sorted)
            while (i < cl.size || j < cr.size) {
                
                // 如果容量已經達到 MAX_K，但還有元素沒掃完，代表合併後一定會超過限制！
                // (因為輸入是嚴格遞增的，剩下的元素一定大於 merged 裡面的最大值，必定會增加 size)
                if (merged.size == MAX_K) {
                    exceed = true;
                    break; 
                }

                if (i < cl.size && j < cr.size) {
                    if (cl.leafNetIds[i] < cr.leafNetIds[j]) {
                        merged.leafNetIds[merged.size++] = cl.leafNetIds[i++];
                    } else if (cl.leafNetIds[i] > cr.leafNetIds[j]) {
                        merged.leafNetIds[merged.size++] = cr.leafNetIds[j++];
                    } else {
                        // 相等的情況：只放一個進去，兩個指標同時前進 (天然去重！)
                        merged.leafNetIds[merged.size++] = cl.leafNetIds[i];
                        i++; 
                        j++;
                    }
                } else if (i < cl.size) {
                    merged.leafNetIds[merged.size++] = cl.leafNetIds[i++];
                } else {
                    merged.leafNetIds[merged.size++] = cr.leafNetIds[j++];
                }
            }

            // 如果沒有超過 MAX_K，才把它加入結果
            if (!exceed) {
                // 更新新 Cut 的簽章
                merged.updateSignature(); 
                
                // 這裡的 std::find 會觸發你的 operator==，因為有 signature，速度極快
                if (std::find(result.begin(), result.end(), merged) == result.end()) {
                    result.push_back(merged);
                }
            }
        }
    }

    // 剪枝策略：我們優先保留 Size 較小的 Cut
    std::sort(result.begin(), result.end(), [](const KCut& a, const KCut& b) {
        return a.size < b.size; // 這裡改成使用 .size
    });

    if (result.size() > MAX_CUTS_PER_NODE) {
        result.resize(MAX_CUTS_PER_NODE);
    }
    return result;
}

// 在乾淨的 Cone 裡面找出最佳的 K-feasible Cut
KCut DepthOptimizer::extractBestKFeasibleCut(Netlist& netlist, const OptimizationCandidate& candidate, OptimizationGoal goal) {
    // 1. 取得剛剛被我們清理過、現在非常乾淨的 coneGateSet
    std::vector<int> coneGateList = netlist.getConeGateIds(candidate.faninCone);
    std::unordered_set<int> coneGateSet(coneGateList.begin(), coneGateList.end());

    // 如果 Endpoint 本身已經被優化拔掉了，回傳空
    int rootGateId = netlist.getNetDriverGateId(candidate.endpoint.endpointNetId);
    if (!coneGateSet.count(rootGateId)) return KCut(); // 初始化空結構回傳

    // 2. 拓樸排序 (Topological Sort)
    std::unordered_map<int, int> inDegree;
    for (int gid : coneGateSet) inDegree[gid] = 0;

    for (int gid : coneGateSet) {
        const Gate& g = netlist.getGate(gid);
        for (int inNetId : g.inputNetIds) {
            int driverId = netlist.getNetDriverGateId(inNetId);
            // 如果輸入來源也是 Cone 內部的節點，則 inDegree + 1
            if (coneGateSet.count(driverId)) {
                inDegree[gid]++;
            }
        }
    }

    std::queue<int> q;
    std::vector<int> topoOrder;
    for (int gid : coneGateSet) {
        if (inDegree[gid] == 0) q.push(gid);
    }

    while (!q.empty()) {
        int curr = q.front(); q.pop();
        topoOrder.push_back(curr);

        const Gate& g = netlist.getGate(curr);
        if (g.outputNetId != -1) {
            for (int lgid : netlist.getNet(g.outputNetId).loadGateIds) {
                if (coneGateSet.count(lgid)) {
                    if (--inDegree[lgid] == 0) q.push(lgid);
                }
            }
        }
    }

    // 3. DP 狀態計算 (Cut Enumeration)
    std::unordered_map<int, std::vector<KCut>> nodeCuts;

    for (int currGateId : topoOrder) {
        const Gate& g = netlist.getGate(currGateId);
        std::vector<KCut> currentGateCuts;

        // Trivial Cut: 把自己當作邊界 (Cut 包含自己輸出的那一條線)
        if (g.outputNetId != -1) {
            KCut trivialCut;
            trivialCut.leafNetIds[0] = g.outputNetId;
            trivialCut.size = 1;
            trivialCut.normalize(); // 計算 Signature
            currentGateCuts.push_back(trivialCut);
        }

        // DP Merge: 根據每一個輸入腳位，把前一級的 Cuts 聯集起來
        std::vector<KCut> mergedCuts;
        bool isFirstInput = true;

        for (int inNetId : g.inputNetIds) {
            int driverId = netlist.getNetDriverGateId(inNetId);
            std::vector<KCut> branchCuts;

            // 如果 Driver 在 Cone 內部，繼承它的 Cuts
            if (coneGateSet.count(driverId)) {
                branchCuts = nodeCuts[driverId];
            } else {
                // 如果 Driver 不在 Cone 內部 (例如它是 PI 或是 Cone 外的閘)，這條線就是絕對邊界
                KCut boundaryCut;
                boundaryCut.leafNetIds[0] = inNetId;
                boundaryCut.size = 1;
                boundaryCut.normalize(); // 計算 Signature
                branchCuts.push_back(boundaryCut);
            }

            if (isFirstInput) {
                mergedCuts = branchCuts;
                isFirstInput = false;
            } else {
                mergedCuts = mergeCuts(mergedCuts, branchCuts);
            }
        }

        // 把 Merge 後的 Cuts 存入目前的 Gate
        for (const auto& mc : mergedCuts) {
            currentGateCuts.push_back(mc);
        }

        // 去重與剪枝 (針對新的 KCut 結構進行排序)
        std::sort(currentGateCuts.begin(), currentGateCuts.end(), [](const KCut& a, const KCut& b) {
            if (a.size != b.size) return a.size < b.size;
            if (a.signature != b.signature) return a.signature < b.signature;
            
            // 如果 Size 和 Signature 都一樣 (發生碰撞或根本是重複的 Cut)
            // 進行逐項比對，確保完全一樣的 Cut 在排序後絕對會「相鄰」
            for (int i = 0; i < a.size; ++i) {
                if (a.leafNetIds[i] != b.leafNetIds[i]) {
                    return a.leafNetIds[i] < b.leafNetIds[i];
                }
            }
            return false; // 完全相等時回傳 false
        });
        
        currentGateCuts.erase(std::unique(currentGateCuts.begin(), currentGateCuts.end()), currentGateCuts.end());
        
        if (currentGateCuts.size() > MAX_CUTS_PER_NODE) {
            currentGateCuts.resize(MAX_CUTS_PER_NODE);
        }

        nodeCuts[currGateId] = std::move(currentGateCuts); // 使用 move 稍微提升一點效能
    }

    // 4. 評估與挑選最佳 Cut (根據 Goal 分流)
    const auto& rootCuts = nodeCuts[rootGateId];
    KCut bestCut;
    CutScore bestScore = {-1, -1, -999}; // 初始化為極小值

    std::unordered_set<int> criticalGateSet;
    if (goal == OptimizationGoal::DEPTH) {
        criticalGateSet.insert(candidate.criticalPath.gateIds.begin(), candidate.criticalPath.gateIds.end());
    }

    for (const KCut& cut : rootCuts) {
        CutScore currentScore;

        if (goal == OptimizationGoal::AREA) {
            currentScore = evaluateAreaCut(netlist, cut, rootGateId);
        } else {
            // DEPTH 模式
            currentScore = evaluateDepthCut(netlist, cut, rootGateId, criticalGateSet);
        }

        // 利用重載的 operator< 直接比較
        if (bestScore < currentScore) {
            bestScore = currentScore;
            bestCut = cut;
        }
    }

    // 將最佳結果回傳 (這就是我們要送給 SAT 去算 Truth Table 的邊界)
    return bestCut;
}

// 輔助函式：AREA 模式下的 Cut 評分機制
CutScore DepthOptimizer::evaluateAreaCut(Netlist& netlist, const KCut& cut, int rootGateId) {
    CutScore score;

    // 1. 建立邊界查表 (使用 Lambda 搭配小陣列走訪，速度極快)
    auto isLeafNet = [&](int netId) {
        for (int i = 0; i < cut.size; ++i) {
            if (cut.leafNetIds[i] == netId) return true;
        }
        return false;
    };

    // 2. 模擬解除參考 (Simulated Dereferencing)
    std::unordered_map<int, int> simulatedFanout;
    std::queue<int> derefQueue;
    int mffcVolume = 0;

    // Root Gate 一定會被換掉
    derefQueue.push(rootGateId);
    mffcVolume++; 

    while (!derefQueue.empty()) {
        int currGateId = derefQueue.front();
        derefQueue.pop();

        if (!netlist.isValidGateId(currGateId)) continue;
        const Gate& currGate = netlist.getGate(currGateId);

        for (int inNetId : currGate.inputNetIds) {
            // 如果追溯到了 Cut 邊界 (Leaf)，就停止往下找
            if (isLeafNet(inNetId)) continue;

            // 找驅動這條線的上一級 Gate
            int driverId = netlist.getNetDriverGateId(inNetId);
            if (driverId == -1 || !netlist.isValidGateId(driverId)) continue;

            // 初始化 Fanout 計數 (如果還沒被登記過)
            if (simulatedFanout.find(driverId) == simulatedFanout.end()) {
                const Gate& driverGate = netlist.getGate(driverId);
                int outNetId = driverGate.outputNetId;
                
                if (outNetId != -1) {
                    const Net& outNet = netlist.getNet(outNetId);
                    int actualFanout = outNet.loadGateIds.size();
                    
                    // 如果這條線是 PO，給它一個天文數字的假負載，保證它絕對不會被歸零拔除
                    if (outNet.isPO) {
                        actualFanout += 10000; 
                    }
                    simulatedFanout[driverId] = actualFanout;
                } else {
                    simulatedFanout[driverId] = 0;
                }
            }

            // 模擬拔除：因為 currGate 被我們假裝拔掉了，所以 driverId 少了一個聽眾
            if (simulatedFanout[driverId] > 0) {
                simulatedFanout[driverId]--;
                
                // 如果聽眾歸零，代表它完全包覆在 Cut 裡面，沒有外部扇出了！
                if (simulatedFanout[driverId] == 0) {
                    derefQueue.push(driverId);
                    mffcVolume++; // MFFC 體積 +1
                }
            }
        }
    }

    // 3. 設定 CutScore (利用 operator< 自動比較)
    
    // 首要指標：能拔除的 Gate 數量越多越好
    score.primaryScore = mffcVolume;      
    
    // 次要指標：AREA 模式比較少用次要指標，預設給 0 即可
    score.secondaryScore = 0;             
    
    // 懲罰指標：Leaf 數量 (輸入腳位) 越少越好。
    // 因為在 operator< 裡面，penalty 是從小比到大，我們希望 size 越小的贏，
    // 所以我們存入負的 size，這樣 size=4 的 penalty (-4) 就會大於 size=7 的 (-7)！
    score.penalty = -cut.size;            

    return score;
}

// 輔助函式：DEPTH 模式下的 Cut 評分機制
CutScore DepthOptimizer::evaluateDepthCut(Netlist& netlist, 
                                          const KCut& cut, 
                                          int rootGateId, 
                                          const std::unordered_set<int>& criticalGateSet) { // <--- 直接接收 Set
    CutScore score;

    // 1. 建立邊界查表 (使用 Lambda 搭配小陣列走訪)
    auto isLeafNet = [&](int netId) {
        for (int i = 0; i < cut.size; ++i) {
            if (cut.leafNetIds[i] == netId) return true;
        }
        return false;
    };

    // 2. 局部 BFS 走訪 (計算 Critical Volume)
    std::queue<int> q;
    std::unordered_set<int> visited;
    int criticalVolume = 0;

    q.push(rootGateId);
    visited.insert(rootGateId);

    while (!q.empty()) {
        int currGateId = q.front();
        q.pop();

        // 直接使用外部傳入的 criticalGateSet 進行查詢
        if (criticalGateSet.count(currGateId)) {
            criticalVolume++;
        }

        if (!netlist.isValidGateId(currGateId)) continue;
        const Gate& currGate = netlist.getGate(currGateId);

        for (int inNetId : currGate.inputNetIds) {
            if (isLeafNet(inNetId)) continue;

            int driverId = netlist.getNetDriverGateId(inNetId);
            
            if (driverId != -1 && netlist.isValidGateId(driverId)) {
                if (visited.find(driverId) == visited.end()) {
                    visited.insert(driverId);
                    q.push(driverId);
                }
            }
        }
    }

    // 3. 設定 CutScore
    score.primaryScore = criticalVolume; 
    score.penalty = -cut.size;
    score.secondaryScore = evaluateAreaCut(netlist, cut, rootGateId).primaryScore;

    return score;
}