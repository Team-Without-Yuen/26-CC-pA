# Startpoint-to-Endpoint Path Analysis 整理

這份文件整理「從某類起點走到某類終點」的 path analysis 類型，並記錄目前已完成的統一 `PathQuery` API 使用方式。

## 1. 核心抽象

大部分 path 類 prompt 都可以抽象成：

```text
startpoints -> combinational graph -> endpoints
```

並套用額外條件：

```text
required nodes：路徑必須經過的 net / gate
avoided nodes ：路徑必須避開的 net / gate
mode          ：exists / find any / enumerate all / min depth / max depth
boundary      ：遇到 DFF 是否停止
```

目前 Problem A 的主要網表語意是 gate-level netlist，所以基本 traversal 先採用：

```text
Net -> Gate -> Net
```

組合邏輯 path 的重要規則：

```text
DFF 是 sequential boundary。
組合路徑不從 DFF input 穿越到 DFF Q。
若要分析 register-to-register，應從前一顆 DFF 的 Q net 出發，到下一顆 DFF 的 D pin net 結束。
```

DFF pin 儲存規則：

```text
DFF 的 inputNetIds 與 inputPinNames 是同位置對應。
例如 inputPinNames[i] = "D" 時，inputNetIds[i] 才是 D pin 的 net。
若某個 DFF input 被 disconnect，必須把 inputNetIds[i] 設成 -1，不能 erase。
原因是 erase 會讓後面的 CK/RN/SN 往前遞補，造成 named pin 查詢錯位。
```

## 2. Startpoint 類型

Startpoint 是 path analysis 的起點。常見類型如下。

| 類型 | 例子 | 轉成 traversal 起點 |
|---|---|---|
| Primary Input | `input a` | PI 對應的 net |
| Bus Primary Input | `input [3:0] a` | `a[0] ... a[3]` 多個 net |
| DFF Q-pin | `ff1.Q` | DFF 的 output net |
| Specific Net | `n123` | 該 net |
| Gate Output | `g0 output` | gate 的 output net |
| Clock Source | `clk` | clock net，語意偏 control path |
| Reset Source | `rst_n` | reset net，語意偏 control path |
| Enable Source | `en` | enable net，若設計有 enable pin |

目前已有支援：

```text
Primary Input：可從 getPrimaryInputs() 取得。
Bus Primary Input：Port.netIds 已保存每個 bit net。
DFF Q-pin：Gate.outputNetId 可取得。
Specific Net：getNetId() / findNet() 可取得。
Gate Output：Gate.outputNetId 可取得。
```

目前缺比較明確的 helper：

```cpp
std::vector<int> getPrimaryInputNetIds() const;
int getDffOutputNetId(int dffGateId) const;
int getGateOutputNetId(int gateId) const;
```

目前已新增 helper：

```cpp
std::vector<int> getPrimaryInputNetIds() const;
int getDffOutputNetId(int dffGateId) const;
int getGateOutputNetId(int gateId) const;
```

## 3. Endpoint 類型

Endpoint 是 path analysis 的終點。常見類型如下。

| 類型 | 例子 | 轉成 traversal 終點 |
|---|---|---|
| Primary Output | `output y` | PO 對應的 net |
| Bus Primary Output | `output [3:0] y` | `y[0] ... y[3]` 多個 net |
| DFF D-pin | `ff1.D` | DFF input pin `"D"` 接到的 net |
| DFF Clock-pin | `ff1.CK` | DFF input pin `"CK"` 接到的 net |
| DFF Reset-pin | `ff1.RN` / `ff1.SN` | DFF reset/set pin 接到的 net |
| DFF Enable-pin | `ff1.EN` | 若 benchmark 或 cell 定義有 enable pin |
| Specific Net | `n16` | 該 net |
| Gate Input | `g0 input pin` | gate input pin 接到的 net |
| Gate Output | `g0 output` | gate output net |

目前已有支援：

```text
Primary Output：可從 getPrimaryOutputs() 取得。
DFF D-pin / CK / RN / SN：getDffInputNetId(dffGateId, pinName) 已可查。
Specific Net：getNetId() / findNet() 可查。
```

目前缺比較明確的 helper：

```cpp
std::vector<int> getPrimaryOutputNetIds() const;
int getGateInputNetId(int gateId, int pinIndex) const;
int getGateInputNetId(int gateId, const std::string& pinName) const;
```

目前已新增 helper：

```cpp
std::vector<int> getPrimaryOutputNetIds() const;
int getGateInputNetId(int gateId, int pinIndex) const;
int getGateInputNetId(int gateId, const std::string& pinName) const;
```

注意：目前 primitive gates 主要是 positional pins，沒有正式 pin name。  
若要支援「特定 gate input pin」查詢，通常要用 pin index，例如 input 0 / input 1。

## 4. 常見 Timing Path 類型

這些是 STA / EDA 中最常見的 startpoint-to-endpoint 組合。

| 名稱 | 路徑 | 目前支援狀態 |
|---|---|---|
| input-to-output | PI -> PO | 可用現有 path API 組合 |
| input-to-register | PI -> DFF.D | 已有 `getMaximumLogicDepthFromPiToDffD()` |
| register-to-register | DFF.Q -> DFF.D | 可用 `PathEndpoint(DffQ)` + `PathEndpoint(DffD)` 組合 |
| register-to-output | DFF.Q -> PO | 可用 `PathEndpoint(DffQ)` + `PathEndpoint(PrimaryOutput)` 組合 |
| net-to-net | specific net -> specific net | 已支援 |
| gate-output-to-net | gate output -> specific net | 可用 `PathEndpoint(GateOutput)` + `PathEndpoint(SpecificNet)` 組合 |
| net-to-gate-input | specific net -> gate input net | 可用 `PathEndpoint(SpecificNet)` + `PathEndpoint(GateInput)` 組合 |
| gate-output-to-gate-input | gate output -> gate input net | 可用 `PathEndpoint(GateOutput)` + `PathEndpoint(GateInput)` 組合 |

## 5. Control Path 類型

Clock / reset / enable path 通常不完全等同 data path，但在 netlist exploration 題目中仍可能被問到。

| 類型 | 路徑 | 注意事項 |
|---|---|---|
| clock-to-register-clock | clock source -> DFF.CK | 通常屬 clock tree / clock path |
| reset-to-register-reset | reset source -> DFF.RN / DFF.SN | 屬 async control path |
| enable-to-register-enable | enable source -> DFF.EN | 題目目前未明確說 dff 有 EN |

目前 benchmark 的 dff pins 需要依實際格式確認。已知常見 named pins：

```text
.D
.Q
.CK
.RN
.SN
```

控制類 path 建議先保留在同一套 endpoint resolver 中，但在結果中標註它是 control path。

## 6. Query Mode 類型

同一組 startpoints / endpoints 可以問不同問題。

| Mode | 問題例子 | 目前支援狀態 |
|---|---|---|
| Exists | 是否存在 path？ | 已支援 |
| FindAny | 回傳任意一條 path | 已支援 |
| EnumerateAll | 列出所有 path | 已支援；預設自動寫檔保存完整 enumeration |
| MinDepth | 最短 logic depth | 已支援 |
| MaxDepth | 最長 logic depth | 已支援 start/end net；PI->DFF.D 已有 wrapper |
| EveryPathThrough | 所有 path 是否都經過指定節點 | 已支援 |
| EveryPathAvoids | 所有 path 是否都避開指定節點 | 已支援 |
| CountPaths | path 數量 | 可由 enumerate size 得到，但大型電路不安全 |

## 7. Filter / Constraint 類型

目前 path API 已支援：

```cpp
std::vector<PathNode> requiredNodes;
std::vector<PathNode> avoidedNodes;
```

語意：

```text
requiredNodes 中的每個節點都必須經過。
avoidedNodes 中的任一節點都不能碰到。
PathNode 可表示 Net 或 Gate。
```

後續可擴充：

```text
限定最大深度 / 最小深度
限定只能走某個 cone 內部
限定不能穿過 DFF
限定 endpoint 必須是 PO / DFF.D
限定 startpoint 必須是 PI / DFF.Q
```

## 8. 目前已能回答的 prompt 類型

### 8.1 all gates reachable from n2

可視為 fanout cone query：

```cpp
getTransitiveFanoutConeGateNames("n2")
```

語意：

```text
從 n2 往下游走，列出所有 reachable gates。
目前 traversal 不穿越 DFF。
```

### 8.2 every gate connected to output of g0

可視為 direct fanout query：

```cpp
getGateFanoutNames("g0")
```

語意：

```text
g0.outputNet -> loadGateIds
```

### 8.3 maximum logic depth from any PI to any DFF D-pin

已新增：

```cpp
getMaximumLogicDepthFromPiToDffD()
```

語意：

```text
for each PI bit:
    for each DFF:
        D_net = getDffInputNetId(dff, "D")
        path = findLongestCombinationalPath(PI, D_net)
取最大 depth。
```

## 9. 目前還不能完整回答的 prompt 類型

### 9.1 Is output n16 always 0?

需要 constant SAT query，例如：

```cpp
bool isAlwaysConstant(const std::string& netName, int value) const;
```

目前 `checkEquivalence(net, "1'b0")` 不一定可靠，因為 constant net 的 name lookup 還要檢查。

### 9.2 Derive Boolean equation for n16

需要 symbolic expression builder，例如：

```cpp
std::string deriveBooleanEquation(const std::string& netName) const;
```

基本做法：

```text
從 target net 往 fanin 遞迴追 driver gate。
遇到 PI 停。
遇到 constant 回傳 0 / 1。
遇到 DFF.Q 視為 sequential boundary。
```

## 10. 目前的統一 API

目前已將常見 startpoint-to-endpoint path query 收斂成：

```cpp
enum class PathEndpointType {
    SpecificNet,
    PrimaryInput,
    PrimaryOutput,
    DffQ,
    DffD,
    DffClock,
    DffReset,
    GateOutput,
    GateInput
};

struct PathEndpoint {
    PathEndpointType type;
    std::string name;
    std::string pinName;
    int pinIndex = -1;
};

enum class PathQueryMode {
    Exists,
    FindAny,
    EnumerateAll,
    MinDepth,
    MaxDepth,
    EveryPathThrough,
    EveryPathAvoids
};

struct PathQuery {
    std::vector<PathEndpoint> startpoints;
    std::vector<PathEndpoint> endpoints;
    std::vector<Netlist::PathNode> requiredNodes;
    std::vector<Netlist::PathNode> avoidedNodes;
    PathQueryMode mode;
    bool combinationalOnly = true;
};

struct PathQueryResult {
    bool exists = false;
    int depth = -1;
    Netlist::CombinationalPath path;
    std::vector<Netlist::CombinationalPath> paths;
};
```

統一入口：

```cpp
PathQueryResult runPathQuery(const PathQuery& query) const;
```

## 11. 如何使用 PathQuery

詳細使用方式已獨立整理在：

```text
API_SPEC/STARTPOINT_ENDPOINT_PATH_QUERY_USAGE.md
```

該文件包含：

```text
startpoint 類型表格
endpoint 類型表格
mode 類型表格
requiredNodes / avoidedNodes 用法
PathQueryResult 欄位說明
常見範例
注意事項
```

## 12. 目前實作狀態

Endpoint resolver helpers：

```cpp
std::vector<int> getPrimaryInputNetIds() const;
std::vector<int> getPrimaryOutputNetIds() const;
int getDffOutputNetId(int dffGateId) const;
int getGateInputNetId(int gateId, int pinIndex) const;
```

目前已完成：

```cpp
std::vector<int> getPrimaryInputNetIds() const;
std::vector<int> getPrimaryOutputNetIds() const;
int getDffOutputNetId(int dffGateId) const;
int getGateOutputNetId(int gateId) const;
int getGateInputNetId(int gateId, int pinIndex) const;
int getGateInputNetId(int gateId, const std::string& pinName) const;
```

也已完成端點解析 API：

```cpp
std::vector<int> resolvePathEndpoint(const PathEndpoint& endpoint) const;
std::vector<int> resolvePathEndpoints(const std::vector<PathEndpoint>& endpoints) const;
```

目前解析規則：

```text
SpecificNet   -> 指定 net name
PrimaryInput  -> name 空字串代表全部 PI；有 name 則解析指定 PI port，bus 會展開
PrimaryOutput -> name 空字串代表全部 PO；有 name 則解析指定 PO port，bus 會展開
DffQ          -> 指定 DFF 的 Q/output net
DffD          -> 指定 DFF 的 D input net
DffClock      -> 預設解析 CK；若 pinName 非空，改用 pinName
DffReset      -> pinName 非空時解析指定 pin；否則同時嘗試 RN 與 SN
GateOutput    -> 指定 gate 的 output net
GateInput     -> pinName 非空時用 named pin；否則用 pinIndex
```

已完成第一版統一查詢入口：

```cpp
PathQueryResult runPathQuery(const PathQuery& query) const;
```

目前支援：

```text
PathQueryMode::Exists
PathQueryMode::FindAny
PathQueryMode::EnumerateAll
PathQueryMode::MinDepth
PathQueryMode::MaxDepth
PathQueryMode::EveryPathThrough
PathQueryMode::EveryPathAvoids
requiredNodes
avoidedNodes
combinationalOnly=true
```

目前尚未支援：

```text
combinationalOnly=false
```

目前測試狀態：

```text
實作檔案：src/analysis/PathAnalysis.cpp
型別檔案：include/core/PathTypes.h
tester：mini test/tester.cpp
目前 regression：Summary: 45 passed, 0 failed.
```

後續可補 convenience wrapper：

```text
PI -> PO
PI -> DFF.D
DFF.Q -> DFF.D
DFF.Q -> PO
```

這些 wrapper 不是底層能力缺口，而是為了讓 prompt handler 呼叫時更直覺。

這些 wrapper 可以直接包 `PathQuery`，不一定要另外寫底層搜尋邏輯。
