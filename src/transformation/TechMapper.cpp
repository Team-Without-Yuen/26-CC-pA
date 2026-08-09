#include "include/core/Netlist.h"
#include "include/core/TechMapper.h"
#include "include/SATEngine/SatTime.h"
#include <string>
#include <iostream>
#include <functional>
#include <algorithm>
#include <vector>
#include <queue>
#include <unordered_set>

// 定義常數
constexpr int MAX_AREA = 16;

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

ConeResult coneForScope(Netlist& netlist, TargetScope scope, const std::string& name) {
    return resolveRewriteScope(netlist, scope, name).cone;
}

int countGateTypeInScope(Netlist& netlist, TargetScope scope, const std::string& name, GateType type) {
    if (scope == TargetScope::WHOLE_NETLIST) {
        return netlist.getGateCountByType(type);
    }

    int count = 0;
    ConeResult cone = coneForScope(netlist, scope, name);
    for (int gateId : netlist.getConeGateIds(cone)) {
        if (!netlist.isValidGateId(gateId) || netlist.isGateRemoved(gateId)) {
            continue;
        }
        if (netlist.getGate(gateId).type == type) {
            ++count;
        }
    }
    return count;
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
    } else {
        Netlist::certifyEquivalence(
            report,
            EquivalenceCheckMethod::LocalRewriteRule,
            "Technology mapping uses function-preserving rewrite rules.");
    }

    return report;
}

}

// 輔助函式：走訪 PatternNode ，並計算計算最大輸入數
int TechMapper::countPrimaryInputs(const std::shared_ptr<PatternNode>& root) const {
    if (!root) return 0;

    int maxIndex = -1;
    std::unordered_set<PatternNode*> visited;
    std::vector<std::shared_ptr<PatternNode>> queue = {root};

    while (!queue.empty()) {
        auto curr = queue.back();
        queue.pop_back();

        // 防呆與防止 DAG 重複走訪
        if (!curr || visited.count(curr.get())) continue;
        visited.insert(curr.get());

        // 如果遇到 PI，檢查它的 index
        if (curr->nodeType == NodeType::PI) {
            if (curr->piIndex > maxIndex) {
                maxIndex = curr->piIndex;
            }
        }

        // 繼續走訪子節點
        for (const auto& next : curr->inputs) {
            if (next) queue.push_back(next);
        }
    }

    // 如果 maxIndex 是 3 (代表有 A, B, C, D)，則需要 4 個 Input
    // 如果完全沒找到 PI (全是常數)，回傳 0
    return maxIndex + 1; 
}

// 輔助函式：走訪 PatternNode ，並計算使用到的gate的數量
std::pair<std::map<GateType, int>, int> TechMapper::countGates(const std::shared_ptr<PatternNode>& root) const {
    std::map<GateType, int> counts;
    std::unordered_set<PatternNode*> visited;
    std::vector<std::shared_ptr<PatternNode>> queue = {root};
    int totalCount = 0;
    
    while (!queue.empty()) {
        auto curr = queue.back();
        queue.pop_back();
        
        // 防呆與 DAG 走訪防護
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
}

// 輔助函式：計算邏輯閘的關鍵路徑層數
int TechMapper::calculateDepth(const std::shared_ptr<PatternNode>& node) const {
    // 防呆或到達輸入端 (PI, CONST) 都是 0 層
    if (!node || node->nodeType != NodeType::GATE) {
        return 0;
    }
    
    int maxInputDepth = 0;
    for (const auto& input : node->inputs) {
        int d = calculateDepth(input);
        if (d > maxInputDepth) {
            maxInputDepth = d;
        }
    }
    // 自己的這層 +1
    return maxInputDepth + 1;
}

// 輔助函式：真值表雜湊編碼
std::string TechMapper::generateTruthTableHash(const std::vector<bool>& truthTable) const {
    if (truthTable.empty()) return "EMPTY";

    std::string hashStr = "";
    int currentHex = 0;
    int bitCount = 0;

    // 將 vector<bool> 每 4 個 bit 壓縮成一個 16 進位字元
    for (size_t i = 0; i < truthTable.size(); ++i) {
        if (truthTable[i]) {
            currentHex |= (1 << bitCount);
        }
        bitCount++;

        // 滿 4 個 bit 結算一次
        if (bitCount == 4) {
            if (currentHex < 10) {
                hashStr += std::to_string(currentHex); // 0~9
            } else {
                hashStr += (char)('A' + (currentHex - 10)); // A~F
            }
            currentHex = 0;
            bitCount = 0;
        }
    }

    // 處理最後如果剩下不足 4 個 bit 的尾數
    if (bitCount > 0) {
        if (currentHex < 10) {
            hashStr += std::to_string(currentHex);
        } else {
            hashStr += (char)('A' + (currentHex - 10));
        }
    }

    // 回傳類似 "7F0A" 這種極短的字串
    return hashStr;
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
                                      std::shared_ptr<PatternNode> patternB,
                                      RuleSource source) { // 預設為標準庫
    // 防呆：確保兩棵樹都存在
    if (!patternA || !patternB) return;

    // 自動萃取這條規則的真值表與 Hash
    int N = countPrimaryInputs(patternA);
    TechMapReport dummyReport;
    std::vector<bool> truthTable = simulatePattern(patternA, N, dummyReport, false);
    std::string hash = generateTruthTableHash(truthTable);

    // 註冊正向規則 (帶入 Source 與 Hash)
    rules.emplace_back(baseName + "_Forward", source, hash, patternA, patternB);
    
    // 註冊反向規則 (帶入 Source 與 Hash)
    rules.emplace_back(baseName + "_Backward", source, hash, patternB, patternA);
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
                                                   const std::vector<RuleSource>& allowedSources,
                                                   bool requireAreaReduction) const {
    
    std::vector<TechMapRule> validRules;

    for (const auto& rule : rules) {
        // 使用原本封裝好的正確函式，檢查規則是否符合指定的積木種類與數量
        if (isRuleSatisfyingConstraints(rule, lhsConstraints, rhsConstraints)) {
            
            // 過濾規則來源
            if (!allowedSources.empty()) {
                if (std::find(allowedSources.begin(), allowedSources.end(), rule.source) == allowedSources.end()) {
                    continue; // 該規則的來源不在允許名單內，直接跳過
                }
            }
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

    // 處理常數比對
    if (pNode->nodeType == NodeType::CONST_1 || pNode->nodeType == NodeType::CONST_0) {
        const Net& net = netlist.getNet(physNetId);
        if (!net.isConst) return false;
        std::string expectedName = (pNode->nodeType == NodeType::CONST_1) ? "1'b1" : "1'b0";
        return net.name == expectedName;
    }

    // 處理 Primary Input (LEAF) 節點，使用 piIndex 作為唯一識別
    if (pNode->nodeType == NodeType::PI) {
        if (ctx.boundLeaves.find(pNode->piIndex) == ctx.boundLeaves.end()) {
            // 第一次遇到這個 PI，把物理 Net ID 綁定上去
            ctx.boundLeaves[pNode->piIndex] = physNetId; 
            return true; 
        }
        // 之後遇到同一個 PI (例如 A() 出現第二次)，物理 Net ID 必須是同一條線
        return ctx.boundLeaves[pNode->piIndex] == physNetId; 
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
bool TechMapper::isValidSubgraph(Netlist& netlist, const MatchContext& ctx, int rootGateId, bool allowLogicDuplication) {
    // 遍歷子圖中的每一個 Gate
    for (int gateId : ctx.matchedGates) {
        if (gateId == rootGateId) continue; // Root Gate 的輸出本來就是要接給別人的，合法。

        const Gate& g = netlist.getGate(gateId);
        if (g.outputNetId == -1) continue;

        // 如果允許複製電路 (Logic Duplication)
        // 我們一律放行，不檢查中介閘的外部依賴，因為我們稍後會保留它們！
        if (allowLogicDuplication) {
            continue; 
        }

        const Net& outNet = netlist.getNet(g.outputNetId);
        if (outNet.isPO) return false; // 不能把連到 Primary Output 的中介閘吃掉！

        // 【最精華的 Fan-out 檢查】(僅在不允許 Duplication 時嚴格執行)
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
void TechMapper::applyRule(Netlist& netlist,
                           const MatchContext& ctx,
                           int rootGateId,
                           const TechMapRule& rule,
                           TechMapReport& report,
                           bool allowLogicDuplication,
                           int* outFinalNetId) {

    /*std::cout << "\n[Debug Apply] Applying rule " << rule.name << " to Root Gate " << rootGateId << "\n";
    std::cout << "  -> RHS Root Node Type: " << (int)rule.replacementPattern->nodeType << "\n";*/

    const Gate& rootGate = netlist.getGate(rootGateId);
    int origOutNetId = rootGate.outputNetId;
    std::string baseInstName = rootGate.instName + "_opt";

    // 計算 Logic Duplication 的「連鎖保留」集合
    std::unordered_set<int> preservedGates;
    
    if (allowLogicDuplication) {
        bool changed;
        do {
            changed = false;
            for (int gateId : ctx.matchedGates) {
                if (gateId == rootGateId) continue; // Root Gate 永遠會被新電路取代，所以絕對不保留
                if (preservedGates.count(gateId)) continue; // 已經標記保留了，跳過
                
                const Gate& g = netlist.getGate(gateId);
                if (g.outputNetId == -1) continue;
                
                const Net& outNet = netlist.getNet(g.outputNetId);
                bool isNeeded = false;
                
                if (outNet.isPO) {
                    isNeeded = true;
                } else {
                    for (int loadId : outNet.loadGateIds) {
                        // 判斷是否被外部需要：
                        // 條件 A: Load 不在這次匹配的子圖內 (真正的外部依賴)
                        // 條件 B: Load 雖然在子圖內，但它已經被判定為「需要保留」 (連鎖依賴)
                        if (ctx.matchedGates.find(loadId) == ctx.matchedGates.end() || 
                            preservedGates.count(loadId)) {
                            
                            // 確保那個 Load 還是活著的，沒有在先前的替換中被移除
                            if (!netlist.isGateRemoved(loadId)) { 
                                isNeeded = true;
                                break;
                            }
                        }
                    }
                }
                
                if (isNeeded) {
                    preservedGates.insert(gateId);
                    changed = true; // 有新的 Gate 被保留，必須再掃描一次看是否影響更前級的 Gate
                }
            }
        } while (changed);
    }

    // 執行惰性刪除 (Lazy Deletion) 與統計
    std::vector<int> internalNetsToRemove;

    for (int gateId : ctx.matchedGates) {
        if (preservedGates.count(gateId)) {
            // Logic Duplication 發生！保留這個 Gate 供外部 / 下游使用，不標記刪除
            continue;
        }

        // 安全拔除
        const Gate& g = netlist.getGate(gateId);
        
        // 只有真的被拔掉的，才計入 report 的 removedCount (修正了原本外層迴圈誤判的問題)
        report.removedCountByType[g.type]++;

        // 抓出這顆 Gate 的輸出線。如果是內部線，記下來準備刪除。
        if (g.outputNetId != -1 && g.outputNetId != origOutNetId) {
            internalNetsToRemove.push_back(g.outputNetId);
        }
        
        // 拔除 Gate (你的底層實作應該會將它的 isRemoved 設為 true)
        netlist.removeGate(gateId); 
    }

    // 準備遞迴引擎，生成新的子圖 (RHS)
    // Memoization 表：解決 RHS 中也有 DAG (共用節點) 的問題
    std::unordered_map<PatternNode*, int> visited;

    std::function<int(std::shared_ptr<PatternNode>, bool)> buildNode = 
        [&](std::shared_ptr<PatternNode> node, bool isRoot) -> int {
        
        if (visited.count(node.get())) return visited[node.get()];

        int outNetId = -1;

        if (node->nodeType == NodeType::PI) {
            outNetId = ctx.getBoundNet(node->piIndex);
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
            std::vector<int> childNetIds;
            for (auto& child : node->inputs) {
                childNetIds.push_back(buildNode(child, false));
            }

            std::string newInstName = baseInstName + "_" + std::to_string(visited.size());
            int newGateId = netlist.addGate(newInstName, node->gateType);

            for (int cNetId : childNetIds) {
                netlist.connectGateInput(newGateId, cNetId);
            }

            if (isRoot) {
                if (origOutNetId != -1) {
                    netlist.connectGateOutput(newGateId, origOutNetId);
                    outNetId = origOutNetId;
                }
            } else {
                std::string newNetName = "net_" + newInstName;
                outNetId = netlist.addNet(newNetName);
                netlist.connectGateOutput(newGateId, outNetId);
            }
        }

        // 處理 Cone-to-Wire 或 Cone-to-Const Bypass
        // 如果這個節點是整個子圖的 Root，但它卻不是邏輯閘 (而是 PI 或 CONST)
        // 代表原本的整個邏輯錐被退化成了一根現有的線。
        if (isRoot && node->nodeType != NodeType::GATE) {
            // 如果這是一個單一線/常數的替換 (0-gate replacement)
            // 且它確實有要接管的原本輸出線
            if (origOutNetId != -1 && outNetId != origOutNetId) {
                
                /*std::cout << "[Debug] Executing Cone-to-Wire Bypass! Merging Net " 
                          << origOutNetId << " into " << outNetId << "\n";*/

                // 呼叫我們剛剛寫好的通用函數！
                netlist.mergeNets(origOutNetId, outNetId);
                
            }
        }

        visited[node.get()] = outNetId;
        return outNetId;
    };

    // 啟動遞迴引擎，開始建造 Replacement Pattern (RHS)
    // 回傳值是這個 root 訊號替換後「真正存活」的 net id：
    // 一般情況等於 origOutNetId，但 cone-to-wire/const bypass 時，
    // origOutNetId 會被 mergeNets 短接掉，真正承接下游負載的是這個回傳值。
    int finalNetId = buildNode(rule.replacementPattern, true);
    if (outFinalNetId != nullptr) *outFinalNetId = finalNetId;
}

// 統一的映射執行引擎
bool TechMapper::executeMappingPass(Netlist& netlist,
                                    const std::unordered_set<int>* scopeGates,
                                    const std::vector<TechMapRule>& validRules,
                                    TechMapReport& report,
                                    bool verbose,
                                    bool allowLogicDuplication,
                                    const std::unordered_set<int>* strictContainment) {
    if (validRules.empty()) return false;
    bool actualChangesMade = false;

    // 候選 root 的合法範圍：scopeGates 給定就固定死在這個集合裡（cone 題不可擴張，
    // 新生成的 Gate id 本來就不在 scopeGates 內，天然被排除）；nullptr 代表全電路皆可。
    auto inCandidateScope = [&](int gateId) {
        return scopeGates == nullptr || scopeGates->count(gateId) > 0;
    };

    // Worklist：待檢查的候選 root gate id，取代「每套用一次就整個重掃全部候選」的 O(N^2) 作法。
    // 一個 Gate 的可匹配性只會因為以下三種情況而改變，其餘未受影響的 Gate 不需要重新檢查：
    //   1) 它本身是這次替換新生成的 Gate。
    //   2) 它是 root 訊號替換後的下游負載（root 的 driver 結構變了)。
    //   3) 它是子圖邊界輸入線的 driver（該線的 fanout 剛好因為內部閘被拔除而變少，
    //      可能因此滿足其他規則的 fanout-containment 檢查)。
    std::queue<int> worklist;
    std::unordered_set<int> queued;

    auto enqueue = [&](int gateId) {
        if (gateId < 0 || gateId >= (int)netlist.getGateCount()) return;
        if (!inCandidateScope(gateId)) return;
        if (netlist.getGate(gateId).type == GateType::UNKNOWN) return;
        if (queued.insert(gateId).second) worklist.push(gateId);
    };

    if (scopeGates != nullptr) {
        for (int gateId : *scopeGates) enqueue(gateId);
    } else {
        for (size_t i = 0; i < netlist.getGateCount(); ++i) enqueue((int)i);
    }

    while (!worklist.empty()) {
        int rootId = worklist.front();
        worklist.pop();
        queued.erase(rootId);

        // 安全防護：這個 Gate 可能已經在稍早的規則套用中被拔掉了
        if (netlist.getGate(rootId).type == GateType::UNKNOWN) continue;

        // 依序嘗試每一條合法的 Rule (已經按 Cost 優化程度排過序了)
        for (const auto& rule : validRules) {
            MatchContext ctx;

            // 形狀比對 (尋找 LHS)
            if (!matchRootGate(netlist, rootId, rule, ctx)) continue;

            // 合法性驗證 (檢查 Fan-out)
            if (!isValidSubgraph(netlist, ctx, rootId, allowLogicDuplication)) continue;

            // 嚴格範圍檢查：匹配到的整個子圖必須落在指定範圍內。
            // 否則替換會動到範圍外的閘（cone 題會破壞受限基底）。
            if (strictContainment != nullptr) {
                bool escaped = false;
                for (int gid : ctx.matchedGates) {
                    if (strictContainment->count(gid) == 0) { escaped = true; break; }
                }
                if (escaped) continue;   // 跨界，換下一條規則
            }

            // 由於 Duplication 可能導致有些 Gate 被保留，
            // 將 report 傳入 applyRule，在真正拔除時才 +1，數字才會精準！
            if (verbose) {
                for (int matchedGateId : ctx.matchedGates) {
                    const Gate& g = netlist.getGate(matchedGateId);
                    report.modifiedGateNames.push_back(g.instName);
                }
            }

            // 替換前先記下受影響範圍的邊界資訊，套用後才知道該把哪些 Gate 丟回 worklist 重新檢查。
            const std::vector<int> boundaryLeafNetIds = [&] {
                std::vector<int> ids;
                ids.reserve(ctx.boundLeaves.size());
                for (const auto& kv : ctx.boundLeaves) ids.push_back(kv.second);
                return ids;
            }();
            const size_t gateCountBefore = netlist.getGateCount();

            // 執行圖形替換 (套用 RHS)，把 report 與 allowLogicDuplication 都傳進去
            int finalOutNetId = -1;
            applyRule(netlist, ctx, rootId, rule, report, allowLogicDuplication, &finalOutNetId);

            actualChangesMade = true;

            // 1) 新生成的 Gate：全域掃描時才可能成為新的候選 root（scope 限定時會被 inCandidateScope 濾掉）
            for (size_t g = gateCountBefore; g < netlist.getGateCount(); ++g) enqueue((int)g);

            // 2) root 訊號替換後真正存活的 net（一般等於原本的輸出線，
            //    cone-to-wire/const bypass 時則是 mergeNets 短接後的目的線）的下游負載
            if (finalOutNetId >= 0 && finalOutNetId < (int)netlist.getNetCount()) {
                for (int loadId : netlist.getNet(finalOutNetId).loadGateIds) enqueue(loadId);
            }

            // 3) 子圖邊界輸入線的 driver：fanout 剛變少，可能因此符合其他規則的 containment 檢查
            for (int leafNetId : boundaryLeafNetIds) {
                if (leafNetId < 0 || leafNetId >= (int)netlist.getNetCount()) continue;
                int driverId = netlist.getNet(leafNetId).driverGateId;
                if (driverId >= 0) enqueue(driverId);
            }

            break; // 這個 root 已經套用過一條規則，換下一個 worklist 項目
        }
    }

    return actualChangesMade;
}

// 底層的實作引擎，約束驅動型 ECO 技術映射核心引擎
TechMapReport TechMapper::mapTechnologyCore(Netlist& netlist, 
                                            const std::map<GateType, int>& targetConstraints, // 拔除目標限制
                                            const std::map<GateType, int>& allowedConstraints, // 生成目標限制
                                            const std::unordered_set<int>* scopeGates,
                                            const std::vector<RuleSource>& allowedSources,
                                            bool verbose,
                                            const std::unordered_set<int>* strictContainment) {
    TechMapReport report;
    
    // 預先紀錄初始快照 (Snapshot)
    std::unordered_map<GateType, int> initialCounts;
    for (const auto& pair : allowedConstraints) initialCounts[pair.first] = netlist.getGateCountByType(pair.first);
    for (const auto& pair : targetConstraints)  initialCounts[pair.first] = netlist.getGateCountByType(pair.first);

    // 取得所有合法的「樹到樹」雙向規則 (已自動按 Cost 排序)
    // 這裡呼叫的是我們之前合併好的單一 API
    std::vector<TechMapRule> validRules = getValidRules(targetConstraints, allowedConstraints, allowedSources);

    // 執行統一的替換引擎
    bool mappingChanged = executeMappingPass(
        netlist,
        scopeGates,
        validRules,
        report,
        verbose,
        false,
        strictContainment);

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
        report.status = TechMapStatus::ERROR_SIMULATION_FAILED;
        report.message = "Notice: Rules matched constraints, but no matching subgraphs in the netlist required modification.";
    }

    // 寫入最終電路快照
    for (const auto& pair : allowedConstraints) report.finalGateCount[pair.first] = netlist.getGateCountByType(pair.first);
    for (const auto& pair : targetConstraints)  report.finalGateCount[pair.first] = netlist.getGateCountByType(pair.first);

    return report;
}

// API 實作 (直接呼叫 Core，scopeGates 給 nullptr 代表掃描全電路)
TechMapReport TechMapper::mapTechnology(Netlist& netlist, 
                                        const std::map<GateType, int>& targetConstraints, 
                                        const std::map<GateType, int>& allowedConstraints, 
                                        const std::vector<RuleSource>& allowedSources,
                                        bool verbose) {
    // 直接將參數與 verbose 往下傳遞 (已移除無用的 MapStrategy)
    return mapTechnologyCore(netlist, targetConstraints, allowedConstraints, nullptr, allowedSources, verbose);
}

NetlistEditReport TechMapper::mapTechnologyWithReport(
    Netlist& netlist,
    const std::map<GateType, int>& targetConstraints,
    const std::map<GateType, int>& allowedConstraints,
    const std::vector<RuleSource>& allowedSources,
    bool verbose)
{
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = mapTechnology(netlist, targetConstraints, allowedConstraints, allowedSources, verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "mapTechnology");
}

// 針對特定 Cone (邏輯錐) 的 API 實作 (局部優化 / ECO 常用)
TechMapReport TechMapper::mapTechnologyForCone(Netlist& netlist, 
                                               const std::map<GateType, int>& targetConstraints, 
                                               const std::map<GateType, int>& allowedConstraints, 
                                               const ConeResult& targetCone,
                                               const std::vector<RuleSource>& allowedSources,
                                               bool verbose) {
                                              
    // 呼叫 Netlist 原本就有的 Function 取得該邏輯錐內的所有 Gate ID 陣列
    std::vector<int> coneGateVec = netlist.getConeGateIds(targetCone);
    
    // 將 vector 轉換為 unordered_set，讓後續引擎在執行 ctx.matchedGates 查詢時擁有 O(1) 的極高效率
    std::unordered_set<int> scopeGates(coneGateVec.begin(), coneGateVec.end());
    
    // 呼叫核心引擎，並將 scopeGates 的記憶體位址傳入
    return mapTechnologyCore(netlist, targetConstraints, allowedConstraints, &scopeGates, allowedSources, verbose);
}

NetlistEditReport TechMapper::mapTechnologyForConeWithReport(
    Netlist& netlist,
    const std::map<GateType, int>& targetConstraints,
    const std::map<GateType, int>& allowedConstraints,
    const ConeResult& targetCone,
    const std::vector<RuleSource>& allowedSources,
    bool verbose)
{
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = mapTechnologyForCone(
        netlist,
        targetConstraints,
        allowedConstraints,
        targetCone,
        allowedSources,
        verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "mapTechnologyForCone");
}

// 精確規則應用引擎，繞過 mapTechnologyCore 的查表與約束過濾機制
// 直接將唯一指定的一條規則 (Rule) 餵給最底層的執行引擎 executeMappingPass
TechMapReport TechMapper::applySpecificRule(Netlist& netlist, 
                                            const TechMapRule& rule, 
                                            const std::unordered_set<int>& targetCone, 
                                            bool verbose) {
    TechMapReport report;
    
    if (verbose) {
        std::cout << "[Info] Direct Application Mode: Bypassing LUT search.\n";
        std::cout << "[Info] Rule locked: [" << rule.name << "] (Hash: " << rule.truthTableHash << ")\n";
        std::cout << "[Info] Scope limited to provided target cone (" << targetCone.size() << " gates).\n";
    }

    // 防呆檢查
    if (targetCone.empty()) {
        report.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
        report.message = "Failed: The provided target cone is empty.";
        if (verbose) std::cout << "[Error] " << report.message << "\n";
        return report;
    }

    // 預先紀錄初始快照 (為了結算新增數量的數學反推法)
    std::unordered_map<GateType, int> initialCounts;
    for (const auto& pair : rule.allowedCounts) initialCounts[pair.first] = netlist.getGateCountByType(pair.first);
    for (const auto& pair : rule.targetCounts)  initialCounts[pair.first] = netlist.getGateCountByType(pair.first);

    // 把唯一的黃金規則包裝成 Vector
    std::vector<TechMapRule> lockedRuleList = { rule };

    // 直接呼叫最底層的「執行引擎」！完美繞過查表機制！
    bool mappingChanged = executeMappingPass(netlist, &targetCone, lockedRuleList, report, verbose, true);

    // 統一結算新增數量
    for (const auto& pair : rule.allowedCounts) {
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
    if (mappingChanged) {
        report.status = TechMapStatus::SUCCESS;
        report.message = "Success: Optimal technology mapping rule applied successfully.";
    } else {
        report.status = TechMapStatus::ERROR_SIMULATION_FAILED;
        report.message = "Notice: Optimal rule identified, but no matching subgraphs in the netlist required modification.";
    }

    // 寫入最終電路快照
    for (const auto& pair : rule.allowedCounts) report.finalGateCount[pair.first] = netlist.getGateCountByType(pair.first);
    for (const auto& pair : rule.targetCounts)  report.finalGateCount[pair.first] = netlist.getGateCountByType(pair.first);

    return report;
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
                                         bool verbose,
                                         int maxDepthConstraint) {
    // Flatten the gate list
    std::vector<GateType> baseGateArray;
    for (const auto& pair : allowedConstraints) {
        for (int i = 0; i < pair.second; ++i) baseGateArray.push_back(pair.first);
    }
    
    int M = baseGateArray.size();
    if (M == 0) return false;

    // 防呆：如果有指定深度約束，且深度 < 1，是不可能排出任何邏輯閘的
    if (maxDepthConstraint != -1 && maxDepthConstraint < 1) {
        report.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
        report.message = "Failed: maxDepthConstraint must be >= 1 for any gate logic.";
        return false;
    }

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

        // --- 配置深度變數 L[g][d] (Unary Encoding) ---
        // L[g][d] 代表 "邏輯閘 g 的層數是否 <= d"
        std::vector<std::vector<int>> L;
        if (maxDepthConstraint != -1) {
            int D = maxDepthConstraint;
            L.assign(M, std::vector<int>(D, 0));
            for (int g = 0; g < M; ++g) {
                // d 從 1 開始，因為邏輯閘深度至少為 1；d 到 D-1 結束，因為深度 <= D 必然為 True (被我們的約束保證)
                for (int d = 1; d < D; ++d) {
                    L[g][d] = var_counter++;
                }
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

        // --- Constraint 6: Max Depth Constraint (DCAR) ---
        if (maxDepthConstraint != -1) {
            int D = maxDepthConstraint;
            
            // 6.1 Domain Clauses: L[g][d] => L[g][d+1] 
            // (如果深度 <= d，那麼深度必然 <= d+1)
            for (int g = 0; g < M; ++g) {
                for (int d = 1; d < D - 1; ++d) {
                    solver.add(-L[g][d]);
                    solver.add(L[g][d+1]);
                    solver.add(0);
                }
            }
            
            // 6.2 Topological Constraints: 確保子節點的層數大於父節點
            for (int g = 0; g < M; ++g) {
                for (int p = 0; p < C[g].size(); ++p) {
                    // 我們只需限制「來源是內部邏輯閘」的接線。PI (s < N) 的深度永遠為 0，不受影響。
                    for (int s = N; s < N + g; ++s) { 
                        int s_gate = s - N; // 將全域訊號 index 轉為邏輯閘 index
                        
                        for (int d = 1; d <= D; ++d) {
                            // 若 g 接到了 s_gate：g 的深度要 <= d，前提是 s_gate 的深度必須 <= d-1
                            // CNF: !Connected(g, s) v !L[g][d] v L[s_gate][d-1]
                            solver.add(-C[g][p][s]);
                            
                            if (d < D) {
                                solver.add(-L[g][d]); // d = D 時 L[g][D] 為 True，被省略
                            }
                            
                            if (d - 1 > 0) {
                                solver.add(L[s_gate][d-1]); // d = 1 時 L[s_gate][0] 為 False，被省略
                            }
                            
                            solver.add(0);
                        }
                    }
                }
            }
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
        nodes[i] = PatternNode::makePI(i);
    }

    // 依照拓樸順序建立 Gate Nodes (DAG)
    for (int g = 0; g < M; ++g) {
        const SynthesizedGate& synGate = topology[g];
        std::vector<std::shared_ptr<PatternNode>> inputs;
        
        // 抓出這個 Gate 的輸入節點 (可能來自 PI 或前面的 Gate)
        for (int srcId : synGate.inputSourceIds) {
            inputs.push_back(nodes[srcId]);
        }

        nodes[N + g] = std::make_shared<PatternNode>(PatternNode{NodeType::GATE, synGate.type, -1, inputs});
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
            for (int i = 0; i < N; ++i) nodes[i] = PatternNode::makePI(i);

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
                nodes[N + g] = std::make_shared<PatternNode>(PatternNode{NodeType::GATE, gateArray[g], -1, inputs});
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
    else if (node->nodeType == NodeType::PI) {
        int idx = node->piIndex;
        if (idx < 0 || idx >= inputVector.size()) {
            report.status = TechMapStatus::ERROR_UNSAT;
            report.message = "Simulation Failed: PI index out of range for current N.";
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
            // 產生真值表特徵 Hash
            std::string variantHash = generateTruthTableHash(truthTable);
            // 在命名上加上 Variant 編號，這樣規則庫名字才不會全部重複
            std::string variantName = ruleName + "_Variant_" + std::to_string(successCount + 1);
            // 建立正向規則 (標記為 AUTO_LEARNED_SAT)
            TechMapRule forwardRule(variantName + "_Forward", RuleSource::AUTO_LEARNED_SAT, variantHash, lhsRoot, rhsRoot);
            forwardRule.targetCounts = lhsConstraints;  // 保持強制綁定，避免嚴格過濾器誤殺
            forwardRule.allowedCounts = rhsConstraints;
            forwardRule.removedGateCount = lhsData.second;
            forwardRule.addedGateCount = rhsData.second;
            rules.push_back(forwardRule);

            // 建立反向規則 (標記為 AUTO_LEARNED_SAT)
            TechMapRule backwardRule(variantName + "_Backward", RuleSource::AUTO_LEARNED_SAT, variantHash, rhsRoot, lhsRoot);
            backwardRule.targetCounts = rhsConstraints;
            backwardRule.allowedCounts = lhsConstraints;
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

// 輔助函式：產生所有積木組合 (重複組合)
void TechMapper::generateCombosRec(const std::vector<GateType>& lib, 
                                   int k, 
                                   int startIdx, 
                                   std::map<GateType, int>& current, 
                                   std::vector<std::map<GateType, int>>& result) const {
    if (k == 0) {
        result.push_back(current);
        return;
    }
    for (size_t i = startIdx; i < lib.size(); ++i) {
        current[lib[i]]++;
        generateCombosRec(lib, k - 1, i, current, result);
        current[lib[i]]--; // 回溯 (Backtracking)
        if (current[lib[i]] == 0) {
            current.erase(lib[i]);
        }
    }
}

// 輔助函式：尋找絕對最小面積的拓樸
std::shared_ptr<PatternNode> TechMapper::findMinimumAreaPattern(const std::vector<bool>& truthTable, 
                                                                int N, 
                                                                int currentArea, 
                                                                TechMapReport& report, 
                                                                bool verbose,
                                                                int maxDepthConstraint,
                                                                const std::vector<GateType>& allowedTypes,
                                                                const std::vector<GateType>& bannedTypes) {
    if (verbose) {
        std::cout << "  [Exact Synthesis] Searching for pattern with Area < " << currentArea << "...\n";
        if (maxDepthConstraint != -1) {
            std::cout << "  [Exact Synthesis] SAT Engine Constrained to Max Depth <= " << maxDepthConstraint << ".\n";
        }
    }

    // 動態建立基礎積木庫 (過濾黑白名單)
    std::vector<GateType> allCombinational = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR, 
        GateType::NOT, GateType::XOR, GateType::XNOR
    };

    std::vector<GateType> baseLibrary;
    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());
    std::unordered_set<GateType> bannedSet(bannedTypes.begin(), bannedTypes.end());

    for (GateType t : allCombinational) {
        // 如果有設白名單，且這個閘不在白名單裡 -> 跳過
        if (!allowedSet.empty() && allowedSet.find(t) == allowedSet.end()) continue; 
        // 如果這個閘在黑名單裡 -> 跳過
        if (bannedSet.find(t) != bannedSet.end()) continue; 
        
        baseLibrary.push_back(t);
    }

    // 防呆：如果過濾完發現沒有任何積木可以用，直接宣告失敗
    if (baseLibrary.empty()) {
        report.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
        report.message = "Failed: No valid gates available in base library after applying allowed/banned constraints.";
        return nullptr;
    }

    // 核心邏輯：從 1 顆閘開始，慢慢增加到 currentArea - 1
    for (int k = 1; k < currentArea; ++k) {
        if (verbose) std::cout << "  [Exact Synthesis] Testing Area = " << k << " gates...\n";

        // 產生大小為 k 的所有積木組合
        std::vector<std::map<GateType, int>> combinations;
        std::map<GateType, int> currentCombo;
        generateCombosRec(baseLibrary, k, 0, currentCombo, combinations);

        if (verbose) std::cout << "    -> Generated " << combinations.size() << " combinations to try.\n";

        // 逐一測試每種組合
        for (size_t i = 0; i < combinations.size(); ++i) {
            const auto& allowedConstraints = combinations[i];
            TechMapReport stepReport;
            
            // SAT 引擎必須保證，如果回傳 isSat == true，則算出來的拓樸深度絕對不會超過 maxDepthConstraint。
            bool isSat = synthesizeFromTruthTable(truthTable, N, allowedConstraints, stepReport, false, maxDepthConstraint);

            // 判斷求解結果
            if (isSat && stepReport.status == TechMapStatus::SUCCESS) {
                if (verbose) {
                    std::cout << "  [Exact Synthesis] SUCCESS! Found optimal topology with " << k << " gates.\n";
                }
                
                // 這裡不用 report.merge，因為 stepReport 裡面的增刪數字是局部拓樸的，我們只要拿走實體就好
                report.status = TechMapStatus::SUCCESS;
                report.message = "Minimum area pattern found.";

                return buildPatternFromTopology(N, stepReport.synthesizedTopology);
            } 
            else if (stepReport.status == TechMapStatus::ERROR_SIMULATION_FAILED || 
                     stepReport.message.find("Timeout") != std::string::npos) {
                
                // 一旦遇到 SAT Timeout，代表該層級太複雜算不出來
                // 繼續試更大的 k 只是白費力氣，果斷放棄。
                if (verbose) {
                    std::cout << "  [Exact Synthesis] Timeout at Area = " << k << " (Combo " << i+1 << "). Aborting search.\n";
                }
                report.status = TechMapStatus::ERROR_UNSAT;
                report.message = "Exact synthesis timed out before finding a smaller pattern.";
                return nullptr;
            }
        } // 結束當前 k 的組合測試
    } // 結束 k 迴圈

    // 如果迴圈跑完都找不到，代表 currentArea 已經是數學上的極限
    if (verbose) {
        std::cout << "  [Exact Synthesis] Exhausted up to Area = " << (currentArea - 1) 
                  << ". The original pattern is already area-optimal.\n";
    }
    report.status = TechMapStatus::ERROR_UNSAT;
    report.message = "Original pattern is mathematically optimal in area.";
    
    return nullptr;
}

// 輔助函式：遞迴生成合法的 Fence (層級分配)
// remaining_nodes: 剩下還有幾顆閘可以分配
// remaining_levels: 剩下還有幾層需要分配
// current_fence: 遞迴過程中暫存的分配狀態
// result: 收集所有合法分配的容器
void TechMapper::generateFencesRec(int remaining_nodes, 
                                   int remaining_levels, 
                                   std::vector<int>& current_fence, 
                                   std::vector<std::vector<int>>& result) const {
    // Base Case: 已經到了最後一層 (Level D)
    if (remaining_levels == 1) {
        // 如果是處理單一輸出的邏輯錐，最後一層必定只能有一顆 Root Gate。
        // 若你的系統支援多重輸出 (MIMO) 替換，可以將此 if 條件移除，直接 push_back(remaining_nodes)。
        if (remaining_nodes == 1) { 
            current_fence.push_back(remaining_nodes);
            result.push_back(current_fence);
            current_fence.pop_back(); // 回溯 (Backtrack)
        }
        return;
    }

    // Recursive Step: 決定當前這一層要放幾顆閘
    // 為了保證後面的每一層「至少」都能分到 1 顆閘，這一層最多只能拿走 (總數 - 剩餘層數 + 1)
    int max_nodes_for_this_level = remaining_nodes - remaining_levels + 1;

    for (int i = 1; i <= max_nodes_for_this_level; ++i) {
        current_fence.push_back(i); // 將 i 顆閘分配給當前層
        
        // 進入下一層遞迴
        generateFencesRec(remaining_nodes - i, remaining_levels - 1, current_fence, result);
        
        current_fence.pop_back(); // 回溯 (Backtrack)，嘗試下一個數字
    }
}

// 輔助函式：給定總閘數 k 與 目標深度 D，回傳所有合法的形狀
std::vector<std::vector<int>> TechMapper::generateValidFences(int k, int D) const {
    std::vector<std::vector<int>> all_fences;
    std::vector<int> current_fence;

    // 防呆機制：如果閘的數量比層數還少，根本無法排成 D 層
    if (k < D) return all_fences;

    // 1. 產生所有數學上合法的分割
    generateFencesRec(k, D, current_fence, all_fences);

    // 2. 過濾掉違反「最大 Fan-in 限制」的物理不可能形狀
    std::vector<std::vector<int>> valid_fences;
    for (const auto& fence : all_fences) {
        bool isValid = true;
        
        // 從最深層 (Root) 往回推算，檢查每一層是否超過了容許的最大閘數
        // (假設我們的系統允許跨層連線 Bypass，所以我們用最寬鬆的二元樹極限：2^(D-L))
        int max_allowed_in_this_level = 1; // Level D (Root) 只能有 1 顆
        
        // fence 的 index: 0 是 Level 1, size-1 是 Level D
        for (int i = fence.size() - 1; i >= 0; --i) {
            if (fence[i] > max_allowed_in_this_level) {
                isValid = false;
                break; // 違反物理極限，這個形狀不可能接得起來
            }
            // 往上一層推，最多可以容納的閘數是當前層的 2 倍 (因為 2-input)
            max_allowed_in_this_level *= 2; 
        }

        if (isValid) {
            valid_fences.push_back(fence);
        }
    }

    return valid_fences;
}

// 精確合成引擎 (加入 Fence 深度約束)
bool TechMapper::synthesizeFromTruthTableWithFence(const std::vector<bool>& targetTruthTable, 
                                                   int N, 
                                                   const std::map<GateType, int>& allowedConstraints, 
                                                   const std::vector<int>& fenceShape,
                                                   TechMapReport& report, 
                                                   bool verbose) {
    // 展開積木庫
    std::vector<GateType> baseGateArray;
    for (const auto& pair : allowedConstraints) {
        for (int i = 0; i < pair.second; ++i) baseGateArray.push_back(pair.first);
    }
    
    int M = baseGateArray.size();
    if (M == 0) return false;

    // 防呆 : 確保 Fence 形狀加總的閘數等於積木總數 M
    int shapeSum = 0;
    for (int count : fenceShape) shapeSum += count;
    if (shapeSum != M) {
        if (verbose) std::cout << "[Error] Fence shape total gates does not match allowed constraints.\n";
        return false;
    }

    size_t numRows = 1ULL << N;
    if (targetTruthTable.size() != numRows) return false;

    std::sort(baseGateArray.begin(), baseGateArray.end());

    TimeLimitTerminator terminator(5.0); 

    // 預先計算每一顆 Gate 合法的最大輸入來源索引 (Max Source Index)
    // 概念：位於第 L 層的 Gate，其輸入只能來自 PI (索引 0~N-1) 或位於第 0~(L-1) 層的 Gate。
    std::vector<int> gate_to_max_source(M);
    int current_gate_idx = 0;
    for (size_t lvl = 0; lvl < fenceShape.size(); ++lvl) {
        int gates_in_level = fenceShape[lvl];
        for (int i = 0; i < gates_in_level; ++i) {
            // max_source = N (所有的 PI) + current_gate_idx (前面所有層級累積的 Gate 總數)
            // 這樣就嚴格禁止了同層互接 (避免 Cyclic) 與同層連線 (強制 Depth)
            gate_to_max_source[current_gate_idx + i] = N + current_gate_idx;
        }
        current_gate_idx += gates_in_level;
    }

    // 窮舉邏輯閘的所有拓樸排列
    do {
        CaDiCaL::Solver solver;
        solver.connect_terminator(&terminator);

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
                // 根據 Fence 計算的限制，套用合法的 Source 數量
                int num_sources = gate_to_max_source[g]; 
                
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
                for (int s = 0; s < C[g][p].size(); ++s) solver.add(C[g][p][s]);
                solver.add(0);
                
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
                for (int p = 0; p < num_pins; ++p) {
                    for (int s = 0; s < C[g][p].size(); ++s) {
                        solver.add(-C[g][p][s]); solver.add(-P[g][p][r]); solver.add(F[s][r]); solver.add(0);
                        solver.add(-C[g][p][s]); solver.add(P[g][p][r]); solver.add(-F[s][r]); solver.add(0);
                    }
                }
                
                int out = F[N + g][r];
                int a = P[g][0][r];
                int b = (num_pins == 2) ? P[g][1][r] : 0; 
                addGateSemantics(solver, gateArray[g], a, b, out);
            }
        }

        // --- Constraint 4: Target Output ---
        for (size_t r = 0; r < numRows; ++r) {
            solver.add(targetTruthTable[r] ? F[N + M - 1][r] : -F[N + M - 1][r]);
            solver.add(0);
        }

        // --- Constraint 5: No Dangling Gates (無懸空閘) ---
        // 確保前面的每一顆 Gate (除了最後輸出的那顆以外) 都必定被後面的層級接走。
        // 如果有 Gate 沒被接走，代表存在更小面積的等價電路，這不符合 Exact Synthesis 的精髓。
        for (int s = 0; s < M - 1; ++s) {
            int source_idx = N + s; // 該 Gate 的實體索引
            for (int g = s + 1; g < M; ++g) {
                // 如果後面的 Gate g 在合法的層級範圍內可以吃到 source_idx
                if (source_idx < gate_to_max_source[g]) {
                    for (int p = 0; p < C[g].size(); ++p) {
                        solver.add(C[g][p][source_idx]);
                    }
                }
            }
            solver.add(0); // 至少有一條連線必須存在
        }

        // ================= Solve =================
        int result = solver.solve();

        if (result == 10) { // SAT
            if (verbose) std::cout << "[Info] Exact synthesis found a valid topology for this Fence!\n";
            
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
            return true;
        } 
        else if (result == 0) { // TIMEOUT
            report.status = TechMapStatus::ERROR_NOT_EQUIVALENT;
            report.message = "Failed: Solver timed out.";
            return false;
        }

    } while (std::next_permutation(baseGateArray.begin(), baseGateArray.end()));

    if (verbose) std::cout << "[Failed] Mathematically impossible to synthesize the truth table with given Fence and constraints.\n";
    report.status = TechMapStatus::ERROR_UNSAT;
    report.message = "Failed (UNSAT).";
    return false;
}

// 輔助函式：尋找絕對最小深度的拓樸
std::shared_ptr<PatternNode> TechMapper::findMinimumDepthPattern(const std::vector<bool>& truthTable, 
                                                                int N, 
                                                                int currentDepth, 
                                                                int currentArea, 
                                                                TechMapReport& report, 
                                                                bool verbose,
                                                                int maxAreaOverhead,
                                                                const std::vector<GateType>& allowedTypes, 
                                                                const std::vector<GateType>& bannedTypes) {
    // 決定最大容許面積 (雙重防護機制)
    int maxAllowedArea;
    if (maxAreaOverhead > 0) {
        // 尊重使用者的設定 (原本面積 + 容許的額外負擔)
        maxAllowedArea = currentArea + maxAreaOverhead; 
    } else {
        // 如果使用者沒設定，使用智慧雙重防護：
        // 1. 最多變兩倍大
        // 2. 絕對物理極限 (限制在 MAX_AREA 顆以內，保護 SAT Solver 不當機)
        maxAllowedArea = std::min(currentArea * 2, MAX_AREA); 
    }

    if (verbose) {
        std::cout << "  [Exact Synthesis] Searching for pattern with Depth < " << currentDepth 
                  << " (Max Area limit: " << maxAllowedArea << ")...\n";
    }

    // 動態建立基礎積木庫 (過濾黑白名單)
    std::vector<GateType> allCombinational = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR, 
        GateType::NOT, GateType::XOR, GateType::XNOR
    };

    std::vector<GateType> baseLibrary;
    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());
    std::unordered_set<GateType> bannedSet(bannedTypes.begin(), bannedTypes.end());

    for (GateType t : allCombinational) {
        // 如果有設白名單，且這個閘不在白名單裡 -> 跳過
        if (!allowedSet.empty() && allowedSet.find(t) == allowedSet.end()) continue; 
        // 如果這個閘在黑名單裡 -> 跳過
        if (bannedSet.find(t) != bannedSet.end()) continue; 
        
        baseLibrary.push_back(t);
    }

    // 防呆：如果過濾完發現沒有任何積木可以用，直接宣告失敗
    if (baseLibrary.empty()) {
        report.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
        report.message = "Failed: No valid gates available in base library after applying allowed/banned constraints.";
        return nullptr;
    }

    // 外層迴圈：深度 D 從 1 開始，直到 currentDepth - 1
    for (int D = 1; D < currentDepth; ++D) {
        if (verbose) std::cout << "  [Exact Synthesis] Testing Depth = " << D << " levels...\n";

        // 內層迴圈：面積 k 從 D (最少需要 D 顆) 到 maxAllowedArea
        for (int k = D; k <= maxAllowedArea; ++k) {
            
            // 1. 產生所有 k 顆閘、D 層深度的合法 Fence 形狀
            std::vector<std::vector<int>> fences = generateValidFences(k, D);
            
            // 如果這個數量無法產生合法的形狀，直接跳過
            if (fences.empty()) continue;

            // 2. 產生大小為 k 的所有積木組合
            std::vector<std::map<GateType, int>> combinations;
            std::map<GateType, int> currentCombo;
            generateCombosRec(baseLibrary, k, 0, currentCombo, combinations);

            if (verbose) {
                std::cout << "    -> Area = " << k << " gates (Fences: " << fences.size() 
                          << ", Combos: " << combinations.size() << ")\n";
            }

            // 3. 雙重遍歷：針對每一種形狀，測試每一種積木組合
            for (size_t f_idx = 0; f_idx < fences.size(); ++f_idx) {
                const auto& fenceShape = fences[f_idx];

                for (size_t c_idx = 0; c_idx < combinations.size(); ++c_idx) {
                    const auto& allowedConstraints = combinations[c_idx];
                    TechMapReport stepReport;
                    
                    bool isSat = false;

                    // 呼叫帶有 Fence 限制的 SAT 編碼器
                    isSat = synthesizeFromTruthTableWithFence(truthTable, 
                                                              N, 
                                                              allowedConstraints, 
                                                              fenceShape, 
                                                              stepReport, 
                                                              false); 

                    // 判斷求解結果
                    if (isSat && stepReport.status == TechMapStatus::SUCCESS) {
                        if (verbose) {
                            std::cout << "  [Exact Synthesis] SUCCESS! Found optimal topology with Depth " << D 
                                      << " (Area: " << k << " gates).\n";
                        }
                        
                        report.status = TechMapStatus::SUCCESS;
                        report.message = "Minimum depth pattern found.";

                        // 成功轉換為 PatternNode 樹並回傳
                        return buildPatternFromTopology(N, stepReport.synthesizedTopology);
                    } 
                    else if (stepReport.status == TechMapStatus::ERROR_SIMULATION_FAILED || 
                             stepReport.message.find("Timeout") != std::string::npos) {
                        
                        // 遇到 SAT Timeout，代表當前這個深度 (D) 加上這個面積 (k) 已經達計算極限
                        if (verbose) {
                            std::cout << "  [Exact Synthesis] Timeout at Depth = " << D << ", Area = " << k 
                                      << " (Fence " << f_idx+1 << ", Combo " << c_idx+1 << "). Aborting Area scaling for this depth.\n";
                        }
                        
                        // 放棄尋找當前深度 D，直接跳出 k 迴圈，前往挑戰下一個深度 D+1
                        goto NEXT_DEPTH; 
                    }
                } // 結束 Combo 迴圈
            } // 結束 Fence 迴圈
        } // 結束 k 迴圈

    NEXT_DEPTH:; // 配合 goto 的跳轉標籤，放在 D 迴圈的最底部
    } // 結束 D 迴圈

    // 如果迴圈跑完都找不到，代表 currentDepth 已經是數學上的極限
    if (verbose) {
        std::cout << "  [Exact Synthesis] Exhausted up to Depth = " << (currentDepth - 1) 
                  << " (Max Area = " << maxAllowedArea << "). The original pattern is already depth-optimal.\n";
    }
    
    report.status = TechMapStatus::ERROR_UNSAT;
    report.message = "Original pattern is mathematically optimal in depth (or requires too much area overhead).";
    
    return nullptr;
}

// 全域電路優化引擎 (Pattern Optimization Engine)
// 針對給定的目標形狀 (LHS) 進行自動化的「面積」或「深度」化簡。
TechMapReport TechMapper::optimizePattern(Netlist& netlist,
                                          std::shared_ptr<PatternNode> lhsTarget,
                                          OptimizationGoal goal,
                                          const std::unordered_set<int>& targetCone, 
                                          bool verbose,
                                          int maxAreaOverhead, 
                                          int maxDepthConstraint,                    
                                          const std::vector<GateType>& allowedTypes, 
                                          const std::vector<GateType>& bannedTypes) {
    TechMapReport finalReport;

    if (!lhsTarget || targetCone.empty()) {
        finalReport.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
        finalReport.message = "Failed: lhsTarget is null or targetCone is empty.";
        return finalReport;
    }

    if (verbose) {
        std::cout << "=================================================\n";
        std::cout << "[Optimize Engine] Goal: " << (goal == OptimizationGoal::AREA ? "AREA" : "DEPTH") << " Reduction\n";
        if (maxDepthConstraint != -1) {
            std::cout << "[Optimize Engine] Constraint: Max Depth <= " << maxDepthConstraint << "\n";
        }
    }

    // 分析目標 LHS，萃取真值表與當前成本
    int N = countPrimaryInputs(lhsTarget); 

    TechMapReport simReport;
    std::vector<bool> truthTable = simulatePattern(lhsTarget, N, simReport, false);

    if (simReport.status != TechMapStatus::SUCCESS || truthTable.empty()) {
        finalReport.status = TechMapStatus::ERROR_SIMULATION_FAILED;
        // 將真正的錯誤原因印出來
        finalReport.message = "Simulation aborted: " + simReport.message; 
        if (verbose) std::cout << "[Error] " << finalReport.message << " (N = " << N << ")\n";
        return finalReport;
    }

    // 計算目前的基準代價 (Baseline Cost)
    auto lhsData = countGates(lhsTarget); 
    int currentArea = lhsData.second;
    int currentDepth = calculateDepth(lhsTarget); 
    int currentCost = (goal == OptimizationGoal::AREA) ? currentArea : currentDepth;

    // 產生真值表 Hash
    std::string targetHash = generateTruthTableHash(truthTable);

    if (verbose) {
        std::cout << "[Info] Extracted Truth Table Hash: [" << targetHash << "] (N = " << N << ")\n";
        std::cout << "[Info] Baseline " << (goal == OptimizationGoal::AREA ? "Area" : "Depth") 
                  << " = " << currentCost << " gates/levels.\n";
    }

    std::string ruleName = "OptRule_" + (goal == OptimizationGoal::AREA ? std::string("A_") : std::string("D_")) + targetHash;

    // 用來追蹤目前找到的最佳解 (初始為無效規則)
    bool ruleExistsAndIsOptimal = false;
    int bestCostSoFar = currentCost; 
    // 預設一條「空轉」規則，代表尚未找到任何比現狀更好的方案
    TechMapRule bestRule("Baseline_No_Op", RuleSource::USER_CUSTOM, targetHash, lhsTarget, lhsTarget); 
    bestRule.targetCounts = lhsData.first;
    bestRule.allowedCounts = lhsData.first; // 允許的等於目標，等於沒換

    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());
    std::unordered_set<GateType> bannedSet(bannedTypes.begin(), bannedTypes.end());

    // 檢查快取 / 查找表 (防污染與分級檢查)
    for (const auto& rule : rules) {
        // 條件 1：真值表必須一模一樣 (邏輯等價)
        if (rule.truthTableHash == targetHash) {
            
            // 檢查這條規則的 replacementPattern 是否使用了違規的 Gate
            bool isValidGateMix = true;
            auto rhsGateCounts = countGates(rule.replacementPattern).first; // 取得該規則使用的閘種類與數量
            
            for (const auto& pair : rhsGateCounts) {
                GateType t = pair.first;
                // 如果有白名單，且這個閘不在白名單內 -> 違規
                if (!allowedSet.empty() && allowedSet.find(t) == allowedSet.end()) {
                    isValidGateMix = false; break;
                }
                // 如果這個閘在黑名單內 -> 違規
                if (bannedSet.find(t) != bannedSet.end()) {
                    isValidGateMix = false; break;
                }
            }

            // 如果這條快取規則違反了當前的 Gate 限制，直接跳過不採用
            if (!isValidGateMix) continue;

            // 檢查深度約束 (DCAR 防護機制)
            int ruleDepth = calculateDepth(rule.replacementPattern);
            if (maxDepthConstraint != -1 && ruleDepth > maxDepthConstraint) {
                if (verbose) std::cout << "[Info] Skipping rule " << rule.name << " due to depth constraint violation (" << ruleDepth << " > " << maxDepthConstraint << ").\n";
                continue;
            }

            // 計算這條規則的實際 Cost
            int ruleCost = (goal == OptimizationGoal::AREA) ? rule.addedGateCount : ruleDepth;
            
            // 條件 2a：這條規則已經是經過認證的「絕對最佳解」
            if ((goal == OptimizationGoal::AREA && rule.source == RuleSource::OPTIMIZED_AREA) ||
                (goal == OptimizationGoal::DEPTH && rule.source == RuleSource::OPTIMIZED_DEPTH)) {
                
                ruleExistsAndIsOptimal = true;
                bestRule = rule;
                bestCostSoFar = ruleCost;
                if (verbose) std::cout << "[Info] Hit verified OPTIMAL cache: " << rule.name << " (Cost: " << ruleCost << ").\n";
                break; // 找到了絕對極限，直接跳出迴圈下班！
            }
            
            // 條件 2b：這是一條標準庫的次佳解，如果比目前的電路小，先當作備胎！
            if (rule.source == RuleSource::STANDARD_LIBRARY || rule.source == RuleSource::AUTO_LEARNED_SAT) {
                if (ruleCost < bestCostSoFar) {
                    bestCostSoFar = ruleCost;
                    bestRule = rule;
                    if (verbose) std::cout << "[Info] Found better baseline in cache: " << rule.name << " (Cost: " << ruleCost << ").\n";
                    // 不 break，因為我們還要看有沒有更完美的 OPTIMIZED 規則
                }
            }
        }
    }

    // 啟動 SAT 引擎尋找絕對最佳解
    if (!ruleExistsAndIsOptimal && bestCostSoFar > 1) {
        if (verbose) std::cout << "[Info] No verified optimal rule found. Firing up EXACT SYNTHESIS engine to beat Cost < " << bestCostSoFar << "...\n";

        TechMapReport optReport;
        std::shared_ptr<PatternNode> bestRhs = nullptr;

        // 核心呼叫：尋找嚴格小於 bestCostSoFar 的結構
        if (goal == OptimizationGoal::AREA) {
            bestRhs = findMinimumAreaPattern(truthTable, N, bestCostSoFar, optReport, verbose, maxDepthConstraint, allowedTypes, bannedTypes);
        } else {
            bestRhs = findMinimumDepthPattern(truthTable, N, bestCostSoFar, currentArea, optReport, verbose, maxAreaOverhead, allowedTypes, bannedTypes);
        }

        if (bestRhs && optReport.status == TechMapStatus::SUCCESS) {
            // SAT 成功找到了突破極限的解！
            auto rhsData = countGates(bestRhs);
            RuleSource newSource = (goal == OptimizationGoal::AREA) ? RuleSource::OPTIMIZED_AREA : RuleSource::OPTIMIZED_DEPTH;
            
            // 建立並註冊這條黃金規則 (使用新的建構子)
            TechMapRule newRule(ruleName, newSource, targetHash, lhsTarget, bestRhs);
            newRule.targetCounts = lhsData.first;
            newRule.allowedCounts = rhsData.first;
            newRule.removedGateCount = lhsData.second;
            newRule.addedGateCount = rhsData.second;
            
            rules.push_back(newRule);
            bestRule = newRule;
            
            if (verbose) std::cout << "[Success] Exact synthesis found a smaller pattern! Registered as: " << ruleName << "\n";
        } 
        else if (bestRule.replacementPattern != nullptr) {
            // SAT 引擎找不到更小的 (或 Timeout 了)，但我們在查找表中剛好有找到比原本還小的標準庫規則！
            if (verbose) std::cout << "[Info] SAT couldn't beat the baseline. Using the best known cached rule instead.\n";
        } 
        else {
            // SAT 找不到，且快取也沒有比原本更好的解 (代表原電路已經是極限了)
            finalReport.status = TechMapStatus::ERROR_UNSAT;
            finalReport.message = "Optimization Failed: The given LHS is already mathematically optimal (or SAT timed out).";
            if (verbose) std::cout << "[Failed] " << finalReport.message << "\n";
            return finalReport;
        }
    }

    // 執行電路替換
    int finalCost = (goal == OptimizationGoal::AREA) ? bestRule.addedGateCount : calculateDepth(bestRule.replacementPattern);
    
    if (finalCost >= currentCost) {
        finalReport.status = TechMapStatus::SUCCESS;
        finalReport.message = "Optimization complete: The original circuit is already mathematically optimal. No changes made.";
        if (verbose) {
            std::cout << "[Success] " << finalReport.message << "\n";
            std::cout << "=================================================\n";
        }
        return finalReport;
    }

    if (verbose) std::cout << "[Info] Applying rule [" << bestRule.name << "] (Cost: " << finalCost << ") to the netlist...\n";

    return applySpecificRule(netlist, 
                             bestRule, 
                             targetCone, 
                             verbose);
}

// 筛选「反相吸收」规则。
std::vector<TechMapRule> TechMapper::getInverterAbsorptionRules(
        const std::map<GateType, int>& rhsAllowed) const {

    std::vector<TechMapRule> picked;

    for (const auto& rule : rules) {
        // (1) 只收标准库规则
        if (rule.source != RuleSource::STANDARD_LIBRARY) continue;

        // (3) 深度必须严格减少（吸收 = 把 NOT+闸 的 2 层压成 1 层）
        if (rule.replacementDepth >= rule.targetDepth) continue;

        // (4) 面积不增（addedGateCount - removedGateCount <= 0）
        if (rule.getCostDelta() >= 0) continue;

        // (2) RHS 只能用允许且非禁的闸：逐一检查 RHS 用到的每种闸
        bool rhsOk = true;
        for (const auto& kv : rule.allowedCounts) {   // allowedCounts = RHS 各 gate 数量
            GateType t = kv.first;
            auto it = rhsAllowed.find(t);
            if (it == rhsAllowed.end()) {   // RHS 用到一个不在允许集里的闸 → 淘汰
                rhsOk = false;
                break;
            }
            // rhsAllowed 的 value 用 -1 表无限制；这里只要「存在于允许集」即可
        }
        if (!rhsOk) continue;

        picked.push_back(rule);
    }

    // 排序：深度收益优先（replacementDepth - targetDepth 越负越好），面积其次
    std::sort(picked.begin(), picked.end(), [](const TechMapRule& a, const TechMapRule& b) {
        int da = a.replacementDepth - a.targetDepth;
        int db = b.replacementDepth - b.targetDepth;
        if (da != db) return da < db;                 // 深度降越多越前面
        return a.getCostDelta() < b.getCostDelta();   // 深度相同看面积
    });

    return picked;
}

// 依 allowed/banned 組出「RHS 可用的積木庫」
std::map<GateType, int> TechMapper::buildRhsAllowedSet(
        const std::vector<GateType>& allowedTypes,
        const std::vector<GateType>& bannedTypes) const {
    std::map<GateType, int> rhsAllowed;
    std::vector<GateType> allComb = {GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
                                     GateType::NOT, GateType::BUF, GateType::XOR, GateType::XNOR};
    std::unordered_set<GateType> bannedSet(bannedTypes.begin(), bannedTypes.end());
    std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());
    for (GateType t : allComb) {
        bool ok = allowedSet.empty() ? true : (allowedSet.count(t) > 0);  // 空白名單 = 全允許
        if (bannedSet.count(t)) ok = false;
        if (ok) rhsAllowed[t] = -1;
    }
    return rhsAllowed;
}

// 用标准库双向规则把 NOT+复合闸 合并成 NAND/NOR/XNOR。
TechMapReport TechMapper::absorbInverters(Netlist& netlist,
                                          const std::vector<GateType>& allowedTypes,
                                          const std::vector<GateType>& bannedTypes,
                                          bool verbose) {
    TechMapReport report;

    // 组 RHS 允许集：allowed 里、且不在 banned 里的闸
    // 关键：banned 的闸（尤其阶段 4 刚消灭的 AND/XOR）必须排除，防止吸收把它合并回来
    std::map<GateType, int> rhsAllowed = buildRhsAllowedSet(allowedTypes, bannedTypes);

    std::vector<TechMapRule> absorbRules = getInverterAbsorptionRules(rhsAllowed);

    if (absorbRules.empty()) {
        report.status = TechMapStatus::ERROR_RULE_NOT_FOUND;
        report.message = "No inverter-absorption rules available under current basis.";
        return report;
    }

    // 全域扫描、不允许 logic duplication
    bool changed = executeMappingPass(netlist, nullptr, absorbRules, report, verbose, false, nullptr);

    report.status = TechMapStatus::SUCCESS;
    report.message = changed
        ? "Absorption applied."
        : "No absorbable inverter pattern found.";
    return report;
}

// 範圍限定的反相吸收：只在 scopeGates 內部進行 NOT+複合閘 的合併。
//   scopeGates 同時作為「root 候選範圍」與「子圖嚴格包含範圍」，
//   確保絕不動到範圍外的閘（cone 題用來隔離 cone 內 / cone 外）。
TechMapReport TechMapper::absorbInvertersOnGateSet(
        Netlist& netlist,
        const std::unordered_set<int>& scopeGates,
        const std::vector<GateType>& allowedTypes,
        const std::vector<GateType>& bannedTypes,
        bool verbose) {

    TechMapReport report;

    if (scopeGates.empty()) {
        report.status = TechMapStatus::SUCCESS;   // 沒東西可做，不算失敗
        report.message = "Empty scope; nothing to absorb.";
        return report;
    }

    std::map<GateType, int> rhsAllowed = buildRhsAllowedSet(allowedTypes, bannedTypes);
    std::vector<TechMapRule> absorbRules = getInverterAbsorptionRules(rhsAllowed);

    if (absorbRules.empty()) {
        // 這個範圍的基底下沒有可用的吸收規則（例如只允許 NOR+NOT）
        report.status = TechMapStatus::SUCCESS;   // 不是錯誤，只是沒得吸
        report.message = "No absorption rules available under this basis.";
        return report;
    }

    bool changed = executeMappingPass(
        netlist,
        &scopeGates,                       // root 候選限定在範圍內
        absorbRules,
        report,
        verbose,
        /*allowLogicDuplication=*/false,   // 吸收不複製邏輯
        /*strictContainment=*/&scopeGates  // 子圖也必須完全落在範圍內
    );

    report.status = TechMapStatus::SUCCESS;   // 有沒有吸到都算成功（機會型優化）
    report.message = changed ? "Absorption applied." : "No absorbable pattern in scope.";
    return report;
}

// 直接對「指定的 gate id 集合」做基底強制，不重算 cone。
// cone 範圍由 ConeReport.gateIds 給定，避免與內部 cone 演算法不一致。
TechMapReport TechMapper::convertToBasisOnGateSet(Netlist& netlist,
                                                  const std::unordered_set<int>& coneGates,
                                                  const std::vector<GateType>& allowedTypes,
                                                  const std::vector<GateType>& bannedTypes,
                                                  bool verbose) {
    TechMapReport finalReport;
    finalReport.status = TechMapStatus::SUCCESS;
    finalReport.message = "Cone basis conversion succeeded.";

    std::vector<GateType> allComb = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
        GateType::NOT, GateType::BUF, GateType::XOR, GateType::XNOR
    };

    // 1. 決定要消滅的目標集合（黑名單 + 不在白名單內的）
    std::unordered_set<GateType> targetsToRemoveSet(bannedTypes.begin(), bannedTypes.end());
    if (!allowedTypes.empty()) {
        std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());
        for (GateType t : allComb)
            if (!allowedSet.count(t)) targetsToRemoveSet.insert(t);
    }

    // 2. 允許的積木庫（給替換引擎的 RHS 約束）
    std::map<GateType, int> allowedConstraints;
    for (GateType t : allComb)
        if (!targetsToRemoveSet.count(t)) allowedConstraints[t] = -1;

    if (allowedConstraints.empty()) {
        finalReport.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
        finalReport.message = "All gates banned; no basis.";
        return finalReport;
    }

    std::vector<RuleSource> strictSources = { RuleSource::STANDARD_LIBRARY };

    // 3. 逐一消滅每種非法閘，scope 固定為傳入的 coneGates
    for (GateType targetType : targetsToRemoveSet) {

        // 電路裡根本沒有這種閘 → 本來就合規，跳過，不要誤判失敗
        if (netlist.getGateCountByType(targetType) == 0) continue;

        std::map<GateType, int> targetConstraints = {{targetType, 1}};

        // 只在 cone 內還有這種閘時才處理（避免無謂呼叫）
        // 注意：getGateCountByType 是全域計數，這裡改成掃 coneGates 判斷
        bool existsInCone = false;
        for (int gid : coneGates) {
            if (gid >= 0 && gid < (int)netlist.getGateCount() &&
                netlist.getGate(gid).type == targetType) { existsInCone = true; break; }
        }
        if (!existsInCone) continue;

        if (verbose)
            std::cout << "[Cone Basis] eliminating type " << (int)targetType << " in cone...\n";
        // 直接把 coneGates 當 scope 傳給 core，不重算 cone
        TechMapReport stepReport = mapTechnologyCore(
            netlist, targetConstraints, allowedConstraints,
            &coneGates,
            strictSources,
            verbose,
            &coneGates);

        // 檢查 cone 內是否還殘留該類型（用 coneGates 掃，不用全域計數）
        bool stillExists = false;
        for (int gid : coneGates) {
            if (gid >= 0 && gid < (int)netlist.getGateCount() &&
                netlist.getGate(gid).type == targetType) { stillExists = true; break; }
        }
        if (stepReport.status != TechMapStatus::SUCCESS || stillExists) {
            finalReport.status = (stepReport.status != TechMapStatus::SUCCESS)
                               ? stepReport.status : TechMapStatus::ERROR_NOT_EQUIVALENT;
            finalReport.message = "Cone basis conversion failed: type "
                                + std::to_string((int)targetType) + " remains.";
            return finalReport;
        }

        for (const auto& p : stepReport.removedCountByType) finalReport.removedCountByType[p.first] += p.second;
        for (const auto& p : stepReport.addedCountByType)   finalReport.addedCountByType[p.first]   += p.second;
    }

    return finalReport;
}

//-----------------------------------------------------------------------------------------------------------------------------
// 高階 API

// 全域/區域 邏輯閘轉換引擎 (Technology Mapping / Basis Conversion)
TechMapReport TechMapper::convertToBasis(Netlist& netlist,  
                                         TargetScope scope, 
                                         const std::string& name, 
                                         const std::vector<GateType>& allowedTypes, 
                                         const std::vector<GateType>& bannedTypes,
                                         bool verbose) {

    TechMapReport finalReport;
    finalReport.status = TechMapStatus::SUCCESS;
    finalReport.message = "Successfully converted circuit to the specified basis.";

    const RewriteScopeResolution initialScope = resolveRewriteScope(netlist, scope, name);
    if (!initialScope.ok) {
        finalReport.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
        finalReport.message = "Failed to resolve rewrite scope: " + initialScope.message;
        return finalReport;
    }

    std::vector<GateType> allCombinational = {
        GateType::AND, GateType::OR, GateType::NAND, GateType::NOR,
        GateType::NOT, GateType::BUF, GateType::XOR, GateType::XNOR
    };

    // 1. 決定要移除的目標集合 (黑名單 + 不在白名單內的所有人)
    std::unordered_set<GateType> targetsToRemoveSet;

    // 將使用者指定的黑名單加入移除清單
    for (GateType t : bannedTypes) {
        targetsToRemoveSet.insert(t);
    }

    // 如果使用者有指定白名單，則把「不在白名單內」的所有組合邏輯閘都加入移除清單
    if (!allowedTypes.empty()) {
        std::unordered_set<GateType> allowedSet(allowedTypes.begin(), allowedTypes.end());
        for (GateType type : allCombinational) {
            if (allowedSet.find(type) == allowedSet.end()) {
                targetsToRemoveSet.insert(type);
            }
        }
    }

    // 將 Set 轉回 Vector 以利後續迴圈處理
    std::vector<GateType> targetsToRemove(targetsToRemoveSet.begin(), targetsToRemoveSet.end());

    // 2. 計算目前環境「允許使用」的剩餘積木庫 (給底層 SAT 引擎的約束)
    std::map<GateType, int> allowedConstraints;
    for (GateType type : allCombinational) {
        // 只要不是即將被消滅的目標，都可以拿來當作替換用的積木 (-1 代表無數量限制)
        if (targetsToRemoveSet.find(type) == targetsToRemoveSet.end()) {
            allowedConstraints[type] = -1; 
        }
    }

    // 如果所有積木都被禁用了 (代表參數設定不合理)
    if (allowedConstraints.empty()) {
        finalReport.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
        finalReport.message = "Failed: All combinational gates are banned. No available basis.";
        return finalReport;
    }

    // 定義 Basis Conversion 允許的規則來源
    std::vector<RuleSource> strictSources = { RuleSource::STANDARD_LIBRARY };

    // 3. 迴圈依序消滅每一種不合法的 Gate
    for (GateType targetType : targetsToRemove) {

        // 電路裡根本沒有這種閘 → 本來就合規，跳過，不要誤判失敗
        if (netlist.getGateCountByType(targetType) == 0) continue;
        
        if (countGateTypeInScope(netlist, scope, name, targetType) == 0) {
            continue;
        }
        
        // 強制設為 1，確保引擎進入「拆解 (1-to-N)」模式
        std::map<GateType, int> targetConstraints = {{targetType, 1}};

        if (verbose) {
            std::cout << "\n[Basis Conversion] Eliminating GateType: " << (int)targetType << "...\n";
        }

        TechMapReport stepReport;
        
        // 呼叫底層對應 Scope 的遞迴查找與替換引擎
        const RewriteScopeResolution stepScope = resolveRewriteScope(netlist, scope, name);
        if (!stepScope.ok) {
            finalReport.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
            finalReport.message = "Failed to resolve rewrite scope: " + stepScope.message;
            return finalReport;
        }

        switch (scope) {
            case TargetScope::WHOLE_NETLIST:
                stepReport = mapTechnology(netlist, targetConstraints, allowedConstraints, strictSources, verbose);
                break;
            case TargetScope::NET_FANIN:
            case TargetScope::NET_FANOUT:
            case TargetScope::GATE_FANIN:
            case TargetScope::GATE_FANOUT:
                stepReport = mapTechnologyForCone(
                    netlist,
                    targetConstraints,
                    allowedConstraints,
                    stepScope.cone,
                    strictSources,
                    verbose);
                break;
            default:
                finalReport.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
                finalReport.message = "Failed: Invalid TargetScope.";
                return finalReport;
        }

        // 檢查該步驟是否失敗
        // 只有當電路中還殘留該種類的 Gate 時，才回報錯誤 (代表我們用現有的白名單積木，無法數學等價地展開它)
        const int remainingInScope = countGateTypeInScope(netlist, scope, name, targetType);
        if (stepReport.status != TechMapStatus::SUCCESS || remainingInScope > 0) {
            finalReport.status = (stepReport.status != TechMapStatus::SUCCESS) ? stepReport.status : TechMapStatus::ERROR_NOT_EQUIVALENT;
            finalReport.message = "Basis conversion failed! Remaining gates of type: " + std::to_string((int)targetType);
            return finalReport;
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

NetlistEditReport TechMapper::convertToBasisWithReport(
    Netlist& netlist,
    TargetScope scope,
    const std::string& name,
    const std::vector<GateType>& allowedTypes,
    const std::vector<GateType>& bannedTypes,
    bool verbose)
{
    Netlist before = netlist.cloneForRollback();
    TechMapReport techReport = convertToBasis(
        netlist,
        scope,
        name,
        allowedTypes,
        bannedTypes,
        verbose);
    return finalizeTechMapEditReport(netlist, before, techReport, "convertToBasis");
}

NetlistEditReport TechMapper::replaceGateTypeWithReport(
    Netlist& netlist,
    GateType targetType,
    const std::vector<GateType>& allowedTypes,
    TargetScope scope,
    const std::string& name,
    bool verbose)
{
    Netlist before = netlist.cloneForRollback();

    std::map<GateType, int> allowedConstraints;
    for (GateType type : allowedTypes) {
        allowedConstraints[type] = -1;
    }

    TechMapReport techReport;
    if (countGateTypeInScope(netlist, scope, name, targetType) == 0) {
        techReport.status = TechMapStatus::SUCCESS;
        techReport.message = "No gates of the requested type were found in the selected scope.";
        techReport.finalGateCount[targetType] = netlist.getGateCountByType(targetType);
        for (GateType type : allowedTypes) {
            techReport.finalGateCount[type] = netlist.getGateCountByType(type);
        }
    } else {
        techReport = customMapTechnology(
            netlist,
            {{targetType, 1}},
            allowedConstraints,
            scope,
            name,
            verbose);
    }

    return finalizeTechMapEditReport(netlist, before, techReport, "replaceGateType");
}

// 自訂規則映射引擎 (Interactive Custom Technology Mapping)
// 此函式允許使用者透過指定一個替換規則中的「欲拔除的積木 (Target)」與「欲生成的積木 (Allowed)」的數量限制
// 在電路上進行子圖同構掃描 (Subgraph Matching) 與結構替換。
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

    // 因為 customMapTechnology 的核心目的就是允許使用甚至發明新規則 (AUTO_LEARNED_SAT)
    std::vector<RuleSource> allSources = {};
    
    // 建立一個 Lambda 函式來封裝底層 API 呼叫，方便我們在學習新規則後重複執行
    auto runMapping = [&]() -> TechMapReport {
        const RewriteScopeResolution resolvedScope = resolveRewriteScope(netlist, scope, name);
        if (!resolvedScope.ok) {
            TechMapReport errReport;
            errReport.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
            errReport.message = "Failed to resolve rewrite scope: " + resolvedScope.message;
            return errReport;
        }

        switch (scope) {
            case TargetScope::WHOLE_NETLIST:
                return mapTechnology(netlist, targetConstraints, allowedConstraints, allSources, verbose);
            case TargetScope::NET_FANIN:
            case TargetScope::GATE_FANIN:
            case TargetScope::NET_FANOUT:
            case TargetScope::GATE_FANOUT:
                return mapTechnologyForCone(
                    netlist,
                    targetConstraints,
                    allowedConstraints,
                    resolvedScope.cone,
                    allSources,
                    verbose);
            default:
                TechMapReport errReport;
                errReport.status = TechMapStatus::ERROR_INVALID_CONSTRAINTS;
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
        
        // 如果有 -1，我們設定一個搜尋上限防止跑太久 (例如最多用到 3 顆 Gate)
        // 如果沒有 -1，那就只跑 1 次迴圈 (使用原本的確切數量)
        int maxDepth = hasAnyUnlimited ? 3 : 1; 
        
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
