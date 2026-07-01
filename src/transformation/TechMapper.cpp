#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/lib/cadical.hpp"
#include <string>
#include <iostream>
#include <functional>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>

namespace {

std::map<GateType, int> toOrderedMap(const std::unordered_map<GateType, int>& values) {
    std::map<GateType, int> ordered;
    for (const auto& item : values) {
        ordered[item.first] += item.second;
    }
    return ordered;
}

MappingDelta makeMappingDelta(const TechMapReport& techReport) {
    MappingDelta delta;
    delta.removedCountByType = toOrderedMap(techReport.removedCountByType);
    delta.addedCountByType = toOrderedMap(techReport.addedCountByType);
    delta.finalGateCountByType = toOrderedMap(techReport.finalGateCount);
    delta.modifiedGateNames = techReport.modifiedGateNames;
    return delta;
}

NetlistEditReport finalizeTechMapEditReport(
    Netlist& netlist,
    const Netlist& before,
    const TechMapReport& techReport,
    const std::string& operationName)
{
    NetlistEditReport report = Netlist::buildEditReport(
        before,
        netlist,
        operationName,
        NetlistEditOperationKind::TechnologyMapping);

    report.mappingDelta = makeMappingDelta(techReport);
    report.changed = report.changed || !techReport.modifiedGateNames.empty();

    if (techReport.status != TechMapStatus::SUCCESS) {
        report.success = false;
        report.message = techReport.message;
        report.addWarning("Technology mapping reported a non-success status.");
    } else {
        report.message = techReport.message;
    }

    if (!report.success) {
        netlist.restoreFrom(before);
        report.rolledBack = true;
    }

    return report;
}

}

// 輔助函式：將 NodeType 對應回 Input Vector 的 Index (0, 1, 2...)
int TechMapper::getLeafIndex(NodeType type) const {
    switch (type) {
        case NodeType::LEAF_A: return 0;
        case NodeType::LEAF_B: return 1;
        case NodeType::LEAF_C: return 2;
        case NodeType::LEAF_D: return 3;
        case NodeType::LEAF_E: return 4;
        case NodeType::LEAF_F: return 5;
        case NodeType::LEAF_G: return 6; // 新增對應
        default: return -1; 
    }
}

// 輔助函式：根據 Input Index 產生對應的 LEAF 節點 (A=0, B=1, C=2...)
std::shared_ptr<PatternNode> TechMapper::createLeafNode(int index) const{
    NodeType type;
    switch (index) {
        case 0: type = NodeType::LEAF_A; break;
        case 1: type = NodeType::LEAF_B; break;
        case 2: type = NodeType::LEAF_C; break;
        case 3: type = NodeType::LEAF_D; break;
        case 4: type = NodeType::LEAF_E; break;
        case 5: type = NodeType::LEAF_F; break;
        case 6: type = NodeType::LEAF_G; break; // 新增對應
        default: throw std::out_of_range("Leaf index exceeds maximum supported inputs (7)."); // 順手把錯誤訊息的上限改為 7
    }
    return std::make_shared<PatternNode>(PatternNode{type, GateType::UNKNOWN, {}});
}

// 輔助函式：用來單獨檢查「某一側 (LHS 或 RHS)」是否符合給定的數量限制
bool TechMapper::checkSideConstraints(const std::map<GateType, int>& ruleCounts, 
                                      const std::map<GateType, int>& constraints) const {
    // 正向檢查：Rule 實際用到的 Gate，是否在允許名單內？數量是否超標？
    for (const auto& pair : ruleCounts) {
        GateType usedType = pair.first;
        int usedAmount = pair.second;

        auto it = constraints.find(usedType);
        if (it == constraints.end()) {
            return false; // 根本不在 constraints 名單內，代表被禁用，直接淘汰
        }

        int allowedAmount = it->second;
        // 如果不是 -1 (無限制)，且數量不等於強制要求的數量，就淘汰
        if (allowedAmount != -1 && allowedAmount != usedAmount) {
            return false; 
        }
    }

    // 反向檢查：Constraints「強制要求」要用的 Gate 數量，Rule 有滿足嗎？
    for (const auto& pair : constraints) {
        GateType requiredType = pair.first;
        int requiredAmount = pair.second;

        if (requiredAmount == -1) continue; // -1 代表隨意，不強制檢查

        // 如果 rule 裡找不到，代表該 rule 包含此 Gate 的數量為 0
        int actualAmount = 0;
        auto it = ruleCounts.find(requiredType);
        if (it != ruleCounts.end()) {
            actualAmount = it->second;
        }

        // 比對實際數量與要求數量
        if (actualAmount != requiredAmount) {
            return false; 
        }
    }

    return true;
}

// 輔助函式：註冊雙向規則 (Bidirectional Rule Registration)
void TechMapper::addBidirectionalRule(const std::string& baseName, 
                            std::shared_ptr<PatternNode> patternA, 
                            std::shared_ptr<PatternNode> patternB) {
    // 防呆：確保兩棵樹都存在
    if (!patternA || !patternB) return;

    // 註冊正向規則 (PatternA -> PatternB)
    // 命名加上 "_Forward" 後綴方便 Debug
    rules.emplace_back(baseName + "_Forward", patternA, patternB);
    
    // 註冊反向規則 (PatternB -> PatternA)
    // 直接將傳入的兩棵樹參數對調即可！
    rules.emplace_back(baseName + "_Backward", patternB, patternA);
}

// 檢查某個 Rule 的「左邊與右邊」是否都滿足使用者的數量約束
bool TechMapper::isRuleSatisfyingConstraints(const TechMapRule& rule, 
                                             const std::map<GateType, int>& lhsConstraints,  // 對左邊 (拔除目標) 的限制
                                             const std::map<GateType, int>& rhsConstraints) const { // 對右邊 (替換生成) 的限制 
    // 兩側都必須通過檢查才算合法
    return checkSideConstraints(rule.targetCounts, lhsConstraints) && checkSideConstraints(rule.allowedCounts, rhsConstraints);
}

// 統一的篩選器：取得所有合法的規則，並自動按照「面積優化程度」排序
std::vector<TechMapRule> TechMapper::getValidRules(const std::map<GateType, int>& lhsConstraints, 
                                                   const std::map<GateType, int>& rhsConstraints,
                                                   bool requireAreaReduction) const {
    
    std::vector<TechMapRule> validRules;

    for (const auto& rule : rules) {
        // 使用原本封裝好的正確函式，檢查規則是否符合指定的積木種類與數量
        if (isRuleSatisfyingConstraints(rule, lhsConstraints, rhsConstraints)) {
            
            // 決定是否放行此規則
            // 1. 如果不強制要求面積縮小 (requireAreaReduction == false)，直接放行
            // 2. 如果強制要求面積縮小，則 Delta 必須 < 0
            if (!requireAreaReduction || rule.getCostDelta() < 0) {
                validRules.push_back(rule);
            }
        }
    }

    // 排序：以 Cost Delta 為基準 ( addedGateCount - removedGateCount )
    // 數值越小代表優化效果越好 (或面積增加越少)，排在最前面！
    std::sort(validRules.begin(), validRules.end(), [](const TechMapRule& a, const TechMapRule& b) {
        return a.getCostDelta() < b.getCostDelta();
    });

    return validRules;
}

// 輔助函式：判斷該 Gate 是否具有交換律 (Commutativity) 
bool TechMapper::isCommutative(GateType type) const {
    return type == GateType::AND || type == GateType::OR || 
           type == GateType::NAND || type == GateType::NOR || 
           type == GateType::XOR || type == GateType::XNOR;
    // 如果未來加入 MUX，MUX(sel, in1, in0) 不具備交換律，這裡就不會回傳 true
}

// 遞迴比對引擎核心 (Backward Pattern Matching)
bool TechMapper::matchNet(Netlist& netlist, std::shared_ptr<PatternNode> pNode, int physNetId, MatchContext& ctx) {
    if (physNetId == -1) return false;

    // 處理 Leaf 節點 (動態支援任意數量的 LEAF)
    // 只要不是 GATE 也不是 CONST，那就是 LEAF (如 LEAF_A, LEAF_B, LEAF_C)
    if (pNode->nodeType != NodeType::GATE && 
        pNode->nodeType != NodeType::CONST_1 && 
        pNode->nodeType != NodeType::CONST_0) 
    {
        if (!ctx.isLeafBound(pNode->nodeType)) {
            // 第一次遇到這個 LEAF 類型，把物理 Net ID 綁定上去
            ctx.boundLeaves[pNode->nodeType] = physNetId; 
            return true; 
        }
        // 之後遇到同一個 LEAF，物理 Net ID 必須是同一條線
        return ctx.getBoundNet(pNode->nodeType) == physNetId; 
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
    if (driverGateId == -1) return false; // 這條線沒有驅動閘 (可能是 Primary Input)

    // DAG 檢查：如果這個 PatternNode 之前比對過了，物理實體必須是同一個 Gate
    if (ctx.mappedNodes.count(pNode.get())) {
        return ctx.mappedNodes[pNode.get()] == driverGateId;
    }

    const Gate& physGate = netlist.getGate(driverGateId);
    if (physGate.type != pNode->gateType) return false; // 閘類型不符
    if (physGate.inputNetIds.size() != pNode->inputs.size()) return false; // 輸入數量不符

    // 註冊這個物理 Gate (標記為已被目前子圖涵蓋)
    ctx.mappedNodes[pNode.get()] = driverGateId;
    ctx.matchedGates.insert(driverGateId);

    // 遞迴比對輸入線
    if (pNode->inputs.size() == 1) {
        return matchNet(netlist, pNode->inputs[0], physGate.inputNetIds[0], ctx);
    } 
    else if (pNode->inputs.size() == 2) {
        MatchContext backupCtx = ctx; // 建立備份，以便回溯
        
        // 嘗試正向順序：(Pattern Input 0 == Phys Input 0) && (Pattern Input 1 == Phys Input 1)
        if (matchNet(netlist, pNode->inputs[0], physGate.inputNetIds[0], ctx) &&
            matchNet(netlist, pNode->inputs[1], physGate.inputNetIds[1], ctx)) {
            return true;
        }
        
        // 如果失敗，且該邏輯閘支援交換律，則嘗試反向順序
        if (isCommutative(physGate.type)) {
            ctx = backupCtx; // 恢復綁定與註冊狀態
            if (matchNet(netlist, pNode->inputs[0], physGate.inputNetIds[1], ctx) &&
                matchNet(netlist, pNode->inputs[1], physGate.inputNetIds[0], ctx)) {
                return true;
            }
        }
    }
    return false;
}

// 觸發比對的入口點
bool TechMapper::matchRootGate(Netlist& netlist, int physGateId, const TechMapRule& rule, MatchContext& ctx) {
    // 換成 targetPattern (LHS)
    auto pRoot = rule.targetPattern; 
    if (pRoot->nodeType != NodeType::GATE) return false;
    
    // 把目前的物理 Gate 當作 Root，它的 Output 不用比對，我們只比對它的 Input 往下長相
    const Gate& physGate = netlist.getGate(physGateId);
    if (physGate.type != pRoot->gateType) return false;
    if (physGate.inputNetIds.size() != pRoot->inputs.size()) return false;

    // 註冊 Root Gate
    ctx.mappedNodes[pRoot.get()] = physGateId;
    ctx.matchedGates.insert(physGateId);

    // 進入遞迴 (同樣加上交換律的判斷)
    if (pRoot->inputs.size() == 1) {
        return matchNet(netlist, pRoot->inputs[0], physGate.inputNetIds[0], ctx);
    } 
    else if (pRoot->inputs.size() == 2) {
        MatchContext backup = ctx;
        if (matchNet(netlist, pRoot->inputs[0], physGate.inputNetIds[0], ctx) &&
            matchNet(netlist, pRoot->inputs[1], physGate.inputNetIds[1], ctx)) {
            return true;
        }
        
        if (isCommutative(physGate.type)) {
            ctx = backup;
            if (matchNet(netlist, pRoot->inputs[0], physGate.inputNetIds[1], ctx) &&
                matchNet(netlist, pRoot->inputs[1], physGate.inputNetIds[0], ctx)) {
                return true;
            }
        }
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

// 統一的替換執行引擎
void TechMapper::applyRule(Netlist& netlist, const MatchContext& ctx, int rootGateId, const TechMapRule& rule) {
    const Gate& rootGate = netlist.getGate(rootGateId);
    int origOutNetId = rootGate.outputNetId;
    std::string baseInstName = rootGate.instName + "_opt";

    // 將舊的子圖 (LHS) 從電路中拔除
    std::vector<int> internalNetsToRemove;

    for (int gateId : ctx.matchedGates) {
        const Gate& g = netlist.getGate(gateId);
        
        // 抓出這顆 Gate 的輸出線。如果是內部線，記下來準備刪除。
        if (g.outputNetId != -1 && g.outputNetId != origOutNetId) {
            internalNetsToRemove.push_back(g.outputNetId);
        }
        
        // 拔除 Gate
        netlist.removeGate(gateId); 
    }

    // 準備遞迴引擎，用來生成新的子圖 (RHS)
    // Memoization 表：解決 RHS 中也有 DAG (共用節點) 的問題
    std::unordered_map<PatternNode*, int> visited;

    std::function<int(std::shared_ptr<PatternNode>, bool)> buildNode = 
        [&](std::shared_ptr<PatternNode> node, bool isRoot) -> int {
        
        // 如果節點已經生成過，直接回傳之前產生的實體 Net ID (維持 DAG 結構)
        if (visited.count(node.get())) {
            return visited[node.get()];
        }

        int outNetId = -1;

        // 情況 A：遇到葉節點 (LEAF_A, LEAF_B, LEAF_C...)
        if (node->nodeType != NodeType::GATE && 
            node->nodeType != NodeType::CONST_1 && 
            node->nodeType != NodeType::CONST_0) 
        {
            // 直接從 ctx 中取出當初 Match 到的真實物理線路 ID
            outNetId = ctx.getBoundNet(node->nodeType);
        } 
        // 情況 B：遇到常數節點
        else if (node->nodeType == NodeType::CONST_1 || node->nodeType == NodeType::CONST_0) {
            std::string constName = (node->nodeType == NodeType::CONST_1) ? "1'b1" : "1'b0";
            outNetId = netlist.getNetId(constName);
            
            if (outNetId == -1) {
                outNetId = netlist.addNet(constName);
                int constVal = (node->nodeType == NodeType::CONST_1) ? 1 : 0;
                netlist.setNetConst(outNetId, true, constVal);  
            }
        } 
        // 情況 C：遇到內部邏輯閘
        else if (node->nodeType == NodeType::GATE) {
            // C-1. 先遞迴產生所有的 Input 子樹
            std::vector<int> childNetIds;
            for (auto& child : node->inputs) {
                childNetIds.push_back(buildNode(child, false));
            }

            // C-2. 在 Netlist 中實體化這個新的 Gate
            std::string newInstName = baseInstName + "_" + std::to_string(visited.size());
            int newGateId = netlist.addGate(newInstName, node->gateType);

            // C-3. 將前面取得的 Input Nets 接上
            for (int cNetId : childNetIds) {
                netlist.connectGateInput(newGateId, cNetId);
            }

            // C-4. 處理 Output Net
            if (isRoot) {
                // 如果這是新子圖的 Root，它必須負責驅動原本舊 Root 的輸出線
                if (origOutNetId != -1) {
                    netlist.connectGateOutput(newGateId, origOutNetId);
                    outNetId = origOutNetId;
                }
            } else {
                // 如果是新子圖的內部閘，產生一條新的內部連線
                std::string newNetName = "net_" + newInstName;
                outNetId = netlist.addNet(newNetName);
                netlist.connectGateOutput(newGateId, outNetId);
            }
        }

        visited[node.get()] = outNetId;
        return outNetId;
    };

    // 啟動遞迴引擎，開始建造 Replacement Pattern (RHS)
    buildNode(rule.replacementPattern, true);
}

// 統一的映射執行引擎 (取代原有的 Forward / Backward Pass)
bool TechMapper::executeMappingPass(Netlist& netlist, 
                                    const std::unordered_set<int>* scopeGates, 
                                    const std::vector<TechMapRule>& validRules, 
                                    TechMapReport& report, 
                                    bool verbose) {
    if (validRules.empty()) return false;
    bool actualChangesMade = false;
    bool isChanged;

    // 迴圈直到電路無法再被這些規則優化為止 (達到局部最佳解)
    do {
        isChanged = false;

        // 收集本次 Iteration 要掃描的候選 Gate ID
        // (必須在迴圈內收集，因為替換會產生新的 Gate ID，它們也可能成為下一波優化的目標)
        std::vector<int> candidates;
        if (scopeGates != nullptr) {
            for (int gateId : *scopeGates) {
                if (gateId >= 0 && gateId < (int)netlist.getGateCount() && netlist.getGate(gateId).type != GateType::UNKNOWN) {
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

        // 遍歷所有候選節點
        for (int rootId : candidates) {
            // 安全防護：如果這個 Gate 已經在剛才的規則套用中被拔掉了，跳過它
            if (netlist.getGate(rootId).type == GateType::UNKNOWN) continue;

            // 依序嘗試每一條合法的 Rule (已經按 Cost 優化程度排過序了)
            for (const auto& rule : validRules) {
                MatchContext ctx;
                
                // 形狀比對 (尋找 LHS)
                if (matchRootGate(netlist, rootId, rule, ctx)) {
                    // 合法性驗證 (檢查 Fanout)
                    if (isValidSubgraph(netlist, ctx, rootId)) {
                        
                        // 紀錄 Report
                        for (int matchedGateId : ctx.matchedGates) {
                            const Gate& g = netlist.getGate(matchedGateId);
                            report.removedCountByType[g.type]++;
                            if (verbose) report.modifiedGateNames.push_back(g.instName);
                        }
                        
                        // 執行圖形替換 (套用 RHS)
                        applyRule(netlist, ctx, rootId, rule); 
                        
                        isChanged = true;
                        actualChangesMade = true;
                        break; // 已經成功套用了一條規則並改變了圖形，跳出 rule 迴圈，換下一個 Root 檢查
                    }
                }
            }
            // 如果圖形已變更，強烈建議立刻中斷 candidates 的遍歷，重新回到 do-while 頂部收集最新名單。
            // 這樣可以避免剛長出來的新結構被舊的、髒掉的 candidate 索引干擾。
            if (isChanged) break; 
        }
    } while (isChanged);
    
    return actualChangesMade;
}

// targetConstraints  : 使用者想要「拔除/替換掉」的 Gate 類型與數量限制
// allowedConstraints : 使用者允許「新增/使用」的 Gate 類型與數量限制
// scopeGates         : 作用範圍 (Scope)。若提供，則僅對名單內的 Gate 進行操作；若為 nullptr 則掃描全電路。
// verbose            : 是否輸出詳細的替換 Log
// 回傳值             : TechMapReport (包含變更狀態、增刪數量統計與最終快照)
// 底層的實作引擎，約束驅動型 ECO 技術映射核心引擎
TechMapReport TechMapper::mapTechnologyCore(Netlist& netlist, 
                                            const std::map<GateType, int>& targetConstraints, // 拔除目標限制
                                            const std::map<GateType, int>& allowedConstraints, // 生成目標限制
                                            const std::unordered_set<int>* scopeGates,
                                            bool verbose) {
    TechMapReport report;
    
    // 預先紀錄初始快照 (Snapshot)
    std::unordered_map<GateType, int> initialCounts;
    for (const auto& pair : allowedConstraints) initialCounts[pair.first] = netlist.getGateCountByType(pair.first);
    for (const auto& pair : targetConstraints)  initialCounts[pair.first] = netlist.getGateCountByType(pair.first);

    // 取得所有合法的「樹到樹」雙向規則 (已自動按 Cost 排序)
    // 這裡呼叫的是我們之前合併好的單一 API
    std::vector<TechMapRule> validRules = getValidRules(targetConstraints, allowedConstraints);

    // 強制只取最好的一個 Rule 執行
    if (validRules.size() > 1) {
        // 因為已經排序過，index 0 就是對面積縮減最有利的規則
        // 裁切 Vector，確保本次掃描全電路時，只會套用這一種規則！
        validRules.erase(validRules.begin() + 1, validRules.end()); 
    }

    // 執行統一的替換引擎
    bool mappingChanged = executeMappingPass(netlist, scopeGates, validRules, report, verbose);

    // 統一使用 Snapshot 數學反推法結算「新增數量」
    for (const auto& pair : allowedConstraints) {
        GateType t = pair.first;
        int currentCount = netlist.getGateCountByType(t);
        int explicitlyRemoved = 0;
        
        auto it = report.removedCountByType.find(t);
        if (it != report.removedCountByType.end()) {
            explicitlyRemoved = it->second;
        }
        
        // 新增的 = 現在擁有的 - 最初擁有的 + 被拔掉的
        int calculatedAdded = currentCount - initialCounts[t] + explicitlyRemoved;
        if (calculatedAdded > 0) {
            report.addedCountByType[t] += calculatedAdded;
        }
    }

    // 狀態判定與回報
    if (validRules.empty()) {
        report.status = TechMapStatus::ERROR_RULE_NOT_FOUND;
        report.message = "Failed: No rules found matching the specified LHS/RHS constraints.";
    } else if (mappingChanged) {
        report.status = TechMapStatus::SUCCESS;
        report.message = "Success: Tree-to-Tree technology mapping applied successfully.";
    } else {
        report.status = TechMapStatus::SUCCESS;
        report.message = "Notice: Rules matched constraints, but no matching subgraphs in the netlist required modification.";
    }

    // 寫入最終電路快照
    for (const auto& pair : allowedConstraints) report.finalGateCount[pair.first] = netlist.getGateCountByType(pair.first);
    for (const auto& pair : targetConstraints)  report.finalGateCount[pair.first] = netlist.getGateCountByType(pair.first);

    return report;
}

// 萬用 API 實作 (直接呼叫 Core，scopeGates 給 nullptr 代表掃描全電路)
TechMapReport TechMapper::mapTechnology(Netlist& netlist, 
                                        const std::map<GateType, int>& targetConstraints, 
                                        const std::map<GateType, int>& allowedConstraints, 
                                        bool verbose) {
    // 直接將參數與 verbose 往下傳遞 (已移除無用的 MapStrategy)
    return mapTechnologyCore(netlist, targetConstraints, allowedConstraints, nullptr, verbose);
}

NetlistEditReport TechMapper::mapTechnologyWithReport(
    Netlist& netlist,
    const std::map<GateType, int>& targetConstraints,
    const std::map<GateType, int>& allowedConstraints,
    bool verbose)
{
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = mapTechnology(netlist, targetConstraints, allowedConstraints, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "mapTechnology");
}

// 針對特定 Cone (邏輯錐) 的 API 實作 (局部優化 / ECO 常用)
TechMapReport TechMapper::mapTechnologyForCone(Netlist& netlist, 
                                               const std::map<GateType, int>& targetConstraints, 
                                               const std::map<GateType, int>& allowedConstraints, 
                                               const ConeResult& targetCone,
                                               bool verbose) {
                                              
    // 呼叫 Netlist 原本就有的 Function 取得該邏輯錐內的所有 Gate ID 陣列
    std::vector<int> coneGateVec = netlist.getConeGateIds(targetCone);
    
    // 將 vector 轉換為 unordered_set，讓後續引擎在執行 ctx.matchedGates 查詢時擁有 O(1) 的極高效率
    std::unordered_set<int> scopeGates(coneGateVec.begin(), coneGateVec.end());
    
    // 呼叫核心引擎，並將 scopeGates 的記憶體位址傳入
    return mapTechnologyCore(netlist, targetConstraints, allowedConstraints, &scopeGates, verbose);
}

NetlistEditReport TechMapper::mapTechnologyForConeWithReport(
    Netlist& netlist,
    const std::map<GateType, int>& targetConstraints,
    const std::map<GateType, int>& allowedConstraints,
    const ConeResult& targetCone,
    bool verbose)
{
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = mapTechnologyForCone(
        netlist,
        targetConstraints,
        allowedConstraints,
        targetCone,
        verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "mapTechnologyForCone");
}

// 輔助函式：取得指定邏輯閘的輸入腳位數量 (Fan-in)
int getGateFaninCount(GateType type) {
    switch (type) {
        case GateType::NOT:
        case GateType::BUF:
            return 1;
        case GateType::AND:
        case GateType::OR:
        case GateType::NAND:
        case GateType::NOR:
        case GateType::XOR:
        case GateType::XNOR:
            return 2;
        default:
            return 2; // 預設安全值或觸發 Exception
    }
}

// 根據給定的 Gate 組成，計算最大可能的輸入腳位數 (N_max)
int TechMapper::calculateMaxInputs(const std::map<GateType, int>& gateCounts) const {
    int totalGates = 0;
    int totalFanins = 0;

    for (const auto& pair : gateCounts) {
        GateType type = pair.first;
        int count = pair.second;
        
        totalGates += count;
        totalFanins += getGateFaninCount(type) * count;
    }

    // 防呆：如果沒有任何邏輯閘，輸入視為 0 或 1 (純直通線)
    if (totalGates == 0) {
        return 0; 
    }

    // 計算 Tree 結構下的最大輸入數
    // N_max = 總輸入腳位數 - (內部必須相連的最少連線數)
    int maxInputs = totalFanins - (totalGates - 1);

    return maxInputs;
}

// 輔助函數：將動態變數的邏輯閘語意轉為 CNF (專供 Exact Synthesis 使用)
void TechMapper::addGateSemantics(CaDiCaL::Solver& solver, GateType type, int inA, int inB, int out) {
    if (type == GateType::AND) {
        solver.add(-inA); solver.add(-inB); solver.add(out); solver.add(0);
        solver.add(inA); solver.add(-out); solver.add(0);
        solver.add(inB); solver.add(-out); solver.add(0);
    } 
    else if (type == GateType::NAND) {
        solver.add(-inA); solver.add(-inB); solver.add(-out); solver.add(0);
        solver.add(inA); solver.add(out); solver.add(0);
        solver.add(inB); solver.add(out); solver.add(0);
    } 
    else if (type == GateType::OR) {
        solver.add(inA); solver.add(inB); solver.add(-out); solver.add(0);
        solver.add(-inA); solver.add(out); solver.add(0);
        solver.add(-inB); solver.add(out); solver.add(0);
    } 
    else if (type == GateType::NOR) {
        solver.add(inA); solver.add(inB); solver.add(out); solver.add(0);
        solver.add(-inA); solver.add(-out); solver.add(0);
        solver.add(-inB); solver.add(-out); solver.add(0);
    } 
    else if (type == GateType::NOT) {
        solver.add(inA); solver.add(out); solver.add(0);
        solver.add(-inA); solver.add(-out); solver.add(0);
    }
    else if (type == GateType::BUF) {
        solver.add(-inA); solver.add(out); solver.add(0);
        solver.add(inA); solver.add(-out); solver.add(0);
    }
    else if (type == GateType::XOR) {
        solver.add(-inA); solver.add(-inB); solver.add(-out); solver.add(0);
        solver.add(inA);  solver.add(inB);  solver.add(-out); solver.add(0);
        solver.add(inA);  solver.add(-inB); solver.add(out);  solver.add(0);
        solver.add(-inA); solver.add(inB);  solver.add(out);  solver.add(0);
    }
    else if (type == GateType::XNOR) {
        solver.add(-inA); solver.add(-inB); solver.add(out);  solver.add(0);
        solver.add(inA);  solver.add(inB);  solver.add(out);  solver.add(0);
        solver.add(inA);  solver.add(-inB); solver.add(-out); solver.add(0);
        solver.add(-inA); solver.add(inB);  solver.add(-out); solver.add(0);
    }
}

// 精確合成引擎 (SAT-based Exact Synthesis)
// 負責無中生有，用指定的積木數量拼湊出符合 targetTruthTable 的電路
bool TechMapper::synthesizeFromTruthTable(const std::vector<bool>& targetTruthTable, 
                                         int N, 
                                         const std::map<GateType, int>& allowedConstraints, 
                                         TechMapReport& report, 
                                         bool verbose) {
    // Flatten the gate list
    std::vector<GateType> baseGateArray;
    for (const auto& pair : allowedConstraints) {
        for (int i = 0; i < pair.second; ++i) baseGateArray.push_back(pair.first);
    }
    
    int M = baseGateArray.size();
    if (M == 0) return false;

    size_t numRows = 1ULL << N;
    if (targetTruthTable.size() != numRows) return false;

    // 必須先進行排序，這是 std::next_permutation 的前置條件，
    // 也能確保我們從字典序最小的拓樸排列開始尋找。
    std::sort(baseGateArray.begin(), baseGateArray.end());

    TimeLimitTerminator terminator(60.0); 

    // 窮舉邏輯閘的所有拓樸排列 (Topological Permutations)
    // 解決 Enum 順序造成的有向無環圖 (DAG) 依賴性鎖死問題！
    do {
        CaDiCaL::Solver solver;
        solver.connect_terminator(&terminator);

        // 使用當前排列的 Gate 順序
        const std::vector<GateType>& gateArray = baseGateArray;

        int var_counter = 1;
        
        std::vector<std::vector<int>> F(N + M, std::vector<int>(numRows));
        for (int n = 0; n < N + M; ++n)
            for (size_t r = 0; r < numRows; ++r) F[n][r] = var_counter++;

        std::vector<std::vector<std::vector<int>>> C(M);
        std::vector<std::vector<std::vector<int>>> P(M);

        for (int g = 0; g < M; ++g) {
            GateType type = gateArray[g];
            int num_pins = (type == GateType::NOT || type == GateType::BUF) ? 1 : 2;
            C[g].resize(num_pins); 
            P[g].resize(num_pins);
            
            for (int p = 0; p < num_pins; ++p) {
                int num_sources = N + g; 
                C[g][p].resize(num_sources);
                for (int s = 0; s < num_sources; ++s) C[g][p][s] = var_counter++;
                
                P[g][p].resize(numRows);
                for (size_t r = 0; r < numRows; ++r) P[g][p][r] = var_counter++;
            }
        }

        solver.resize(var_counter - 1);

        // --- Constraint 1: Initialize Primary Inputs ---
        for (int i = 0; i < N; ++i) {
            for (size_t r = 0; r < numRows; ++r) {
                bool bitValue = (r & (1ULL << i)) != 0;
                solver.add(bitValue ? F[i][r] : -F[i][r]);
                solver.add(0);
            }
        }

        // --- Constraint 2: Connectivity (Exactly one source per pin) ---
        for (int g = 0; g < M; ++g) {
            for (int p = 0; p < C[g].size(); ++p) {
                // At least one
                for (int s = 0; s < C[g][p].size(); ++s) solver.add(C[g][p][s]);
                solver.add(0);
                
                // At most one
                for (int s1 = 0; s1 < C[g][p].size(); ++s1) {
                    for (int s2 = s1 + 1; s2 < C[g][p].size(); ++s2) {
                        solver.add(-C[g][p][s1]); solver.add(-C[g][p][s2]); solver.add(0);
                    }
                }
            }
            
            // Commutativity Symmetry Breaking
            if (C[g].size() == 2) {
                GateType t = gateArray[g];
                if (t == GateType::AND || t == GateType::OR || t == GateType::NAND || 
                    t == GateType::NOR || t == GateType::XOR || t == GateType::XNOR) {
                    for (int s1 = 0; s1 < C[g][0].size(); ++s1) {
                        // CHANGED to < to allow s1 == s2 (e.g., A AND A)
                        for (int s2 = 0; s2 < s1; ++s2) {
                            solver.add(-C[g][0][s1]); solver.add(-C[g][1][s2]); solver.add(0);
                        }
                    }
                }
            }
        }

        // --- Constraint 3: Signal Propagation ---
        for (int g = 0; g < M; ++g) {
            int num_pins = C[g].size();
            for (size_t r = 0; r < numRows; ++r) {
                // Link connection choices C to the actual logic values P
                for (int p = 0; p < num_pins; ++p) {
                    for (int s = 0; s < C[g][p].size(); ++s) {
                        solver.add(-C[g][p][s]); solver.add(-P[g][p][r]); solver.add(F[s][r]); solver.add(0);
                        solver.add(-C[g][p][s]); solver.add(P[g][p][r]); solver.add(-F[s][r]); solver.add(0);
                    }
                }
                
                // Apply gate truth table logic
                int out = F[N + g][r];
                int a = P[g][0][r];
                int b = (num_pins == 2) ? P[g][1][r] : 0; // 0 is safe here, addGateSemantics handles 1-input gates
                addGateSemantics(solver, gateArray[g], a, b, out);
            }
        }

        // --- Constraint 4: Target Output ---
        for (size_t r = 0; r < numRows; ++r) {
            solver.add(targetTruthTable[r] ? F[N + M - 1][r] : -F[N + M - 1][r]);
            solver.add(0);
        }

        // Constraint 5: 無懸空輸入 (No Dangling Primary Inputs)
        // 確保真值表的每一個輸入變數，都真實地參與了邏輯電路的運算。
        // 這對 Exact Synthesis 極為重要，能大幅縮減搜索空間，防止引擎在錯誤的對稱分支中迷失導致 UNSAT。
        for (int s = 0; s < N; ++s) {
            for (int g = 0; g < M; ++g) {
                for (int p = 0; p < C[g].size(); ++p) {
                    // 將所有「有機會接到 PI s」的連接變數加入子句
                    solver.add(C[g][p][s]);
                }
            }
            solver.add(0); // 至少有一個連接變數必須為 True
        }

        // ================= Solve =================
        int result = solver.solve();

        if (result == 10) { // SAT
            if (verbose) std::cout << "[Info] Exact synthesis found a valid topology!\n";
            
            report.status = TechMapStatus::SUCCESS;
            report.message = "Success: Exact synthesis found a valid topology.";
            report.synthesizedTopology.clear();
            
            for (int g = 0; g < M; ++g) {
                SynthesizedGate synGate;
                synGate.type = gateArray[g];
                for (int p = 0; p < C[g].size(); ++p) {
                    for (int s = 0; s < C[g][p].size(); ++s) {
                        if (solver.val(C[g][p][s]) > 0) {
                            synGate.inputSourceIds.push_back(s);
                            break; 
                        }
                    }
                }
                report.synthesizedTopology.push_back(synGate);
            }
            return true; // 找到了就立刻下班！
        } 
        else if (result == 0) { // TIMEOUT
            report.status = TechMapStatus::ERROR_NOT_EQUIVALENT;
            report.message = "Failed: Solver timed out.";
            return false;
        }

        // UNSAT (result == 20) -> 代表「這種 Gate 排序下無解」，
        // 迴圈會自動進入 std::next_permutation 嘗試下一種排序！

    } while (std::next_permutation(baseGateArray.begin(), baseGateArray.end()));

    // 全部排列都試過了還是 UNSAT
    if (verbose) std::cout << "[Failed] Mathematically impossible to synthesize the truth table with given constraints.\n";
    report.status = TechMapStatus::ERROR_UNSAT;
    report.message = "Failed (UNSAT): Mathematically impossible to synthesize the truth table with the given constraints.";
    return false;
}

// 將 SAT 算出來的抽象拓樸轉換為樹 (Replacement Pattern)
std::shared_ptr<PatternNode> TechMapper::buildPatternFromTopology(int N, const std::vector<SynthesizedGate>& topology) {
    if (topology.empty()) return nullptr;
    int M = topology.size();

    // 準備一個陣列存放所有的 PatternNode (前 N 個是 PI，後 M 個是 Gate)
    std::vector<std::shared_ptr<PatternNode>> nodes(N + M);

    // 初始化 PI (Leaves)
    for (int i = 0; i < N; ++i) {
        nodes[i] = createLeafNode(i);
    }

    // 依照拓樸順序建立 Gate Nodes (DAG)
    for (int g = 0; g < M; ++g) {
        const SynthesizedGate& synGate = topology[g];
        std::vector<std::shared_ptr<PatternNode>> inputs;
        
        // 抓出這個 Gate 的輸入節點 (可能來自 PI 或前面的 Gate)
        for (int srcId : synGate.inputSourceIds) {
            inputs.push_back(nodes[srcId]);
        }

        nodes[N + g] = std::make_shared<PatternNode>(PatternNode{NodeType::GATE, synGate.type, inputs});
    }

    // 拓樸陣列的最後一個 Gate 就是整個電路的 Root
    return nodes[N + M - 1];
}

// 核心引擎：窮舉所有合法的 LHS 拓樸 (支援 DAG)
std::vector<std::shared_ptr<PatternNode>> TechMapper::generateAllValidTopologies(const std::map<GateType, int>& gateCounts, 
                                                                                 int N,
                                                                                 TechMapReport& report, 
                                                                                 bool verbose) const {
    std::vector<std::shared_ptr<PatternNode>> validTopologies;

    // 將積木攤平為一維陣列
    std::vector<GateType> gateArray;
    for (const auto& pair : gateCounts) {
        for (int i = 0; i < pair.second; ++i) gateArray.push_back(pair.first);
    }
    
    int M = gateArray.size();
    if (M == 0) {
        if (verbose) std::cout << "[Info] LHS gate count is 0. Returning empty topology.\n";
        return validTopologies;
    }

    // 將積木排序 (群聚相同類型的閘，有助於 SAT 引擎的內部優化)
    std::sort(gateArray.begin(), gateArray.end());

    // 全局的時間限制器 (例如設定為 40 秒)
    TimeLimitTerminator terminator(40.0);

    // 單一的陣列順序，搭配 "後面可以接前面" 的接線策略，已經足夠產生該組合下所有的圖形拓樸！
    // 這可以避免產生海量 (M!) 的同構重複解。

    CaDiCaL::Solver solver;
    solver.connect_terminator(&terminator);
    int var_counter = 1;

    // C[g][p][s] 代表：閘 g 的 第 p 個 pin，連接到了 來源 s
    std::vector<std::vector<std::vector<int>>> C(M);

    for (int g = 0; g < M; ++g) {
        int num_pins = getGateFaninCount(gateArray[g]);
        C[g].resize(num_pins);
        
        for (int p = 0; p < num_pins; ++p) {
            int num_sources = N + g; // 只能接 PI 或前面的閘 (保證無 Cycle)
            C[g][p].resize(num_sources);
            for (int s = 0; s < num_sources; ++s) {
                C[g][p][s] = var_counter++;
            }
        }
    }

    // 預約變數空間
    solver.resize(var_counter - 1);

    // ================= SAT 約束 (Constraints) =================

    // 條件 1：每個 Pin 「恰好」只能連接一個來源 (Exactly-One)
    for (int g = 0; g < M; ++g) {
        for (int p = 0; p < C[g].size(); ++p) {
            // At-least-one (至少接一個)
            for (int s = 0; s < C[g][p].size(); ++s) solver.add(C[g][p][s]);
            solver.add(0);

            // At-most-one (最多接一個)
            for (int s1 = 0; s1 < C[g][p].size(); ++s1) {
                for (int s2 = s1 + 1; s2 < C[g][p].size(); ++s2) {
                    solver.add(-C[g][p][s1]); 
                    solver.add(-C[g][p][s2]); 
                    solver.add(0);
                }
            }
        }
    }

    // 條件 2：無懸空閘 (No Dangling Gates) - 保證圖形只有一個 Root
    for (int g = 0; g < M - 1; ++g) {
        int gate_node_id = N + g;
        for (int h = g + 1; h < M; ++h) {
            for (int p = 0; p < C[h].size(); ++p) {
                // 如果 h 的 pin p 接到了 g，就成立
                solver.add(C[h][p][gate_node_id]);
            }
        }
        solver.add(0); // 至少有一個後面的閘接了這顆閘的輸出
    }

    // 條件 3：交換律對稱性消除 (Commutativity Symmetry Breaking)
    for (int g = 0; g < M; ++g) {
        if (C[g].size() == 2 && isCommutative(gateArray[g])) {
            for (int s1 = 0; s1 < C[g][0].size(); ++s1) {
                // 改為 s2 < s1，允許 s1 == s2 (也就是 A AND A 這種接法)
                for (int s2 = 0; s2 < s1; ++s2) { 
                    solver.add(-C[g][0][s1]); 
                    solver.add(-C[g][1][s2]); 
                    solver.add(0);
                }
            }
        }
    }

    // 條件 4：無懸空輸入 (No Dangling Primary Inputs)
    // 保證所有的 PI (來源 0 到 N-1) 都至少連接到某個閘的某個 Pin 上
    // 這能直接在 SAT 引擎內部消滅退化拓樸，避免組合爆炸與 Timeout！
    for (int s = 0; s < N; ++s) {
        for (int g = 0; g < M; ++g) {
            for (int p = 0; p < C[g].size(); ++p) {
                // 將所有「有機會接到來源 s」的變數收集起來
                solver.add(C[g][p][s]);
            }
        }
        solver.add(0); // 這些變數中，至少必須有一個為 True (At-least-one)
    }

    // ================= 進入求解迴圈 =================
    while (true) {
        int result = solver.solve();

        if (result == 10) { // SAT：找到一組合法拓樸
            std::vector<std::shared_ptr<PatternNode>> nodes(N + M);
            for (int i = 0; i < N; ++i) nodes[i] = createLeafNode(i);

            for (int g = 0; g < M; ++g) {
                std::vector<std::shared_ptr<PatternNode>> inputs;
                for (int p = 0; p < C[g].size(); ++p) {
                    for (int s = 0; s < C[g][p].size(); ++s) {
                        if (solver.val(C[g][p][s]) > 0) {
                            inputs.push_back(nodes[s]);
                            break;
                        }
                    }
                }
                nodes[N + g] = std::make_shared<PatternNode>(PatternNode{NodeType::GATE, gateArray[g], inputs});
            }
            
            validTopologies.push_back(nodes[N + M - 1]);

            // 加入 Blocking Clause (禁止產生一模一樣的接線圖)
            std::vector<int> blocking_literals;

            // 在 SATISFIED 狀態下，安全地讀取並收集所有為 True 的變數
            for (int g = 0; g < M; ++g) {
                for (int p = 0; p < C[g].size(); ++p) {
                    for (int s = 0; s < C[g][p].size(); ++s) {
                        if (solver.val(C[g][p][s]) > 0) {
                            blocking_literals.push_back(-C[g][p][s]); // 收集要反轉的變數
                        }
                    }
                }
            }

            // 安全地改變 Solver 的狀態，加入新約束
            for (int literal : blocking_literals) {
                solver.add(literal);
            }
            solver.add(0); // 結束這個 Clause
        } 
        else if (result == 0) { 
            // UNKNOWN: 被 Terminator 強制中斷
            report.message = "Warning: LHS Topology generation timed out! Returning " + std::to_string(validTopologies.size()) + " topologies found so far.";
            if (verbose) std::cout << "[Warning] " << report.message << "\n";
            return validTopologies; 
        } 
        else { 
            // UNSAT: 所有可能的合法 DAG 都已經找完了
            break; 
        }
    }

    return validTopologies;
}

// 核心遞迴引擎：計算單一節點在「給定輸入向量」下的布林值 (支援 DAG 快取)
bool TechMapper::evaluateNode(const std::shared_ptr<PatternNode>& node, 
                              const std::vector<bool>& inputVector,
                              std::unordered_map<PatternNode*, bool>& memo,
                              TechMapReport& report) const {

    // 如果其他遞迴分支已經報錯，直接短路中斷 (Short-circuit)
    if (report.status != TechMapStatus::SUCCESS) return false;

    if (!node) {
        // 【發生錯誤】立刻更改狀態並寫入原因
        report.status = TechMapStatus::ERROR_SIMULATION_FAILED;
        report.message = "Simulation Failed: Encountered null node.";
        return false; 
    }

    // 如果這個節點在這一輪已經算過了，直接回傳結果 (專為 DAG 設計)
    auto it = memo.find(node.get());
    if (it != memo.end()) {
        return it->second;
    }

    bool result = false;

    // 判斷節點類型並求值
    if (node->nodeType == NodeType::CONST_0) {
        result = false;
    } 
    else if (node->nodeType == NodeType::CONST_1) {
        result = true;
    } 
    else if (node->nodeType != NodeType::GATE) {
        int idx = getLeafIndex(node->nodeType);
        if (idx < 0 || idx >= inputVector.size()) {
            report.status = TechMapStatus::ERROR_UNSAT;
            report.message = "Simulation Failed: Leaf index out of range for current N.";
            return false;
        }
        result = inputVector[idx];
    }
    else {
        std::vector<bool> inVals;
        inVals.reserve(node->inputs.size());
        for (const auto& inNode : node->inputs) {
            inVals.push_back(evaluateNode(inNode, inputVector, memo, report));
            // 每次檢查是否有錯誤，有的話提早抽身
            if (report.status != TechMapStatus::SUCCESS) return false;
        }

        switch (node->gateType) {
            case GateType::BUF:   result = inVals[0]; break;
            case GateType::NOT:   result = !inVals[0]; break;
            case GateType::AND:   result = inVals[0] && inVals[1]; break;
            case GateType::OR:    result = inVals[0] || inVals[1]; break;
            case GateType::NAND:  result = !(inVals[0] && inVals[1]); break;
            case GateType::NOR:   result = !(inVals[0] || inVals[1]); break;
            case GateType::XOR:   result = inVals[0] ^ inVals[1]; break;
            case GateType::XNOR:  result = !(inVals[0] ^ inVals[1]); break;
            default:
                report.status = TechMapStatus::ERROR_SIMULATION_FAILED;
                report.message = "Simulation Failed: Unsupported GateType.";
                return false;
        }
    }

    // 算完後存入快取，並回傳
    memo[node.get()] = result;
    return result;
}

// 對單一 Pattern 進行完整的布林邏輯模擬，回傳長度為 2^N 的 Truth Table
std::vector<bool> TechMapper::simulatePattern(const std::shared_ptr<PatternNode>& root, 
                                              int N,
                                              TechMapReport& report,
                                              bool verbose) const {
    size_t numRows = 1ULL << N;
    std::vector<bool> truthTable(numRows);

    // 跑 2^N 次迴圈，填滿真值表
    for (size_t r = 0; r < numRows; ++r) {
        
        // 將整數 r 轉換為當前列的布林輸入向量
        std::vector<bool> inputVector(N);
        for (int i = 0; i < N; ++i) {
            // 透過位元遮罩擷取第 i 個 bit
            inputVector[i] = (r & (1ULL << i)) != 0;
        }

        // 準備該輪的快取 (每個 Row 必須使用全新的快取)
        std::unordered_map<PatternNode*, bool> memo;

        // 呼叫引擎求出該列的最終輸出值
        truthTable[r] = evaluateNode(root, inputVector, memo, report);

        // 如果在遞迴中發生錯誤，中斷整個 Truth Table 的生成
        if (report.status != TechMapStatus::SUCCESS) {
            if (verbose) {
                std::cout << "[Error] " << report.message << "\n";
            }
            return {}; // 失敗時回傳空陣列
        }
    }

    if (verbose) {
        std::cout << "[Info] Truth table successfully extracted (Rows: " << numRows << ").\n";
    }

    return truthTable;
}

// 自動合成並註冊等價的 TechMapRule
bool TechMapper::createAndRegisterCustomRule(const std::string& ruleName,
                                             const std::map<GateType, int>& lhsConstraints,
                                             const std::map<GateType, int>& rhsConstraints,
                                             TechMapReport& finalReport, // 統一的狀態回報出口
                                             bool verbose) {
    // 計算所需的外部輸入數量 (N)
    int N = calculateMaxInputs(lhsConstraints);
    if (verbose) {
        std::cout << "=================================================\n";
        std::cout << "[Generate Rule] Starting process for: " << ruleName << "\n";
    }

    TechMapReport topoReport;
    auto lhsCandidates = generateAllValidTopologies(lhsConstraints, N, topoReport, verbose);

    if (lhsCandidates.empty()) {
        finalReport.status = TechMapStatus::ERROR_RULE_NOT_FOUND;
        finalReport.message = "Failed: Cannot generate any valid LHS topologies.";
        return false;
    }

    // 輔助函式：計算真實的閘數
    auto countGates = [](std::shared_ptr<PatternNode> root) {
        std::map<GateType, int> counts;
        std::unordered_set<PatternNode*> visited;
        std::vector<std::shared_ptr<PatternNode>> queue = {root};
        int totalCount = 0;
        
        while (!queue.empty()) {
            auto curr = queue.back();
            queue.pop_back();
            
            if (!curr || visited.count(curr.get())) continue;
            visited.insert(curr.get());
            
            if (curr->nodeType == NodeType::GATE) {
                counts[curr->gateType]++;
                totalCount++;
            }
            for (const auto& next : curr->inputs) {
                if (next) queue.push_back(next);
            }
        }
        return std::make_pair(counts, totalCount);
    };

    int candidateIdx = 0;
    // 新增追蹤變數，以便找出所有合法的rule
    bool foundAnyValidMapping = false; 
    int successCount = 0; // 順便算一下總共學到了幾種變體

    for (const auto& lhsRoot : lhsCandidates) {
        candidateIdx++;
        
        TechMapReport simReport;
        std::vector<bool> truthTable = simulatePattern(lhsRoot, N, simReport, false); 
        if (simReport.status != TechMapStatus::SUCCESS || truthTable.empty()) {
            // if (verbose) std::cout << "[Debug] Candidate #" << candidateIdx << " died at: Simulation Failed.\n";
            continue; 
        } 

        // 核心防護 1：真值表變數依賴性檢查 (過濾 A AND A 這種退化拓樸)
        // 確保每一個 PI (Primary Input) 都能對輸出產生影響
        bool dependsOnAll = true;
        for (int i = 0; i < N; ++i) {
            bool depends = false;
            int step = 1 << i; // 對應第 i 個變數的切換步長
            for (int j = 0; j < (1 << N); j += (step * 2)) {
                for (int k = 0; k < step; ++k) {
                    if (truthTable[j + k] != truthTable[j + k + step]) {
                        depends = true; // 只要有一組輸入會因為變數 i 切換而改變輸出，就代表有依賴
                        break;
                    }
                }
                if (depends) break;
            }
            if (!depends) { 
                dependsOnAll = false; 
                break; 
            }
        }
        
        // 如果這個拓樸有「懸空/無用」的輸入分支，代表它不具代表性，跳過！
        if (!dependsOnAll) {
            // if (verbose) std::cout << "[Debug] Candidate #" << candidateIdx << " died at: dependsOnAll (Degenerate/Floating inputs). N = " << N << "\n";
            continue; 
        }

        // 送進去合成
        TechMapReport synReport;
        bool success = synthesizeFromTruthTable(truthTable, N, rhsConstraints, synReport, false);
        if (!success || synReport.status != TechMapStatus::SUCCESS) {
            // if (verbose) std::cout << "[Debug] Candidate #" << candidateIdx << " died at: Synthesis UNSAT.\n";
            continue;
        }

        if (success && synReport.status == TechMapStatus::SUCCESS) {
            auto rhsRoot = buildPatternFromTopology(N, synReport.synthesizedTopology);
            if (!rhsRoot) continue;

            auto rhsData = countGates(rhsRoot);

            // 核心防護 2：嚴格檢查 RHS 數量，拒絕 SAT 擅自優化
            bool strictMatch = true;
            for (const auto& pair : rhsConstraints) {
                GateType reqType = pair.first;
                int reqAmount = pair.second;
                
                // 如果不是 -1，我們就強制比對物理算出來的數量
                if (reqAmount != -1) {
                    int actualAmount = rhsData.first.count(reqType) ? rhsData.first[reqType] : 0;
                    if (actualAmount != reqAmount) {
                        strictMatch = false;
                        // if (verbose) std::cout << "[Debug] Candidate #" << candidateIdx << " died at: strictMatch. " ;
                        break;
                    }
                }
            }

            // 如果 SAT 擅自減少了 Gate 數量，不符合使用者的精確要求，就淘汰換下一個拓樸
            if (!strictMatch) {
                if (verbose) std::cout << "  -> [Skip] RHS synthesis succeeded, but gate counts did not strictly match user constraints.\n";
                continue;
            }

            auto lhsData = countGates(lhsRoot);

            // 註冊規則
            // 在命名上加上 Variant 編號，這樣規則庫名字才不會全部重複
            std::string variantName = ruleName + "_Variant_" + std::to_string(successCount + 1);
            TechMapRule forwardRule(variantName + "_Forward", lhsRoot, rhsRoot);
            forwardRule.targetCounts = lhsData.first; 
            forwardRule.allowedCounts = rhsData.first; // 綁定真實數量 (已經通過 strictMatch 檢查)
            forwardRule.removedGateCount = lhsData.second;
            forwardRule.addedGateCount = rhsData.second;
            rules.push_back(forwardRule);

            TechMapRule backwardRule(variantName + "_Backward", rhsRoot, lhsRoot);
            backwardRule.targetCounts = rhsData.first;
            backwardRule.allowedCounts = lhsData.first;
            backwardRule.removedGateCount = rhsData.second;
            backwardRule.addedGateCount = lhsData.second;
            rules.push_back(backwardRule);

            finalReport.status = TechMapStatus::SUCCESS;
            finalReport.message = "Successfully created and registered bidirectional rule: " + ruleName;
            // 標記成功
            foundAnyValidMapping = true;
            successCount++;

            if (verbose) {
                std::cout << "  -> [Success] Learned exact matching rule from LHS candidate #" << candidateIdx << "!\n";
                std::cout << "=================================================\n";
            }
        } 
    }
    // 迴圈全部跑完後，根據追蹤變數決定最終結果
    if (foundAnyValidMapping) {
        finalReport.status = TechMapStatus::SUCCESS;
        finalReport.message = "Successfully created and registered " + std::to_string(successCount) + " bidirectional rule variants for: " + ruleName;
        if (verbose) {
            std::cout << "[Info] " << finalReport.message << "\n";
            std::cout << "=================================================\n";
        }
        return true;
    }

    // 全部都 UNSAT 或被 strictMatch 擋下來了
    finalReport.status = TechMapStatus::ERROR_UNSAT;
    finalReport.message = "Exhausted all LHS candidates. None could strictly satisfy the given conditions.";
    if (verbose) std::cout << "=================================================\n";
    return false;
}

// 核心輔助函式：給定「允許使用的基礎閘」，自動把其他所有的組合邏輯閘拆解
// 根據 TargetScope 解析 Cone，並呼叫底層引擎
// 給定「允許使用的基礎閘」，自動把其他所有的組合邏輯閘拆解
TechMapReport TechMapper::convertToBasis(Netlist& netlist, 
                                         const std::vector<GateType>& allowedTypes, 
                                         TargetScope scope, 
                                         const std::string& name, 
                                         bool verbose) {

    TechMapReport finalReport;
    finalReport.status = TechMapStatus::SUCCESS;
    finalReport.message = "Successfully converted circuit to the specified basis.";

    // 建立允許清單 (allowedConstraints: 數量 -1 代表無限制)
    std::map<GateType, int> allowedConstraints;
    for (GateType t : allowedTypes) {
        allowedConstraints[t] = -1;
    }

    std::vector<GateType> allCombinational = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
        GateType::NOT, GateType::BUF, GateType::XOR, GateType::XNOR
    };

    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());
    std::vector<GateType> targetsToRemove;

    // 找出所有「不合法」的 Gate 類型
    for (GateType type : allCombinational) {
        if (allowedSet.find(type) == allowedSet.end()) {
            targetsToRemove.push_back(type);
        }
    }

    // 迴圈依序消滅每一種不合法的 Gate
    for (GateType targetType : targetsToRemove) {
        
        // 強制設為 1，確保引擎進入「拆解 (1-to-N)」模式
        // 這樣就不會讓引擎在多對多 (M:N) 的邏輯中不知所措
        std::map<GateType, int> targetConstraints = {{targetType, 1}};

        if (verbose) {
            std::cout << "\n[Basis Conversion] Eliminating GateType: " << (int)targetType << "...\n";
        }

        // 使用 switch 搭配基礎的 API (不走 customMapTechnology)
        TechMapReport stepReport;
        
        switch (scope) {
            case TargetScope::WHOLE_NETLIST:
                stepReport = mapTechnology(netlist, targetConstraints, allowedConstraints, verbose);
                break;
            case TargetScope::NET_FANIN:
                stepReport = mapTechnologyForCone(netlist, targetConstraints, allowedConstraints, netlist.getTransitiveFaninCone(name), verbose);
                break;
            case TargetScope::NET_FANOUT:
                stepReport = mapTechnologyForCone(netlist, targetConstraints, allowedConstraints, netlist.getTransitiveFanoutCone(name), verbose);
                break;
            case TargetScope::GATE_FANIN:
                stepReport = mapTechnologyForCone(netlist, targetConstraints, allowedConstraints, netlist.getGateTransitiveFaninCone(name), verbose);
                break;
            case TargetScope::GATE_FANOUT:
                stepReport = mapTechnologyForCone(netlist, targetConstraints, allowedConstraints, netlist.getGateTransitiveFanoutCone(name), verbose);
                break;
            default:
                finalReport.status = TechMapStatus::ERROR_NOT_EQUIVALENT;
                finalReport.message = "Failed: Invalid TargetScope.";
                return finalReport;
        }

        // 檢查該步驟是否失敗
        // 只有當電路中還殘留該種類的 Gate 時，才回報錯誤 (代表 LUT 真的缺了這條展開規則)
        if (stepReport.status != TechMapStatus::SUCCESS) {
            if (netlist.getGateCountByType(targetType) > 0) {
                finalReport.status = stepReport.status;
                finalReport.message = "Basis conversion failed at type: " + std::to_string((int)targetType);
                return finalReport;
            }
        }

        // 累加本回合的 Report 統計
        for (const auto& pair : stepReport.removedCountByType) {
            finalReport.removedCountByType[pair.first] += pair.second;
        }
        for (const auto& pair : stepReport.addedCountByType) {
            finalReport.addedCountByType[pair.first] += pair.second;
        }
        
        if (verbose && !stepReport.modifiedGateNames.empty()) {
            finalReport.modifiedGateNames.insert(
                finalReport.modifiedGateNames.end(),
                stepReport.modifiedGateNames.begin(),
                stepReport.modifiedGateNames.end()
            );
        }
    }

    // 更新最終快照
    for (GateType t : allCombinational) {
        finalReport.finalGateCount[t] = netlist.getGateCountByType(t);
    }

    return finalReport;
}

// 將整個 netlist 轉成 {AND, NOT} (AIG: And-Inverter Graph)
TechMapReport TechMapper::convertToAndNot(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    return convertToBasis(netlist, {GateType::AND, GateType::NOT}, scope, name, verbose);
}

NetlistEditReport TechMapper::convertToAndNotWithReport(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = convertToAndNot(netlist, scope, name, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "convertToAndNot");
}

// 將整個 netlist 轉成 {OR, NOT} (OIG: Or-Inverter Graph)
TechMapReport TechMapper::convertToOrNot(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    return convertToBasis(netlist, {GateType::OR, GateType::NOT}, scope, name, verbose);
}

NetlistEditReport TechMapper::convertToOrNotWithReport(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = convertToOrNot(netlist, scope, name, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "convertToOrNot");
}

// 將整個 netlist 轉成 {NAND} (純 NAND 網路)
// 說明：NAND 是 Universal Gate (萬用閘)。在早期 TTL 或現代 CMOS 中，NAND 的電晶體堆疊最少，
// 速度最快，這是一個非常符合物理特性的轉換。
TechMapReport TechMapper::convertToNand(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    return convertToBasis(netlist, {GateType::NAND}, scope, name, verbose);
}

NetlistEditReport TechMapper::convertToNandWithReport(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = convertToNand(netlist, scope, name, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "convertToNand");
}

// 將整個 netlist 轉成 {NOR} (純 NOR 網路)
// 說明：NOR 同樣是 Universal Gate，常用於一些特殊的記憶體周邊控制電路。
TechMapReport TechMapper::convertToNor(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    return convertToBasis(netlist, {GateType::NOR}, scope, name, verbose);
}

NetlistEditReport TechMapper::convertToNorWithReport(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = convertToNor(netlist, scope, name, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "convertToNor");
}

// 將整個 netlist 轉成 XAG (XOR-AND Graph)
// 說明：由 {XOR, AND, NOT} 組成。XAG 在現代 EDA 非常紅！
// 在全同態加密 (FHE) 與量子運算中，XOR 通常是 Free (不用成本) 的，
// 而 AND 需要消耗極大的資源，所以會特別使用 XAG 來做進一步的最佳化。
TechMapReport TechMapper::convertToXag(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    return convertToBasis(netlist, {GateType::XOR, GateType::AND, GateType::NOT}, scope, name, verbose);
}

NetlistEditReport TechMapper::convertToXagWithReport(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = convertToXag(netlist, scope, name, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "convertToXag");
}

// 將整個 netlist 轉成 {XOR, AND} (ANF: Algebraic Normal Form)
// 說明：又稱 Reed-Muller 展開。這是一種沒有 NOT 閘的代數結構！
// 引擎非常聰明，遇到 NOT 閘時，會自動使用查表裡的 A XOR 1 來替換，
TechMapReport TechMapper::convertToAnf(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    return convertToBasis(netlist, {GateType::XOR, GateType::AND}, scope, name, verbose);
}

NetlistEditReport TechMapper::convertToAnfWithReport(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = convertToAnf(netlist, scope, name, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "convertToAnf");
}

// 將整個 netlist 轉成 {XOR, OR}
// 說明：這是另一種特化的代數基底映射。
TechMapReport TechMapper::convertToXorOr(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    return convertToBasis(netlist, {GateType::XOR, GateType::OR}, scope, name, verbose);
}

NetlistEditReport TechMapper::convertToXorOrWithReport(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = convertToXorOr(netlist, scope, name, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "convertToXorOr");
}

// 將整個 netlist 轉成 {XNOR, AND}
// 說明：XNOR 與 XOR 具有對稱性，在某些 Cell Library 中 XNOR 的面積更小。
// NOT 閘會被自動替換為 A XNOR 0。
TechMapReport TechMapper::convertToXnorAnd(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    return convertToBasis(netlist, {GateType::XNOR, GateType::AND}, scope, name, verbose);
}

NetlistEditReport TechMapper::convertToXnorAndWithReport(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = convertToXnorAnd(netlist, scope, name, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "convertToXnorAnd");
}

// 將整個 netlist 轉成 {XNOR, OR}
// 說明：特化的邏輯合成基底。
TechMapReport TechMapper::convertToXnorOr(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    return convertToBasis(netlist, {GateType::XNOR, GateType::OR}, scope, name, verbose);
}

NetlistEditReport TechMapper::convertToXnorOrWithReport(Netlist& netlist, TargetScope scope, const std::string& name, bool verbose) {
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = convertToXnorOr(netlist, scope, name, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "convertToXnorOr");
}

// 給使用者呼叫的萬用任意修改 API (自動推導方向 + 約束驅動 + 智慧 Fallback)
TechMapReport TechMapper::customMapTechnology(Netlist& netlist, 
                                              const std::map<GateType, int>& targetConstraints, 
                                              const std::map<GateType, int>& allowedConstraints, 
                                              TargetScope scope, 
                                              const std::string& name,
                                              bool verbose) {

    // 確保至少有一個明確的數量基準
    // 如果所有的 constraints 都是 -1 (無限制) 或 0，引擎將失去建構樹狀圖的基準大小
    bool hasValidAnchor = false;
    for (const auto& pair : targetConstraints) {
        if (pair.second > 0) { hasValidAnchor = true; break; }
    }
    if (!hasValidAnchor) {
        for (const auto& pair : allowedConstraints) {
            if (pair.second > 0) { hasValidAnchor = true; break; }
        }
    }

    if (!hasValidAnchor) {
        TechMapReport errReport;
        errReport.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS; 
        errReport.message = "Failed: Invalid constraints. At least one gate type in target or allowed constraints must have a specific count (> 0). Both cannot be entirely unlimited (-1).";
        
        if (verbose) {
            std::cout << "[Error] " << errReport.message << "\n";
            std::cout << "[Info] Aborting custom mapping process.\n";
        }
        return errReport;
    }
    
    // 建立一個 Lambda 函式來封裝底層 API 呼叫，方便我們在學習新規則後重複執行
    auto runMapping = [&]() -> TechMapReport {
        switch (scope) {
            case TargetScope::WHOLE_NETLIST:
                return mapTechnology(netlist, targetConstraints, allowedConstraints, verbose);
            case TargetScope::NET_FANIN:
                return mapTechnologyForCone(netlist, targetConstraints, allowedConstraints, netlist.getTransitiveFaninCone(name), verbose);
            case TargetScope::NET_FANOUT:
                return mapTechnologyForCone(netlist, targetConstraints, allowedConstraints, netlist.getTransitiveFanoutCone(name), verbose);
            case TargetScope::GATE_FANIN:
                return mapTechnologyForCone(netlist, targetConstraints, allowedConstraints, netlist.getGateTransitiveFaninCone(name), verbose);
            case TargetScope::GATE_FANOUT:
                return mapTechnologyForCone(netlist, targetConstraints, allowedConstraints, netlist.getGateTransitiveFanoutCone(name), verbose);
            default:
                TechMapReport errReport;
                errReport.status = TechMapStatus::ERROR_RULE_NOT_FOUND;
                errReport.message = "Failed: Invalid TargetScope provided.";
                return errReport;
        }
    };

    // 第一次嘗試：使用目前的 LUT (內建規則庫) 進行映射
    TechMapReport report = runMapping();

    // 智慧判定是否需要啟動 Fallback (自動發明新規則)
    bool needsSatFallback = false;

    // 狀況 A：LUT 字典裡根本沒有符合的規則
    if (report.status == TechMapStatus::ERROR_RULE_NOT_FOUND) {
        needsSatFallback = true;
    } 
    // 狀況 B：LUT 字典裡有規則，但掃描後發現電路上根本沒有長那個形狀的子圖
    else if (report.status == TechMapStatus::SUCCESS) {
        bool actuallyModified = false;
        for (const auto& pair : targetConstraints) {
            auto it = report.removedCountByType.find(pair.first);
            if (it != report.removedCountByType.end() && it->second > 0) {
                actuallyModified = true; 
                break;
            }
        }
        
        // 如果沒改到東西，代表 LUT 的形狀跟實體電路不吻合，我們啟動 SAT 尋找「其他可能的 LHS 拓樸」
        if (!actuallyModified) {
            needsSatFallback = true;
            if (verbose) {
                std::cout << "[Info] LUT has rule(s), but pattern matching found no targets in the circuit.\n";
                std::cout << "[Info] Forcing Automated Rule Discovery (SAT Fallback)...\n";
            }
        }
    }

    // 執行 SAT Fallback (發明並學習新規則)
    if (needsSatFallback) {
        
        // 找出哪些 Gate 被設定為無限制 (-1)
        std::vector<GateType> unlimitedTargets;
        std::vector<GateType> unlimitedAlloweds;
        for (const auto& pair : targetConstraints) {
            if (pair.second == -1) unlimitedTargets.push_back(pair.first);
        }
        for (const auto& pair : allowedConstraints) {
            if (pair.second == -1) unlimitedAlloweds.push_back(pair.first);
        }

        bool hasAnyUnlimited = !unlimitedTargets.empty() || !unlimitedAlloweds.empty();
        
        // 如果有 -1，我們設定一個搜尋上限防止跑太久 (例如最多用到 4 顆 Gate)
        // 如果沒有 -1，那就只跑 1 次迴圈 (使用原本的確切數量)
        int maxDepth = hasAnyUnlimited ? 4 : 1; 
        
        TechMapReport fallbackReport;
        std::string customRuleName = "AutoLearnedRule_" + std::to_string(rules.size() / 2);
        bool learned = false;

        // 啟動迭代加深搜尋 (Iterative Deepening)
        for (int depth = 1; depth <= maxDepth; ++depth) {
            
            // 建立一份餵給 SAT 的「確切數量」複本
            std::map<GateType, int> exactTargets = targetConstraints;
            std::map<GateType, int> exactAlloweds = allowedConstraints;

            // 將所有的 -1 替換為當前的探索深度 (depth)
            for (GateType gt : unlimitedTargets) exactTargets[gt] = depth;
            for (GateType gt : unlimitedAlloweds) exactAlloweds[gt] = depth;

            if (verbose && hasAnyUnlimited) {
                std::cout << "[Info] SAT Fallback: Trying iterative depth = " << depth << "...\n";
            }

            // 呼叫學習引擎 (記得使用上一篇修正過、沒有 Early Return 盲點的 createAndRegisterCustomRule)
            learned = createAndRegisterCustomRule(
                customRuleName, exactTargets, exactAlloweds, fallbackReport, verbose
            );

            // 如果 SAT 在這個深度找到了合法規則，就立刻跳出迴圈！
            if (learned) {
                break; 
            }
        }

        // 處理學習結果
        if (learned) {
            if (verbose) {
                std::cout << "[Info] New rule learned via SAT! Re-running the mapping engine to apply it...\n";
            }
            // 既然學會了新規則，再呼叫一次 runMapping()，新規則就會被自動套用
            return runMapping(); 
        } else {
            // SAT 經過所有迴圈依然失敗
            fallbackReport.message = "SAT Fallback Failed (exhausted constraints/depths): " + fallbackReport.message;
            return fallbackReport;
        }
    }

    return report;
}

NetlistEditReport TechMapper::customMapTechnologyWithReport(
    Netlist& netlist,
    const std::map<GateType, int>& targetConstraints,
    const std::map<GateType, int>& allowedConstraints,
    TargetScope scope,
    const std::string& name,
    bool verbose) {
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = customMapTechnology(
        netlist, targetConstraints, allowedConstraints, scope, name, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "customMapTechnology");
}
