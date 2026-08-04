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
    int constValue = -1
) const;

std::vector<std::string> getGateNamesWithConstInput(
    GateType type = GateType::UNKNOWN,
    int constValue = -1
) const;

int countGatesWithConstInput(
    GateType type = GateType::UNKNOWN,
    int constValue = -1
) const;
```

用途：

```text
找出 input 接到 1'b0 / 1'b1 的 gates。
限制 gate type，例如只找 NAND gates。
統計 constant input 使用情況。
```

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
| `getUndrivenNetNames()` | 非 PI、非 constant，且沒有合法 driver 的 net |
| `getNoLoadNetNames()` | 非 PO、非 constant，且沒有 load gate 的 net |
| `getFloatingNetNames()` | undriven 與 no-load 的 union，不重複 |
| `getUnconnectedGateNames()` | gate 的 input 或 output 存在非法 / disconnected net ID |

注意：

```text
DFF input 若被 disconnect，可能以 -1 留在 inputNetIds 裡。
因此 getUnconnectedGateNames() 需要把 -1 視為值得回報的結構狀態。
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
| `Summary` | design 基本規模與 gate type 統計 | counts、`gateTypeCounts` |
| `ListGates` | 列出所有 gate | `gateIds`、`gateNames` |
| `ListNets` | 列出所有 net | `netIds`、`netNames` |
| `ListPrimaryInputs` | 列出所有 PI port 與 width/range | `portNames`、`ports` |
| `ListPrimaryOutputs` | 列出所有 PO port 與 width/range | `portNames`、`ports` |
| `ListDffs` | 列出所有 DFF | `gateIds`、`gateNames` |
| `ListCombinationalGates` | 列出所有組合邏輯 gates | `gateIds`、`gateNames` |
| `GateInfo` | 查單一 gate | `objectId`、`typeName`、predicate flags、`formattedInfo` |
| `NetInfo` | 查單一 net | `objectId`、`typeName`、PI/PO/constant flags |
| `PortInfo` | 查單一 port | `portWidth`、`isBus`、direction flags、bit net names/IDs |
| `CountByGateType` | 統計 gate type | `gateTypeCounts`、`gateCount` |
| `GatesByType` | 列出指定 gate type | `gateIds`、`gateNames` |
| `GatesWithConstantInput` | 找 constant input gates | `gateIds`、`gateNames`、`gateCount` |
| `StructuralIssues` | 回報結構問題 | `undrivenNets`、`noLoadNets`、`floatingNets`、`unconnectedGates` |

`ports` 是依 declaration 順序排列的 `PortSummary`，每筆包含 `name`、`width`、`msb`、`lsb`、`isBus`、`isInput`、`isOutput`。因此「列出所有 PI/PO 並附 bit width」只需要一次 `ListPrimaryInputs` 或 `ListPrimaryOutputs`。

`ListPrimaryInputs` / `ListPrimaryOutputs` 會固定填入 structured `ports`；`includeNames=false` 只省略重複的 flat `portNames`，不會移除 width、range 與 direction metadata。

`PortInfo` 的 `netNames` 與 `netIds` 都依該 port 的 Verilog declaration order 回傳，且兩者使用相同的 active-net filter，因此同一個 index 必定描述同一個 bit。`includeIds` / `includeNames` 只控制欄位是否填入，不會改變另一個欄位的排序。方向由 `isPrimaryInput` / `isPrimaryOutput` 表示。

`Summary`、`ListGates`、`ListNets`、`CountByGateType` 與 structural issue helpers 都以 current active design 為準。`GateInfo` / `NetInfo` 查詢已移除的 tombstone 會回報 not found，不會將 `UNKNOWN` 暴露成題目中的 gate type。

核心資料結構：

```cpp
struct BasicQuery {
    BasicQueryType type = BasicQueryType::Summary;
    std::string name;
    GateType gateType = GateType::UNKNOWN;
    int constValue = -1;
    bool includeIds = true;
    bool includeNames = true;
};

struct BasicReport {
    bool ok = false;
    std::string message;

    size_t gateCount = 0;
    size_t netCount = 0;
    size_t logicalWireCount = 0;

    std::vector<int> gateIds;
    std::vector<int> netIds;
    std::vector<std::string> gateNames;
    std::vector<std::string> netNames;
    std::vector<std::string> portNames;

    std::map<GateType, int> gateTypeCounts;

    std::vector<std::string> undrivenNets;
    std::vector<std::string> noLoadNets;
    std::vector<std::string> floatingNets;
    std::vector<std::string> unconnectedGates;
};
```

---

## 6. Prompt 對應方向

| Prompt 類型 | 低階 helper | 高階 query |
|---|---|---|
| How many gates are in this design? | active gate scan | `Summary` |
| List all DFFs. | `getDffNames()` | `ListDffs` |
| Count NAND gates. | `getGateCountByType(GateType::NAND)` | `CountByGateType` |
| Is n1 a primary input? | `isPrimaryInputNet(getNetId("n1"))` | `NetInfo` |
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
Port / bus helper
Structural issue helper
Edit 後 active-design / tombstone 過濾
BasicQuery / BasicReport 高階統一 API
```

測試狀態：

```text
mini test/tester.cpp 已覆蓋 runBasicQuery() 的 Summary / GateInfo / NetInfo / GatesWithConstantInput。
CLI integration regression test9-test16：142 passed, 0 failed。
```

---

## 8. 建議下一步

Direct Connectivity / Cone / Function / Path / Depth 文件目前都已獨立整理。  
BasicQuery 後續不需要再擴張成「所有查詢總入口」，它應維持只回答物件基本資訊。

後續可補：

```text
1. 自然語言到 BasicQueryType 的 prompt routing。
2. 更完整的 structural issue report，例如區分 undriven internal net / no-load internal net / unconnected pin。
3. 和未來 unified AnalysisQuery routing 串接，但不改 BasicQuery 本身責任。
```
