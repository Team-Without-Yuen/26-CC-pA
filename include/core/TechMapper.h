#pragma once
#include <vector>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include "include/core/Netlist.h"
#include "include/core/RequestTimeBudget.h"

// 定義優化目標
enum class OptimizationGoal {
    AREA,   // 最小化總閘數
    DEPTH   // 最小化關鍵路徑層數 (Critical Path)
};

// 宣告 PatternNode 的類型
// 只保留一個代表「輸入」的類型 PI (Primary Input)
enum class NodeType { GATE, PI, CONST_1, CONST_0 };

struct PatternNode {
    NodeType nodeType;
    GateType gateType; // 只有當 nodeType == GATE 時有效
    int piIndex; // 只有當 nodeType == PI 時有效 (0=A, 1=B, 2=C...)
    std::vector<std::shared_ptr<PatternNode>> inputs;

    // --- Helpers for building the tree easily ---
    // 一個動態生成 PI 的 Helper
    static std::shared_ptr<PatternNode> makePI(int index) {
        return std::make_shared<PatternNode>(PatternNode{NodeType::PI, GateType::UNKNOWN, index, {}});
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
        return std::make_shared<PatternNode>(PatternNode{NodeType::GATE, type, -1, ins});
    }
};

enum class RuleSource {
    STANDARD_LIBRARY,   // 原本就內建在 LUT 中的標準規則 (最安全、最基礎)
    USER_CUSTOM,        // 使用者強制指定的替換
    AUTO_LEARNED_SAT,   // SAT Fallback 學會的拓樸變體
    OPTIMIZED_AREA,     // 化簡引擎算出的面積絕對最佳解
    OPTIMIZED_DEPTH     // 化簡引擎算出的深度絕對最佳解
};

// 替換規則結構
struct TechMapRule {
    std::string name; // 規則名稱 (例如: "OR3_to_NOR2_NAND1")
    RuleSource source;         // 這條規則是誰產生的？
    std::string truthTableHash; // 這條規則對應的真值表特徵

    // 左邊 (LHS)：目標要在電路裡尋找的形狀
    std::shared_ptr<PatternNode> targetPattern;  
    std::map<GateType, int> targetCounts;  // 例: {OR: 3}
    int removedGateCount;                  // 總計拔掉幾顆閘
    int targetDepth;       // LHS 树深度

    // 右邊 (RHS)：替換上去的新形狀
    std::shared_ptr<PatternNode> replacementPattern; 
    std::map<GateType, int> allowedCounts; // 例: {NOR: 2, NAND: 1}
    int addedGateCount;      
    int replacementDepth;  // RHS 树深度
    
    // 建構子
    TechMapRule(std::string ruleName, 
                RuleSource ruleSource,
                std::string hash,
                std::shared_ptr<PatternNode> lhs_target, 
                std::shared_ptr<PatternNode> rhs_replacement) 
        : name(std::move(ruleName)), 
          source(ruleSource),
          truthTableHash(std::move(hash)),
          targetPattern(lhs_target), 
          removedGateCount(0), 
          replacementPattern(rhs_replacement), 
          addedGateCount(0) {
        
        // 走訪並分析左邊 (Target) 的特徵
        std::unordered_set<PatternNode*> visitedTarget;
        analyzePattern(targetPattern, visitedTarget, targetCounts, removedGateCount);
        targetDepth = computePatternDepth(targetPattern);

        // 走訪並分析右邊 (Replacement) 的特徵
        std::unordered_set<PatternNode*> visitedReplacement;
        analyzePattern(replacementPattern, visitedReplacement, allowedCounts, addedGateCount);
        replacementDepth = computePatternDepth(replacementPattern);
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

    int computePatternDepth(const std::shared_ptr<PatternNode>& node) {
        if (!node || node->nodeType != NodeType::GATE) return 0;   // PI/CONST = 0
        int mx = 0;
        for (const auto& in : node->inputs)
            mx = std::max(mx, computePatternDepth(in));
        return mx + 1;                                             // 这颗闸 +1
    }
};

// --- Syntax Sugar ---
using NodePtr = std::shared_ptr<PatternNode>;

inline NodePtr A() { return PatternNode::makePI(0); }
inline NodePtr B() { return PatternNode::makePI(1); }
inline NodePtr C() { return PatternNode::makePI(2); }
inline NodePtr D() { return PatternNode::makePI(3); }
inline NodePtr E() { return PatternNode::makePI(4); }
inline NodePtr F() { return PatternNode::makePI(5); }
inline NodePtr G() { return PatternNode::makePI(6); }
// 需要寫 H, I, J... 或更多腳位時，直接呼叫 PI(index) 即可
inline NodePtr PI(int index) { return PatternNode::makePI(index); }
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
    // 紀錄 Pattern 中的輸入接腳 (PI) 分別對應到物理電路上的哪條 Net ID
    std::unordered_map<int, int> boundLeaves; 
    // 紀錄 Pattern 節點對應到物理的哪個 Gate ID (解決 DAG 共用節點比對)
    std::unordered_map<PatternNode*, int> mappedNodes; 
    // 記錄這個子圖到底包含了哪些實體 Gate ID (用於替換時拔除舊 Gate)
    std::unordered_set<int> matchedGates; 
    // 檢查某個 piIndex 是否已經綁定過實體 Net
    bool isLeafBound(int piIndex) const {
        return boundLeaves.count(piIndex) > 0;
    }
    // 取得該 piIndex 綁定的實體 Net ID
    int getBoundNet(int piIndex) const {
        auto it = boundLeaves.find(piIndex);
        return (it != boundLeaves.end()) ? it->second : -1;
    }
};

// --- 建立全域或類別內的查找表 ---
class TechMapper {
public:
    std::vector<TechMapRule> rules;

    explicit TechMapper(
        const request_time_budget::RequestDeadline* requestDeadline = nullptr)
        : requestDeadline_(requestDeadline) {
        // =======================================================
        // NOT 閘的替換規則
        // =======================================================
        // 用 NAND：A NAND A
        addBidirectionalRule("NOT_to_NAND_AA", _NOT(A()), _NAND(A(), A()));
        
        // 用 NOR：A NOR A
        addBidirectionalRule("NOT_to_NOR_AA",  _NOT(A()), _NOR(A(), A()));
        
        // 用 XOR：A XOR 1
        addBidirectionalRule("NOT_to_XOR_1",   _NOT(A()), _XOR(A(), C1()));
        
        // 用 XNOR：A XNOR 0
       addBidirectionalRule("NOT_to_XNOR_0",  _NOT(A()), _XNOR(A(), C0()));
        
        // 用 NAND：A NAND 1
        addBidirectionalRule("NOT_to_NAND_1",  _NOT(A()), _NAND(A(), C1()));
        
        // 用 NOR：A NOR 0
        addBidirectionalRule("NOT_to_NOR_0",   _NOT(A()), _NOR(A(), C0()));

        // =======================================================
        // BUF 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 AND：A AND A
        addBidirectionalRule("BUF_to_AND_AA", _BUF(A()), _AND(A(), A()));
        
        // 用 AND：A AND 1
        addBidirectionalRule("BUF_to_AND_1",  _BUF(A()), _AND(A(), C1()));
        
        // 用 OR：A OR A
        addBidirectionalRule("BUF_to_OR_AA",  _BUF(A()), _OR(A(), A()));
        
        // 用 OR：A OR 0
        addBidirectionalRule("BUF_to_OR_0",   _BUF(A()), _OR(A(), C0()));
        
        // 用 NOT：NOT (NOT A)
        addBidirectionalRule("BUF_to_NOT_NOT",_BUF(A()), _NOT(_NOT(A())));
        
        // 用 XOR：A XOR 0
        addBidirectionalRule("BUF_to_XOR_0",  _BUF(A()), _XOR(A(), C0()));
        
        // 用 XNOR：A XNOR 1
        addBidirectionalRule("BUF_to_XNOR_1", _BUF(A()), _XNOR(A(), C1()));
        
        // 用 NAND：(A NAND A) NAND (A NAND A)
        {
            // 建立 DAG 結構，並同時註冊展開與濃縮規則
            auto nand_a = _NAND(A(), A());
            addBidirectionalRule("BUF_to_NAND_NAND", _BUF(A()), _NAND(nand_a, nand_a));
        }
        
        // 用 NOR：(A NOR A) NOR (A NOR A)
        {
            // 建立 DAG 結構，並同時註冊展開與濃縮規則
            auto nor_a = _NOR(A(), A());
            addBidirectionalRule("BUF_to_NOR_NOR", _BUF(A()), _NOR(nor_a, nor_a));
        }

        // --- 用 2 種閘 ---
        // 用 {NOT, NAND}：NOT (A NAND 1)
        addBidirectionalRule("BUF_to_NOT_NAND_1", _BUF(A()), _NOT(_NAND(A(), C1())));
        
        // 用 {NOT, NOR}：NOT (A NOR 0)
        addBidirectionalRule("BUF_to_NOT_NOR_0",  _BUF(A()), _NOT(_NOR(A(), C0())));

        // =======================================================
        // AND 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NAND：(A NAND B) NAND (A NAND B)
        {
            // 建立 DAG：確保共用同一顆內部 NAND 閘
            auto nand_ab = _NAND(A(), B());
            addBidirectionalRule("AND_to_NAND_NAND", _AND(A(), B()), _NAND(nand_ab, nand_ab));
        }
        
        // 用 NOR：(A NOR A) NOR (B NOR B)
        addBidirectionalRule("AND_to_NOR_NOR", _AND(A(), B()), _NOR(_NOR(A(), A()), _NOR(B(), B())));

        // --- 用 2 種閘 ---
        // 用 {NOT, OR}：NOT ((NOT A) OR (NOT B))  (迪摩根定律)
        addBidirectionalRule("AND_to_NOT_OR", _AND(A(), B()), _NOT(_OR(_NOT(A()), _NOT(B()))));
        
        // 用 {NOT, NAND}：NOT (A NAND B)
        addBidirectionalRule("AND_to_NOT_NAND", _AND(A(), B()), _NOT(_NAND(A(), B())));
        
        // 用 {NOT, NOR}：(NOT A) NOR (NOT B)
        addBidirectionalRule("AND_to_NOT_NOR", _AND(A(), B()), _NOR(_NOT(A()), _NOT(B())));

        // =======================================================
        // OR 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NAND：(A NAND A) NAND (B NAND B)
        addBidirectionalRule("OR_to_NAND_NAND", _OR(A(), B()), _NAND(_NAND(A(), A()), _NAND(B(), B())));
        
        // 用 NOR：(A NOR B) NOR (A NOR B)
        {
            // 建立 DAG：確保共用同一顆內部 NOR 閘
            auto nor_ab = _NOR(A(), B());
            addBidirectionalRule("OR_to_NOR_NOR", _OR(A(), B()), _NOR(nor_ab, nor_ab));
        }

        // --- 用 2 種閘 ---
        // 用 {NOT, AND}：NOT ((NOT A) AND (NOT B)) (迪摩根定律)
        addBidirectionalRule("OR_to_NOT_AND", _OR(A(), B()), _NOT(_AND(_NOT(A()), _NOT(B()))));
        
        // 用 {NOT, NAND}：(NOT A) NAND (NOT B)
        addBidirectionalRule("OR_to_NOT_NAND", _OR(A(), B()), _NAND(_NOT(A()), _NOT(B())));
        
        // 用 {NOT, NOR}：NOT (A NOR B)
        addBidirectionalRule("OR_to_NOT_NOR", _OR(A(), B()), _NOT(_NOR(A(), B())));

        // =======================================================
        // NAND 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NOR：((A NOR A) NOR (B NOR B)) NOR ((A NOR A) NOR (B NOR B))
        {
            // 建立 DAG：inner 節點被最外層的 NOR 使用了兩次 (Fanout = 2)
            auto inner = _NOR(_NOR(A(), A()), _NOR(B(), B()));
            addBidirectionalRule("NAND_to_NOR_DAG", _NAND(A(), B()), _NOR(inner, inner));
        }

        // --- 用 2 種閘 ---
        // 用 {NOT, AND}：NOT (A AND B)
        addBidirectionalRule("NAND_to_NOT_AND", _NAND(A(), B()), _NOT(_AND(A(), B())));
        
        // 用 {NOT, OR}：(NOT A) OR (NOT B)  (迪摩根定律)
        addBidirectionalRule("NAND_to_NOT_OR",  _NAND(A(), B()), _OR(_NOT(A()), _NOT(B())));
        
        // 用 {NOT, NOR}：NOT ((NOT A) NOR (NOT B))
        addBidirectionalRule("NAND_to_NOT_NOR", _NAND(A(), B()), _NOT(_NOR(_NOT(A()), _NOT(B()))));

        // =======================================================
        // NOR 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NAND：((A NAND A) NAND (B NAND B)) NAND ((A NAND A) NAND (B NAND B))
        {
            // 建立 DAG：inner 節點被最外層的 NAND 使用了兩次 (Fanout = 2)
            auto inner = _NAND(_NAND(A(), A()), _NAND(B(), B()));
            addBidirectionalRule("NOR_to_NAND_DAG", _NOR(A(), B()), _NAND(inner, inner));
        }

        // --- 用 2 種閘 ---
        // 用 {NOT, OR}：NOT (A OR B)
        addBidirectionalRule("NOR_to_NOT_OR",   _NOR(A(), B()), _NOT(_OR(A(), B())));
        
        // 用 {NOT, AND}：(NOT A) AND (NOT B) (迪摩根定律)
        addBidirectionalRule("NOR_to_NOT_AND",  _NOR(A(), B()), _AND(_NOT(A()), _NOT(B())));
        
        // 用 {NOT, NAND}：NOT ((NOT A) NAND (NOT B))
        addBidirectionalRule("NOR_to_NOT_NAND", _NOR(A(), B()), _NOT(_NAND(_NOT(A()), _NOT(B()))));

        // =======================================================
        // XOR 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NAND (標準 4-NAND 實作): (A NAND (A NAND B)) NAND (B NAND (A NAND B))
        {
            // 建立 DAG：nand_ab 被使用了 2 次 (Fanout = 2)
            auto nand_ab = _NAND(A(), B());
            addBidirectionalRule("XOR_to_NAND_DAG", _XOR(A(), B()), _NAND(_NAND(A(), nand_ab), _NAND(B(), nand_ab)));
        }
        
        // 用 NOR: (A NOR B) NOR ((A NOR A) NOR (B NOR B))
        addBidirectionalRule("XOR_to_NOR_Tree", _XOR(A(), B()), _NOR(_NOR(A(), B()), _NOR(_NOR(A(), A()), _NOR(B(), B()))));

        // --- 用 2 種閘 ---
        // 用 {NOT, XNOR}: NOT (A XNOR B)
        addBidirectionalRule("XOR_to_NOT_XNOR", _XOR(A(), B()), _NOT(_XNOR(A(), B())));
        
        // 用 {NOT, AND}: NOT ((NOT A) AND (NOT B)) AND (NOT (A AND B))
        addBidirectionalRule("XOR_to_NOT_AND", _XOR(A(), B()), _AND(_NOT(_AND(_NOT(A()), _NOT(B()))), _NOT(_AND(A(), B()))));
                             
        // 用 {NOT, OR}: (NOT ((NOT A) OR B)) OR (NOT (A OR (NOT B)))
        addBidirectionalRule("XOR_to_NOT_OR", _XOR(A(), B()), _OR(_NOT(_OR(_NOT(A()), B())), _NOT(_OR(A(), _NOT(B())))));

        // =======================================================
        // XNOR 閘的替換規則
        // =======================================================
        // --- 只用 1 種閘 ---
        // 用 NOR (標準 4-NOR 實作): (A NOR (A NOR B)) NOR (B NOR (A NOR B))
        {
            // 建立 DAG：nor_ab 被使用了 2 次 (Fanout = 2)
            auto nor_ab = _NOR(A(), B());
            addBidirectionalRule("XNOR_to_NOR_DAG", _XNOR(A(), B()), _NOR(_NOR(A(), nor_ab), _NOR(B(), nor_ab)));
        }
        
        // 用 NAND: (A NAND B) NAND ((A NAND A) NAND (B NAND B))
        addBidirectionalRule("XNOR_to_NAND_Tree", _XNOR(A(), B()), _NAND(_NAND(A(), B()), _NAND(_NAND(A(), A()), _NAND(B(), B()))));

        // --- 用 2 種閘 ---
        // 用 {NOT, XOR}: NOT (A XOR B)
        addBidirectionalRule("XNOR_to_NOT_XOR", _XNOR(A(), B()), _NOT(_XOR(A(), B())));
        
        // 用 {NOT, AND}: NOT ( NOT ((NOT A) AND (NOT B)) AND (NOT (A AND B)) )
        addBidirectionalRule("XNOR_to_NOT_AND", _XNOR(A(), B()), _NOT(_AND(_NOT(_AND(_NOT(A()), _NOT(B()))), _NOT(_AND(A(), B())))));
                             
        // 用 {NOT, OR}: NOT ( (NOT (A OR (NOT B))) OR (NOT ((NOT A) OR B)) )
        addBidirectionalRule("XNOR_to_NOT_OR", _XNOR(A(), B()), _NOT(_OR(_NOT(_OR(A(), _NOT(B()))), _NOT(_OR(_NOT(A()), B())))));
    }

    // 輔助函式：走訪 PatternNode ，並計算計算最大輸入數
    int countPrimaryInputs(const std::shared_ptr<PatternNode>& root) const;

    // 輔助函式：走訪 PatternNode ，並計算使用到的gate的數量
    std::pair<std::map<GateType, int>, int> countGates(const std::shared_ptr<PatternNode>& root) const;

    // 輔助函式：計算邏輯閘的關鍵路徑層數
    int calculateDepth(const std::shared_ptr<PatternNode>& node) const;

    // 輔助函式：真值表雜湊編碼
    std::string generateTruthTableHash(const std::vector<bool>& truthTable) const;

    // 輔助函式：用來單獨檢查「某一側 (LHS 或 RHS)」是否符合給定的數量限制
    bool checkSideConstraints(const std::map<GateType, int>& ruleCounts, const std::map<GateType, int>& constraints) const;

    // 輔助函式：註冊雙向規則 (Bidirectional Rule Registration)
    void addBidirectionalRule(const std::string& baseName, 
                             std::shared_ptr<PatternNode> patternA, 
                             std::shared_ptr<PatternNode> patternB,
                             RuleSource source = RuleSource::STANDARD_LIBRARY);

    // 檢查某個 Rule 的「左邊與右邊」是否都滿足使用者的數量約束
    bool isRuleSatisfyingConstraints(const TechMapRule& rule, 
                                                 const std::map<GateType, int>& lhsConstraints,  // 對左邊 (拔除目標) 的限制
                                                 const std::map<GateType, int>& rhsConstraints) const; // 對右邊 (替換生成) 的限制

    // 統一的篩選器：取得所有合法的規則，並自動按照「面積優化程度」排序
    std::vector<TechMapRule> getValidRules(const std::map<GateType, int>& lhsConstraints, 
                                           const std::map<GateType, int>& rhsConstraints,
                                           const std::vector<RuleSource>& allowedSources,
                                           bool requireAreaReduction = false) const;

    // 遞迴比對核心
    // 輔助函式：判斷該 Gate 是否具有交換律 (Commutativity)
    bool isCommutative(GateType type) const;
    // 遞迴比對引擎核心 (Backward Pattern Matching)
    bool matchNet(Netlist& netlist, std::shared_ptr<PatternNode> pNode, int physNetId, MatchContext& ctx);
    // 觸發比對的入口點
    bool matchRootGate(Netlist& netlist, int physGateId, const TechMapRule& rule, MatchContext& ctx);
    
    // 驗證子圖合法性 (內部節點 Fan-out 檢查)
    bool isValidSubgraph(Netlist& netlist, const MatchContext& ctx, int rootGateId, bool allowLogicDuplication = false);
    
    // 針對一個rule的替換執行引擎
    void applyRule(Netlist& netlist,
                   const MatchContext& ctx,
                   int rootGateId,
                   const TechMapRule& rule,
                   TechMapReport& report,
                   bool allowLogicDuplication = false,
                   int* outFinalNetId = nullptr);

    // 統一的映射執行引擎
    bool executeMappingPass(Netlist& netlist, 
                            const std::unordered_set<int>* scopeGates, 
                            const std::vector<TechMapRule>& validRules, 
                            TechMapReport& report, 
                            bool verbose,
                            bool allowLogicDuplication = false,
                            const std::unordered_set<int>* strictContainment = nullptr);

    // 底層的實作引擎
    TechMapReport mapTechnologyCore(Netlist& netlist,
                                    const std::map<GateType, int>& targetConstraints,
                                    const std::map<GateType, int>& allowedConstraints,
                                    const std::unordered_set<int>* scopeGates,
                                    const std::vector<RuleSource>& allowedSources,
                                    bool verbose,
                                    const std::unordered_set<int>* strictContainment = nullptr);
    // 處理整個 Netlist 的 API
    TechMapReport mapTechnology(Netlist& netlist, 
                                const std::map<GateType, int>& targetConstraints, 
                                const std::map<GateType, int>& allowedConstraints, 
                                const std::vector<RuleSource>& allowedSources,
                                bool verbose = false);
    
    NetlistEditReport mapTechnologyWithReport(Netlist& netlist,
                                               const std::map<GateType, int>& targetConstraints,
                                               const std::map<GateType, int>& allowedConstraints,
                                               const std::vector<RuleSource>& allowedSources,
                                               bool verbose = false);
    // 針對特定 Cone 的 API
    TechMapReport mapTechnologyForCone(Netlist& netlist, 
                                       const std::map<GateType, int>& targetConstraints, 
                                       const std::map<GateType, int>& allowedConstraints, 
                                       const ConeResult& targetCone = ConeResult(), 
                                       const std::vector<RuleSource>& allowedSources = {},
                                       bool verbose = false);
    
    NetlistEditReport mapTechnologyForConeWithReport(Netlist& netlist,
                                                     const std::map<GateType, int>& targetConstraints,
                                                     const std::map<GateType, int>& allowedConstraints,
                                                     const ConeResult& targetCone = ConeResult(),
                                                     const std::vector<RuleSource>& allowedSources = {},
                                                     bool verbose = false);

    // 精確規則應用引擎，繞過 mapTechnologyCore 的查表與約束過濾機制
    TechMapReport applySpecificRule(Netlist& netlist, 
                                    const TechMapRule& rule, 
                                    const std::unordered_set<int>& targetCone, 
                                    bool verbose);

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
                                  bool verbose,
                                  int maxDepthConstraint = -1);

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

    // 輔助函式：產生所有積木組合 (重複組合)
    void generateCombosRec(const std::vector<GateType>& lib, 
                           int k, 
                           int startIdx, 
                           std::map<GateType, int>& current, 
                           std::vector<std::map<GateType, int>>& result) const;

    // 輔助函式：尋找絕對最小面積的拓樸
    std::shared_ptr<PatternNode> findMinimumAreaPattern(const std::vector<bool>& truthTable, 
                                                        int N, 
                                                        int currentArea, 
                                                        TechMapReport& report, 
                                                        bool verbose,
                                                        int maxDepthConstraint, 
                                                        const std::vector<GateType>& allowedTypes,
                                                        const std::vector<GateType>& bannedTypes);

    // 輔助函式：遞迴生成合法的 Fence (層級分配)
    // remaining_nodes: 剩下還有幾顆閘可以分配
    // remaining_levels: 剩下還有幾層需要分配
    // current_fence: 遞迴過程中暫存的分配狀態
    // result: 收集所有合法分配的容器
    void generateFencesRec(int remaining_nodes, 
                           int remaining_levels, 
                           std::vector<int>& current_fence, 
                           std::vector<std::vector<int>>& result) const;

    // 輔助函式：給定總閘數 k 與 目標深度 D，回傳所有合法的形狀
    std::vector<std::vector<int>> generateValidFences(int k, int D) const;

    // 精確合成引擎 (加入 Fence 深度約束)
    bool synthesizeFromTruthTableWithFence(const std::vector<bool>& targetTruthTable, 
                                           int N, 
                                           const std::map<GateType, int>& allowedConstraints, 
                                           const std::vector<int>& fenceShape, 
                                           TechMapReport& report, 
                                           bool verbose);

    // 輔助函式：尋找絕對最小深度的拓樸
    std::shared_ptr<PatternNode> findMinimumDepthPattern(const std::vector<bool>& truthTable, 
                                                         int N, 
                                                         int currentDepth, 
                                                         int currentArea, 
                                                         TechMapReport& report, 
                                                         bool verbose,
                                                         int maxAreaOverhead,
                                                         const std::vector<GateType>& allowedTypes, 
                                                         const std::vector<GateType>& bannedTypes);

    // 全域電路優化引擎 (Pattern Optimization Engine)
    // 針對給定的目標形狀 (LHS) 進行自動化的「面積」或「深度」化簡。
    // targetCone         : 圖形匹配與替換的掃描錐體範圍 (Gate ID 集合)
    // maxAreaOverhead    : 針對深度優化時，容許的額外面積極限 (預設為 0，代表由引擎動態決定)
    // maxDepthConstraint : 深度優化的最大層數限制 (預設 -1，代表不設限)
    // allowedTypes       : RHS 允許使用的閘類型白名單 (例如 AIG 模式傳入 {AND, NOT})
    // bannedTypes        : RHS 嚴格禁止使用的閘類型黑名單
    TechMapReport optimizePattern(Netlist& netlist,
                                  std::shared_ptr<PatternNode> lhsTarget,
                                  OptimizationGoal goal,
                                  const std::unordered_set<int>& targetCone,
                                  bool verbose = false,
                                  int maxAreaOverhead = 0,
                                  int maxDepthConstraint= -1,
                                  const std::vector<GateType>& allowedTypes = {},
                                  const std::vector<GateType>& bannedTypes = {});

    // 筛选「反相吸收」规则。
    //   条件全部满足才收：
    //   (1) 只用 STANDARD_LIBRARY（不碰 user custom / SAT 学来的规则）
    //   (2) RHS 只用「允许且非禁」的闸（rhsAllowed 控制，防止合并回被禁闸）
    //   (3) 深度严格减少（replacementDepth < targetDepth）——这才是吸收的意义
    //   (4) 面积不增（getCostDelta() <= 0）——吸收顺便省面积，绝不为深度爆面积
    //   排序键：深度收益优先（越负越好），面积其次。
    std::vector<TechMapRule> getInverterAbsorptionRules(const std::map<GateType, int>& rhsAllowed) const;

    // 依 allowed/banned 組出「RHS 可用的積木庫」
    std::map<GateType, int> buildRhsAllowedSet(const std::vector<GateType>& allowedTypes,
                                               const std::vector<GateType>& bannedTypes) const;

    // 用标准库双向规则把 NOT+复合闸 合并成 NAND/NOR/XNOR。
    TechMapReport absorbInverters(Netlist& netlist,
                                  const std::vector<GateType>& allowedTypes,
                                  const std::vector<GateType>& bannedTypes, bool verbose);

    // 範圍限定的反相吸收：只在 scopeGates 內部進行 NOT+複合閘 的合併。
    //   scopeGates 同時作為「root 候選範圍」與「子圖嚴格包含範圍」，
    //   確保絕不動到範圍外的閘（cone 題用來隔離 cone 內 / cone 外）。
    TechMapReport absorbInvertersOnGateSet(Netlist& netlist,
                                           const std::unordered_set<int>& scopeGates,
                                           const std::vector<GateType>& allowedTypes,
                                           const std::vector<GateType>& bannedTypes, bool verbose);

    // 直接對「指定的 gate id 集合」做基底強制，不重算 cone。
    // cone 範圍由 ConeReport.gateIds 給定，避免與內部 cone 演算法不一致。                              
    TechMapReport convertToBasisOnGateSet(Netlist& netlist,
                                          const std::unordered_set<int>& coneGates,
                                          const std::vector<GateType>& allowedTypes,
                                          const std::vector<GateType>& bannedTypes, 
                                          bool verbose);

    //----------------------------------------------------------------------------------------------------------------------------------------------------
    // 高階 API

    // ============================================================================
    // 全域/區域 邏輯閘轉換引擎 (Technology Mapping / Basis Conversion)
    // ============================================================================
    // 透過指定 allowedTypes (白名單) 與 bannedTypes (黑名單)，可以自由組合出以下經典的 EDA 拓樸結構：
    //   - AIG (And-Inverter Graph) : allowedTypes = {AND, NOT}
    //   - OIG (Or-Inverter Graph)  : allowedTypes = {OR, NOT}
    //   - NAND 網路 (高速萬用閘)   : allowedTypes = {NAND}
    //   - XAG (XOR-AND Graph)      : allowedTypes = {XOR, AND, NOT} (常用於全同態加密與量子運算)
    //   - ANF (代數正規式)         : allowedTypes = {XOR, AND} (無 NOT 閘的特化結構)
    // ============================================================================
    // netlist      : 要進行操作與替換的實體電路
    // allowedTypes : 「允許使用」的白名單。若提供此名單，不在名單上的閘都會被強制拆除。若為空，代表無白名單限制。
    // bannedTypes  : 「禁止使用」的黑名單。若只想單獨消滅某種閘 (例如只拆除 XOR/XNOR)，可留空白名單，只填黑名單。
    // scope        : 圖形匹配與替換的掃描範圍
    // name         : 搭配 scope 使用的目標名稱
    // verbose      : 是否印出詳細的轉換日誌
    // ============================================================================
    TechMapReport convertToBasis(Netlist& netlist,  
                                 TargetScope scope, 
                                 const std::string& name, 
                                 const std::vector<GateType>& allowedTypes, 
                                 const std::vector<GateType>& bannedTypes = {},
                                 bool verbose = false);

    NetlistEditReport convertToBasisWithReport(Netlist& netlist,
                                               TargetScope scope,
                                               const std::string& name,
                                               const std::vector<GateType>& allowedTypes,
                                               const std::vector<GateType>& bannedTypes = {},
                                               bool verbose = false);

    NetlistEditReport replaceGateTypeWithReport(Netlist& netlist,
                                                GateType targetType,
                                                const std::vector<GateType>& allowedTypes,
                                                TargetScope scope,
                                                const std::string& name,
                                                bool verbose = false);
        
    // 自訂規則映射引擎 (Interactive Custom Technology Mapping)
    // 此函式允許使用者透過指定一個替換規則中的「欲拔除的積木 (Target)」與「欲生成的積木 (Allowed)」的數量限制
    // 在電路上進行子圖同構掃描 (Subgraph Matching) 與結構替換，一次只會進行一個規則的替換。
    // 若內建查找表 (LUT) 中無符合的規則，會自動觸發 SAT 引擎進行等價拓樸的學習，會學習多個規則。
    // netlist            : 要進行掃描與結構替換的實體電路網表 (Netlist)
    // targetConstraints  : 替換條件 (LHS) - 使用者想要「拔除/尋找」的 Gate 類型與數量限制 ( -1 為無限制)
    // allowedConstraints : 替換條件 (RHS) - 使用者允許「新增/替換成」的 Gate 類型與數量限制 ( -1 無限制)
    // targetConstraints 與 allowedConstraints 中必須至少有一個 Gate 的類型大於 1
    // scope              : 圖形匹配的掃描範圍 (例如 WHOLE_NETLIST 掃描全圖，或 GATE_FANIN 掃描特定邏輯錐)
    // name               : 搭配 scope 使用的目標基準名稱 (例如特定的 Net 名稱或 Gate ID)
    // verbose            : 是否印出詳細的匹配過程、SAT 學習日誌與替換報表 (true 為開啟)
    // 回傳值             : TechMapReport (包含執行結果狀態、實際增減的邏輯閘數量統計與詳細訊息)
    TechMapReport customMapTechnology(Netlist& netlist, 
                                      const std::map<GateType, int>& targetConstraints, 
                                      const std::map<GateType, int>& allowedConstraints, 
                                      TargetScope scope, 
                                      const std::string& name,
                                      bool verbose = false);

    NetlistEditReport customMapTechnologyWithReport(Netlist& netlist,
                                                const std::map<GateType, int>& targetConstraints,
                                                const std::map<GateType, int>& allowedConstraints,
                                                TargetScope scope,
                                                const std::string& name,
                                                bool verbose);

private:
    const request_time_budget::RequestDeadline* requestDeadline_ = nullptr;
    mutable bool requestTimedOut_ = false;

    bool requestExpired() const;
    double boundedStrategySeconds(double strategyCapSeconds) const;
};
