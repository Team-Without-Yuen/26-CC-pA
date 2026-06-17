#pragma once
#include <vector>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include "include/core/Netlist.h"

// 定義轉換的作用範圍
enum class TargetScope {
    WHOLE_NETLIST,  // 針對整個電路 (預設)
    NET_FANIN,      // 針對 Net 的 Fanin
    NET_FANOUT,     // 針對 Net 的 Fanout
    GATE_FANIN,     // 針對 Gate 的 Fanin
    GATE_FANOUT     // 針對 Gate 的 Fanout
};

// 宣告 PatternNode 的類型
enum class NodeType { GATE, LEAF_A, LEAF_B, LEAF_C, LEAF_D, LEAF_E, LEAF_F, LEAF_G, CONST_1, CONST_0 };

struct PatternNode {
    NodeType nodeType;
    GateType gateType; // 只有當 nodeType == GATE 時有效
    std::vector<std::shared_ptr<PatternNode>> inputs;

    // --- Helpers for building the tree easily ---
    static std::shared_ptr<PatternNode> A() {
        return std::make_shared<PatternNode>(PatternNode{NodeType::LEAF_A, GateType::UNKNOWN, {}});
    }
    static std::shared_ptr<PatternNode> B() {
        return std::make_shared<PatternNode>(PatternNode{NodeType::LEAF_B, GateType::UNKNOWN, {}});
    }
    static std::shared_ptr<PatternNode> Const1() {
        return std::make_shared<PatternNode>(PatternNode{NodeType::CONST_1, GateType::UNKNOWN, {}});
    }
    static std::shared_ptr<PatternNode> Const0() {
        return std::make_shared<PatternNode>(PatternNode{NodeType::CONST_0, GateType::UNKNOWN, {}});
    }
    static std::shared_ptr<PatternNode> Gate(GateType type, std::shared_ptr<PatternNode> in1, std::shared_ptr<PatternNode> in2 = nullptr) {
        std::vector<std::shared_ptr<PatternNode>> ins = {in1};
        if (in2) ins.push_back(in2);
        return std::make_shared<PatternNode>(PatternNode{NodeType::GATE, type, ins});
    }
};

// 替換規則結構
struct TechMapRule {
    std::string name; // 規則名稱 (例如: "OR3_to_NOR2_NAND1")

    // 左邊 (LHS)：目標要在電路裡尋找的形狀
    std::shared_ptr<PatternNode> targetPattern;  
    std::map<GateType, int> targetCounts;  // 例: {OR: 3}
    int removedGateCount;                  // 總計拔掉幾顆閘

    // 右邊 (RHS)：替換上去的新形狀
    std::shared_ptr<PatternNode> replacementPattern; 
    std::map<GateType, int> allowedCounts; // 例: {NOR: 2, NAND: 1}
    int addedGateCount;                      
    
    // 建構子：傳入兩棵樹，並自動計算兩邊的 Cost
    TechMapRule(std::string ruleName, 
                std::shared_ptr<PatternNode> lhs_target, 
                std::shared_ptr<PatternNode> rhs_replacement) 
        : name(std::move(ruleName)), 
          targetPattern(lhs_target), 
          replacementPattern(rhs_replacement), 
          removedGateCount(0), 
          addedGateCount(0) {
        
        // 走訪並分析左邊 (Target) 的特徵
        std::unordered_set<PatternNode*> visitedTarget;
        analyzePattern(targetPattern, visitedTarget, targetCounts, removedGateCount);

        // 走訪並分析右邊 (Replacement) 的特徵
        std::unordered_set<PatternNode*> visitedReplacement;
        analyzePattern(replacementPattern, visitedReplacement, allowedCounts, addedGateCount);
    }

    // 計算這條規則套用後，電路總閘數的變化 (負數代表變好)
    int getCostDelta() const {
        return addedGateCount - removedGateCount; 
    }

private:
    // 遞迴走訪 Pattern 樹並統計資料
    void analyzePattern(const std::shared_ptr<PatternNode>& node, 
                        std::unordered_set<PatternNode*>& visited, 
                        std::map<GateType, int>& gateCounts, 
                        int& totalCount) {
        // 基本防呆與防止重複走訪 (支援 DAG 結構)
        if (!node || visited.count(node.get())) return;
        visited.insert(node.get());

        // 只有遇到真正的邏輯閘才增加計數，忽略 LEAF 或 CONST
        if (node->nodeType == NodeType::GATE) {
            gateCounts[node->gateType]++;
            totalCount++;
        }

        // 繼續遞迴走訪所有輸入節點
        for (const auto& input : node->inputs) {
            analyzePattern(input, visited, gateCounts, totalCount);
        }
    }
};

// --- Syntax Sugar ---
using NodePtr = std::shared_ptr<PatternNode>;

inline NodePtr A() { return PatternNode::A(); }
inline NodePtr B() { return PatternNode::B(); }
//擴充 C() 或 D() 以支援超過 2 個輸入的 Pattern
inline NodePtr C() { return std::make_shared<PatternNode>(PatternNode{NodeType::LEAF_C, GateType::UNKNOWN, {}}); }
inline NodePtr D() { return std::make_shared<PatternNode>(PatternNode{NodeType::LEAF_D, GateType::UNKNOWN, {}}); }
inline NodePtr E() { return std::make_shared<PatternNode>(PatternNode{NodeType::LEAF_E, GateType::UNKNOWN, {}}); }
inline NodePtr F() { return std::make_shared<PatternNode>(PatternNode{NodeType::LEAF_F, GateType::UNKNOWN, {}}); }
inline NodePtr G() { return std::make_shared<PatternNode>(PatternNode{NodeType::LEAF_G, GateType::UNKNOWN, {}}); }
inline NodePtr C1() { return PatternNode::Const1(); }  
inline NodePtr C0() { return PatternNode::Const0(); }  
inline NodePtr _NOT(NodePtr in) { return PatternNode::Gate(GateType::NOT, in); }
inline NodePtr _BUF(NodePtr in) { return PatternNode::Gate(GateType::BUF, in); }
inline NodePtr _AND(NodePtr in1, NodePtr in2) { return PatternNode::Gate(GateType::AND, in1, in2); }
inline NodePtr _OR(NodePtr in1, NodePtr in2) { return PatternNode::Gate(GateType::OR, in1, in2); }
inline NodePtr _NAND(NodePtr in1, NodePtr in2) { return PatternNode::Gate(GateType::NAND, in1, in2); }
inline NodePtr _NOR(NodePtr in1, NodePtr in2) { return PatternNode::Gate(GateType::NOR, in1, in2); }
inline NodePtr _XOR(NodePtr in1, NodePtr in2) { return PatternNode::Gate(GateType::XOR, in1, in2); }
inline NodePtr _XNOR(NodePtr in1, NodePtr in2) { return PatternNode::Gate(GateType::XNOR, in1, in2); }

// --- MatchContext ---
// 用來記錄比對過程中的狀態 
struct MatchContext {
    // 將 netA, netB 改為 Map。
    // 紀錄 Pattern 中的葉節點 (LEAF_A, LEAF_B, LEAF_C...) 分別對應到物理電路上的哪條 Net ID
    std::unordered_map<NodeType, int> boundLeaves; 
    // 紀錄 Pattern 節點對應到物理的哪個 Gate ID (解決 DAG 共用節點比對)
    std::unordered_map<PatternNode*, int> mappedNodes; 
    // 記錄這個子圖到底包含了哪些實體 Gate ID (用於替換時拔除舊 Gate)
    std::unordered_set<int> matchedGates; 
    // 檢查某個 NodeType (如 LEAF_A) 是否已經綁定過實體 Net
    bool isLeafBound(NodeType leafType) const {
        return boundLeaves.count(leafType) > 0;
    }
    // 取得綁定的實體 Net ID
    int getBoundNet(NodeType leafType) const {
        auto it = boundLeaves.find(leafType);
        return (it != boundLeaves.end()) ? it->second : -1;
    }
};

// --- 建立全域或類別內的查找表 ---
class TechMapper {
public:
    std::vector<TechMapRule> rules;

    TechMapper() {
        // =======================================================
        // NOT 閘的替換規則
        // =======================================================
        // 用 NAND：A NAND A
        rules.emplace_back("NOT_to_NAND_AA", _NOT(A()), _NAND(A(), A()));
        rules.emplace_back("NAND_AA_to_NOT", _NAND(A(), A()), _NOT(A()));
        
        // 用 NOR：A NOR A
        rules.emplace_back("NOT_to_NOR_AA",  _NOT(A()), _NOR(A(), A()));
        rules.emplace_back("NOR_AA_to_NOT",  _NOR(A(), A()), _NOT(A()));
        
        // 用 XOR：A XOR 1
        rules.emplace_back("NOT_to_XOR_1",   _NOT(A()), _XOR(A(), C1()));
        rules.emplace_back("XOR_1_to_NOT",   _XOR(A(), C1()), _NOT(A()));
        
        // 用 XNOR：A XNOR 0
        rules.emplace_back("NOT_to_XNOR_0",  _NOT(A()), _XNOR(A(), C0()));
        rules.emplace_back("XNOR_0_to_NOT",  _XNOR(A(), C0()), _NOT(A()));
        
        // 用 NAND：A NAND 1
        rules.emplace_back("NOT_to_NAND_1",  _NOT(A()), _NAND(A(), C1()));
        rules.emplace_back("NAND_1_to_NOT",  _NAND(A(), C1()), _NOT(A()));
        
        // 用 NOR：A NOR 0
        rules.emplace_back("NOT_to_NOR_0",   _NOT(A()), _NOR(A(), C0()));
        rules.emplace_back("NOR_0_to_NOT",   _NOR(A(), C0()), _NOT(A()));

        // =======================================================
        // BUF 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 AND：A AND A
        rules.emplace_back("BUF_to_AND_AA", _BUF(A()), _AND(A(), A()));
        rules.emplace_back("AND_AA_to_BUF", _AND(A(), A()), _BUF(A()));
        
        // 用 AND：A AND 1
        rules.emplace_back("BUF_to_AND_1",  _BUF(A()), _AND(A(), C1()));
        rules.emplace_back("AND_1_to_BUF",  _AND(A(), C1()), _BUF(A()));
        
        // 用 OR：A OR A
        rules.emplace_back("BUF_to_OR_AA",  _BUF(A()), _OR(A(), A()));
        rules.emplace_back("OR_AA_to_BUF",  _OR(A(), A()), _BUF(A()));
        
        // 用 OR：A OR 0
        rules.emplace_back("BUF_to_OR_0",   _BUF(A()), _OR(A(), C0()));
        rules.emplace_back("OR_0_to_BUF",   _OR(A(), C0()), _BUF(A()));
        
        // 用 NOT：NOT (NOT A)
        rules.emplace_back("BUF_to_NOT_NOT",_BUF(A()), _NOT(_NOT(A())));
        rules.emplace_back("NOT_NOT_to_BUF",_NOT(_NOT(A())), _BUF(A()));
        
        // 用 XOR：A XOR 0
        rules.emplace_back("BUF_to_XOR_0",  _BUF(A()), _XOR(A(), C0()));
        rules.emplace_back("XOR_0_to_BUF",  _XOR(A(), C0()), _BUF(A()));
        
        // 用 XNOR：A XNOR 1
        rules.emplace_back("BUF_to_XNOR_1", _BUF(A()), _XNOR(A(), C1()));
        rules.emplace_back("XNOR_1_to_BUF", _XNOR(A(), C1()), _BUF(A()));
        
        // 用 NAND：(A NAND A) NAND (A NAND A)
        {
            // 建立 DAG 結構，並同時註冊展開與濃縮規則
            auto nand_a = _NAND(A(), A());
            rules.emplace_back("BUF_to_NAND_NAND", _BUF(A()), _NAND(nand_a, nand_a));
            rules.emplace_back("NAND_NAND_to_BUF", _NAND(nand_a, nand_a), _BUF(A()));
        }
        
        // 用 NOR：(A NOR A) NOR (A NOR A)
        {
            // 建立 DAG 結構，並同時註冊展開與濃縮規則
            auto nor_a = _NOR(A(), A());
            rules.emplace_back("BUF_to_NOR_NOR", _BUF(A()), _NOR(nor_a, nor_a));
            rules.emplace_back("NOR_NOR_to_BUF", _NOR(nor_a, nor_a), _BUF(A()));
        }

        // --- 用 2 種閘 ---
        // 用 {NOT, NAND}：NOT (A NAND 1)
        rules.emplace_back("BUF_to_NOT_NAND_1", _BUF(A()), _NOT(_NAND(A(), C1())));
        rules.emplace_back("NOT_NAND_1_to_BUF", _NOT(_NAND(A(), C1())), _BUF(A()));
        
        // 用 {NOT, NOR}：NOT (A NOR 0)
        rules.emplace_back("BUF_to_NOT_NOR_0",  _BUF(A()), _NOT(_NOR(A(), C0())));
        rules.emplace_back("NOT_NOR_0_to_BUF",  _NOT(_NOR(A(), C0())), _BUF(A()));

        // =======================================================
        // AND 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NAND：(A NAND B) NAND (A NAND B)
        {
            // 建立 DAG：確保共用同一顆內部 NAND 閘
            auto nand_ab = _NAND(A(), B());
            rules.emplace_back("AND_to_NAND_NAND", _AND(A(), B()), _NAND(nand_ab, nand_ab));
            rules.emplace_back("NAND_NAND_to_AND", _NAND(nand_ab, nand_ab), _AND(A(), B()));
        }
        
        // 用 NOR：(A NOR A) NOR (B NOR B)
        rules.emplace_back("AND_to_NOR_NOR", _AND(A(), B()), _NOR(_NOR(A(), A()), _NOR(B(), B())));
        rules.emplace_back("NOR_NOR_to_AND", _NOR(_NOR(A(), A()), _NOR(B(), B())), _AND(A(), B()));

        // --- 用 2 種閘 ---
        // 用 {NOT, OR}：NOT ((NOT A) OR (NOT B))  (迪摩根定律)
        rules.emplace_back("AND_to_NOT_OR", _AND(A(), B()), _NOT(_OR(_NOT(A()), _NOT(B()))));
        rules.emplace_back("NOT_OR_to_AND", _NOT(_OR(_NOT(A()), _NOT(B()))), _AND(A(), B()));
        
        // 用 {NOT, NAND}：NOT (A NAND B)
        rules.emplace_back("AND_to_NOT_NAND", _AND(A(), B()), _NOT(_NAND(A(), B())));
        rules.emplace_back("NOT_NAND_to_AND", _NOT(_NAND(A(), B())), _AND(A(), B()));
        
        // 用 {NOT, NOR}：(NOT A) NOR (NOT B)
        rules.emplace_back("AND_to_NOT_NOR", _AND(A(), B()), _NOR(_NOT(A()), _NOT(B())));
        rules.emplace_back("NOT_NOR_to_AND", _NOR(_NOT(A()), _NOT(B())), _AND(A(), B()));
        
        // 用 {XOR, OR}：(A XOR B) XOR (A OR B)
        rules.emplace_back("AND_to_XOR_OR", _AND(A(), B()), _XOR(_XOR(A(), B()), _OR(A(), B())));
        rules.emplace_back("XOR_OR_to_AND", _XOR(_XOR(A(), B()), _OR(A(), B())), _AND(A(), B()));
        
        // 用 {XNOR, OR}：(A XNOR B) XNOR (A OR B)
        rules.emplace_back("AND_to_XNOR_OR", _AND(A(), B()), _XNOR(_XNOR(A(), B()), _OR(A(), B())));
        rules.emplace_back("XNOR_OR_to_AND", _XNOR(_XNOR(A(), B()), _OR(A(), B())), _AND(A(), B()));

        // =======================================================
        // OR 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NAND：(A NAND A) NAND (B NAND B)
        rules.emplace_back("OR_to_NAND_NAND", _OR(A(), B()), _NAND(_NAND(A(), A()), _NAND(B(), B())));
        rules.emplace_back("NAND_NAND_to_OR", _NAND(_NAND(A(), A()), _NAND(B(), B())), _OR(A(), B()));
        
        // 用 NOR：(A NOR B) NOR (A NOR B)
        {
            // 建立 DAG：確保共用同一顆內部 NOR 閘
            auto nor_ab = _NOR(A(), B());
            rules.emplace_back("OR_to_NOR_NOR", _OR(A(), B()), _NOR(nor_ab, nor_ab));
            rules.emplace_back("NOR_NOR_to_OR", _NOR(nor_ab, nor_ab), _OR(A(), B()));
        }

        // --- 用 2 種閘 ---
        // 用 {NOT, AND}：NOT ((NOT A) AND (NOT B)) (迪摩根定律)
        rules.emplace_back("OR_to_NOT_AND", _OR(A(), B()), _NOT(_AND(_NOT(A()), _NOT(B()))));
        rules.emplace_back("NOT_AND_to_OR", _NOT(_AND(_NOT(A()), _NOT(B()))), _OR(A(), B()));
        
        // 用 {NOT, NAND}：(NOT A) NAND (NOT B)
        rules.emplace_back("OR_to_NOT_NAND", _OR(A(), B()), _NAND(_NOT(A()), _NOT(B())));
        rules.emplace_back("NOT_NAND_to_OR", _NAND(_NOT(A()), _NOT(B())), _OR(A(), B()));
        
        // 用 {NOT, NOR}：NOT (A NOR B)
        rules.emplace_back("OR_to_NOT_NOR", _OR(A(), B()), _NOT(_NOR(A(), B())));
        rules.emplace_back("NOT_NOR_to_OR", _NOT(_NOR(A(), B())), _OR(A(), B()));
        
        // 用 {XOR, AND}：(A XOR B) XOR (A AND B)
        rules.emplace_back("OR_to_XOR_AND", _OR(A(), B()), _XOR(_XOR(A(), B()), _AND(A(), B())));
        rules.emplace_back("XOR_AND_to_OR", _XOR(_XOR(A(), B()), _AND(A(), B())), _OR(A(), B()));
        
        // 用 {XNOR, AND}：(A XNOR B) XNOR (A AND B)
        rules.emplace_back("OR_to_XNOR_AND", _OR(A(), B()), _XNOR(_XNOR(A(), B()), _AND(A(), B())));
        rules.emplace_back("XNOR_AND_to_OR", _XNOR(_XNOR(A(), B()), _AND(A(), B())), _OR(A(), B()));

        // =======================================================
        // NAND 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NOR：((A NOR A) NOR (B NOR B)) NOR ((A NOR A) NOR (B NOR B))
        {
            // 建立 DAG：inner 節點被最外層的 NOR 使用了兩次 (Fanout = 2)
            auto inner = _NOR(_NOR(A(), A()), _NOR(B(), B()));
            rules.emplace_back("NAND_to_NOR_DAG", _NAND(A(), B()), _NOR(inner, inner));
            rules.emplace_back("NOR_DAG_to_NAND", _NOR(inner, inner), _NAND(A(), B()));
        }

        // --- 用 2 種閘 ---
        // 用 {NOT, AND}：NOT (A AND B)
        rules.emplace_back("NAND_to_NOT_AND", _NAND(A(), B()), _NOT(_AND(A(), B())));
        rules.emplace_back("NOT_AND_to_NAND", _NOT(_AND(A(), B())), _NAND(A(), B()));
        
        // 用 {NOT, OR}：(NOT A) OR (NOT B)  (迪摩根定律)
        rules.emplace_back("NAND_to_NOT_OR",  _NAND(A(), B()), _OR(_NOT(A()), _NOT(B())));
        rules.emplace_back("NOT_OR_to_NAND",  _OR(_NOT(A()), _NOT(B())), _NAND(A(), B()));
        
        // 用 {NOT, NOR}：NOT ((NOT A) NOR (NOT B))
        rules.emplace_back("NAND_to_NOT_NOR", _NAND(A(), B()), _NOT(_NOR(_NOT(A()), _NOT(B()))));
        rules.emplace_back("NOT_NOR_to_NAND", _NOT(_NOR(_NOT(A()), _NOT(B()))), _NAND(A(), B()));

        // =======================================================
        // NOR 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NAND：((A NAND A) NAND (B NAND B)) NAND ((A NAND A) NAND (B NAND B))
        {
            // 建立 DAG：inner 節點被最外層的 NAND 使用了兩次 (Fanout = 2)
            auto inner = _NAND(_NAND(A(), A()), _NAND(B(), B()));
            rules.emplace_back("NOR_to_NAND_DAG", _NOR(A(), B()), _NAND(inner, inner));
            rules.emplace_back("NAND_DAG_to_NOR", _NAND(inner, inner), _NOR(A(), B()));
        }

        // --- 用 2 種閘 ---
        // 用 {NOT, OR}：NOT (A OR B)
        rules.emplace_back("NOR_to_NOT_OR",   _NOR(A(), B()), _NOT(_OR(A(), B())));
        rules.emplace_back("NOT_OR_to_NOR",   _NOT(_OR(A(), B())), _NOR(A(), B()));
        
        // 用 {NOT, AND}：(NOT A) AND (NOT B) (迪摩根定律)
        rules.emplace_back("NOR_to_NOT_AND",  _NOR(A(), B()), _AND(_NOT(A()), _NOT(B())));
        rules.emplace_back("NOT_AND_to_NOR",  _AND(_NOT(A()), _NOT(B())), _NOR(A(), B()));
        
        // 用 {NOT, NAND}：NOT ((NOT A) NAND (NOT B))
        rules.emplace_back("NOR_to_NOT_NAND", _NOR(A(), B()), _NOT(_NAND(_NOT(A()), _NOT(B()))));
        rules.emplace_back("NOT_NAND_to_NOR", _NOT(_NAND(_NOT(A()), _NOT(B()))), _NOR(A(), B()));

        // =======================================================
        // XOR 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NAND (標準 4-NAND 實作): (A NAND (A NAND B)) NAND (B NAND (A NAND B))
        {
            // 建立 DAG：nand_ab 被使用了 2 次 (Fanout = 2)
            auto nand_ab = _NAND(A(), B());
            rules.emplace_back("XOR_to_NAND_DAG", _XOR(A(), B()), _NAND(_NAND(A(), nand_ab), _NAND(B(), nand_ab)));
            rules.emplace_back("NAND_DAG_to_XOR", _NAND(_NAND(A(), nand_ab), _NAND(B(), nand_ab)), _XOR(A(), B()));
        }
        
        // 用 NOR: (A NOR B) NOR ((A NOR A) NOR (B NOR B))
        rules.emplace_back("XOR_to_NOR_Tree", _XOR(A(), B()), _NOR(_NOR(A(), B()), _NOR(_NOR(A(), A()), _NOR(B(), B()))));
        rules.emplace_back("NOR_Tree_to_XOR", _NOR(_NOR(A(), B()), _NOR(_NOR(A(), A()), _NOR(B(), B()))), _XOR(A(), B()));

        // --- 用 2 種閘 ---
        // 用 {NOT, XNOR}: NOT (A XNOR B)
        rules.emplace_back("XOR_to_NOT_XNOR", _XOR(A(), B()), _NOT(_XNOR(A(), B())));
        rules.emplace_back("NOT_XNOR_to_XOR", _NOT(_XNOR(A(), B())), _XOR(A(), B()));
        
        // 用 {NOT, AND}: NOT ((NOT A) AND (NOT B)) AND (NOT (A AND B))
        rules.emplace_back("XOR_to_NOT_AND", _XOR(A(), B()), _AND(_NOT(_AND(_NOT(A()), _NOT(B()))), _NOT(_AND(A(), B()))));
        rules.emplace_back("NOT_AND_to_XOR", _AND(_NOT(_AND(_NOT(A()), _NOT(B()))), _NOT(_AND(A(), B()))), _XOR(A(), B()));
                             
        // 用 {NOT, OR}: (NOT ((NOT A) OR B)) OR (NOT (A OR (NOT B)))
        rules.emplace_back("XOR_to_NOT_OR", _XOR(A(), B()), _OR(_NOT(_OR(_NOT(A()), B())), _NOT(_OR(A(), _NOT(B())))));
        rules.emplace_back("NOT_OR_to_XOR", _OR(_NOT(_OR(_NOT(A()), B())), _NOT(_OR(A(), _NOT(B())))), _XOR(A(), B()));


        // =======================================================
        // XNOR 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NOR (標準 4-NOR 實作): (A NOR (A NOR B)) NOR (B NOR (A NOR B))
        {
            // 建立 DAG：nor_ab 被使用了 2 次 (Fanout = 2)
            auto nor_ab = _NOR(A(), B());
            rules.emplace_back("XNOR_to_NOR_DAG", _XNOR(A(), B()), _NOR(_NOR(A(), nor_ab), _NOR(B(), nor_ab)));
            rules.emplace_back("NOR_DAG_to_XNOR", _NOR(_NOR(A(), nor_ab), _NOR(B(), nor_ab)), _XNOR(A(), B()));
        }
        
        // 用 NAND: (A NAND B) NAND ((A NAND A) NAND (B NAND B))
        rules.emplace_back("XNOR_to_NAND_Tree", _XNOR(A(), B()), _NAND(_NAND(A(), B()), _NAND(_NAND(A(), A()), _NAND(B(), B()))));
        rules.emplace_back("NAND_Tree_to_XNOR", _NAND(_NAND(A(), B()), _NAND(_NAND(A(), A()), _NAND(B(), B()))), _XNOR(A(), B()));

        // --- 用 2 種閘 ---
        // 用 {NOT, XOR}: NOT (A XOR B)
        rules.emplace_back("XNOR_to_NOT_XOR", _XNOR(A(), B()), _NOT(_XOR(A(), B())));
        rules.emplace_back("NOT_XOR_to_XNOR", _NOT(_XOR(A(), B())), _XNOR(A(), B()));
        
        // 用 {NOT, AND}: NOT ( NOT ((NOT A) AND (NOT B)) AND (NOT (A AND B)) )
        rules.emplace_back("XNOR_to_NOT_AND", _XNOR(A(), B()), _NOT(_AND(_NOT(_AND(_NOT(A()), _NOT(B()))), _NOT(_AND(A(), B())))));
        rules.emplace_back("NOT_AND_to_XNOR", _NOT(_AND(_NOT(_AND(_NOT(A()), _NOT(B()))), _NOT(_AND(A(), B())))), _XNOR(A(), B()));
                             
        // 用 {NOT, OR}: NOT ( (NOT (A OR (NOT B))) OR (NOT ((NOT A) OR B)) )
        rules.emplace_back("XNOR_to_NOT_OR", _XNOR(A(), B()), _NOT(_OR(_NOT(_OR(A(), _NOT(B()))), _NOT(_OR(_NOT(A()), B())))));
        rules.emplace_back("NOT_OR_to_XNOR", _NOT(_OR(_NOT(_OR(A(), _NOT(B()))), _NOT(_OR(_NOT(A()), B())))), _XNOR(A(), B()));
    }

    // 輔助函式：將 NodeType 對應回 Input Vector 的 Index (0, 1, 2...)
    int getLeafIndex(NodeType type) const;
    // 輔助函式：根據 Input Index 產生對應的 LEAF 節點 (A=0, B=1, C=2...)
    std::shared_ptr<PatternNode> createLeafNode(int index) const;

    // 輔助函式：用來單獨檢查「某一側 (LHS 或 RHS)」是否符合給定的數量限制
    bool checkSideConstraints(const std::map<GateType, int>& ruleCounts, const std::map<GateType, int>& constraints) const;

    // 輔助函式：註冊雙向規則 (Bidirectional Rule Registration)
    void addBidirectionalRule(const std::string& baseName, 
                             std::shared_ptr<PatternNode> patternA, 
                             std::shared_ptr<PatternNode> patternB);

    // 檢查某個 Rule 的「左邊與右邊」是否都滿足使用者的數量約束
    bool isRuleSatisfyingConstraints(const TechMapRule& rule, 
                                                 const std::map<GateType, int>& lhsConstraints,  // 對左邊 (拔除目標) 的限制
                                                 const std::map<GateType, int>& rhsConstraints) const; // 對右邊 (替換生成) 的限制

    // 統一的篩選器：取得所有合法的規則，並自動按照「面積優化程度」排序
    std::vector<TechMapRule> getValidRules(const std::map<GateType, int>& lhsConstraints, 
                                           const std::map<GateType, int>& rhsConstraints,
                                           bool requireAreaReduction = false) const;

    // 遞迴比對核心
    // 輔助函式：判斷該 Gate 是否具有交換律 (Commutativity)
    bool isCommutative(GateType type) const;
    // 遞迴比對引擎核心 (Backward Pattern Matching)
    bool matchNet(Netlist& netlist, std::shared_ptr<PatternNode> pNode, int physNetId, MatchContext& ctx);
    // 觸發比對的入口點
    bool matchRootGate(Netlist& netlist, int physGateId, const TechMapRule& rule, MatchContext& ctx);
    
    // 驗證子圖合法性 (內部節點 Fan-out 檢查)
    bool isValidSubgraph(Netlist& netlist, const MatchContext& ctx, int rootGateId);
    
    // 針對一個rule的替換執行引擎
    void applyRule(Netlist& netlist, const MatchContext& ctx, int rootGateId, const TechMapRule& rule);

    // 統一的映射執行引擎
    bool executeMappingPass(Netlist& netlist, 
                            const std::unordered_set<int>* scopeGates, 
                            const std::vector<TechMapRule>& validRules, 
                            TechMapReport& report, 
                            bool verbose);

    // targetConstraints  : 使用者想要「拔除/替換掉」的 Gate 類型與數量限制
    // allowedConstraints : 使用者允許「新增/使用」的 Gate 類型與數量限制
    // scopeGates         : 作用範圍 (Scope)。若提供，則僅對名單內的 Gate 進行操作；若為 nullptr 則掃描全電路。
    // verbose            : 是否輸出詳細的替換 Log
    // 回傳值             : TechMapReport (包含變更狀態、增刪數量統計與最終快照)
    // 底層的實作引擎
    TechMapReport mapTechnologyCore(Netlist& netlist, 
                                const std::map<GateType, int>& targetConstraints, 
                                const std::map<GateType, int>& allowedConstraints,
                                const std::unordered_set<int>* scopeGates,
                                bool verbose);
    // 處理整個 Netlist 的 API
    TechMapReport mapTechnology(Netlist& netlist, 
                                const std::map<GateType, int>& targetConstraints, 
                                const std::map<GateType, int>& allowedConstraints, // 建議預設為雙向
                                bool verbose = false);
    // 針對特定 Cone 的 API
    TechMapReport mapTechnologyForCone(Netlist& netlist, 
                                       const std::map<GateType, int>& targetConstraints, 
                                       const std::map<GateType, int>& allowedConstraints, 
                                       const ConeResult& targetCone = ConeResult(), 
                                       bool verbose = false);

    // 根據給定的 Gate 組成，計算最大可能的輸入腳位數 (N_max)
    int calculateMaxInputs(const std::map<GateType, int>& gateCounts) const;

    // 輔助函數：將動態變數的邏輯閘語意轉為 CNF (專供 Exact Synthesis 使用)
    void addGateSemantics(CaDiCaL::Solver& solver, GateType type, int inA, int inB, int out);

    // 精確合成引擎 (SAT-based Exact Synthesis)
    // 負責無中生有，用指定的積木數量拼湊出符合 targetTruthTable 的電路
    bool synthesizeFromTruthTable(const std::vector<bool>& targetTruthTable, 
                                  int N, 
                                  const std::map<GateType, int>& allowedConstraints, 
                                  TechMapReport& report, 
                                  bool verbose);

    // 將 SAT 算出來的抽象拓樸轉換為樹 (Replacement Pattern)
    std::shared_ptr<PatternNode> buildPatternFromTopology(int N, const std::vector<SynthesizedGate>& topology);

    // 核心引擎：窮舉所有合法的 LHS 拓樸 (支援 DAG)
    std::vector<std::shared_ptr<PatternNode>> generateAllValidTopologies(const std::map<GateType, int>& gateCounts,
                                                                        int N,
                                                                        TechMapReport& report,   // 新增
                                                                        bool verbose) const; 

    // 核心遞迴引擎：計算單一節點在「給定輸入向量」下的布林值 (支援 DAG 快取)
    bool evaluateNode(const std::shared_ptr<PatternNode>& node, 
                      const std::vector<bool>& inputVector,
                      std::unordered_map<PatternNode*, bool>& memo,
                      TechMapReport& report) const;

    // 對單一 Pattern 進行完整的布林邏輯模擬，回傳長度為 2^N 的 Truth Table
    std::vector<bool> simulatePattern(const std::shared_ptr<PatternNode>& root, 
                                      int N,
                                      TechMapReport& report,
                                      bool verbose) const;

    // 自動合成並註冊等價的 TechMapRule
    bool createAndRegisterCustomRule(const std::string& ruleName,
                                    const std::map<GateType, int>& lhsConstraints,
                                    const std::map<GateType, int>& rhsConstraints,
                                    TechMapReport& finalReport, 
                                    bool verbose);

    //--------------------------------------------------------------------------------------------------------

    // 輔助函式：自動過濾不需要的 Gate，並呼叫轉換引擎
    TechMapReport convertToBasis(Netlist& netlist, const std::vector<GateType>& allowedTypes, TargetScope scope = TargetScope::WHOLE_NETLIST, const std::string& name = "", bool verbose = false);

    // 如果不傳入後兩個參數，預設就是執行 WHOLE_NETLIST 的轉換
    // 基礎網路轉換 (AIG 相關)
    // 轉為 AIG (And-Inverter Graph)
    TechMapReport convertToAndNot(Netlist& netlist, TargetScope scope = TargetScope::WHOLE_NETLIST, const std::string& name = "", bool verbose = false);
    // 轉為 OIG (Or-Inverter Graph)
    TechMapReport convertToOrNot(Netlist& netlist, TargetScope scope = TargetScope::WHOLE_NETLIST, const std::string& name = "", bool verbose = false);         
    
    // 萬用閘轉換 (Universal Gates)
    // 轉為純 NAND 網路 (適合 CMOS 實體合成)
    TechMapReport convertToNand(Netlist& netlist, TargetScope scope = TargetScope::WHOLE_NETLIST, const std::string& name = "", bool verbose = false);
    // 轉為純 NOR 網路
    TechMapReport convertToNor(Netlist& netlist, TargetScope scope = TargetScope::WHOLE_NETLIST, const std::string& name = "", bool verbose = false);          

    // 密碼學與特定代數結構轉換
    // 轉為 XAG (XOR-AND Graph)
    TechMapReport convertToXag(Netlist& netlist, TargetScope scope = TargetScope::WHOLE_NETLIST, const std::string& name = "", bool verbose = false);
    // 轉為 ANF 網路 (代數正規式: XOR + AND)
    TechMapReport convertToAnf(Netlist& netlist, TargetScope scope = TargetScope::WHOLE_NETLIST, const std::string& name = "", bool verbose = false); 
    
    // 特殊邏輯組合
    // 轉為 {XOR, OR}
    TechMapReport convertToXorOr(Netlist& netlist, TargetScope scope = TargetScope::WHOLE_NETLIST, const std::string& name = "", bool verbose = false);
    // 轉為 {XNOR, AND}
    TechMapReport convertToXnorAnd(Netlist& netlist, TargetScope scope = TargetScope::WHOLE_NETLIST, const std::string& name = "", bool verbose = false);
    // 轉為 {XNOR, OR}
    TechMapReport convertToXnorOr(Netlist& netlist, TargetScope scope = TargetScope::WHOLE_NETLIST, const std::string& name = "", bool verbose = false);
        
    // 提供給使用者的萬用任意修改 API
    // 預設對整個電路 (WHOLE_NETLIST) 進行操作，此時 name 不需填寫
    TechMapReport customMapTechnology(Netlist& netlist, 
                                      const std::map<GateType, int>& targetConstraints, 
                                      const std::map<GateType, int>& allowedConstraints, 
                                      TargetScope scope, 
                                      const std::string& name,
                                      bool verbose);
};