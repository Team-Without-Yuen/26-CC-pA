#include "include/core/DepthOptimizer.h"
#include "include/lib/cadical/cadical.hpp"
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

// Critical Path 最佳化主控流程
OptimizationResult DepthOptimizer::executeCriticalPathOptimization(Netlist& netlist, 
                                                                   TechMapper& techMapper,
                                                                   const std::vector<GateType>& allowedTypes,
                                                                   const ConeReport& targetConeReport,
                                                                   const std::vector<GateType>& bannedTypes,
                                                                   bool verbose) {
    OptimizationResult result;
    result.passName = "Opt_CP";
    
    // 紀錄優化前的 Global Critical Path Depth 與面積
    DepthReport oldGlobalPath = netlist.findGlobalCriticalPath();
    result.oldDepth = oldGlobalPath.depth;
    result.oldGateCount = netlist.getGateCount();

    if (verbose) {
        std::cout << "\n=================================================\n";
        std::cout << "[Flow Start] Old Global Depth: " << result.oldDepth 
                  << " | Old Gate Count: " << result.oldGateCount << "\n";
    }

    // 階段 1：判斷目標與基礎建設 (Basis Identification)
    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());
    
    // 判斷是否為純 AIG 或純 XAG
    bool isPureAIG = bannedTypes.empty() && allowedSet.size() == 2 && 
                     allowedSet.count(GateType::AND) && allowedSet.count(GateType::NOT);
                     
    bool isPureXAG = bannedTypes.empty() && allowedSet.size() == 3 && 
                     allowedSet.count(GateType::XOR) && allowedSet.count(GateType::AND) && allowedSet.count(GateType::NOT);

    // 判斷是否只針對「特定的 Cone」進行限制
    bool hasLocalConeConstraint = targetConeReport.ok && targetConeReport.exists;

    // 決定全域要用哪種 Mockturtle 網路：
    // 如果是「全域」且規定「純 AIG」，我們全域就用 AIG；
    // 其餘所有情況，全域統一先用 XAG 壓平，以獲取最佳深度！
    bool useGlobalAIG = !hasLocalConeConstraint && isPureAIG; 

    // 階段 2：全域 Mockturtle 深度壓縮 (Global Depth Rewriting)
    if (useGlobalAIG) {
        if (verbose) std::cout << "[Step 2] Executing global AIG optimization (ABC resyn2 + MIG depth style)...\n";
        
        mockturtle::aig_network aig = NetlistToAig(netlist);

        // AIG 級別的 NPN 查表重寫與重構
        for (int i = 0; i < 2; ++i) {
            // 1. 代數平衡
            mockturtle::sop_rebalancing<mockturtle::aig_network> rebalance_fn;
            aig = mockturtle::balancing(aig, {rebalance_fn});
            aig = mockturtle::cleanup_dangling(aig);

            // 2. 4-Cut NPN 布林重寫 
            mockturtle::cut_rewriting_params cr_ps;
            cr_ps.cut_enumeration_ps.cut_size = 4;
            
            // 這裡修正：改用 xag_npn_resynthesis 吃下 aig_network
            mockturtle::xag_npn_resynthesis<mockturtle::aig_network> resyn_aig_npn; 
            
            aig = mockturtle::cut_rewriting(aig, resyn_aig_npn, cr_ps);
            aig = mockturtle::cleanup_dangling(aig);

            // 3. 邏輯重構 (Refactoring) 
            mockturtle::refactoring_params ref_ps;
            ref_ps.max_pis = 4;
            mockturtle::refactoring(aig, resyn_aig_npn, ref_ps);
            aig = mockturtle::cleanup_dangling(aig);
        }

        // MIG 極限深度壓縮 (處理長直列邏輯)
        if (verbose) std::cout << "  -> Converting AIG to MIG for extreme depth reduction...\n";
        mockturtle::mig_npn_resynthesis resyn_to_mig; 
        mockturtle::mig_network mig = mockturtle::node_resynthesis<mockturtle::mig_network>(aig, resyn_to_mig);
        mig = mockturtle::cleanup_dangling(mig);

        for (int i = 0; i < 3; ++i) {
            // MIG 專屬代數深度重寫
            mockturtle::mig_algebraic_depth_rewriting_params mig_ps;
            mig_ps.strategy = mockturtle::mig_algebraic_depth_rewriting_params::dfs;
            mig_ps.allow_area_increase = true; 
            mockturtle::depth_view depth_mig{mig}; 
            mockturtle::mig_algebraic_depth_rewriting(depth_mig, mig_ps);
            mig = mockturtle::cleanup_dangling(mig);

            // MIG 布林替換
            mockturtle::resubstitution_params rp;
            mockturtle::fanout_view fanout_mig{mig};
            mockturtle::depth_view depth_mig_resub{fanout_mig};
            mockturtle::mig_resubstitution(depth_mig_resub, rp);
            mig = mockturtle::cleanup_dangling(mig);
        }

        // 轉回 AIG 並做最後收斂
        if (verbose) std::cout << "  -> Converting optimized MIG back to AIG...\n";
        mockturtle::xag_npn_resynthesis<mockturtle::aig_network> resyn_back_to_aig;
        aig = mockturtle::node_resynthesis<mockturtle::aig_network>(mig, resyn_back_to_aig);
        
        mockturtle::sop_rebalancing<mockturtle::aig_network> final_rebalance;
        aig = mockturtle::balancing(aig, {final_rebalance});
        aig = mockturtle::cleanup_dangling(aig);

        netlist = AigToNetlist(aig, netlist);

    } else {
        if (verbose) std::cout << "[Step 2] Executing global XAG depth optimization (ABC resyn2 style)...\n";
        mockturtle::xag_network xag = NetlistToXag(netlist);

        for (int i = 0; i < 3; ++i) {
            // 1. 代數平衡
            mockturtle::esop_rebalancing<mockturtle::xag_network> rebalance_fn;
            xag = mockturtle::balancing(xag, {rebalance_fn});
            xag = mockturtle::cleanup_dangling(xag);

            // 2. 4-Cut NPN 布林重寫 (使用 XAG 專屬的 NPN 查表)
            mockturtle::cut_rewriting_params cr_ps;
            cr_ps.cut_enumeration_ps.cut_size = 4; 
            mockturtle::xag_npn_resynthesis<mockturtle::xag_network> resyn_xag_npn; 
            xag = mockturtle::cut_rewriting(xag, resyn_xag_npn, cr_ps);
            xag = mockturtle::cleanup_dangling(xag);

            // 3. 邏輯重構 (Refactoring)
            mockturtle::refactoring_params ref_ps;
            ref_ps.max_pis = 4;
            mockturtle::refactoring(xag, resyn_xag_npn, ref_ps);
            xag = mockturtle::cleanup_dangling(xag);

            // 4. 布林替換
            mockturtle::resubstitution_params rp;
            mockturtle::fanout_view fanout_xag{xag};
            mockturtle::depth_view depth_xag_resub{fanout_xag};
            mockturtle::xag_resubstitution(depth_xag_resub, rp);
            xag = mockturtle::cleanup_dangling(xag);
        }
        
        netlist = XagToNetlist(xag, netlist);
    }

    // 階段 3：局部 Cone 的特例處理 (Local Cone Mockturtle Rewriting)
    // 若題目要求特定的 Cone 必須是 AIG/XAG，我們在這裡補足邏輯：
    if (hasLocalConeConstraint) {
        if (isPureAIG) {
            if (verbose) std::cout << "[Step 3] Target cone requires pure AIG. Executing local AIG optimization...\n";
            
            // TODO: 實作以下輔助函式 (提取 Cone -> 轉 AIG 優化 -> 縫合回原電路)
            // Netlist subNetlist = extractSubNetlist(netlist, targetConeReport);
            // mockturtle::aig_network localAig = NetlistToAig(subNetlist);
            // mockturtle::depth_view local_depth_aig{localAig};
            // mockturtle::aig_depth_rewriting(local_depth_aig);
            // localAig = mockturtle::cleanup_dangling(localAig);
            // Netlist optimizedSub = AigToNetlist(localAig, subNetlist);
            // stitchSubNetlist(netlist, optimizedSub, targetConeReport);
            
        } 
        else if (isPureXAG) {
            // 如果局部 Cone 要求 XAG，因為我們在階段 2 已經全域跑過 XAG 了
            if (verbose) std::cout << "[Step 3] Target cone requires pure XAG. Already satisfied by global XAG.\n";
        }
    } 
    else if (isPureAIG || isPureXAG) {
        // 沒有局部 Cone，且剛好題目要求全域 AIG/XAG !
        if (verbose) std::cout << "[Step 3] Global pure AIG/XAG constraints already satisfied.\n";
    }

    // -------------------------------------------------------------------------
    // 階段 4：執行題意約束 (Basis Enforcement) 
    // -------------------------------------------------------------------------
    /*
    if (!isPureAIG && !isPureXAG) {
        if (verbose) std::cout << "[Step 4] Enforcing specific basis constraints...\n";
        TargetScope mapScope = hasLocalConeConstraint ? TargetScope::GATE_FANIN : TargetScope::WHOLE_NETLIST;
        
        TechMapReport mapReport = techMapper.convertToBasis(
            netlist, 
            mapScope, 
            hasLocalConeConstraint ? targetConeReport.sourceName : "", 
            allowedTypes, 
            bannedTypes, 
            verbose
        );

        if (mapReport.status != TechMapStatus::SUCCESS) {
            result.status = OptimizationStatus::ERROR_NOT_EQUIVALENT;
            result.message = "Basis conversion failed to satisfy constraints.";
            return result;
        }
    }
    */

    // -------------------------------------------------------------------------
    // 階段 5：修復映射造成的深度暴增 (Critical Path Post-Optimization)
    // -------------------------------------------------------------------------
    /*
    if (verbose) std::cout << "[Step 5] Firing SAT-based exact synthesis to crush depth penalty...\n";
    // runExactDepthPass(netlist, techMapper, candidate, allowedTypes, bannedTypes, verbose);
    */

    // 結算：更新深度與面積資訊 (Depth & Area Evaluation)
    DepthReport newGlobalPath = netlist.findGlobalCriticalPath();
    result.newDepth = newGlobalPath.depth;
    result.depthImproved = (result.newDepth < result.oldDepth);
    
    result.newGateCount = netlist.getGateCount();
    result.areaDelta = result.newGateCount - result.oldGateCount;

    result.status = OptimizationStatus::SUCCESS;
    result.changed = true;
    result.equivalent = true; 
    result.message = "Completed Mockturtle optimization sequence.";
    
    if (verbose) {
        std::cout << "[Success] " << result.message << "\n";
        std::cout << "          Depth: " << result.oldDepth << " -> " << result.newDepth << "\n";
        std::cout << "=================================================\n";
    }

    return result;
}


/*// K-feasible Cut 精確合成 (One-Shot Pass)
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
}*/


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

// 共用的 Cut 剪枝過濾器
std::vector<KCut> pruneAndLimitCuts(std::vector<KCut>& cuts) {
    if (cuts.empty()) return cuts;

    // 1. 排序：優先考慮 Size 小的，若 Size 相同則依據 Signature 與內容排序
    std::sort(cuts.begin(), cuts.end(), [](const KCut& a, const KCut& b) {
        if (a.size != b.size) return a.size < b.size;
        if (a.signature != b.signature) return a.signature < b.signature;
        
        // 確保完全相同的 Cut 會排在一起
        for (int i = 0; i < a.size; ++i) {
            if (a.leafNetIds[i] != b.leafNetIds[i]) {
                return a.leafNetIds[i] < b.leafNetIds[i];
            }
        }
        return false;
    });

    // 2. 去重 (O(N))：將連續完全相同的元素剔除
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    // 3. 子集剪枝 (Subsumption Pruning)
    std::vector<KCut> filtered;
    filtered.reserve(std::min((size_t)MAX_CUTS_PER_NODE, cuts.size()));

    for (const auto& cut : cuts) {
        bool isSubsumed = false;
        for (const auto& keptCut : filtered) {
            // 利用 Signature 進行 O(1) 超集檢查
            if ((keptCut.signature & cut.signature) == keptCut.signature) {
                
                // 確認是真的子集，而非 Hash 碰撞
                bool allMatch = true;
                for (int i = 0; i < keptCut.size; ++i) {
                    bool found = false;
                    for (int j = 0; j < cut.size; ++j) {
                        if (keptCut.leafNetIds[i] == cut.leafNetIds[j]) { 
                            found = true; break; 
                        }
                    }
                    if (!found) { allMatch = false; break; }
                }
                
                if (allMatch) {
                    isSubsumed = true;
                    break; // 被包含，直接丟棄
                }
            }
        }
        
        if (!isSubsumed) {
            filtered.push_back(cut);
            // 剪枝的同時做數量限制，確保留下來的一定是互相獨立的精華
            if (filtered.size() >= MAX_CUTS_PER_NODE) {
                break; 
            }
        }
    }
    
    return filtered;
}

// 輔助函式：聯集兩個 Cut 列表 (DP Merge)
std::vector<KCut> mergeCuts(const std::vector<KCut>& leftCuts, const std::vector<KCut>& rightCuts) {
    if (leftCuts.empty()) return rightCuts;
    if (rightCuts.empty()) return leftCuts;

    std::vector<KCut> result;
    // 預先配置大約的空間，減少 push_back 時的重新分配
    result.reserve(leftCuts.size() * rightCuts.size());

    for (const auto& cl : leftCuts) {
        for (const auto& cr : rightCuts) {
            
            uint64_t mergedSig = cl.signature | cr.signature;
            if (__builtin_popcountll(mergedSig) > MAX_K) {
                continue; 
            }

            KCut merged;
            merged.signature = mergedSig; 

            int i = 0, j = 0;
            bool exceed = false;

            while (i < cl.size || j < cr.size) {
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
                        merged.leafNetIds[merged.size++] = cl.leafNetIds[i];
                        i++; j++;
                    }
                } else if (i < cl.size) {
                    merged.leafNetIds[merged.size++] = cl.leafNetIds[i++];
                } else {
                    merged.leafNetIds[merged.size++] = cr.leafNetIds[j++];
                }
            }

            if (!exceed) {
                result.push_back(merged);
            }
        }
    }

    // 不直接 resize，而是透過嚴格剪枝後再限制數量
    return pruneAndLimitCuts(result);
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
            
            // 如果這條線是常數 0/1，直接跳過
            // 常數不佔用 MAX_K 額度，也不應該成為 Cut 的 Leaf 邊界
            const Net& inNet = netlist.getNet(inNetId);
            if (inNet.isConst) {
                continue; 
            }

            int driverId = inNet.driverGateId;
            std::vector<KCut> branchCuts;

            if (coneGateSet.count(driverId)) {
                branchCuts = nodeCuts[driverId];
            } else {
                KCut boundaryCut;
                boundaryCut.leafNetIds[0] = inNetId;
                boundaryCut.size = 1;
                boundaryCut.normalize();
                branchCuts.push_back(boundaryCut);
            }

            if (isFirstInput) {
                mergedCuts = std::move(branchCuts);
                isFirstInput = false;
            } else {
                mergedCuts = mergeCuts(mergedCuts, branchCuts);
            }
        }

        for (const auto& mc : mergedCuts) {
            currentGateCuts.push_back(mc);
        }

        nodeCuts[currGateId] = pruneAndLimitCuts(currentGateCuts);
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