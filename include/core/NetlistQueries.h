#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "include/core/NetlistTypes.h"

// =========================================================================
// Unified Query / Report Types
//
// 這個 header 只放高階查詢 API 的純資料型別。
// 不放 Netlist graph 儲存、不放 traversal 實作、不放 mutation API。
//
// 注意：
// - Path / Depth 相關 query/report 已拆到 PathTypes.h。
// - 本檔只保留 Basic / Connectivity / Function / Cone 這類非路徑型查詢。
// =========================================================================

// =========================================================================
// Basic Query Types
// =========================================================================

enum class BasicQueryType {
    Summary,                 // 回傳 design 規模與 gate type 統計
    ListGates,               // 列出所有 gate names
    ListNets,                // 列出所有 net names
    ListPrimaryInputs,       // 列出所有 primary input port names
    ListPrimaryOutputs,      // 列出所有 primary output port names
    ListDffs,                // 列出所有 DFF instance names
    ListCombinationalGates,  // 列出所有 combinational gate names
    GateInfo,                // 查單一 gate 的基本資訊
    NetInfo,                 // 查單一 net 的基本資訊
    PortInfo,                // 查單一 PI/PO port 的基本資訊
    CountByGateType,         // 統計指定 gate type 數量；UNKNOWN 表示回傳全部統計
    GatesByType,             // 列出指定 gate type 的 gates
    GatesWithConstantInput,  // 列出 input 接 constant 的 gates
    StructuralIssues         // 回報 undriven/no-load/floating/unconnected
};

struct BasicQuery {
    BasicQueryType type = BasicQueryType::Summary;
    std::string name;                       // GateInfo/NetInfo/PortInfo 使用的物件名稱
    GateType gateType = GateType::UNKNOWN;  // gate type filter；UNKNOWN 表示不限制或列全部
    int constValue = -1;                    // constant input filter：-1 不限制，0 表示 1'b0，1 表示 1'b1
    int inputCount = -1;                    // gate input-count filter：-1 表示不限制
    bool includeIds = true;                 // 回傳 report 時是否填 gateIds/netIds
    bool includeNames = true;               // 回傳 report 時是否填 gateNames/netNames/portNames
};

struct PortSummary {
    std::string name;
    int width = 0;
    int msb = -1;
    int lsb = -1;
    bool isBus = false;
    bool isInput = false;
    bool isOutput = false;
};

struct BasicReport {
    bool ok = false;                        // 查詢是否成功；名稱不存在或 type 不合法時為 false
    std::string message;                    // 給 debug / LLM response 使用的簡短訊息

    size_t gateCount = 0;                   // design 或篩選後 gate 數量
    size_t netCount = 0;                    // design 或篩選後 net 數量
    size_t logicalWireCount = 0;            // Verilog declaration 層級的 wire 數量
    size_t primaryInputCount = 0;           // primary input port 數量
    size_t primaryOutputCount = 0;          // primary output port 數量

    int objectId = -1;                      // GateInfo/NetInfo 的 ID
    std::string objectName;                 // GateInfo/NetInfo/PortInfo 的名稱
    std::string typeName;                   // gate type 或 net/port 類型描述
    std::string formattedInfo;              // getGateInfo() 這類人類可讀格式

    bool exists = false;                    // 指定 name 是否存在
    bool isDff = false;                     // GateInfo 使用
    bool isCombinational = false;           // GateInfo 使用
    bool isPrimaryInput = false;            // NetInfo 使用
    bool isPrimaryOutput = false;           // NetInfo 使用
    bool isConstant = false;                // NetInfo 使用
    bool isBus = false;                     // PortInfo 使用
    int portWidth = -1;                     // PortInfo 使用

    std::vector<int> gateIds;               // 查詢得到的 gate IDs
    std::vector<int> netIds;                // 查詢得到的 net IDs
    std::vector<std::string> gateNames;     // 查詢得到的 gate names
    std::vector<std::string> netNames;      // 查詢得到的 net names
    std::vector<std::string> portNames;     // 查詢得到的 port names
    std::vector<PortSummary> ports;         // batch PI/PO name、width、range、direction

    std::map<GateType, int> gateTypeCounts; // 各 gate type 統計

    std::vector<std::string> undrivenNets;      // structural issue：無 driver 的 nets
    std::vector<std::string> noLoadNets;        // structural issue：無 load 的 nets
    std::vector<std::string> floatingNets;      // structural issue：undriven/no-load union
    std::vector<std::string> unconnectedGates;  // structural issue：有未連接 pin 的 gates
};

// =========================================================================
// Direct Connectivity Query Types
// =========================================================================

enum class DirectConnectivityQueryType {
    NetDriverGates,     // 查某個 net / bus 的直接 driver gates
    NetLoadGates,       // 查某個 net / bus 直接 load 到哪些 gates
    FanoutLoadReport,   // 依 Problem A QA fanout load 定義回報 pin-level loads
    GlobalFanoutReport, // 掃描全設計或所有 PI 的 fanout loads / max / violations
    GateInputs,         // 查某個 gate 的直接 input nets
    GateOutput,         // 查某個 gate 的 output net
    GateFanin,          // 查直接驅動某 gate inputs 的上一層 gates
    GateFanout,         // 查某 gate output 直接 fanout 到哪些 gates
    DirectlyConnected   // 判斷指定 gate 與指定 net 是否直接相連
};

struct DirectConnectivityQuery {
    DirectConnectivityQueryType type = DirectConnectivityQueryType::NetDriverGates;
    std::string gateName;     // GateInputs/GateOutput/GateFanin/GateFanout/DirectlyConnected 使用
    std::string netName;      // NetDriverGates/NetLoadGates/FanoutLoadReport/DirectlyConnected 使用
    int fanoutLimit = -1;     // GlobalFanoutReport 使用；-1 表示只回報 max，不檢查 violation
    bool primaryInputsOnly = false; // GlobalFanoutReport 使用；true 時只掃 PI nets
    bool includeZeroFanout = false; // GlobalFanoutReport 使用；true 時保留 0 fanout nets
    bool includeIds = true;   // 是否填 gateIds/netIds
    bool includeNames = true; // 是否填 gateNames/netNames
};

struct FanoutLoadReport {
    bool ok = false;                         // net 是否存在並成功建立 report
    std::string message;                     // debug / LLM response 用簡短訊息

    int netId = -1;                          // scalar net ID；bus aggregate 時可為 -1
    std::string netName;                     // scalar net 或 query name

    // Problem A QA fanout load 分類：
    // totalLoadCount =
    //   combinational gate input pins
    // + DFF D pins
    // + DFF CK pins
    // + DFF RN/SN pins
    // + DFF other named input pins
    // + primary output connections
    std::vector<int> combinationalGateLoads; // primitive gate input pin loads；同 gate 多 pin 可能重複
    std::vector<int> dffDataLoads;           // DFF .D pin loads
    std::vector<int> dffClockLoads;          // DFF .CK pin loads
    std::vector<int> dffResetSetLoads;       // DFF .RN / .SN pin loads
    std::vector<int> dffOtherLoads;          // 其他 DFF input pin loads，保留給未來擴充
    std::vector<int> allGateLoadIds;         // 上面所有 gate/DFF pin loads 的合併結果

    bool drivesPrimaryOutput = false;        // 是否直接連到 primary output
    size_t primaryOutputLoadCount = 0;       // scalar PO 為 1；bus aggregate 可大於 1
    size_t totalLoadCount = 0;               // 符合 QA 定義的總 fanout load 數
};

struct GlobalFanoutReport {
    bool ok = false;                         // 是否成功建立全域 report
    std::string message;                     // debug / LLM response 用簡短訊息

    int fanoutLimit = -1;                    // 檢查限制；-1 表示未指定
    bool primaryInputsOnly = false;          // 是否只掃 primary input nets
    bool includeZeroFanout = false;          // 是否包含 0 fanout nets
    bool satisfiesLimit = true;              // fanoutLimit >= 0 時，是否沒有 violation

    size_t checkedNetCount = 0;              // 實際納入統計的 net 數
    size_t maxFanout = 0;                    // 最大 QA fanout load count

    std::vector<FanoutLoadReport> netReports;       // 所有納入統計的 net reports
    std::vector<FanoutLoadReport> maxFanoutReports; // fanout 等於 maxFanout 的 nets
    std::vector<FanoutLoadReport> violatingReports; // fanoutLimit >= 0 且超標的 nets
};

struct DirectConnectivityReport {
    bool ok = false;                    // query 是否成功；名稱不存在或必要欄位缺失時為 false
    bool exists = false;                // 指定 gate/net 是否存在
    bool connected = false;             // DirectlyConnected 的主要結果
    std::string message;                // 給 debug / LLM response 的簡短訊息

    std::string gateName;               // 查詢指定或解析出的 gate name
    std::string netName;                // 查詢指定或解析出的 net name
    int gateId = -1;                    // 單一 gate ID 結果；沒有唯一 gate 時為 -1
    int netId = -1;                     // 單一 net ID 結果；沒有唯一 net 時為 -1
    size_t count = 0;                   // 結果數量

    std::vector<int> gateIds;           // 查詢結果中的 gate IDs
    std::vector<int> netIds;            // 查詢結果中的 net IDs
    std::vector<std::string> gateNames; // 查詢結果中的 gate names
    std::vector<std::string> netNames;  // 查詢結果中的 net names

    FanoutLoadReport fanoutLoadReport;  // FanoutLoadReport query 的完整分類結果
    GlobalFanoutReport globalFanoutReport; // GlobalFanoutReport query 的完整彙整結果
};

// =========================================================================
// Function Analysis Query Types
// =========================================================================

enum class FunctionQueryType {
    Equivalence,       // 檢查 netNameA 與 netNameB 是否功能等價
    ConditionalEquivalence, // 固定 conditionNetName 後檢查 netNameA/netNameB 是否等價
    CanBeValue,        // 檢查 netNameA 是否存在某組輸入可等於 constValue
    ConstantFunction,  // 檢查 netNameA 是否恆等於 constValue
    AlwaysZero,        // 檢查 netNameA 是否永遠為 0
    AlwaysOne,         // 檢查 netNameA 是否永遠為 1
    TruthStatus,       // 回報 netNameA 是 always 0、always 1，或 non-constant
    FunctionalDependence, // 檢查 netNameA 的 Boolean function 是否依賴 netNameB PI / pseudo-PI
    Symmetry,          // 檢查 netNameA function 交換兩個 PI / pseudo-PI 後是否不變
    BooleanExpression, // 展開 netNameA 的完整 Boolean expression
    SimplifiedBooleanExpression, // 以 maxExpressionDepth 限制展開深度
    PrimaryInputsOfNet // 回報 netNameA fanin cone 的 PI / pseudo-PI leaves
};

struct FunctionQuery {
    FunctionQueryType type = FunctionQueryType::TruthStatus;
    std::string netNameA;   // 主要分析目標；Equivalence 時代表第一個 net / bus
    std::string netNameB;   // Equivalence 使用的第二個 net / bus
    std::string conditionNetName; // ConditionalEquivalence 使用的 scalar condition net
    std::string symmetryInputNameA; // Symmetry 使用的第一個 scalar PI / DFF.Q pseudo-PI
    std::string symmetryInputNameB; // Symmetry 使用的第二個 scalar PI / DFF.Q pseudo-PI
    int conditionValue = -1; // ConditionalEquivalence 使用；只能是 0 或 1
    int constValue = -1;    // CanBeValue / ConstantFunction 使用；只能是 0 或 1
    int maxExpressionDepth = 10; // SimplifiedBooleanExpression 使用；必須 >= 0
    double timeLimitSeconds = 30.0; // SAT query wall-clock limit
};

struct FunctionReport {
    bool ok = false;              // query 是否成功；名稱不存在或參數不合法時為 false
    bool exists = false;          // 對 yes/no 問題的主要布林答案
    std::string message;          // 給 debug / LLM response 的簡短訊息

    std::string netNameA;         // 查詢目標 A
    std::string netNameB;         // 查詢目標 B
    std::string conditionNetName; // ConditionalEquivalence 的 condition net
    std::string symmetryInputNameA; // Symmetry 的第一個交換輸入
    std::string symmetryInputNameB; // Symmetry 的第二個交換輸入
    int netIdA = -1;              // scalar net A 的 ID；bus 或不存在時可為 -1
    int netIdB = -1;              // scalar net B 的 ID；bus 或不存在時可為 -1
    int conditionNetId = -1;      // condition net ID；未使用時為 -1
    int symmetryInputNetIdA = -1; // 第一個交換輸入 net ID
    int symmetryInputNetIdB = -1; // 第二個交換輸入 net ID
    int conditionValue = -1;      // condition 固定值；未使用時為 -1
    int constValue = -1;          // query 指定或推導出的常數值；unknown 時為 -1

    bool equivalent = false;      // Equivalence 結果
    bool canBeZero = false;       // TruthStatus / CanBeValue 輔助資訊
    bool canBeOne = false;        // TruthStatus / CanBeValue 輔助資訊
    bool isConstant = false;      // TruthStatus 結果；always 0 或 always 1 時為 true
    bool inputInStructuralSupport = false; // FunctionalDependence：input 是否出現在 fanin support
    bool dependsOnInput = false;  // FunctionalDependence 的 exact SAT/cofactor 結果
    bool symmetric = false;       // Symmetry 的主要答案；UNSAT proof 時為 true
    bool symmetryInputAInStructuralSupport = false;
    bool symmetryInputBInStructuralSupport = false;
    bool counterexampleFound = false; // SAT 找到交換後 target 改變的 assignment
    std::vector<std::string> mismatchedTargetBitNames; // bus target 可有多個不同 bits
    std::map<std::string, int> counterexampleAssignments; // 交換前的 cone leaf values
    std::map<std::string, int> outputValuesBeforeSwap;
    std::map<std::string, int> outputValuesAfterSwap;
    std::string status;           // "ALWAYS_ZERO"、"ALWAYS_ONE"、"NON_CONSTANT" 或錯誤描述
    bool solverRan = false;       // SAT 類 query 是否真的呼叫 solver
    bool solverTimedOut = false;  // true 表示 SAT solver 因 time limit 回 UNKNOWN
    bool solverUnknown = false;   // true 表示 solver 回 UNKNOWN，不能當成 false
    bool unsupported = false;     // true 表示 cone / gate type / argument 不支援
    std::string solverStatus;     // "SAT"、"UNSAT"、"TIMEOUT"、"UNKNOWN"、"UNSUPPORTED"

    std::string expression;        // BooleanExpression / SimplifiedBooleanExpression 結果
    size_t expressionLength = 0;   // expression.size()，方便 LLM 決定是否摘要
    int maxExpressionDepth = -1;   // SimplifiedBooleanExpression 實際使用的 depth limit
    bool expressionDepthLimited = false; // true 表示這次輸出使用 depth-limited expansion
    std::vector<std::string> supportPrimaryInputs; // fanin cone PI / DFF.Q pseudo-PI leaves（三桶聯集，不分類）
    // supportPrimaryInputs 的分類版本：同一次 BFS 分出的三個互斥子集，聯集等於
    // supportPrimaryInputs。BooleanExpression / SimplifiedBooleanExpression /
    // PrimaryInputsOfNet 都會填。
    std::vector<std::string> supportRealPrimaryInputs; // 真正宣告的 top-level primary input
    std::vector<std::string> supportDffPseudoInputs;   // DFF.Q pseudo primary input（跨 sequential boundary）
    std::vector<std::string> supportUndrivenLeaves;    // 沒有 driver 也不是 PI 的懸空 fanin leaf
};

// =========================================================================
// Function Search Query Types
// =========================================================================

enum class FunctionSearchQueryType {
    NandEquivalentInputPairs, // 搜尋 NAND(a, b) 與 targetNetName 功能等價的 internal signal pair
    EquivalentGatePairs       // 搜尋 output function 相同的 active combinational gate pairs
};

enum class FunctionSearchMode {
    FindAny, // 找到第一組已證明的結果即可停止
    FindAll  // 搜尋全部候選；受 maxResults / timeLimitSeconds 限制
};

enum class FunctionSearchScope {
    WholeDesign,
    NetFanin,
    NetFanout,
    GateFanin,
    GateFanout
};

struct FunctionSearchQuery {
    FunctionSearchQueryType type = FunctionSearchQueryType::NandEquivalentInputPairs;
    FunctionSearchMode mode = FunctionSearchMode::FindAny;
    std::string targetNetName;

    FunctionSearchScope scope = FunctionSearchScope::WholeDesign;
    std::string scopeName;
    GateType gateTypeFilter = GateType::UNKNOWN;

    // 第一版只搜尋 active、scalar、非 PI/PO/constant、且有 driver 的 internal signals。
    bool internalSignalsOnly = true;
    bool allowSameSignalPair = false;

    size_t maxResults = 0; // 0 = 不限制完整搜尋；正值只供明確 result limit
    size_t maxStoredMatches = 256; // report.matches 最多保留的 samples；0 = 不保留
    size_t simulationPatternCount = 256;
    double timeLimitSeconds = 30.0;
    bool expandEquivalentPairs = true; // false 時只回傳 SAT-proven equivalenceClasses
    bool writeMatchesToFile = false; // FindAll 可將完整 records 以 streaming 寫檔
    std::string outputFilePath;       // 空字串時使用 backend 相容預設檔名
};

struct FunctionSearchMatch {
    int gateIdA = -1;
    int gateIdB = -1;
    int netIdA = -1;
    int netIdB = -1;
    std::string gateNameA;
    std::string gateNameB;
    std::string netNameA;
    std::string netNameB;
    bool provenEquivalent = false;
    std::string proofMethod;  // 目前為 "SAT_UNSAT_MITER"
    std::string solverStatus; // 等價 proof 成功時為 "UNSAT"
};

struct FunctionSearchEquivalenceClass {
    std::vector<int> gateIds;
    std::vector<int> netIds;
    std::vector<std::string> gateNames;
    std::vector<std::string> netNames;
    bool provenEquivalent = false;
    std::string proofMethod;
};

struct FunctionSearchReport {
    bool ok = false;       // request 合法且答案已確定；partial/timeout 時為 false
    bool found = false;    // 至少找到一組 SAT-proven match
    bool complete = false; // FindAny 已決定 exists，或 FindAll 已完整列舉
    bool allCandidatesExamined = false;
    bool timedOut = false;
    bool truncated = false;
    bool unsupported = false;

    std::string status;
    std::string message;
    FunctionSearchQueryType queryType = FunctionSearchQueryType::NandEquivalentInputPairs;
    std::string targetNetName;
    int targetNetId = -1;
    FunctionSearchScope scope = FunctionSearchScope::WholeDesign;
    std::string scopeName;
    GateType gateTypeFilter = GateType::UNKNOWN;

    size_t candidateSignalCount = 0;
    size_t candidateGateCount = 0;
    size_t simulationEligibleSignalCount = 0;
    size_t simulationBucketCount = 0;
    size_t candidatePairsConsidered = 0;
    size_t candidatePairsRejectedBySimulation = 0;
    size_t satChecks = 0;
    size_t satUnknownCount = 0;
    size_t unsupportedSignalCount = 0;
    size_t equivalenceClassCount = 0;
    size_t equivalentPairCount = 0;
    size_t matchCount = 0; // 完整找到的 match 數；不等於 samples vector 大小
    size_t simulationPatternCount = 0;
    double elapsedSeconds = 0.0;

    bool wroteMatchesToFile = false;
    std::string outputFilePath;

    std::vector<FunctionSearchMatch> matches; // 只保存 query.maxStoredMatches 筆 samples
    std::vector<FunctionSearchEquivalenceClass> equivalenceClasses;
};

// =========================================================================
// Sequential Pattern Query Types
// =========================================================================

enum class SequentialPatternQueryType {
    DffEnableHold
};

enum class DffInputPatternKind {
    MuxHold,
    DataGatingWithoutHoldFeedback
};

enum class SequentialPatternDetectionMethod {
    StructuralCanonical,
    StructuralCanonicalWithSat,
    FunctionalCofactorSat
};

struct SequentialPatternQuery {
    SequentialPatternQueryType type = SequentialPatternQueryType::DffEnableHold;
    std::string dffName;              // 空字串表示分析所有 active DFF
    bool includeAndGatedCandidates = true;
    bool verifyCanonicalMatchesWithSat = false;
    // Opt-in because full-design functional fallback can consume a bounded SAT budget.
    bool enableFunctionalFallback = false;
    // Limits candidates that still require reachability/SAT; simulation-safe Rejects do not consume quota.
    size_t maxFunctionalCandidates = 64;
    size_t maxFunctionalMatchesPerDff = 8;
    bool findAllFunctionalMatches = true;
    bool resolveFunctionalDataNets = true;
    size_t maxFunctionalDataCandidatesPerMatch = 16;
    bool enableFunctionalSimulationFilter = true;
    size_t functionalSimulationPatternCount = 256;
    double functionalPerDffTimeLimitSeconds = 0.25;
    double functionalTimeLimitSeconds = 5.0;
};

struct DffInputPattern {
    DffInputPatternKind kind = DffInputPatternKind::MuxHold;
    SequentialPatternDetectionMethod detectionMethod =
        SequentialPatternDetectionMethod::StructuralCanonical;

    std::string enableNetName;
    std::string dataNetName;
    std::string dataBranchNetName; // 實際接到 branch gate 的 net；可能是反相後訊號
    std::string feedbackNetName;
    int enableNetId = -1;
    int dataNetId = -1;
    int dataBranchNetId = -1;
    int feedbackNetId = -1;
    int activeLevel = -1;            // 1: active-high, 0: active-low, -1: unknown
    int holdLevel = -1;
    bool dataInverted = false;
    bool dataFunctionResolved = false;
    bool dataSearchAttempted = false;
    bool dataSearchComplete = true;
    bool dataSearchTimedOut = false;
    size_t dataCandidateCount = 0;
    size_t dataCandidatesExamined = 0;

    bool structuralMatch = false;
    bool holdFunctionallyProven = false;
    bool loadFunctionallyProven = false;
    bool confirmed = false;
    bool semanticsPending = false;

    bool solverRan = false;
    bool solverTimedOut = false;
    bool solverUnknown = false;
    std::string solverStatus;

    std::vector<int> evidenceGateIds;
    std::vector<std::string> evidenceGateNames;
    std::vector<std::string> candidateInputNetNames;
    std::string message;
};

struct DffInputPatternReport {
    bool ok = false;
    bool matched = false;            // 至少一個 confirmed enable/hold pattern
    std::string status;
    std::string message;

    int dffGateId = -1;
    std::string dffName;
    int dNetId = -1;
    int qNetId = -1;
    std::string dNetName;
    std::string qNetName;
    bool qFeedbackObserved = false; // canonical 或 functional pattern 中觀察到自己的 Q feedback
    bool functionalFallbackAttempted = false;
    bool functionalFallbackComplete = true;
    bool functionalFallbackTimedOut = false;
    bool functionalCandidateLimitReached = false;
    size_t functionalCandidateCount = 0;           // all structural control candidates
    size_t functionalSearchableCandidateCount = 0; // structural minus simulation-safe Reject
    size_t functionalCandidatesExamined = 0;       // searchable candidates entering the loop
    size_t functionalUnexaminedCandidateCount = 0; // guarded searchable minus examined
    size_t functionalInconclusiveCandidateCount = 0; // entered candidates with timeout/unknown/unsupported
    size_t functionalSimulationCandidateCount = 0;
    size_t functionalSimulationRejectedCandidateCount = 0;
    size_t functionalSatCheckCount = 0;

    std::vector<DffInputPattern> patterns;
};

struct SequentialPatternReportSet {
    bool ok = false;
    bool exists = false;
    bool complete = true;
    bool timedOut = false;
    std::string status;
    std::string message;

    SequentialPatternQueryType type = SequentialPatternQueryType::DffEnableHold;
    size_t totalDffCount = 0;
    size_t analyzedDffCount = 0;
    size_t matchedDffCount = 0;      // 依 DFF instance 去重
    size_t candidateDffCount = 0;    // 不含 data-gating-only non-match diagnostics
    size_t functionalCandidateCount = 0;           // sum of per-DFF structural candidates
    size_t functionalSearchableCandidateCount = 0; // sum of per-DFF searchable candidates
    size_t functionalCandidatesExamined = 0;       // sum of per-DFF examined candidates
    size_t functionalUnexaminedCandidateCount = 0; // sum of per-DFF unexamined candidates
    size_t functionalInconclusiveCandidateCount = 0; // sum of per-DFF inconclusive candidates
    size_t functionalSimulationPatternCount = 0;
    size_t functionalSimulationCandidateCount = 0;
    size_t functionalSimulationRejectedCandidateCount = 0;
    size_t functionalSatCheckCount = 0;
    size_t functionalMatchCount = 0;
    double functionalSimulationSeconds = 0.0;
    double elapsedSeconds = 0.0;
    std::vector<DffInputPatternReport> reports;
};

// =========================================================================
// Directed Combinational Graph Query Types
// =========================================================================

enum class GraphQueryType {
    IsCutNetBetweenPiPo,       // 指定 internal net 是否切斷至少一組 PI-to-PO connectivity
    ArticulationPointsBetween // 列出 source/target 間出現在所有 directed paths 的 internal nets
};

struct GraphQuery {
    GraphQueryType type = GraphQueryType::IsCutNetBetweenPiPo;
    std::string netName;       // IsCutNetBetweenPiPo candidate
    std::string sourceNetName; // ArticulationPointsBetween source
    std::string targetNetName; // ArticulationPointsBetween target
};

struct GraphReport {
    bool ok = false;
    bool exists = false;       // cut yes/no，或 articulation list 是否非空
    bool unsupported = false;
    bool combinationalCycleDetected = false;
    std::string message;
    std::string status;

    std::string candidateNetName;
    int candidateNetId = -1;
    std::string sourceNetName;
    int sourceNetId = -1;
    std::string targetNetName;
    int targetNetId = -1;

    bool pathExists = false;
    bool isCut = false;
    size_t checkedPrimaryInputCount = 0;
    size_t checkedPrimaryOutputCount = 0;
    std::string witnessPrimaryInput;
    std::string witnessPrimaryOutput;

    std::vector<int> articulationNetIds;
    std::vector<std::string> articulationNetNames;
};

// =========================================================================
// Cone Query Types
// =========================================================================

enum class ConeQueryType {
    NetTransitiveFanin,   // 從指定 net 往 fanin 方向追溯
    NetTransitiveFanout,  // 從指定 net 往 fanout 方向展開
    GateTransitiveFanin,  // 從指定 gate output net 往 fanin 方向追溯
    GateTransitiveFanout, // 從指定 gate output net 往 fanout 方向展開
    LargestOutputCone,    // 掃描所有 primary outputs，找 fanin cone gateCount 最大者
    SharedFaninGates      // 取得兩個 net fanin cones 的 shared gate intersection
};

struct ConeQuery {
    ConeQueryType type = ConeQueryType::NetTransitiveFanin;
    std::string netName;       // NetTransitiveFanin / NetTransitiveFanout 使用
    std::string secondNetName; // SharedFaninGates 使用的第二個 net
    std::string gateName;      // GateTransitiveFanin / GateTransitiveFanout 使用
    bool includeIds = true;    // 是否填 netIds/gateIds/rootNetIds
    bool includeNames = true;  // 是否填 netNames/gateNames/rootNetNames
    bool includeLocalPaths = false; // 是否附帶 cone 內 longest/shortest net path
};

struct ConeReport {
    bool ok = false;                       // query 是否成功
    bool exists = false;                   // 指定 net/gate 是否存在並成功建立 cone
    std::string message;                   // 給 debug / LLM response 的簡短訊息

    ConeQueryType type = ConeQueryType::NetTransitiveFanin;
    std::string sourceName;                // 使用者指定的來源名稱
    int sourceId = -1;                     // 來源 net ID 或 gate ID
    std::string secondSourceName;           // SharedFaninGates 的第二個來源 net
    int secondSourceId = -1;                // SharedFaninGates 的第二個來源 net ID
    ConeResult cone;                       // 原始 cone 結果，供底層演算法繼續使用

    size_t netCount = 0;                   // cone 內有效 net 數量
    size_t gateCount = 0;                  // cone 內有效 combinational gate 數量
    size_t checkedOutputCount = 0;         // LargestOutputCone 掃描的 PO bit 數量
    std::vector<int> rootNetIds;           // cone roots，支援 bus 多 root
    std::vector<std::string> rootNetNames; // root net names
    std::vector<int> netIds;               // cone 內 net IDs
    std::vector<int> gateIds;              // cone 內 gate IDs
    std::vector<std::string> netNames;     // cone 內 net names
    std::vector<std::string> gateNames;    // cone 內 gate names
    std::map<GateType, int> gateTypeCounts; // cone/shared result 內各 gate type 數量

    int longestDepth = -1;                 // cone 內 longest net path depth
    int shortestDepth = -1;                // cone 內 shortest net path depth
    std::vector<int> longestPathNetIds;    // cone 內 longest path 的 net IDs
    std::vector<int> shortestPathNetIds;   // cone 內 shortest path 的 net IDs
    std::vector<std::string> longestPathNetNames;  // cone 內 longest path 的 net names
    std::vector<std::string> shortestPathNetNames; // cone 內 shortest path 的 net names
};
