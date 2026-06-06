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
                netlist.setNetConst(outNetId, true); // 使用新增的安全 API
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
int TechMapper::mapTechnology(Netlist& netlist, 
                              const std::vector<GateType>& targetTypes, 
                              const std::vector<GateType>& allowedTypes,
                              bool isOneToMany) {
    
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
            for (size_t i = 0; i < netlist.getGateCount(); ++i) {
                if (netlist.getGate(i).type != GateType::UNKNOWN) {
                    candidates.push_back(i);
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
        for (size_t i = 0; i < netlist.getGateCount(); ++i) {
            const Gate& g = netlist.getGate(i);
            if (g.type != GateType::UNKNOWN && targetSet.count(g.type)) {
                forwardCandidates.push_back(i);
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

// ─────────────────────────────────────────────────────────────────────────────
//  reconstructToAndNot
//  將整個 netlist 重新建構成只使用 AND 和 NOT gates
//  策略：先備份所有需要替換的 gate info，再做替換
//  De Morgan 定理：
//  - OR(a,b)   → NOT(AND(NOT(a), NOT(b)))
//  - NAND(a,b) → NOT(AND(a,b))
//  - NOR(a,b)  → AND(NOT(a), NOT(b))
//  - XOR(a,b)  → NOT(AND(NOT(AND(a,NOT(b))), NOT(AND(NOT(a),b))))
//  - XNOR(a,b) → NOT(XOR(a,b))
//  - BUF       → 直接連線（移除 gate）
//  - DFF       → 保留不動
//  回傳新增的 gate 數量
// ─────────────────────────────────────────────────────────────────────────────
int Netlist::reconstructToAndNot() {
    int newGateCount = 0;
    int synCounter = 0;
 
    // Step 1: 備份所有需要替換的 gate info
    struct GateInfo {
        int id;
        GateType type;
        std::vector<int> inputNetIds;
        int outputNetId;
    };
 
    std::vector<GateInfo> toReplace;
    for (int gi = 0; gi < (int)gates.size(); gi++) {
        GateType t = gates[gi].type;
        if (t == GateType::OR  || t == GateType::NAND || t == GateType::NOR  ||
            t == GateType::XOR || t == GateType::XNOR || t == GateType::BUF) {
            GateInfo info;
            info.id          = gi;
            info.type        = t;
            info.inputNetIds = gates[gi].inputNetIds;
            info.outputNetId = gates[gi].outputNetId;
            toReplace.push_back(info);
        }
    }
 
    // Step 2: 替換每個 gate
    for (int ri = 0; ri < (int)toReplace.size(); ri++) {
        GateInfo& info = toReplace[ri];
        int gi       = info.id;
        int outNetId = info.outputNetId;
        int a        = (info.inputNetIds.size() > 0) ? info.inputNetIds[0] : -1;
        int b        = (info.inputNetIds.size() > 1) ? info.inputNetIds[1] : -1;
 
        // 從 input net 的 loadGateIds 移除舊 gate
        for (int inNetId : info.inputNetIds) {
            std::vector<int> newLoads;
            for (int x : nets[inNetId].loadGateIds)
                if (x != gi) newLoads.push_back(x);
            nets[inNetId].loadGateIds = newLoads;
        }
        nets[outNetId].driverGateId = -1;
 
        // 清空舊 gate
        gates[gi].type = GateType::UNKNOWN;
        gates[gi].inputNetIds.clear();
        gates[gi].outputNetId = -1;
 
        std::string sid = std::to_string(synCounter++);
 
        if (info.type == GateType::BUF) {
            // BUF: 把所有接到 outNet 的 gate 改接到 a
            for (int loadGateId : nets[outNetId].loadGateIds) {
                for (int k = 0; k < (int)gates[loadGateId].inputNetIds.size(); k++) {
                    if (gates[loadGateId].inputNetIds[k] == outNetId) {
                        gates[loadGateId].inputNetIds[k] = a;
                        nets[a].loadGateIds.push_back(loadGateId);
                    }
                }
            }
            if (nets[outNetId].isPO) {
                nets[a].isPO = true;
                for (int pi = 0; pi < (int)primaryOutputs.size(); pi++)
                    for (int pj = 0; pj < (int)primaryOutputs[pi].netIds.size(); pj++)
                        if (primaryOutputs[pi].netIds[pj] == outNetId)
                            primaryOutputs[pi].netIds[pj] = a;
            }
            nets[outNetId].loadGateIds.clear();
 
        } else if (info.type == GateType::OR) {
            // OR(a,b) = NOT(AND(NOT(a), NOT(b)))
            int notA_net = addNet("_na_" + sid);
            int notB_net = addNet("_nb_" + sid);
            int and_net  = addNet("_and_" + sid);
            int notA_g = addGate("_notA_" + sid, GateType::NOT);
            int notB_g = addGate("_notB_" + sid, GateType::NOT);
            int and_g  = addGate("_and_"  + sid, GateType::AND);
            int notO_g = addGate("_notO_" + sid, GateType::NOT);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(and_g, notA_net);  connectGateInput(and_g, notB_net);
            connectGateOutput(and_g, and_net);
            connectGateInput(notO_g, and_net);  connectGateOutput(notO_g, outNetId);
            nets[outNetId].driverGateId = notO_g;
            newGateCount += 4;
 
        } else if (info.type == GateType::NAND) {
            // NAND(a,b) = NOT(AND(a,b))
            int and_net = addNet("_and_" + sid);
            int and_g   = addGate("_and_" + sid, GateType::AND);
            int not_g   = addGate("_not_" + sid, GateType::NOT);
            connectGateInput(and_g, a);     connectGateInput(and_g, b);
            connectGateOutput(and_g, and_net);
            connectGateInput(not_g, and_net); connectGateOutput(not_g, outNetId);
            nets[outNetId].driverGateId = not_g;
            newGateCount += 2;
 
        } else if (info.type == GateType::NOR) {
            // NOR(a,b) = AND(NOT(a), NOT(b))
            int notA_net = addNet("_na_" + sid);
            int notB_net = addNet("_nb_" + sid);
            int notA_g = addGate("_notA_" + sid, GateType::NOT);
            int notB_g = addGate("_notB_" + sid, GateType::NOT);
            int and_g  = addGate("_and_"  + sid, GateType::AND);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(and_g, notA_net);  connectGateInput(and_g, notB_net);
            connectGateOutput(and_g, outNetId);
            nets[outNetId].driverGateId = and_g;
            newGateCount += 3;
 
        } else if (info.type == GateType::XOR) {
            // XOR(a,b) = NOT(AND(NOT(AND(a,NOT(b))), NOT(AND(NOT(a),b))))
            std::string s2  = std::to_string(synCounter++);
            int notB_net  = addNet("_nb_"  + sid);
            int notA_net  = addNet("_na_"  + sid);
            int and1_net  = addNet("_a1_"  + sid);
            int and2_net  = addNet("_a2_"  + sid);
            int notX_net  = addNet("_nx_"  + s2);
            int notY_net  = addNet("_ny_"  + s2);
            int and3_net  = addNet("_a3_"  + s2);
            int notB_g  = addGate("_notB_"  + sid, GateType::NOT);
            int notA_g  = addGate("_notA_"  + sid, GateType::NOT);
            int and1_g  = addGate("_and1_"  + sid, GateType::AND);
            int and2_g  = addGate("_and2_"  + sid, GateType::AND);
            int notX_g  = addGate("_notX_"  + s2,  GateType::NOT);
            int notY_g  = addGate("_notY_"  + s2,  GateType::NOT);
            int and3_g  = addGate("_and3_"  + s2,  GateType::AND);
            int notF_g  = addGate("_notF_"  + s2,  GateType::NOT);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(and1_g, a);        connectGateInput(and1_g, notB_net);
            connectGateOutput(and1_g, and1_net);
            connectGateInput(and2_g, notA_net); connectGateInput(and2_g, b);
            connectGateOutput(and2_g, and2_net);
            connectGateInput(notX_g, and1_net); connectGateOutput(notX_g, notX_net);
            connectGateInput(notY_g, and2_net); connectGateOutput(notY_g, notY_net);
            connectGateInput(and3_g, notX_net); connectGateInput(and3_g, notY_net);
            connectGateOutput(and3_g, and3_net);
            connectGateInput(notF_g, and3_net); connectGateOutput(notF_g, outNetId);
            nets[outNetId].driverGateId = notF_g;
            newGateCount += 8;
 
        } else if (info.type == GateType::XNOR) {
            // XNOR(a,b) = NOT(XOR(a,b)) → 先展開 XOR 再加 NOT
            std::string s2  = std::to_string(synCounter++);
            std::string s3  = std::to_string(synCounter++);
            int notB_net  = addNet("_nb_"  + sid);
            int notA_net  = addNet("_na_"  + sid);
            int and1_net  = addNet("_a1_"  + sid);
            int and2_net  = addNet("_a2_"  + sid);
            int notX_net  = addNet("_nx_"  + s2);
            int notY_net  = addNet("_ny_"  + s2);
            int and3_net  = addNet("_a3_"  + s2);
            int xor_net   = addNet("_xr_"  + s3);
            int notB_g  = addGate("_notB_"  + sid, GateType::NOT);
            int notA_g  = addGate("_notA_"  + sid, GateType::NOT);
            int and1_g  = addGate("_and1_"  + sid, GateType::AND);
            int and2_g  = addGate("_and2_"  + sid, GateType::AND);
            int notX_g  = addGate("_notX_"  + s2,  GateType::NOT);
            int notY_g  = addGate("_notY_"  + s2,  GateType::NOT);
            int and3_g  = addGate("_and3_"  + s2,  GateType::AND);
            int notF_g  = addGate("_notF_"  + s2,  GateType::NOT);
            int notXor_g = addGate("_nxor_" + s3,  GateType::NOT);
            connectGateInput(notB_g, b);        connectGateOutput(notB_g, notB_net);
            connectGateInput(notA_g, a);        connectGateOutput(notA_g, notA_net);
            connectGateInput(and1_g, a);        connectGateInput(and1_g, notB_net);
            connectGateOutput(and1_g, and1_net);
            connectGateInput(and2_g, notA_net); connectGateInput(and2_g, b);
            connectGateOutput(and2_g, and2_net);
            connectGateInput(notX_g, and1_net); connectGateOutput(notX_g, notX_net);
            connectGateInput(notY_g, and2_net); connectGateOutput(notY_g, notY_net);
            connectGateInput(and3_g, notX_net); connectGateInput(and3_g, notY_net);
            connectGateOutput(and3_g, and3_net);
            connectGateInput(notF_g, and3_net); connectGateOutput(notF_g, xor_net);
            connectGateInput(notXor_g, xor_net); connectGateOutput(notXor_g, outNetId);
            nets[outNetId].driverGateId = notXor_g;
            newGateCount += 9;
        }
    }
 
    trimDeadLogic();
    return newGateCount;
}