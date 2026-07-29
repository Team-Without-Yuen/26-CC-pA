#pragma once

#include <string>
#include <vector>

// =========================================================================
// Path / Depth Analysis Types
//
// 這個 header 只放 path analysis 與 depth analysis 共用的純資料型別。
// 不放 Netlist graph 儲存、不放 traversal 實作、不放 mutation API。
// Netlist.h 會用 using alias 保留 Netlist::Xxx 的既有寫法。
// =========================================================================

// 表示限制條件指定的節點種類；可用相同 API 查詢 net 或 gate。
enum class PathNodeType {
    Net,
    Gate
};

// 表示路徑條件中的一個具名節點；多個條件以 std::vector<PathNode> 傳入。
struct PathNode {
    PathNodeType type;
    std::string name;

    // 建立一個路徑限制節點，type 決定 name 要在 nets 或 gates 中查找。
    PathNode(PathNodeType _type, const std::string& _name)
        : type(_type), name(_name) {
    }
};

// 保存一條組合邏輯路徑；netIds 與 gateIds 都依照起點到終點排列。
struct CombinationalPath {
    std::vector<int> netIds;   // 路徑經過的 nets，包含 startNet 與 endNet
    std::vector<int> gateIds;  // 路徑中真正穿越的 combinational gates

    // 回傳此路徑的邏輯深度，也就是經過的 combinational gate 數量。
    int depth() const {
        return static_cast<int>(gateIds.size());
    }

    // 判斷是否保存了一條有效路徑；沒有找到路徑時 netIds 為空。
    bool exists() const {
        return !netIds.empty();
    }
};

// 標記 DepthReport 的 endpoint 類型，方便回覆 prompt 與後續 optimization selector 使用。
enum class DepthEndpointType {
    Unknown,
    SpecificNet,
    PrimaryOutput,
    DffD
};

// 保存單一 endpoint 的 depth 分析結果。
struct DepthReport {
    DepthEndpointType endpointType = DepthEndpointType::Unknown; // endpoint 的來源類型
    std::string endpointName;        // 可讀名稱，例如 y、n10、ff1.D
    int endpointNetId = -1;          // 被分析的 endpoint net ID
    int depth = -1;                  // 到該 endpoint 的最大 combinational depth
    CombinationalPath criticalPath;  // 到該 endpoint 的一條 critical path
};

// 表示 DepthQuery 要執行哪一種 depth/timing 查詢。
enum class DepthQueryType {
    SpecificNet,              // 分析單一 net 的最大 depth 與 critical path
    PrimaryOutputs,           // 分析所有 primary output endpoints
    DffD,                     // 分析所有 DFF D-pin endpoints
    GlobalCriticalPath,       // 找出 PO 與 DFF.D 中最深的 timing endpoint
    EndpointsExceedingDepth,  // 找出所有 depth 大於 threshold 的 timing endpoints
    PrimaryOutputsExceedingDepth, // 只找 depth 大於 threshold 的 primary outputs
    GateOnCriticalPath,       // 判斷 gate 是否位在任一 global maximum-depth path 上
    DeepestOutputCone         // 找出 fanin logic cone depth 最深的 primary output
};

// 描述一個 Depth Query；這一層只處理 depth/timing，不處理 function 或 through/avoid path 條件。
struct DepthQuery {
    DepthQueryType type = DepthQueryType::SpecificNet;
    std::string netName;             // SpecificNet 使用的 endpoint net name
    std::string gateName;            // GateOnCriticalPath 使用的 gate instance name
    int threshold = -1;              // EndpointsExceedingDepth 使用的 depth 門檻
    bool includeCriticalPath = true; // false 時回傳 report 會清空 criticalPath，降低資料量
};

// 保存 DepthQuery 的統一回傳結果。
struct DepthReportSet {
    bool ok = false;                 // query 是否成功
    bool exists = false;             // yes/no 型 depth query 的主要答案
    std::string message;             // 給 debug / LLM response 的簡短訊息
    DepthQueryType type = DepthQueryType::SpecificNet;

    std::vector<DepthReport> reports; // 查詢得到的一個或多個 endpoint report
    DepthReport worst;                // reports 中 depth 最大者；GlobalCriticalPath 的主要結果

    int threshold = -1;               // query 使用的 depth 門檻；未使用時為 -1
    size_t count = 0;                 // reports 數量

    std::string gateName;             // GateOnCriticalPath 的查詢目標
    int gateId = -1;                  // GateOnCriticalPath 的 gate ID
    bool gateOnCriticalPath = false;  // GateOnCriticalPath 的主要 yes/no 答案
};

// 表示 path query 的抽象起點或終點類型。
enum class PathEndpointType {
    SpecificNet,     // 指定某條 net，例如 "n16"
    PrimaryInput,    // primary input，可表示單一 PI 或整個 PI bus
    PrimaryOutput,   // primary output，可表示單一 PO 或整個 PO bus
    DffQ,            // DFF 的 Q 輸出 pin，通常作為 register-to-* 的 startpoint
    DffD,            // DFF 的 D 輸入 pin，通常作為 *-to-register 的 endpoint
    DffClock,        // DFF clock pin，例如 CK；偏 control path
    DffReset,        // DFF reset/set pin，例如 RN/SN；偏 control path
    GateOutput,      // 指定 gate 的 output net
    GateInput        // 指定 gate 的某個 input net
};

// 描述一個抽象 path 起點或終點；實際查詢前要先 resolve 成 net ID。
struct PathEndpoint {
    PathEndpointType type;
    std::string name;       // net name、port name、gate instance name 或 DFF instance name
    std::string pinName;    // named pin 使用，例如 "D"、"Q"、"CK"、"RN"
    int pinIndex = -1;      // positional gate input 使用，例如 input 0 / input 1

    // 建立一個 path endpoint；不同 type 會使用不同欄位。
    PathEndpoint(PathEndpointType _type,
                 const std::string& _name,
                 const std::string& _pinName = "",
                 int _pinIndex = -1)
        : type(_type), name(_name), pinName(_pinName), pinIndex(_pinIndex) {
    }
};

// 表示同一組 startpoints/endpoints 要執行哪一種 path query。
enum class PathQueryMode {
    Exists,             // 是否至少存在一條符合條件的路徑
    FindAny,            // 回傳任意一條符合條件的路徑
    EnumerateAll,       // 回傳所有符合條件的路徑
    MinDepth,           // 回傳最短邏輯深度路徑
    MaxDepth,           // 回傳最長邏輯深度路徑
    EveryPathThrough,   // 判斷所有路徑是否都經過 requiredNodes
    EveryPathAvoids,    // 判斷所有路徑是否都避開 avoidedNodes
    FindMandatoryNodes, // 找出 source/target 間所有 directed paths 的共同 internal nets
    IsSeparator,        // 指定 net 是否為 endpoints 間的 directed separator
    DirectPiPoConnections // 找出 PI/PO 共用同一 net 的 depth-0 direct wires
};

// 描述一個完整的 startpoint-to-endpoint path query。
struct PathQuery {
    std::vector<PathEndpoint> startpoints;  // 一個或多個抽象起點
    std::vector<PathEndpoint> endpoints;    // 一個或多個抽象終點
    std::vector<PathNode> requiredNodes;    // 每條符合條件的路徑必須經過的 net/gate
    std::vector<PathNode> avoidedNodes;     // 每條符合條件的路徑必須避開的 net/gate
    std::string separatorCandidateNetName;  // IsSeparator 使用；endpoints 皆空時採 PI-to-PO cut 語意
    PathQueryMode mode = PathQueryMode::Exists;
    bool combinationalOnly = true;          // true 時遇到 DFF 視為 sequential boundary
    bool writePathsToFile = true;           // EnumerateAll 使用；預設自動將完整路徑列表寫入檔案
    std::string outputFilePath;             // EnumerateAll 寫檔路徑；空字串時使用預設檔名
    size_t maxPrintedPaths = 20;            // CLI / report 顯示用；不限制 result.paths 的完整內容
    size_t maxEnumeratedPaths = 100000;     // Legacy compatibility；目前不限制 EnumerateAll
    double enumerationTimeLimitSeconds = 55.0; // EnumerateAll wall-clock 上限；<=0 表示不限制
    bool countOnly = false;                 // EnumerateAll 只計數，不保存每條 path
};

// 保存統一 path query 的結果；不同 mode 會使用不同欄位。
struct PathQueryResult {
    bool ok = false;                        // request 是否有效並完成高階 dispatch
    bool unsupported = false;               // request 合法但目前模式/設定不支援
    std::string message;                    // success/no-path/validation error 語意
    std::string status;                     // mode-specific result status，例如 NO_PATH / SEPARATOR
    bool exists = false;                    // exists/every-path 類查詢的主要結果
    bool pathExists = false;                 // separator/mandatory mode：endpoints 是否原本相連
    bool isSeparator = false;                // IsSeparator 的主要結果
    bool combinationalCycleDetected = false;// dominator-based mode 遇到 cycle
    std::string separatorCandidateNetName;
    int separatorCandidateNetId = -1;
    std::vector<int> mandatoryNetIds;        // FindMandatoryNodes / IsSeparator 的共同節點
    std::vector<std::string> mandatoryNetNames;
    std::string witnessStartpoint;           // PI-to-PO cut 成立時的 witness PI
    std::string witnessEndpoint;             // PI-to-PO cut 成立時的 witness PO
    size_t checkedStartpointCount = 0;
    size_t checkedEndpointCount = 0;
    int depth = -1;                         // min/max depth 類查詢的邏輯深度
    CombinationalPath path;                 // find any/min/max depth 的代表路徑
    std::vector<CombinationalPath> paths;   // enumerate all 的所有路徑
    size_t pathCount = 0;                   // enumerate all 的完整路徑數量
    bool wrotePathsToFile = false;          // 是否已將完整 enumerate 結果寫到檔案
    std::string outputFilePath;             // 實際輸出檔案路徑
    bool completeEnumeration = true;        // true 表示沒有截斷 enumerate 結果
    bool enumerationTimedOut = false;       // true 表示因 time limit 停止
    bool enumerationPathLimitReached = false; // Legacy result field；目前應維持 false
    bool countOnly = false;                 // true 表示 paths 可能為空，只保留 pathCount
    std::string enumerationStopReason;      // 截斷原因，完整列舉時為空
    std::vector<std::string> unresolvedStartpoints; // 無法解析的 start endpoint descriptions
    std::vector<std::string> unresolvedEndpoints;   // 無法解析的 end endpoint descriptions
    std::vector<std::string> unresolvedRequiredNodes; // 無法解析的 required nodes
    std::vector<std::string> unresolvedAvoidedNodes;  // 無法解析的 avoided nodes
};

// 表示 register-to-register path wrapper 要執行哪一種查詢。
// 這一層會自動把 DFF.Q 展開成 startpoints、DFF.D 展開成 endpoints。
enum class RegisterPathQueryMode {
    Exists,        // 是否存在任一 DFF.Q 到任一 DFF.D 的組合路徑
    FindAny,       // 回傳任意一條 DFF.Q 到 DFF.D 的組合路徑
    EnumerateAll,  // 列出所有 DFF.Q 到 DFF.D 的組合路徑；預設自動寫檔
    MinDepth,      // 找最短 register-to-register 組合路徑
    MaxDepth       // 找最長 register-to-register 組合路徑
};

// 描述 register-to-register path query。
// startDffNames / endDffNames 為空時，代表使用設計中所有 DFF。
struct RegisterPathQuery {
    RegisterPathQueryMode mode = RegisterPathQueryMode::MaxDepth;
    std::vector<std::string> startDffNames; // 空 = 所有 DFF.Q
    std::vector<std::string> endDffNames;   // 空 = 所有 DFF.D
    std::vector<PathNode> requiredNodes;    // 每條符合條件的路徑必須經過的 net/gate
    std::vector<PathNode> avoidedNodes;     // 每條符合條件的路徑必須避開的 net/gate
    bool combinationalOnly = true;          // true 時 DFF 是 sequential boundary
    std::string outputFilePath;             // EnumerateAll 寫檔路徑；空字串時使用預設檔名
    size_t maxPrintedPaths = 20;            // CLI / report 顯示用；不限制完整結果
    size_t maxEnumeratedPaths = 100000;     // Legacy compatibility；目前不限制 EnumerateAll
    double enumerationTimeLimitSeconds = 55.0; // EnumerateAll wall-clock 上限；<=0 表示不限制
    bool countOnly = false;                 // EnumerateAll 只計數，不保存每條 path
};

// 保存 register-to-register path query 的結果。
struct RegisterPathReport {
    bool ok = false;                        // query 是否成功解析並執行
    std::string message;                    // 給 debug / LLM response 的簡短訊息
    RegisterPathQueryMode mode = RegisterPathQueryMode::MaxDepth;

    bool exists = false;                    // 是否找到符合條件的 register-to-register path
    int depth = -1;                         // 代表路徑 depth；沒有路徑時為 -1
    PathQueryResult pathResult;             // 底層 PathQuery 的完整結果

    std::vector<std::string> startDffNames; // 實際展開的起點 DFF
    std::vector<std::string> endDffNames;   // 實際展開的終點 DFF
    std::string startDffName;               // 代表路徑起點 DFF；例如 max depth path 的來源
    std::string endDffName;                 // 代表路徑終點 DFF；例如 max depth path 的目的
};
