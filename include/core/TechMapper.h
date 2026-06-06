#pragma once
#include <vector>
#include <memory>
#include <string>
#include <unordered_set>
#include "include/core/Netlist.h"

// 宣告 PatternNode 的類型
enum class NodeType { GATE, LEAF_A, LEAF_B, CONST_1, CONST_0 };

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
    GateType targetGate;                         // 目標要被替換掉的 Gate (e.g., AND)
    std::shared_ptr<PatternNode> pattern;        // 替換後的長相 (AST 樹)
    std::unordered_set<GateType> requiredGates;  // 這個 Pattern 總共用到了哪些類型的 Gate
    int addedGateCount;                          // 這個 Pattern 包含的 Gate 總數 (用來做 Cost / 優先級排序)
    
    TechMapRule(GateType target, std::shared_ptr<PatternNode> pat) 
        : targetGate(target), pattern(pat), addedGateCount(0) {
        // 在建構時，自動走訪 pattern 樹，計算出 requiredGates 與 addedGateCount
        analyzePattern(pattern);
    }

private:
    void analyzePattern(const std::shared_ptr<PatternNode>& node) {
        if (node->nodeType == NodeType::GATE) {
            requiredGates.insert(node->gateType);
            addedGateCount++;
            for (auto& in : node->inputs) analyzePattern(in);
        }
    }
};

// --- Syntax Sugar ---
inline auto A() { return PatternNode::A(); }
inline auto B() { return PatternNode::B(); }
inline auto C1() { return PatternNode::Const1(); }  // 之後會被轉成 "1'b1" 且 isConst=true
inline auto C0() { return PatternNode::Const0(); }  // 之後會被轉成 "1'b0" 且 isConst=true

inline auto _NOT(auto in) { return PatternNode::Gate(GateType::NOT, in); }
inline auto _AND(auto in1, auto in2) { return PatternNode::Gate(GateType::AND, in1, in2); }
inline auto _OR(auto in1, auto in2) { return PatternNode::Gate(GateType::OR, in1, in2); }
inline auto _NAND(auto in1, auto in2) { return PatternNode::Gate(GateType::NAND, in1, in2); }
inline auto _NOR(auto in1, auto in2) { return PatternNode::Gate(GateType::NOR, in1, in2); }
inline auto _XOR(auto in1, auto in2) { return PatternNode::Gate(GateType::XOR, in1, in2); }
inline auto _XNOR(auto in1, auto in2) { return PatternNode::Gate(GateType::XNOR, in1, in2); }

// 用來記錄比對過程中的狀態 
struct MatchContext {
    int netA = -1; // 對應到 Pattern 中的 LEAF_A 實際的 Net ID
    int netB = -1; // 對應到 Pattern 中的 LEAF_B 實際的 Net ID
    
    // 紀錄 Pattern 節點對應到物理的哪個 Gate ID (解決 DAG 共用節點比對)
    std::unordered_map<PatternNode*, int> mappedNodes; 
    
    // 記錄這個子圖到底包含了哪些實體 Gate ID
    std::unordered_set<int> matchedGates; 
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
        {
            auto a = A();
            rules.push_back({GateType::NOT, _NAND(a, a)});
        }
        // 用 NOR：A NOR A
        {
            auto a = A();
            rules.push_back({GateType::NOT, _NOR(a, a)});
        }
        // 用 XOR：A XOR 1
        rules.push_back({GateType::NOT, _XOR(A(), C1())});
        // 用 XNOR：A XNOR 0
        rules.push_back({GateType::NOT, _XNOR(A(), C0())});

        // =======================================================
        // BUF 閘的替換規則
        // =======================================================
        // 用 AND：A AND A
        {
            auto a = A();
            rules.push_back({GateType::BUF, _AND(a, a)});
        }
        // 用 OR：A OR A
        {
            auto a = A();
            rules.push_back({GateType::BUF, _OR(a, a)});
        }
        // 用 NOT：NOT (NOT A)
        rules.push_back({GateType::BUF, _NOT(_NOT(A()))});
        // 用 XOR：A XOR 0
        rules.push_back({GateType::BUF, _XOR(A(), C0())});
        // 用 XNOR：A XNOR 1
        rules.push_back({GateType::BUF, _XNOR(A(), C1())});
        // 用 NAND：(A NAND A) NAND (A NAND A)
        {
            auto a = A();
            auto nand_a = _NAND(a, a);
            rules.push_back({GateType::BUF, _NAND(nand_a, nand_a)});
        }
        // 用 NOR：(A NOR A) NOR (A NOR A)
        {
            auto a = A();
            auto nor_a = _NOR(a, a);
            rules.push_back({GateType::BUF, _NOR(nor_a, nor_a)});
        }

        // =======================================================
        // AND 閘的替換規則
        // =======================================================
        // 只用 1 種閘
        // 用 NAND：(A NAND B) NAND (A NAND B)
        {
            auto a = A(), b = B();
            auto nand_ab = _NAND(a, b);
            rules.push_back({GateType::AND, _NAND(nand_ab, nand_ab)});
        }
        // 用 NOR：(A NOR A) NOR (B NOR B)
        {
            auto a = A(), b = B();
            rules.push_back({GateType::AND, _NOR(_NOR(a, a), _NOR(b, b))});
        }
        // 用 2 種閘
        // 用 {NOT, OR}：NOT ((NOT A) OR (NOT B))
        {
            auto a = A(), b = B();
            rules.push_back({GateType::AND, _NOT(_OR(_NOT(a), _NOT(b)))});
        }
        // 用 {NOT, NAND}：NOT (A NAND B)
        rules.push_back({GateType::AND, _NOT(_NAND(A(), B()))});
        // 用 {NOT, NOR}：(NOT A) NOR (NOT B)
        {
            auto a = A(), b = B();
            rules.push_back({GateType::AND, _NOR(_NOT(a), _NOT(b))});
        }
        // 用 {XOR, OR}：(A XOR B) XOR (A OR B)
        {
            auto a = A(), b = B();
            rules.push_back({GateType::AND, _XOR(_XOR(a, b), _OR(a, b))});
        }
        // 用 {XNOR, OR}：(A XNOR B) XNOR (A OR B)
        {
            auto a = A(), b = B();
            rules.push_back({GateType::AND, _XNOR(_XNOR(a, b), _OR(a, b))});
        }

        // =======================================================
        // OR 閘的替換規則
        // =======================================================
        // 只用 1 種閘
        // 用 NAND：(A NAND A) NAND (B NAND B)
        {
            auto a = A(), b = B();
            rules.push_back({GateType::OR, _NAND(_NAND(a, a), _NAND(b, b))});
        }
        // 用 NOR：(A NOR B) NOR (A NOR B)
        {
            auto a = A(), b = B();
            auto nor_ab = _NOR(a, b);
            rules.push_back({GateType::OR, _NOR(nor_ab, nor_ab)});
        }
        // 用 2 種閘
        // 用 {NOT, AND}：NOT ((NOT A) AND (NOT B))
        {
            auto a = A(), b = B();
            rules.push_back({GateType::OR, _NOT(_AND(_NOT(a), _NOT(b)))});
        }
        // 用 {NOT, NAND}：(NOT A) NAND (NOT B)
        rules.push_back({GateType::OR, _NAND(_NOT(A()), _NOT(B()))});
        // 用 {NOT, NOR}：NOT (A NOR B)
        rules.push_back({GateType::OR, _NOT(_NOR(A(), B()))});
        // 用 {XOR, AND}：(A XOR B) XOR (A AND B)
        {
            auto a = A(), b = B();
            rules.push_back({GateType::OR, _XOR(_XOR(a, b), _AND(a, b))});
        }
        // 用 {XNOR, AND}：(A XNOR B) XNOR (A AND B)
        {
            auto a = A(), b = B();
            rules.push_back({GateType::OR, _XNOR(_XNOR(a, b), _AND(a, b))});
        }

        // =======================================================
        // NAND 閘的替換規則
        // =======================================================
        // 只用 1 種閘
        // 用 NOR：((A NOR A) NOR (B NOR B)) NOR ((A NOR A) NOR (B NOR B))
        {
            auto a = A(), b = B();
            auto inner = _NOR(_NOR(a, a), _NOR(b, b));
            rules.push_back({GateType::NAND, _NOR(inner, inner)});
        }
        // 用 2 種閘
        // 用 {NOT, AND}：NOT (A AND B)
        rules.push_back({GateType::NAND, _NOT(_AND(A(), B()))});
        // 用 {NOT, OR}：(NOT A) OR (NOT B)
        rules.push_back({GateType::NAND, _OR(_NOT(A()), _NOT(B()))});
        // 用 {NOT, NOR}：NOT ((NOT A) NOR (NOT B))
        rules.push_back({GateType::NAND, _NOT(_NOR(_NOT(A()), _NOT(B())))});

        // =======================================================
        // NOR 閘的替換規則
        // =======================================================
        // 只用 1 種閘
        // 用 NAND：((A NAND A) NAND (B NAND B)) NAND ((A NAND A) NAND (B NAND B))
        {
            auto a = A(), b = B();
            auto inner = _NAND(_NAND(a, a), _NAND(b, b));
            rules.push_back({GateType::NOR, _NAND(inner, inner)});
        }
        // 用 2 種閘
        // 用 {NOT, OR}：NOT (A OR B)
        rules.push_back({GateType::NOR, _NOT(_OR(A(), B()))});
        // 用 {NOT, AND}：(NOT A) AND (NOT B)
        rules.push_back({GateType::NOR, _AND(_NOT(A()), _NOT(B()))});
        // 用 {NOT, NAND}：NOT ((NOT A) NAND (NOT B))
        rules.push_back({GateType::NOR, _NOT(_NAND(_NOT(A()), _NOT(B())))});

        // =======================================================
        // XOR 閘的替換規則
        // =======================================================
        // 只用 1 種閘
        // 用 NAND : (A NAND (A NAND B)) NAND (B NAND (A NAND B))
        {
            auto a = A(), b = B();
            auto nand_ab = _NAND(a, b);
            rules.push_back({GateType::XOR, _NAND(_NAND(a, nand_ab), _NAND(b, nand_ab))});
        }
        // 用 NOR: (A NOR B) NOR ((A NOR A) NOR (B NOR B))
        {
            auto a = A(), b = B();
            rules.push_back({GateType::XOR, _NOR(_NOR(a, b), _NOR(_NOR(a, a), _NOR(b, b)))});
        }
        // 用 2 種閘
        // 用 {NOT, XNOR}: NOT (A XNOR B)
        rules.push_back({GateType::XOR, _NOT(_XNOR(A(), B()))});
        // 用 {NOT, AND}: NOT ((NOT A) AND (NOT B)) AND (NOT (A AND B))
        {
            auto a = A(), b = B();
            rules.push_back({GateType::XOR, _AND(_NOT(_AND(_NOT(a), _NOT(b))), _NOT(_AND(a, b)))});
        }
        // 用 {NOT, OR}: (NOT ((NOT A) OR B)) OR (NOT (A OR (NOT B)))
        {
            auto a = A(), b = B();
            rules.push_back({GateType::XOR, _OR(_NOT(_OR(_NOT(a), b)), _NOT(_OR(a, _NOT(b))))});
        }


        // =======================================================
        // XNOR 閘的替換規則
        // =======================================================
        // 只用 1 種閘
        // 用 NOR (標準 4-NOR 實作): (A NOR (A NOR B)) NOR (B NOR (A NOR B))
        {
            auto a = A(), b = B();
            auto nor_ab = _NOR(a, b);
            rules.push_back({GateType::XNOR, _NOR(_NOR(a, nor_ab), _NOR(b, nor_ab))});
        }
        // 用 NAND: (A NAND B) NAND ((A NAND A) NAND (B NAND B))
        {
            auto a = A(), b = B();
            rules.push_back({GateType::XNOR, _NAND(_NAND(a, b), _NAND(_NAND(a, a), _NAND(b, b)))});
        }
        // 用 2 種閘
        // 用 {NOT, XOR}: NOT (A XOR B)
        rules.push_back({GateType::XNOR, _NOT(_XOR(A(), B()))});
        // 用 {NOT, AND}: NOT ( NOT ((NOT A) AND (NOT B)) AND (NOT (A AND B)) )
        {
            auto a = A(), b = B();
            rules.push_back({GateType::XNOR, _NOT(_AND(_NOT(_AND(_NOT(a), _NOT(b))), _NOT(_AND(a, b))))});
        }
        // 用 {NOT, OR}: NOT ( (NOT (A OR (NOT B))) OR (NOT ((NOT A) OR B)) )
        {
            auto a = A(), b = B();
            rules.push_back({GateType::XNOR, _NOT(_OR(_NOT(_OR(a, _NOT(b))), _NOT(_OR(_NOT(a), b))))});
        }
    }

    // 取得所有「目標為 targetGate」且「需要的 Gate 皆包含在 allowedTypes 內」的規則
    // 並依照 addedGateCount (新增的閘數量) 由小到大排序
    std::vector<TechMapRule> getValidRules(GateType targetGate, const std::vector<GateType>& allowedTypes);

    // 遞迴比對核心
    bool matchNet(Netlist& netlist, std::shared_ptr<PatternNode> pNode, int physNetId, MatchContext& ctx);
    bool matchRootGate(Netlist& netlist, int physGateId, const TechMapRule& rule, MatchContext& ctx);
    
    // 驗證子圖合法性 (內部節點 Fan-out 檢查)
    bool isValidSubgraph(Netlist& netlist, const MatchContext& ctx, int rootGateId);
    
    // 執行替換
    void replaceSubgraph(Netlist& netlist, const MatchContext& ctx, int rootGateId, const TechMapRule& rule);

    // 執行一對多替換
    // 回傳值新增的 Gate 數量
    int applyForwardMapping(Netlist& netlist, int targetGateId, const TechMapRule& rule);

    // 執行多對一反向比對引擎
    // 回傳值: 成功替換並減少的 Gate 總數量
    int applyBackwardMapping(Netlist& netlist);

    // 轉換引擎
    // targetTypes  : 使用者想要「拔除/替換掉」的 Gate 類型 (例如 {OR, AND})
    // allowedTypes : 使用者允許「新增/使用」的 Gate 類型 (例如 {NAND, NOT})
    // 回傳值       : 總共變動的 Gate 數量 (正數代表變多，負數代表變少)
    // isOneToMany=true=展開(面積增加), false=濃縮(面積減少)
    int mapTechnology(Netlist& netlist, const std::vector<GateType>& targetTypes, const std::vector<GateType>& allowedTypes, bool isOneToMany);
};