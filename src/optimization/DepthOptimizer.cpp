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
/*OptimizationResult DepthOptimizer::reduceDepth(Netlist& netlist, 
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
    const int MAX_ITERATIONS = 100; // 防呆：避免未預期的無窮迴圈

    if (verbose) {
        std::cout << "\n[DepthOptimizer] Start reducing depth for endpoint: " 
                  << candidate.endpoint.endpointName << "\n";
    }

    TechMapper mapper;

    // 收斂迴圈 (Convergence Loop)
    while (true) {
        iterationCount++;

        if (iterationCount > MAX_ITERATIONS) {
            if (verbose) std::cout << "  -> [Warning] Reached max iterations (" << MAX_ITERATIONS << "). Forcing exit to prevent infinite loop.\n";
            break; 
        }

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
        if (config.enableConeResynthesis && !passModified) {
            
            // 5A. 執行極限深度壓縮 (Exact Synthesis - Depth)
            bool depthCompressed = runExactDepthPass(netlist, mapper, currentCandidate, allowedTypes, bannedTypes, verbose);
            
            if (depthCompressed) {
                passModified = true;
                if (verbose) std::cout << "     * [Exact Synthesis] Depth pass successfully compressed logic.\n";

                // 【關鍵防護罩】：重新計算「全域」的最新深度，並作為不可逾越的鐵律！
                OptimizationCandidate refreshedCandidate = netlist.buildOptimizationCandidate(currentCandidate.endpoint, currentCandidate.targetDepth);
                int newlyAchievedDepth = refreshedCandidate.criticalPath.depth();
                
                if (verbose) std::cout << "     * [Constraint Lock] Area Recovery is now strictly bounded to Depth <= " << newlyAchievedDepth << "\n";

                // 5B. 在嚴格約束下，執行面積回收 (Exact Synthesis - Area)
                bool areaRecovered = runExactAreaPass(netlist, mapper, refreshedCandidate, newlyAchievedDepth, allowedTypes, bannedTypes, verbose);
                
                if (areaRecovered && verbose) {
                    std::cout << "     * [Exact Synthesis] Area Recovery pass successfully reclaimed gates without violating depth constraint.\n";
                }
            } 
            else {
                // 如果深度壓不下去，我們還是可以嘗試做一次純粹的面積回收 (約束為當前深度)
                int currentDepth = currentCandidate.criticalPath.depth();
                if (runExactAreaPass(netlist, mapper, currentCandidate, currentDepth, allowedTypes, bannedTypes, verbose)) {
                    passModified = true;
                    if (verbose) std::cout << "     * [Exact Synthesis] Area Recovery pass improved area at current depth.\n";
                }
            }
        }

        if (passModified) {
            globalModified = true;
            continue; // 拓樸已改變，立刻跳回 while 開頭，重新抓取 Critical Path！
        } else {
            break; // 所有 Pass 都無事可做，電路已完美收斂，跳出迴圈
        }
    }

    // 步驟 6. 全域死碼消除 (Global Dead Code Elimination)
    // 迴圈收斂後，把掉出邏輯錐、失去連線的孤魂野鬼一次清乾淨
    if (globalModified) {
        int removedZombies = netlist.removeDanglingLogic();
        if (verbose && removedZombies > 0) {
            std::cout << "  -> [Global DCE] Cleaned up " << removedZombies << " dangling logic gates.\n";
        }
    }

    // 步驟 7. 結算成效與 Rollback 決策 (Resolution)
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
}*/

// K-feasible Cut 精確合成 (One-Shot Pass)
bool DepthOptimizer::runExactDepthPass(Netlist& netlist, 
                                       TechMapper& mapper, 
                                       OptimizationCandidate& candidate, 
                                       const std::vector<GateType>& allowedTypes, 
                                       const std::vector<GateType>& bannedTypes, 
                                       bool verbose) {

    int driverId = netlist.getNetDriverGateId(candidate.endpoint.endpointNetId);
    if (driverId == -1 || netlist.isGateRemoved(driverId)) return false;

    KCut depthCut = extractBestKFeasibleCut(netlist, candidate, OptimizationGoal::DEPTH);
    if (depthCut.size == 0) return false;

    std::unordered_set<int> targetCone;
    int actualN = 0; // 準備接收真實的 N

    // 傳入 actualN 參考
    std::shared_ptr<PatternNode> depthLhs = extractLhsFromCut(netlist, driverId, depthCut, targetCone, actualN);

    // 如果 N 超過 MAX_K，計算真值表會非常昂貴，直接跳過
    if (actualN > MAX_K) {
        if (verbose) {
            std::cout << "     -> [Skip] Actual N (" << actualN << ") exceeds exact synthesis limit (" << MAX_K << "). Skipping Depth Pass.\n";
        }
        return false; 
    }

    /*std::cout << "[Debug] Extracted LHS Pattern Depth: " 
              << mapper.calculateDepth(depthLhs) << ", Gate Count: " << targetCone.size() << "\n";*/

    TechMapReport depthReport = mapper.optimizePattern(
        netlist, depthLhs, OptimizationGoal::DEPTH, targetCone, verbose,  // 將真實的 N 傳遞給 optimizePattern
        0, -1, allowedTypes, bannedTypes 
    );

    return (depthReport.status == TechMapStatus::SUCCESS && depthReport.message.find("No changes made") == std::string::npos);
}

bool DepthOptimizer::runExactAreaPass(Netlist& netlist, 
                                      TechMapper& mapper, 
                                      OptimizationCandidate& candidate, 
                                      int strictDepthLimit, 
                                      const std::vector<GateType>& allowedTypes, 
                                      const std::vector<GateType>& bannedTypes, 
                                      bool verbose) {

    int driverId = netlist.getNetDriverGateId(candidate.endpoint.endpointNetId);
    if (driverId == -1 || netlist.isGateRemoved(driverId)) return false;

    KCut areaCut = extractBestKFeasibleCut(netlist, candidate, OptimizationGoal::AREA);
    if (areaCut.size == 0) return false;

    std::unordered_set<int> targetCone;
    int actualN = 0; // 接收真實的 N

    // 傳入 actualN 參考
    std::shared_ptr<PatternNode> areaLhs = extractLhsFromCut(netlist, driverId, areaCut, targetCone, actualN);

    // 記憶體與效能防護網
    if (actualN > MAX_K) {
        if (verbose) {
            std::cout << "     -> [Skip] Actual N (" << actualN << ") exceeds exact synthesis limit (" << MAX_K << "). Skipping Area Pass.\n";
        }
        return false; 
    }

    TechMapReport areaReport = mapper.optimizePattern(
        netlist, areaLhs, OptimizationGoal::AREA, targetCone, verbose,  // 將真實的 N 傳遞給 optimizePattern
        0, strictDepthLimit,
        allowedTypes, bannedTypes
    );

    return (areaReport.status == TechMapStatus::SUCCESS && areaReport.message.find("No changes made") == std::string::npos);
}


// 輔助函式：給定 Root 與 Cut 邊界，從 Netlist 走訪並建立 PatternNode (AST)，同時收集 TargetCone
std::shared_ptr<PatternNode> DepthOptimizer::extractLhsFromCut(Netlist& netlist, 
                                                               int rootGateId, 
                                                               const KCut& cut, 
                                                               std::unordered_set<int>& outTargetCone,
                                                               int& outActualN) { // 用來回傳真實的 N
    outTargetCone.clear();
    
    // 將 Cut 的 Leaf Nets 轉為查表，方便 O(1) 判斷
    std::unordered_map<int, int> leafNetToIndex;
    for (int i = 0; i < cut.size; ++i) {
        leafNetToIndex[cut.leafNetIds[i]] = i; // 記錄這個 Net 對應到第幾個 PI
    }

    // 遞迴建構 Pattern 的 Lambda
    std::unordered_map<int, std::shared_ptr<PatternNode>> visitedGates;

    std::function<std::shared_ptr<PatternNode>(int)> buildAst = [&](int gateId) -> std::shared_ptr<PatternNode> {
        if (visitedGates.count(gateId)) return visitedGates[gateId];

        const Gate& g = netlist.getGate(gateId);
        outTargetCone.insert(gateId); // 將走訪到的 Gate 加入 TargetCone

        std::vector<std::shared_ptr<PatternNode>> childNodes;

        for (int inNetId : g.inputNetIds) {
            // 情況 A：如果這條輸入線是 Cut 的邊界 (Leaf)，停止往下，建立 PI 節點
            if (leafNetToIndex.count(inNetId)) {
                childNodes.push_back(PatternNode::makePI(leafNetToIndex[inNetId]));
            } 
            // 情況 B：常數線
            else if (netlist.getNet(inNetId).isConst) {
                int val = netlist.getNet(inNetId).constVal;
                childNodes.push_back(val == 1 ? PatternNode::Const1() : PatternNode::Const0());
            } 
            // 情況 C：內部線，繼續往下遞迴尋找 Driver Gate
            else {
                int driverId = netlist.getNetDriverGateId(inNetId);
                bool treatAsPI = false;

                if (driverId != -1 && netlist.isValidGateId(driverId)) {
                    // 檢查上一級的 GateType 
                    GateType driverType = netlist.getGate(driverId).type;
                    
                    if (driverType == GateType::DFF) {
                        treatAsPI = true; // 遇到 DFF，強制踩煞車，視為組合邏輯的邊界
                    } else {
                        // 是一般的組合邏輯閘，安全繼續往下長
                        childNodes.push_back(buildAst(driverId));
                    }
                } else {
                    treatAsPI = true; // 找不到 Driver (如系統 PI 或死線)，強制視為邊界
                }

                // 如果判定這條線是新的邊界 (PI)
                if (treatAsPI) {
                    // 安全檢查：如果這個 inNetId 剛剛還沒被註冊過，才給它新的 index
                    if (leafNetToIndex.find(inNetId) == leafNetToIndex.end()) {
                        leafNetToIndex[inNetId] = leafNetToIndex.size();
                    }
                    childNodes.push_back(PatternNode::makePI(leafNetToIndex[inNetId])); 
                }
            }
        }

        // 建立當前的 Gate 節點
        auto node = std::make_shared<PatternNode>(PatternNode{NodeType::GATE, g.type, -1, childNodes});
        visitedGates[gateId] = node;
        return node;
    };

    // 1. 先執行 AST 樹的建立。在這過程中，leafNetToIndex 可能會被動態加入新的變數。
    auto astRoot = buildAst(rootGateId);

    // 2. 將最終實際被使用的 PI 總數寫入 outActualN，回傳給外層！
    outActualN = leafNetToIndex.size();

    return astRoot;
}

// 輔助函式：聯集兩個 Cut 列表 (DP Merge)
std::vector<KCut> mergeCuts(const std::vector<KCut>& leftCuts, const std::vector<KCut>& rightCuts) {
    std::vector<KCut> result;
    
    // 如果其中一邊沒有 Cut (例如遇到 1-input gate)，直接回傳另一邊
    if (leftCuts.empty()) return rightCuts;
    if (rightCuts.empty()) return leftCuts;

    for (const auto& cl : leftCuts) {
        for (const auto& cr : rightCuts) {
            
            // 利用 Signature 極速判斷合併後是否會爆表
            uint64_t mergedSig = cl.signature | cr.signature;
            
            // 如果合併後的獨立 bit 數量已經 > MAX_K，那真實的 Cut size 絕對大於 MAX_K！
            // 直接放棄，避開耗時的陣列操作！
            if (__builtin_popcountll(mergedSig) > MAX_K) {
                continue; 
            }

            KCut merged;
            // O(1) 賦予 Signature，不需重新計算！
            merged.signature = mergedSig; 

            int i = 0, j = 0;
            bool exceed = false;

            // 雙指標掃描：因為 cl 和 cr 都已經是排序好的 (Sorted)
            while (i < cl.size || j < cr.size) {
                
                // 提早中斷防呆
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
                // 這裡的 std::find 會觸發你的 operator==，因為有 signature，速度極快
                if (std::find(result.begin(), result.end(), merged) == result.end()) {
                    result.push_back(merged);
                }
            }
        }
    }

    // 剪枝策略：我們優先保留 Size 較小的 Cut
    std::sort(result.begin(), result.end(), [](const KCut& a, const KCut& b) {
        if (a.size != b.size) return a.size < b.size;
        // 如果 size 相同，加入 signature 當作穩定的第二排序條件
        return a.signature < b.signature; 
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

        // Subsumption Pruning (子集剪枝)
        std::vector<KCut> filteredCuts;
        filteredCuts.reserve(currentGateCuts.size());

        for (const auto& cut : currentGateCuts) {
            bool isSubsumed = false;
            for (const auto& keptCut : filteredCuts) {
                // 利用 Signature 進行 O(1) 的極速超集檢查：
                // 如果 keptCut 的 bitmask 是 cut 的子集，那 cut「有可能」被 subsume
                if ((keptCut.signature & cut.signature) == keptCut.signature) {
                    
                    // Signature 命中，再進行陣列的嚴格比對 (確保是真的子集，而非 Hash 碰撞)
                    bool allMatch = true;
                    for (int i = 0; i < keptCut.size; ++i) {
                        bool found = false;
                        for (int j = 0; j < cut.size; ++j) {
                            if (keptCut.leafNetIds[i] == cut.leafNetIds[j]) { found = true; break; }
                        }
                        if (!found) { allMatch = false; break; }
                    }
                    
                    if (allMatch) {
                        isSubsumed = true;
                        break; // 這個 cut 被包含，可以丟棄了！
                    }
                }
            }
            if (!isSubsumed) {
                filteredCuts.push_back(cut);
            }
        }

        // 截斷超出限制的 Cuts
        if (filteredCuts.size() > MAX_CUTS_PER_NODE) {
            filteredCuts.resize(MAX_CUTS_PER_NODE);
        }
        nodeCuts[currGateId] = std::move(filteredCuts);
    }

    // 4. 評估與挑選最佳 Cut (根據 Goal 分流)
    const auto& rootCuts = nodeCuts[rootGateId];
    KCut bestCut;
    CutScore bestScore = {-1, -1, -999}; // 初始化為極小值
    bool foundValidCut = false;

    // 取得 Root Gate 的輸出線 ID
    int rootOutNetId = netlist.getGate(rootGateId).outputNetId;

    std::unordered_set<int> criticalGateSet;
    if (goal == OptimizationGoal::DEPTH) {
        criticalGateSet.insert(candidate.criticalPath.gateIds.begin(), candidate.criticalPath.gateIds.end());
    }

    for (const KCut& cut : rootCuts) {

        // 絕不能選 Root 自己的 Trivial Cut！
        // 否則 extractLhsFromCut 會直接貫穿整個電路！
        if (cut.size == 1 && cut.leafNetIds[0] == rootOutNetId) {
            continue; 
        }

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
            foundValidCut = true;
        }
    }

    // 防呆：如果真的找不到任何合法的 Cut (例如全部都超過 K 限制)
    if (!foundValidCut) {
        return KCut(); // 回傳空，讓外層跳過這個 Endpoint
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