#pragma once

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
    bool includeIds = true;                 // 回傳 report 時是否填 gateIds/netIds
    bool includeNames = true;               // 回傳 report 時是否填 gateNames/netNames/portNames
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
    NetDriver,          // 查某個 net / bus 的直接 driver gate
    NetLoads,           // 查某個 net / bus 直接 load 到哪些 gates
    GateInputs,         // 查某個 gate 的直接 input nets
    GateOutput,         // 查某個 gate 的 output net
    GateFanin,          // 查直接驅動某 gate inputs 的上一層 gates
    GateFanout,         // 查某 gate output 直接 fanout 到哪些 gates
    DirectlyConnected   // 判斷指定 gate 與指定 net 是否直接相連
};

struct DirectConnectivityQuery {
    DirectConnectivityQueryType type = DirectConnectivityQueryType::NetDriver;
    std::string gateName;     // GateInputs/GateOutput/GateFanin/GateFanout/DirectlyConnected 使用
    std::string netName;      // NetDriver/NetLoads/DirectlyConnected 使用
    bool includeIds = true;   // 是否填 gateIds/netIds
    bool includeNames = true; // 是否填 gateNames/netNames
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
};

// =========================================================================
// Function Analysis Query Types
// =========================================================================

enum class FunctionQueryType {
    Equivalence,       // 檢查 netNameA 與 netNameB 是否功能等價
    CanBeValue,        // 檢查 netNameA 是否存在某組輸入可等於 constValue
    ConstantFunction,  // 檢查 netNameA 是否恆等於 constValue
    AlwaysZero,        // 檢查 netNameA 是否永遠為 0
    AlwaysOne,         // 檢查 netNameA 是否永遠為 1
    TruthStatus        // 回報 netNameA 是 always 0、always 1，或 non-constant
};

struct FunctionQuery {
    FunctionQueryType type = FunctionQueryType::TruthStatus;
    std::string netNameA;   // 主要分析目標；Equivalence 時代表第一個 net / bus
    std::string netNameB;   // Equivalence 使用的第二個 net / bus
    int constValue = -1;    // CanBeValue / ConstantFunction 使用；只能是 0 或 1
};

struct FunctionReport {
    bool ok = false;              // query 是否成功；名稱不存在或參數不合法時為 false
    bool exists = false;          // 對 yes/no 問題的主要布林答案
    std::string message;          // 給 debug / LLM response 的簡短訊息

    std::string netNameA;         // 查詢目標 A
    std::string netNameB;         // 查詢目標 B
    int netIdA = -1;              // scalar net A 的 ID；bus 或不存在時可為 -1
    int netIdB = -1;              // scalar net B 的 ID；bus 或不存在時可為 -1
    int constValue = -1;          // query 指定或推導出的常數值；unknown 時為 -1

    bool equivalent = false;      // Equivalence 結果
    bool canBeZero = false;       // TruthStatus / CanBeValue 輔助資訊
    bool canBeOne = false;        // TruthStatus / CanBeValue 輔助資訊
    bool isConstant = false;      // TruthStatus 結果；always 0 或 always 1 時為 true
    std::string status;           // "ALWAYS_ZERO"、"ALWAYS_ONE"、"NON_CONSTANT" 或錯誤描述
};

// =========================================================================
// Cone Query Types
// =========================================================================

enum class ConeQueryType {
    NetTransitiveFanin,   // 從指定 net 往 fanin 方向追溯
    NetTransitiveFanout,  // 從指定 net 往 fanout 方向展開
    GateTransitiveFanin,  // 從指定 gate output net 往 fanin 方向追溯
    GateTransitiveFanout  // 從指定 gate output net 往 fanout 方向展開
};

struct ConeQuery {
    ConeQueryType type = ConeQueryType::NetTransitiveFanin;
    std::string netName;       // NetTransitiveFanin / NetTransitiveFanout 使用
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
    ConeResult cone;                       // 原始 cone 結果，供底層演算法繼續使用

    size_t netCount = 0;                   // cone 內有效 net 數量
    size_t gateCount = 0;                  // cone 內有效 combinational gate 數量
    std::vector<int> rootNetIds;           // cone roots，支援 bus 多 root
    std::vector<std::string> rootNetNames; // root net names
    std::vector<int> netIds;               // cone 內 net IDs
    std::vector<int> gateIds;              // cone 內 gate IDs
    std::vector<std::string> netNames;     // cone 內 net names
    std::vector<std::string> gateNames;    // cone 內 gate names

    int longestDepth = -1;                 // cone 內 longest net path depth
    int shortestDepth = -1;                // cone 內 shortest net path depth
    std::vector<int> longestPathNetIds;    // cone 內 longest path 的 net IDs
    std::vector<int> shortestPathNetIds;   // cone 內 shortest path 的 net IDs
    std::vector<std::string> longestPathNetNames;  // cone 內 longest path 的 net names
    std::vector<std::string> shortestPathNetNames; // cone 內 shortest path 的 net names
};
