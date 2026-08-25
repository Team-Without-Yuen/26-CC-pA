# Basic Query API 整理

> 公開 CLI 統一由 `structure_query` 提供；`BasicQuery` 保留為內部 C++ 的基礎物件與統計查詢 API。舊 `basic_query` 命令僅供相容，不列入新的公開 tool schema。

這份文件整理 `Netlist` 最基礎的查詢 API，也就是後續 Cone / Path / Depth / Optimization 分析會共用的底層資訊查詢層。

使用範例與 prompt 對應請看：

```text
API_SPEC/BASIC_QUERY_USAGE.md
```

---

## 1. 核心定位

Basic Query 負責回答「網表裡有哪些物件，以及這些物件的基本屬性是什麼」。

典型問題：

```text
這個 design 有幾個 gate / net / port？
某個 gate / net 是否存在？
某個 gate 是什麼 type？
某個 net 是不是 PI / PO / constant？
有哪些 DFF？
有哪些 combinational gates？
某個 port 是 scalar 還是 bus？
哪些 net 可能 undriven / no-load / floating？
```

Basic Query 不負責做：

```text
transitive fanin / fanout cone traversal
startpoint-to-endpoint path search
critical path / depth analysis
optimization rewrite
```

原因是這些屬於更高階分析。Basic Query 應該保持單純，主要提供：

```text
ID 查詢
物件查詢
型別判斷
名稱列舉
簡單結構問題檢查
```

---

## 2. 物件模型

目前 `Netlist` 的主要物件可以分成四類。

| 類型 | 對應資料 | 說明 |
|---|---|---|
| Design | `Netlist` 本身 | 保存 gates / nets / ports / name-to-id map |
| Gate | `Gate` | 一個 primitive gate 或 DFF instance |
| Net | `Net` | 一條 wire / bus bit / constant net |
| Port | `Port` | module input/output declaration |

Gate 和 Net 的關係：

```text
Gate.outputNetId      -> gate 驅動的 net
Gate.inputNetIds      -> gate 讀取的 input nets
Net.driverGateId      -> 驅動這條 net 的 gate，沒有 driver 則為 -1
Net.loadGateIds       -> 使用這條 net 當 input 的 gates
```

DFF 特別規則：

```text
DFF 仍然存在 gates 裡，但在 combinational path/depth analysis 裡視為 sequential boundary。
DFF.outputNetId 代表 Q pin 對外輸出的 net。
DFF.inputNetIds 內可能包含 D / CK / RN / SN 等 pin 對應 net。
DFF 的 inputNetIds 和 inputPinNames 必須保持同 index 對應。
若某個 DFF input 被 disconnect，inputNetIds[i] 應保留 -1，不能 erase。
```

---

## 3. 現有低階 API 分類

### 3.1 Design Count / Summary

```cpp
size_t getGateCount() const;
size_t getNetCount() const;
size_t getLogicalWireCount() const;
const std::vector<Port>& getPrimaryInputs() const;
const std::vector<Port>& getPrimaryOutputs() const;
```

用途：

```text
取得 design 基本規模。
回答 gate / net / wire / PI / PO 數量。
```

注意：

```text
getGateCount() / getNetCount() 是底層 vector slot 數，用來維持 ID 穩定與 bounds check。
edit 後這兩個 helper 可能包含 UNKNOWN gate / removed net tombstone，不應直接當作題目的 current-design 總數。
BasicQuery::Summary 會自動過濾 tombstone；getLogicalWireCount() 也只統計 active net。
net 數仍可能包含展開後的 bus bit 與 constant net。
```

---

### 3.2 ID / Object Lookup

```cpp
const Gate& getGate(int id) const;
const Net& getNet(int id) const;
int getGateId(const std::string& gateInstName) const;
int getNetId(const std::string& netName) const;
const Gate* findGate(const std::string& gateInstName) const;
const Net* findNet(const std::string& netName) const;
```

用途：

```text
把自然語言中的 gate name / net name 轉成內部 ID。
從 ID 取回實際 Gate / Net 物件。
檢查某個 gate / net 是否存在。
```

建議：

```text
對外部 prompt 或測資名稱，優先使用 getGateId() / getNetId() / findGate() / findNet()。
getGate(id) / getNet(id) 比較適合內部已確認合法 ID 的情況。
```

---

### 3.3 Validity / Type Predicate

```cpp
bool isValidGateId(int gateId) const;
bool isValidNetId(int netId) const;
bool isDffGate(int gateId) const;
bool isCombinationalGate(int gateId) const;
bool isPrimaryInputNet(int netId) const;
bool isPrimaryOutputNet(int netId) const;
bool isConstantNet(int netId) const;
```

用途：

```text
判斷 ID 是否合法。
判斷 gate 是否 DFF。
判斷 gate 是否組合邏輯 gate。
判斷 net 是否 PI / PO / constant。
```

Combinational gate 的定義：

```text
and / or / nand / nor / not / buf / xor / xnor
```

DFF 不算 combinational gate，因為 DFF 是 sequential element。

---

### 3.4 Name Listing

```cpp
std::vector<std::string> getAllGateNames() const;
std::vector<std::string> getAllNetNames() const;
std::vector<std::string> getPrimaryInputNames() const;
std::vector<std::string> getPrimaryOutputNames() const;
std::vector<std::string> getDffNames() const;
std::vector<std::string> getCombinationalGateNames() const;
```

用途：

```text
列出 design 內所有 active gate / net / PI / PO。
列出所有 DFF。
列出所有 combinational gates。
```

這一類 API 適合回答 list 類 prompt，也適合作為高階分析的 startpoint / endpoint 候選來源。

---

### 3.5 Gate Type Query

```cpp
std::string gateTypeToString(GateType type) const;
GateType stringToGateType(std::string str) const;
std::map<GateType, int> countGatesByType() const;
std::vector<int> getGatesByType(GateType type) const;
int getGateCountByType(GateType type) const;
```

用途：

```text
統計各 gate type 數量。
列出特定 type 的 gates。
回答 design 中有多少 AND / NAND / DFF。
```

---

### 3.6 Port / Bus Query

```cpp
int getPortWidth(const std::string& portName) const;
bool isBusPort(const std::string& portName) const;
std::vector<std::string> getPortBitNames(const std::string& portName) const;
std::vector<std::string> expandNetToBits(const std::string& name) const;
```

用途：

```text
查 port 寬度。
判斷 port 是否 bus。
把 bus port 展開成 bit net names。
```

差異：

```text
getPortBitNames(portName) 偏 port declaration。
expandNetToBits(name) 偏一般名稱展開，未來可用於 prompt 中傳入 bus name 時的解析。
```

---

### 3.7 Constant Input Query

```cpp
std::vector<int> findGatesWithConstInput(
    GateType type = GateType::UNKNOWN,
    int constValue = -1,
    int inputCount = -1
) const;

std::vector<std::string> getGateNamesWithConstInput(
    GateType type = GateType::UNKNOWN,
    int constValue = -1,
    int inputCount = -1
) const;

int countGatesWithConstInput(
    GateType type = GateType::UNKNOWN,
    int constValue = -1,
    int inputCount = -1
) const;
```

用途：

```text
找出 input 接到 1'b0 / 1'b1 的 gates。
限制 gate type，例如只找 NAND gates。
限制 gate input 數量，例如只找 two-input gates。
統計 constant input 使用情況。
```

高階 `GatesWithConstantInput` 只接受 `constValue=-1/0/1`，且 `inputCount` 必須為 `-1` 或非負數。非法 filter 回傳 `ok=false`，不會偽裝成成功的空結果。低階 helper 會略過 invalid 或 removed input net ID。

注意：

```text
constant net 不一定適合直接用 getNetId("1'b0") / getNetId("1'b1") 查。
比較穩定的做法是透過 gate input net ID，再用 isConstantNet(netId) 判斷。
```

---

### 3.8 Structural Issue Query

```cpp
std::vector<std::string> getUndrivenNetNames() const;
std::vector<std::string> getNoLoadNetNames() const;
std::vector<std::string> getFloatingNetNames() const;
std::vector<std::string> getUnconnectedGateNames() const;
```

用途：

```text
找可能有結構問題的 nets / gates。
可作為 parser 檢查、transformation 後驗證、cleanup 前分析。
```

目前定義：

| API | 定義 |
|---|---|
| `getUndrivenNetNames()` | 非 PI、非 constant，且沒有雙向一致 active driver 的 net |
| `getNoLoadNetNames()` | 非 PO、非 constant，且沒有雙向一致 active load 的 net |
| `getFloatingNetNames()` | undriven 與 no-load 的 union，不重複 |
| `getUnconnectedGateNames()` | gate 的 input 或 output 存在非法 / disconnected net ID；與 `StructuralIssues.unconnectedPins` 共用同一 collector |

注意：

```text
DFF input 若被 disconnect，可能以 -1 留在 inputNetIds 裡。
因此 getUnconnectedGateNames() 需要把 -1 視為值得回報的結構狀態。
StructuralIssues 另回傳 pin-level input/output counts、compact `unconnectedPins`、
floatingPrimaryInputNets 與 unconnectedPrimaryOutputNets，
兩者都是依 declaration/bit net 順序產生的具名 net，不是 base port name。
```

---

## 4. 和 Direct Connectivity Query 的界線

Basic Query 只回答物件本身的基本屬性。

Direct Connectivity Query 應負責回答「直接相連」問題，例如：

```text
某個 gate 的 input nets 是哪些？
某個 gate 的 output net 是哪個？
某個 net 的 driver gate 是誰？
某個 net 的 load gates 是哪些？
某個 gate 是否直接連到某個 net？
```

因此 gate pin / net name helper 比較適合放在 Direct Connectivity Query，而不是 Basic Query。

Basic Query 和 Direct Connectivity Query 的差別：

| 問題 | 應放位置 |
|---|---|
| `g1` 是否存在？ | Basic Query |
| `g1` 是不是 DFF？ | Basic Query |
| design 有幾個 NAND？ | Basic Query |
| `g1` 的 input nets 是哪些？ | Direct Connectivity Query |
| `n1` 的 driver 是誰？ | Direct Connectivity Query |
| `n1` 直接 fanout 到哪些 gates？ | Direct Connectivity Query |

---

## 5. 高階 BasicQuery API

目前 Basic Query 已經像 `PathQuery` 一樣包成單一高階 API。

使用形式：

```cpp
enum class BasicQueryType {
    Summary,
    ListGates,
    ListNets,
    NetClassification,
    ListPrimaryInputs,
    ListPrimaryOutputs,
    ListDffs,
    ListCombinationalGates,
    GateInfo,
    NetInfo,
    PortInfo,
    CountByGateType,
    GatesByType,
    GatesWithConstantInput,
    StructuralIssues
};

BasicReport runBasicQuery(const BasicQuery& query) const;
```

目前支援的 query type：

| Query type | 用途 | 主要填入欄位 |
|---|---|---|
| `Summary` | design 基本規模、PI/PO port/bit counts 與 gate type 統計 | counts、`gateTypeCounts` |
| `ListGates` | 列出所有 gate | `gateIds`、`gateNames` |
| `ListNets` | 列出所有 net | `netIds`、`netNames` |
| `NetClassification` | 一次分類所有 active nets | `netCount`、`netClassification` 的分類 counts/names/IDs |
| `ListPrimaryInputs` | 列出所有 PI port 與 width/range | `portNames`、`ports`、PI port/bit counts |
| `ListPrimaryOutputs` | 列出所有 PO port 與 width/range | `portNames`、`ports`、PO port/bit counts |
| `ListDffs` | 列出所有 DFF | `gateIds`、`gateNames` |
| `ListCombinationalGates` | 列出所有組合邏輯 gates | `gateIds`、`gateNames` |
| `GateInfo` | 查單一 gate 與 ordered pin/net snapshot | `objectId`、`typeName`、predicate flags、`gateConnections[0]`；`formattedInfo` 保留相容 |
| `NetInfo` | 查單一 net | `objectId`、`typeName`、PI/PO/constant flags |
| `PortInfo` | 查單一 port、declaration range 與 ordered bits | `portWidth`、direction flags、`ports[0]`、bit net names/IDs |
| `CountByGateType` | 統計 include-minus-exclude gate types | `gateTypeCounts`、`gateCount`、filter metadata |
| `GatesByType` | 列出 include-minus-exclude gates；可選 batch pin/net detail | `gateIds`、`gateNames`、`gateConnections`、filter metadata |
| `GatesWithConstantInput` | 找 constant input gates；可選 batch pin/net detail | `gateIds`、`gateNames`、`gateConnections`、`gateCount` |
| `StructuralIssues` | 回報結構問題 | net/gate issue lists、具 validity 的 unconnected input/output pin counts、compact `unconnectedPins` |

`ports` 是依 declaration 順序排列的 `PortSummary`，每筆包含 `name`、`width`、`msb`、`lsb`、`isBus`、`isInput`、`isOutput`。因此「列出所有 PI/PO 並附 bit width」只需要一次 `ListPrimaryInputs` 或 `ListPrimaryOutputs`。

`ListPrimaryInputs` / `ListPrimaryOutputs` 會固定填入 structured `ports`；`includeNames=false` 只省略重複的 flat `portNames`，不會移除 width、range 與 direction metadata。

`primaryInputCount` / `primaryOutputCount` 固定表示 Verilog port 數量；
`primaryInputBitCount` / `primaryOutputBitCount` 表示各 port declaration width 的總和。
`Summary` 同時填入四者，PI/PO list query 則只填入對應方向的 port/bit counts。

`PortInfo` 固定在 `ports` 填入一筆 `PortSummary`，提供 name、direction、width、bus flag 與
declaration bounds；不受 `includeNames` 影響。`netNames` 與 `netIds` 都依該 port 的 Verilog
declaration order 回傳，且兩者使用相同的 active-net filter，因此同一個 index 必定描述同一個
bit。`includeIds` / `includeNames` 只控制 flat vectors，不會移除 structured `ports[0]`。

`PortSummary.msb/lsb` 是既有歷史欄位名，實作忠實保存 declaration 的 left/right bounds，
不依數值大小重新排序。例如 `[31:0]` 回 `31/0`，`[0:31]` 回 `0/31`；scalar 回 `-1/-1`。

`GateInfo` 固定在 `gateConnections` 填入一筆 structured object snapshot；
`GatesByType` 或 `GatesWithConstantInput` 則在設定 `includeConnectionDetails=true` 時，依篩選後
gate ID 順序填入同一種 records。每筆 `GateConnectionSummary` 包含 gate ID/name/type、依 pin
順序排列的 inputs，以及 output。每個 `PinConnectionSummary` 明確區分 pin name、net ID/name、
是否 connected、constant value 與 PI/PO flags。這一層不做 traversal。

`GateInfo.formattedInfo` 暫時保留給既有 C++ caller；新程式應讀
`gateConnections[0]`。tools printer 有 structured record 時不重複輸出 legacy pin 字串。

`NetClassification` 對 current active named netlist 做一次 O(N) 掃描。PI、PO、constant 是
獨立 flags；`primaryInputOutput` 是 PI/PO 交集，`internal` 則只包含
`!isPI && !isPO && !isConst` 的 active nets。因此所有分類 count 不能直接相加。
`constant` 只代表 parser/netlist 明確標記的 constant net，不證明某條邏輯 net 的 Boolean
function 恆為 0/1；後者仍屬 Function Query。

`Summary`、`ListGates`、`ListNets`、`CountByGateType` 與 structural issue helpers 都以 current active design 為準。`GateInfo` / `NetInfo` 查詢已移除的 tombstone 會回報 not found，不會將 `UNKNOWN` 暴露成題目中的 gate type。

核心資料結構：

```cpp
struct BasicQuery {
    BasicQueryType type = BasicQueryType::Summary;
    std::string name;
    GateType gateType = GateType::UNKNOWN;
    std::vector<GateType> gateTypeFilters;
    std::vector<GateType> excludedGateTypeFilters;
    int constValue = -1;
    int inputCount = -1;
    bool includeIds = true;
    bool includeNames = true;
    bool includeConnectionDetails = false;
};

struct NetClassificationSummary {
    bool valid = false;
    size_t activeNetCount = 0;
    size_t primaryInputNetCount = 0;
    size_t primaryOutputNetCount = 0;
    size_t primaryInputOutputNetCount = 0;
    size_t constantNetCount = 0;
    size_t internalNetCount = 0;

    std::vector<int> primaryInputNetIds;
    std::vector<int> primaryOutputNetIds;
    std::vector<int> primaryInputOutputNetIds;
    std::vector<int> constantNetIds;
    std::vector<int> internalNetIds;
    std::vector<std::string> primaryInputNetNames;
    std::vector<std::string> primaryOutputNetNames;
    std::vector<std::string> primaryInputOutputNetNames;
    std::vector<std::string> constantNetNames;
    std::vector<std::string> internalNetNames;
};

struct BasicReport {
    bool ok = false;
    std::string message;

    bool hasGateCount = false;
    bool hasNetCount = false;
    bool hasLogicalWireCount = false;
    bool hasPrimaryInputCount = false;
    bool hasPrimaryOutputCount = false;
    bool hasPrimaryInputBitCount = false;
    bool hasPrimaryOutputBitCount = false;
    size_t gateCount = 0;
    size_t netCount = 0;
    size_t logicalWireCount = 0;
    size_t primaryInputCount = 0;
    size_t primaryOutputCount = 0;
    size_t primaryInputBitCount = 0;
    size_t primaryOutputBitCount = 0;

    size_t scopeGateCount = 0;
    bool gateTypeFilterApplied = false;
    bool gateTypeExclusionApplied = false;
    bool gateDetailsIncluded = false;
    std::vector<GateType> appliedGateTypeFilters;
    std::vector<GateType> appliedExcludedGateTypeFilters;

    std::vector<int> gateIds;
    std::vector<int> netIds;
    std::vector<std::string> gateNames;
    std::vector<std::string> netNames;
    std::vector<std::string> portNames;
    std::vector<GateConnectionSummary> gateConnections;
    NetClassificationSummary netClassification;

    std::map<GateType, int> gateTypeCounts;

    std::vector<std::string> undrivenNets;
    std::vector<std::string> noLoadNets;
    std::vector<std::string> floatingNets;
    std::vector<std::string> unconnectedGates;
    bool hasUnconnectedPinCounts = false;
    size_t unconnectedInputPinCount = 0;
    size_t unconnectedOutputPinCount = 0;
    std::vector<UnconnectedPinSummary> unconnectedPins;
    std::vector<std::string> floatingPrimaryInputNets;
    std::vector<std::string> unconnectedPrimaryOutputNets;
};
```

`UnconnectedPinSummary` 只保存 `gateId`、`pinIndex`、`netId`、`PinDirection` 與
`UnconnectedPinReason`，避免在百萬級 pin report 中重複複製 gate/net 名稱。reason 為
`Unconnected`、`InvalidNetId` 或 `RemovedNet`。掃描只處理 active gates 與 named netlist 中
實際存在的 pin slots；未出現在 instance 的 optional DFF pin 不會被合成為問題，明確 `.RN()`
則因 slot 存在且 net ID 為 -1 而被回報。這是結構狀態，不自行推論 functional invalidity。

`CountByGateType`、`GatesByType` 與 `GatesWithConstantInput` 共用 gate-type 集合語意。
`gateTypeFilters` 是 include OR-set，空集合代表全部；`excludedGateTypeFilters` 在 include
之後扣除，因此同一 type 同時出現在兩者時由 exclude 優先。重複 type 會去重，結果依 gate ID
穩定排序。`gateType` 僅保留給既有單型別呼叫；不得同時設定 concrete `gateType` 與
`gateTypeFilters`。所有集合都拒絕 `UNKNOWN`，且 tombstone gate 永遠不進入 scope 或結果。

`hasGateCount`、`hasNetCount`、`hasLogicalWireCount`、`hasPrimaryInputCount`、
`hasPrimaryOutputCount`、`hasPrimaryInputBitCount` 與 `hasPrimaryOutputBitCount` 用來區分
「有效結果剛好為 0」和「此 query 不提供該 count」。呼叫端只能在
對應 `has...Count=true` 時解讀 count；例如沒有 matching gate 時，`GatesWithConstantInput` 會回傳
`hasGateCount=true, gateCount=0`。

---

## 6. Prompt 對應方向

| Prompt 類型 | 低階 helper | 高階 query |
|---|---|---|
| How many gates are in this design? | active gate scan | `Summary` |
| How many PI/PO ports and total PI/PO bits are present? | port vector size + declaration-width sum | `Summary` |
| List all DFFs. | `getDffNames()` | `ListDffs` |
| Count NAND gates. | `getGateCountByType(GateType::NAND)` | `CountByGateType` |
| Is n1 a primary input? | `isPrimaryInputNet(getNetId("n1"))` | `NetInfo` |
| How many internal or structural constant nets exist? | active net flag scan | `NetClassification` |
| Report gate g1. | `getGateInfo("g1")` | `GateInfo` |
| Find gates with constant input. | `getGateNamesWithConstInput()` | `GatesWithConstantInput` |
| Find floating nets. | `getFloatingNetNames()` | `StructuralIssues` |

---

## 7. 目前完成狀態

實作位置：

```text
include/core/NetlistQueries.h
src/analysis/BasicAnalysis.cpp
```

已完成：

```text
Design count / summary 低階 API
ID / object lookup
Gate type query
Constant input query
Validity / type predicate
Name listing
Batch active-net structural classification
Port / bus helper
Structural issue helper
Edit 後 active-design / tombstone 過濾
BasicQuery / BasicReport 高階統一 API
```

測試狀態：

```text
mini test/test50/test50.ps1 已覆蓋 Basic type filters、constant inputs、strict parser、
GateInfo structured pins、PI/PO port/bit counts、net classification、edit 後 tombstone、
pin-level structural issues 與大型 self-contained artifact。
mini test/test15/test15.ps1 以 scalar、descending bus 與 ascending bus 驗證 aggregate width、
single PortInfo declaration bounds 與 bit ordering（17 passed / 0 failed）。
官方 test76 驗證 PI=4 ports/4 bits、PO=3 ports/10 bits，且 `port_info n4` 回 `2/0`；test91 summary 驗證
PI=9 ports/109 bits、PO=7 ports/134 bits；test91 `net_classes` 在 0.196 秒內分類
26,106 條 active nets，完整 artifact footer 為 `Complete: yes`。
```

---

## 8. 建議下一步

Direct Connectivity / Cone / Function / Path / Depth 文件目前都已獨立整理。  
BasicQuery 後續不需要再擴張成「所有查詢總入口」，它應維持只回答物件基本資訊。

後續可補：

```text
1. 自然語言到 BasicQueryType 的 prompt routing。
2. 視 hidden prompt 再細分 undriven internal net / no-load internal net；unconnected pin 已完成。
3. 和未來 unified AnalysisQuery routing 串接，但不改 BasicQuery 本身責任。
```
