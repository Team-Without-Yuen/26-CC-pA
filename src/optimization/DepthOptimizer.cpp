#include "include/core/DepthOptimizer.h"
#include "include/core/SatTime.h"
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

template<typename Ntk>
static uint32_t depth_of(const Ntk& ntk) {
    mockturtle::depth_view dv{ntk};
    return dv.depth();
}

bool isGateAllowed(GateType type, 
                   const std::vector<GateType>& allowedTypes, 
                   const std::vector<GateType>& bannedTypes) {
    
    // 1. 如果在 bannedTypes 裡，絕對不允許 (黑名單優先)
    if (std::find(bannedTypes.begin(), bannedTypes.end(), type) != bannedTypes.end()) {
        return false;
    }

    // 2. 如果 allowedTypes 有內容，且該 type 不在裡面，則不允許 (白名單過濾)
    if (!allowedTypes.empty()) {
        if (std::find(allowedTypes.begin(), allowedTypes.end(), type) == allowedTypes.end()) {
            return false;
        }
    }

    // 通過以上檢查，代表是 Allowed
    return true;
}

// Critical Path 最佳化主控流程
OptimizationResult DepthOptimizer::executeCriticalPathOptimization(Netlist& netlist, 
                                                                   TechMapper& techMapper,
                                                                   const ConeReport& targetConeReport,
                                                                   const std::vector<GateType>& allowedTypes,
                                                                   const std::vector<GateType>& bannedTypes,
                                                                   bool verbose) {
    OptimizationResult result;
    result.passName = "Opt_CP";

    // 紀錄優化前深度與面積
    DepthReport oldGlobalPath = netlist.findGlobalCriticalPath();
    result.oldDepth = oldGlobalPath.depth;
    int oldTotal = 0;
    for (const auto& pair : netlist.countGatesByType()) oldTotal += pair.second;
    result.oldGateCount = oldTotal;

    if (verbose) {
        std::cout << "\n=================================================\n";
        std::cout << "[Flow Start] Old Global Depth: " << result.oldDepth
                  << " | Old Gate Count: " << result.oldGateCount << "\n";
    }

    // ---------------------------------------------------------------------
    // 階段 1：約束分類 (Constraint Classification)
    // ---------------------------------------------------------------------
    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());

    bool isPureAIG = bannedTypes.empty() && allowedSet.size() == 2 &&
                     allowedSet.count(GateType::AND) && allowedSet.count(GateType::NOT);
    bool isPureXAG = bannedTypes.empty() && allowedSet.size() == 3 &&
                     allowedSet.count(GateType::XOR) && allowedSet.count(GateType::AND) &&
                     allowedSet.count(GateType::NOT);

    // 局部 cone 限制？
    bool hasLocalConeConstraint = targetConeReport.ok && targetConeReport.exists;

    // 是否允許把 NOT 吸收進複合閘（NAND/NOR/XNOR 至少一種可用）
    bool canAbsorbInverters =
        isGateAllowed(GateType::NAND, allowedTypes, bannedTypes) ||
        isGateAllowed(GateType::NOR,  allowedTypes, bannedTypes) ||
        isGateAllowed(GateType::XNOR, allowedTypes, bannedTypes);

    // 是否有基礎閘被禁（需要基底強制轉換）
    bool needBasisEnforce = false;

    // 全域決定用哪種 mockturtle 網路：純 AIG 題用 AIG，其餘一律先用 XAG 壓深度
    bool useGlobalAIG = !hasLocalConeConstraint && isPureAIG;

    // ---------------------------------------------------------------------
    // 階段 2：全域深度壓縮 (Global Depth Optimization via mockturtle)
    //   純 AIG → AIG 流；其餘 → XAG 流（含 cone 限制題，先自由壓深度）
    // ---------------------------------------------------------------------
    if (useGlobalAIG) {
        if (verbose) std::cout << "[Step 2] Global AIG optimization...\n";

        mockturtle::aig_network aig0 = mockturtle::cleanup_dangling(NetlistToAig(netlist));
        mockturtle::aig_network candA = aig0, best = aig0;
        int best_real = INT_MAX;

        for (int i = 0; i < 10; ++i) {
            mockturtle::sop_rebalancing<mockturtle::aig_network> reb;
            mockturtle::balancing_params bps; bps.cut_enumeration_ps.cut_size = 6u;
            candA = mockturtle::cleanup_dangling(mockturtle::balancing(candA, {reb}, bps));

            mockturtle::cut_rewriting_params cr;
            cr.cut_enumeration_ps.cut_size = 4; cr.preserve_depth = true; cr.allow_zero_gain = true;
            mockturtle::xag_npn_resynthesis<mockturtle::aig_network> resyn;
            candA = mockturtle::cleanup_dangling(mockturtle::cut_rewriting(candA, resyn, cr));

            Netlist probe = AigToNetlist(candA, netlist);
            eliminateDoubleInverters(probe);
            int real_d = probe.findGlobalCriticalPath().depth;
            if (real_d < best_real) { best_real = real_d; best = candA; } else break;
        }
        netlist = AigToNetlist(best, netlist);
        eliminateDoubleInverters(netlist);
        if (verbose) std::cout << "  -> AIG netlist depth: "
                               << netlist.findGlobalCriticalPath().depth << "\n";

    } else {
        if (verbose) std::cout << "[Step 2] Global XAG optimization...\n";

        mockturtle::xag_network xag = mockturtle::cleanup_dangling(NetlistToXag(netlist));
        mockturtle::xag_network best = xag;
        uint32_t best_depth = depth_of(xag);

        for (int iter = 0; iter < 10; ++iter) {
            mockturtle::esop_rebalancing<mockturtle::xag_network> reb;
            mockturtle::balancing_params bps; bps.cut_enumeration_ps.cut_size = 6u;
            xag = mockturtle::cleanup_dangling(mockturtle::balancing(xag, {reb}, bps));

            mockturtle::cut_rewriting_params cr_ps;
            cr_ps.cut_enumeration_ps.cut_size = 4; cr_ps.preserve_depth = true; cr_ps.allow_zero_gain = true;
            mockturtle::xag_npn_resynthesis<mockturtle::xag_network> resyn;
            xag = mockturtle::cleanup_dangling(mockturtle::cut_rewriting(xag, resyn, cr_ps));

            {
                mockturtle::resubstitution_params rp;
                mockturtle::fanout_view fv{xag};
                mockturtle::depth_view dv{fv};
                mockturtle::xag_resubstitution(dv, rp);
                xag = mockturtle::cleanup_dangling(xag);
            }

            uint32_t d = depth_of(xag);
            if (d < best_depth) { best_depth = d; best = xag; } else break;
        }
        netlist = XagToNetlist(best, netlist);
        eliminateDoubleInverters(netlist);
        if (verbose) std::cout << "  -> XAG best depth (mockturtle metric): " << best_depth << "\n";
    }

    netlist.trimDeadLogic();

    // ---------------------------------------------------------------------
    // 階段 3：局部 Cone 限制處理 (Local Cone Constrained Resynthesis)
    //   只在「某個 cone 內部有基底限制」時執行。
    //   流程：切出 cone（K-feasible cut 界定範圍）→ 依受限基底重合成 → 縫回
    //   語意：targetConeReport 指定「哪個 cone」；allowedTypes/bannedTypes
    //         此時代表「該 cone 內部」的約束。cone 以外不受限。
    // ---------------------------------------------------------------------
    if (hasLocalConeConstraint) {
        if (verbose)
            std::cout << "[Step 3] Local cone enforcement on '"
                      << targetConeReport.sourceName << "'...\n";

        const int depthBeforeCone = netlist.findGlobalCriticalPath().depth;

        // 依 ConeQueryType 分派重查，取得「當下最新」的 cone 閘集合
        auto refreshConeGates = [&]() -> std::unordered_set<int> {
            ConeResult cr;
            switch (targetConeReport.type) {
                case ConeQueryType::NetTransitiveFanin:
                    cr = netlist.getTransitiveFaninCone(targetConeReport.sourceName);      break;
                case ConeQueryType::NetTransitiveFanout:
                    cr = netlist.getTransitiveFanoutCone(targetConeReport.sourceName);     break;
                case ConeQueryType::GateTransitiveFanin:
                    cr = netlist.getGateTransitiveFaninCone(targetConeReport.sourceName);  break;
                case ConeQueryType::GateTransitiveFanout:
                    cr = netlist.getGateTransitiveFanoutCone(targetConeReport.sourceName); break;
                default:
                    cr = netlist.getTransitiveFaninCone(targetConeReport.sourceName);      break;
            }

            std::unordered_set<int> s;
            for (int g : netlist.getConeGateIds(cr)) {
                if (g < 0 || !netlist.isValidGateId(g)) continue;
                GateType t = netlist.getGate(g).type;
                if (t == GateType::UNKNOWN || t == GateType::DFF) continue;
                s.insert(g);   // root 閘已由 cone 函式本身納入
            }
            return s;
        };

        std::unordered_set<int> coneGates = refreshConeGates();
        if (coneGates.empty()) {
            result.status = OptimizationStatus::ERROR_NOT_EQUIVALENT;
            result.message = "Cone gate set empty; cannot enforce local basis.";
            return result;
        }
        if (verbose)
            std::cout << "  -> cone size: " << coneGates.size() << " gates\n";

        // (a) cone 內基底強制（合規，失敗即放棄
        TechMapReport coneRep = techMapper.convertToBasisOnGateSet(
            netlist, coneGates, allowedTypes, bannedTypes, verbose);

        if (coneRep.status != TechMapStatus::SUCCESS) {
            result.status = OptimizationStatus::ERROR_NOT_EQUIVALENT;
            result.message = "Cone basis enforcement failed: " + coneRep.message;
            return result;
        }
        eliminateDoubleInverters(netlist);
        netlist.trimDeadLogic();

        // (b) cone 內反相吸收：RHS 限定 cone 允許的閘
        coneGates = refreshConeGates();                 // (a) 產生新閘，範圍已變
        techMapper.absorbInvertersOnGateSet(
            netlist, coneGates, allowedTypes, bannedTypes, verbose);
        eliminateDoubleInverters(netlist);
        netlist.trimDeadLogic();

        // (c) cone 外反相吸收：任意閘合法 
        coneGates = refreshConeGates();                 // (b) 又改了結構，重查後才能正確排除
        std::unordered_set<int> outsideGates;
        for (int g = 0; g < (int)netlist.getGateCount(); ++g) {
            GateType t = netlist.getGate(g).type;
            if (t == GateType::UNKNOWN || t == GateType::DFF) continue;
            if (coneGates.count(g)) continue;           // 排除 cone 內，保護受限基底
            outsideGates.insert(g);
        }

        static const std::vector<GateType> kAllGates = {
            GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
            GateType::NOT, GateType::BUF, GateType::XOR, GateType::XNOR };

        techMapper.absorbInvertersOnGateSet(
            netlist, outsideGates, kAllGates, {}, verbose);
        eliminateDoubleInverters(netlist);
        netlist.trimDeadLogic();

        if (verbose)
            std::cout << "  -> cone depth: " << depthBeforeCone
                      << " -> " << netlist.findGlobalCriticalPath().depth << "\n";
    }

    // ---------------------------------------------------------------------
    // 階段 4：全域基底強制 (Global Basis Enforcement)
    //   只在「有基礎閘被禁」時執行（合規，不可選）。
    //   把電路中殘留的被禁閘（如 AND/XOR）換成允許的等價組合。
    //   注意：這一步在「反相吸收」之前 —— 先讓結構合規，再吸收 NOT。
    // ---------------------------------------------------------------------

    // 只有「電路裡真的存在被禁的閘」才需要基底強制
    if (!hasLocalConeConstraint) {
        std::vector<GateType> allComb = {GateType::AND, GateType::OR, GateType::NAND,
            GateType::NOR, GateType::NOT, GateType::BUF, GateType::XOR, GateType::XNOR};
        for (GateType t : allComb) {
            if (!isGateAllowed(t, allowedTypes, bannedTypes) &&
                netlist.getGateCountByType(t) > 0) { needBasisEnforce = true; break; }
        }
    }

    if (needBasisEnforce && !hasLocalConeConstraint) {
        if (verbose) std::cout << "[Step 4] Basis enforcement (expand banned gates)...\n";

        TechMapReport rep = techMapper.convertToBasis(
            netlist,
            TargetScope::WHOLE_NETLIST,   // 全域
            "",                            // 全域不需要指定 net/gate 名
            allowedTypes,
            bannedTypes,
            verbose
        );

        // 合規失敗 = 直接放棄（殘留非法閘等於違規、零分），回報錯誤
        if (rep.status != TechMapStatus::SUCCESS) {
            result.status = OptimizationStatus::ERROR_NOT_EQUIVALENT;
            result.message = "Basis enforcement failed: " + rep.message;
            return result;
        }

        // 展開後可能產生可消的雙反相（例如 NOT_to_XXX 疊出 NOT-NOT），先清一次
        eliminateDoubleInverters(netlist);
        netlist.trimDeadLogic();

        if (verbose)
            std::cout << "  -> after basis enforcement depth: "
                      << netlist.findGlobalCriticalPath().depth << "\n";
    }

    // ---------------------------------------------------------------------
    // 階段 5：反相吸收 (Inverter Absorption)
    //   只在「允許 NAND/NOR/XNOR」時執行（省深度，機會型）。
    //   把 NOT(AND)→NAND、NOT(OR)→NOR、NOT(XOR)→XNOR，
    //   以及輸入端德摩根 AND(NOT,NOT)→NOR 等，融掉 NOT 省一層。
    //   放在基底強制「之後」：結構定案後再吸收，且只融出允許的閘。
    // ---------------------------------------------------------------------
    if (canAbsorbInverters && !hasLocalConeConstraint) {
        if (verbose) std::cout << "[Step 5] Inverter absorption...\n";
        techMapper.absorbInverters(netlist, allowedTypes, bannedTypes, verbose);
        eliminateDoubleInverters(netlist);
        netlist.trimDeadLogic();
        if (verbose)
            std::cout << "  -> after absorption depth: "
                      << netlist.findGlobalCriticalPath().depth << "\n";
    }

    // ---------------------------------------------------------------------
    // 階段 6：結算 (Depth & Area Evaluation)
    //   trim 一次確保面積不含死閘，量最終深度與面積。
    // ---------------------------------------------------------------------
    netlist.trimDeadLogic();

    DepthReport newGlobalPath = netlist.findGlobalCriticalPath();
    result.newDepth = newGlobalPath.depth;
    result.depthImproved = (result.newDepth < result.oldDepth);

    int newTotal = 0;
    for (const auto& pair : netlist.countGatesByType()) newTotal += pair.second;
    result.newGateCount = newTotal;
    result.areaDelta = result.newGateCount - result.oldGateCount;

    result.status = OptimizationStatus::SUCCESS;
    result.changed = true;
    result.equivalent = true;   // TODO: 實際接等價驗證（miter + SAT 或 2^n 窮舉）
    result.message = "Completed optimization sequence.";

    if (verbose) {
        std::cout << "[Success] Depth: " << result.oldDepth << " -> " << result.newDepth << "\n";
        std::cout << "=================================================\n";
    }
    return result;
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

// 全 netlist 的 NOT(NOT x) → x 消除。DepthOptimizer 专用，直接砍關鍵路徑深度。
// 回傳消除的 NOT 對數。
int DepthOptimizer::eliminateDoubleInverters(Netlist& netlist) {
    int removed = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int g = 0; g < (int)netlist.getGateCount(); ++g) {
            Gate& gate = netlist.getGateMutable(g);
            if (gate.type != GateType::NOT) continue;
            if (gate.inputNetIds.empty() || gate.inputNetIds[0] < 0) continue;

            int midNet = gate.inputNetIds[0];
            
            // 讀取驅動 midNet 的閘 (drv)
            int drv = netlist.getNet(midNet).driverGateId;
            if (drv < 0 || drv >= (int)netlist.getGateCount()) continue;
            if (netlist.getGate(drv).type != GateType::NOT) continue;          // 上游也是 NOT
            if (netlist.getGate(drv).inputNetIds.empty()) continue;

            int srcNet = netlist.getGate(drv).inputNetIds[0];                  // 第一顆 NOT 的輸入
            int outNet = gate.outputNetId;                                     // 第二顆 NOT 的輸出
            if (srcNet < 0 || outNet < 0) continue;

            // PO 保護：outNet 是 PO 就不動（避免 PO 失去 driver / 語意錯亂）
            if (netlist.getNet(outNet).isPO) continue;

            // 把 outNet 的所有負載改吃 srcNet（= NOT(NOT x) = x）
            redirectNetLoads(netlist, outNet, srcNet);   // 呼叫 Optimizer 內部的 helper
            detachGate(netlist, g);                       // 刪掉第二顆 NOT（第一顆留給後續的清理 pass 或邏輯最佳化）
            ++removed;
            changed = true;
        }
    }
    return removed;
}