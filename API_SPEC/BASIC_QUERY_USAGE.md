# Basic Query 使用說明

> 對外請使用 `structure_query <mode> [args]`。本文件中的 `BasicQuery` 是 `structure_query` 內部 dispatch 的 C++ API；舊 `basic_query` CLI 僅保留相容性。

這份文件只負責說明 `BasicQuery` 怎麼使用。  
設計背景與分類整理請看：

```text
API_SPEC/BASIC_QUERY_API.md
```

---

## 1. 基本概念

`BasicQuery` 用來回答「網表物件的基本資訊」。

基本形式：

```text
query.type + optional filter fields -> runBasicQuery() -> BasicReport
```

使用流程：

```text
1. 建立 Netlist::BasicQuery query
2. 設定 query.type
3. 視 query.type 設定 query.name / gate-type include/exclude filters / query.constValue / query.inputCount
4. 視需求設定 query.includeIds / query.includeNames
5. 呼叫 netlist.runBasicQuery(query)
6. 從 report.ok / report.message / report.* 欄位讀結果
```

注意：

```text
BasicQuery 只處理 gate/net/port 的基本資訊。
它不做 direct connectivity、cone traversal、path search、depth analysis 或 optimization。
如果問題需要跨多層往前或往後找，應改用 Cone / Path / Depth 類 API。
```

基本範例：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::Summary;

Netlist::BasicReport report = netlist.runBasicQuery(query);
if (report.ok) {
    size_t gateCount = report.gateCount;
    size_t netCount = report.netCount;
}
```

---

## 2. BasicQueryType 總表

| Query type | 問題類型 | 需要設定 | 主要讀取欄位 |
|---|---|---|---|
| `Summary` | current active design 規模與統計 | 無 | gate/net/wire counts、PI/PO port/bit counts、`gateTypeCounts` |
| `ListGates` | 列出所有 active gate | 無 | `gateIds`, `gateNames`, `gateCount` |
| `ListNets` | 列出所有 active net | 無 | `netIds`, `netNames`, `netCount` |
| `NetClassification` | 批次統計/列出 PI、PO、PI+PO、constant、internal nets | 無 | `netCount`, `netClassification` |
| `ListPrimaryInputs` | 列出所有 PI ports 與 width/range | 無 | `portNames`, `ports`, `primaryInputCount`, `primaryInputBitCount` |
| `ListPrimaryOutputs` | 列出所有 PO ports 與 width/range | 無 | `portNames`, `ports`, `primaryOutputCount`, `primaryOutputBitCount` |
| `ListDffs` | 列出所有 DFF | 無 | `gateIds`, `gateNames`, `gateCount` |
| `ListCombinationalGates` | 列出所有組合邏輯 gate | 無 | `gateIds`, `gateNames`, `gateCount` |
| `GateInfo` | 查單一 gate 與 pin/net snapshot | `name` = gate instance name | `exists`, `objectId`, `objectName`, `typeName`, `isDff`, `isCombinational`, `gateConnections[0]`；`formattedInfo` 保留相容 |
| `NetInfo` | 查單一 net 基本資訊 | `name` = net name | `exists`, `objectId`, `objectName`, `typeName`, `isPrimaryInput`, `isPrimaryOutput`, `isConstant` |
| `PortInfo` | 查單一 PI/PO port、range 與 ordered bits | `name` = port name | object/direction fields、`ports[0]`、`netNames`, `netIds` |
| `CountByGateType` | 統計一或多種 gate type，可選排除型別 | gate-type filters 可選 | `gateTypeCounts`, `gateCount`, filter metadata |
| `GatesByType` | 列出 include-minus-exclude gate 集合，可選 pin/net detail | gate-type filters、optional `includeConnectionDetails` | `gateIds`, `gateNames`, `gateConnections`, `gateCount`, filter metadata |
| `GatesWithConstantInput` | 找 constant input gates，再套用 include/exclude type | gate-type filters、`constValue`、`inputCount`、`includeConnectionDetails` 可選 | `gateIds`, `gateNames`, `gateConnections`, `gateCount`, filter metadata |
| `StructuralIssues` | 找結構問題 | 無 | `undrivenNets`, `noLoadNets`, `floatingNets`, `unconnectedGates` |

---

## 3. BasicQuery 輸入欄位

`BasicQuery` 的欄位如下：

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `BasicQueryType` | `Summary` | 決定要執行哪一種 basic query |
| `name` | `std::string` | `""` | `GateInfo`, `NetInfo`, `PortInfo` 使用 |
| `gateType` | `GateType` | `UNKNOWN` | legacy 單型別 filter；新呼叫優先使用 `gateTypeFilters` |
| `gateTypeFilters` | `std::vector<GateType>` | `{}` | include OR-set；空集合代表全部 |
| `excludedGateTypeFilters` | `std::vector<GateType>` | `{}` | exclude set；在 include 後扣除且優先 |
| `constValue` | `int` | `-1` | `GatesWithConstantInput` 使用；`-1` 不限制、`0` 找 `1'b0`、`1` 找 `1'b1` |
| `inputCount` | `int` | `-1` | `GatesWithConstantInput` 使用；`-1` 不限制，非負數限制 gate input 數量 |
| `includeIds` | `bool` | `true` | 是否填入 `gateIds` / `netIds` |
| `includeNames` | `bool` | `true` | 是否填入 `gateNames` / `netNames` / `portNames`；不控制 structured `ports` |
| `includeConnectionDetails` | `bool` | `false` | `GatesByType` / `GatesWithConstantInput` 是否填入每顆 gate 的 structured input/output pin-net records；`GateInfo` 不受此 flag 影響，固定回一筆 snapshot |

`gateType` 的常見值：

| GateType | 意思 |
|---|---|
| `GateType::AND` | AND gate |
| `GateType::OR` | OR gate |
| `GateType::NAND` | NAND gate |
| `GateType::NOR` | NOR gate |
| `GateType::NOT` | NOT gate |
| `GateType::BUF` | BUF gate |
| `GateType::XOR` | XOR gate |
| `GateType::XNOR` | XNOR gate |
| `GateType::DFF` | DFF |
| `GateType::UNKNOWN` | 不限制 type，或要求回傳全部 type 統計 |

---

## 4. BasicReport 回傳欄位

`BasicReport` 是統一回傳格式，不同 query type 會填不同欄位。

### 4.1 共用狀態

| 欄位 | 意思 |
|---|---|
| `ok` | 查詢是否成功 |
| `message` | 簡短狀態訊息，方便 debug 或回覆 LLM |
| `exists` | 對 `GateInfo` / `NetInfo` / `PortInfo`，表示指定物件是否存在 |

### 4.2 Count 類欄位

| 欄位 | 意思 |
|---|---|
| `gateCount` | gate 數量，可能是全部或篩選後數量 |
| `netCount` | net 數量，可能是全部或篩選後數量 |
| `logicalWireCount` | Verilog declaration 層級的 wire 數量 |
| `primaryInputCount` | PI port 數量 |
| `primaryOutputCount` | PO port 數量 |
| `primaryInputBitCount` | 所有 PI declaration width 的總和 |
| `primaryOutputBitCount` | 所有 PO declaration width 的總和 |
| `gateTypeCounts` | 各 gate type 統計 |

每個 count 都有對應 validity flag：`hasGateCount`、`hasNetCount`、`hasLogicalWireCount`、
`hasPrimaryInputCount`、`hasPrimaryOutputCount`、`hasPrimaryInputBitCount`、
`hasPrimaryOutputBitCount`。flag 為 `true` 時 count 即使是 `0` 也是完整、有效
答案；flag 為 `false` 則代表該 mode 不提供此 metric，不能把預設零值當成答案。

### 4.3 單一物件資訊

| 欄位 | 意思 |
|---|---|
| `objectId` | `GateInfo` / `NetInfo` 的 gate ID 或 net ID |
| `objectName` | 查詢到的 gate/net/port 名稱 |
| `typeName` | gate type 或 net/port 類型描述 |
| `formattedInfo` | `GateInfo` 的 legacy 人類可讀相容欄位；新 caller 應讀 `gateConnections[0]` |
| `isDff` | gate 是否 DFF |
| `isCombinational` | gate 是否 combinational gate |
| `isPrimaryInput` | `NetInfo` 的 net 是否 PI，或 `PortInfo` 的 port 是否 input |
| `isPrimaryOutput` | `NetInfo` 的 net 是否 PO，或 `PortInfo` 的 port 是否 output |
| `isConstant` | net 是否 constant |
| `isBus` | port 是否 bus |
| `portWidth` | port bit width |
| `ports` | PI/PO structured summaries；batch list 回多筆，`PortInfo` 固定回一筆；每筆含 name、width、msb/lsb、bus 與 direction flags |

### 4.4 List 類欄位

| 欄位 | 意思 |
|---|---|
| `gateIds` | 查詢結果中的 gate IDs |
| `netIds` | 查詢結果中的 net IDs |
| `gateNames` | 查詢結果中的 gate names |
| `netNames` | 查詢結果中的 net names |
| `portNames` | 查詢結果中的 port names |
| `gateConnections` | `GatesByType` 的 optional structured details；每筆含 gate ID/name/type、ordered input pins 與 output pin |
| `netClassification` | `NetClassification` 的 structured counts 與分類 names/IDs；先確認 `valid=true` |

`GateConnectionSummary.inputs` 依 netlist pin order 排列。每個 `PinConnectionSummary` 都包含
`pinName`、`netId`、`netName`、`connected`、constant value 與 PI/PO flags；DFF output pin 使用 `Q`，
一般 combinational output 使用 `OUT`。

`NetClassificationSummary` 的 count 不受 `includeNames/includeIds` 影響；兩個 flags 只控制對應
name/ID vectors。PI 與 PO 可重疊，交集另存在 `primaryInputOutputNet*`；`internalNet*` 是排除
PI、PO、constant 後的 exclusive remainder。

### 4.5 Structural issue 欄位

| 欄位 | 意思 |
|---|---|
| `undrivenNets` | 非 PI、非 constant，且沒有合法 driver 的 nets |
| `noLoadNets` | 非 PO、非 constant，且沒有 load gate 的 nets |
| `floatingNets` | `undrivenNets` 和 `noLoadNets` 的 union |
| `unconnectedGates` | input 或 output 有無效 / unconnected net ID 的 gates |
| `hasUnconnectedPinCounts` | true 時 input/output pin counts 為有效結果，包括合法的 0 |
| `unconnectedInputPinCount` | 有問題的 existing input pin slots 數量 |
| `unconnectedOutputPinCount` | 有問題的 output pin slots 數量 |
| `unconnectedPins` | compact pin-level records：gate/pin/net ID、方向與原因 |
| `floatingPrimaryInputNets` | 沒有雙向一致 active load 的 PI bit nets |
| `unconnectedPrimaryOutputNets` | 沒有雙向一致 active driver 的 PO bit nets |

---

## 5. Summary

用途：

```text
回答 design 規模、PI/PO 數量、gate type 統計。
```

寫法：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::Summary;

Netlist::BasicReport report = netlist.runBasicQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| gate 數量 | `report.gateCount` |
| net 數量 | `report.netCount` |
| logical wire 數量 | `report.logicalWireCount` |
| PI port 數量 | `report.primaryInputCount` |
| PO port 數量 | `report.primaryOutputCount` |
| PI bit 總數 | `report.primaryInputBitCount` |
| PO bit 總數 | `report.primaryOutputBitCount` |
| 各 gate type 數量 | `report.gateTypeCounts` |

`gateCount` / `netCount` 是 current design 的 active object 數，不包含 edit 留下的 `UNKNOWN` gate 或 `isRemoved` net slot。底層 `getGateCount()` / `getNetCount()` 則保留 physical slot count，不應由 LLM 直接當作題目答案。

`primaryInputCount` / `primaryOutputCount` 永遠是 port count，不是展開後的 signal/bit count；
bit count 由每個 `Port.netIds.size()` 加總 declaration width。不要再由 caller 自行加總 `ports.width`。

---

## 6. List 類 Query

用途：

```text
列出某一類物件名稱或 ID。
```

| Query type | 寫法 | 回傳 |
|---|---|---|
| `ListGates` | `query.type = BasicQueryType::ListGates` | `gateIds`, `gateNames` |
| `ListNets` | `query.type = BasicQueryType::ListNets` | `netIds`, `netNames` |
| `ListPrimaryInputs` | `query.type = BasicQueryType::ListPrimaryInputs` | `portNames`, `ports` |
| `ListPrimaryOutputs` | `query.type = BasicQueryType::ListPrimaryOutputs` | `portNames`, `ports` |
| `ListDffs` | `query.type = BasicQueryType::ListDffs` | `gateIds`, `gateNames` |
| `ListCombinationalGates` | `query.type = BasicQueryType::ListCombinationalGates` | `gateIds`, `gateNames` |

例子：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::ListDffs;

Netlist::BasicReport report = netlist.runBasicQuery(query);
std::vector<std::string> dffNames = report.gateNames;
```

`ListPrimaryInputs` / `ListPrimaryOutputs` 的 `ports` 一律包含 declaration-order metadata，並直接回傳
該方向的 aggregate bit count。若設定 `includeNames=false`，只有重複的 `portNames` 會省略，
`ports` 與 port/bit counts 仍然有效。

---

## 7. GateInfo

用途：

```text
查單一 gate 是否存在、ID、type、是否 DFF/組合 gate，以及 ordered input/output pin-net snapshot。
```

寫法：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::GateInfo;
query.name = "g1";

Netlist::BasicReport report = netlist.runBasicQuery(query);
```

`name` 的意思：

| 欄位 | 意思 |
|---|---|
| `query.name` | gate instance name，例如 `g1`、`ff1` |

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| gate 是否存在 | `report.ok && report.exists` |
| gate ID | `report.objectId` |
| gate name | `report.objectName` |
| gate type | `report.typeName` |
| 是否 DFF | `report.isDff` |
| 是否 combinational | `report.isCombinational` |
| structured gate/pin/net record | `report.gateConnections[0]` |
| input pin names/nets/constant/PI flags | `report.gateConnections[0].inputs` |
| output pin/net/PO flag | `report.gateConnections[0].output` |
| legacy 格式化文字 | `report.formattedInfo` |

成功的 `GateInfo` 固定回傳一筆 `gateConnections`，不需要設定
`includeConnectionDetails`。`formattedInfo` 只供舊 caller 相容；tools printer 在 structured
record 存在時不重複輸出這段文字。

---

## 8. NetInfo

用途：

```text
查單一 net 是否存在、ID、是否 PI/PO/constant。
```

寫法：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::NetInfo;
query.name = "n1";

Netlist::BasicReport report = netlist.runBasicQuery(query);
```

`name` 的意思：

| 欄位 | 意思 |
|---|---|
| `query.name` | net name，例如 `n1`、`a`、`bus[0]` |

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| net 是否存在 | `report.ok && report.exists` |
| net ID | `report.objectId` |
| net name | `report.objectName` |
| net 類型文字 | `report.typeName` |
| 是否 PI | `report.isPrimaryInput` |
| 是否 PO | `report.isPrimaryOutput` |
| 是否 constant | `report.isConstant` |

注意：

```text
constant net 可能是 parser 內部建立的 net。
不要強烈依賴 getNetId("1'b0") 或 getNetId("1'b1") 一定能查到。
若要判斷某個 gate input 是否 constant，建議查該 input net ID 後看 isConstant。
```

---

## 8.1 NetClassification

用途：

```text
一次統計並列出 current active named netlist 中的 PI、PO、PI+PO、constant 與 internal nets。
```

寫法：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::NetClassification;

Netlist::BasicReport report = netlist.runBasicQuery(query);
const Netlist::NetClassificationSummary& classes = report.netClassification;
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 結果是否有效 | `classes.valid` |
| 全部 active net 數 | `classes.activeNetCount` 或 `report.netCount` |
| PI/PO bit-net 數 | `primaryInputNetCount`, `primaryOutputNetCount` |
| 同時為 PI 與 PO 的 net 數 | `primaryInputOutputNetCount` |
| parser 標記的 constant net 數 | `constantNetCount` |
| 排除 PI/PO/constant 後的 internal net 數 | `internalNetCount` |
| 各分類完整名稱/ID | 對應 `*NetNames` / `*NetIds` vectors |

PI、PO、constant 採獨立 flag 判斷；PI+PO 會同時出現在 PI、PO 與交集 vectors，因此不能把
所有 count 相加。`internal` 才是 exclusive remainder。這裡的 constant 是結構標記，不是
Boolean functional constant；例如 `n = a & !a` 的 `n` 在此仍是 internal net。

對外 CLI：

```text
structure_query net_classes
```

大型名稱結果由 tools 自動完整寫入 artifact，caller 不設定截斷數量或輸出檔名。

---

## 9. PortInfo

用途：

```text
查 PI/PO port 是否存在、方向、是否 bus、bit width、declaration range，以及展開後的 bit net names/IDs。
```

寫法：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::PortInfo;
query.name = "bus";

Netlist::BasicReport report = netlist.runBasicQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| port 是否存在 | `report.ok && report.exists` |
| port name | `report.objectName` |
| port 是否 input/output | `report.isPrimaryInput`, `report.isPrimaryOutput` |
| 是否 bus | `report.isBus` |
| bit width | `report.portWidth` |
| structured port snapshot | `report.ports[0]` |
| declaration left bound（歷史欄位名 msb） | `report.ports[0].msb` |
| declaration right bound（歷史欄位名 lsb） | `report.ports[0].lsb` |
| bit net names | `report.netNames` |
| bit net IDs | `report.netIds` |

成功的 `PortInfo` 固定回傳一筆 `ports`，不受 `includeNames` 影響。`netNames` 與 `netIds` 都依
Verilog port declaration order 排列，且相同 index 對應同一個 bit。例如 `[3:0] data_in` 依序
回 `data_in[3]` 到 `data_in[0]`，`[0:2] ascending_in` 則回 `[0]`、`[1]`、`[2]`。

`PortSummary.msb/lsb` 沿用既有名稱，但代表 declaration left/right bounds，不會正規化成
數值最大/最小值。因此 `[0:31]` 必須讀成 `msb=0, lsb=31`。scalar 使用 `-1/-1`。
關閉 `includeIds` 或 `includeNames` 只會讓對應 flat vector 保持空白，不會移除 `ports[0]`。

---

## 10. Gate Type Query

用途：

```text
統計或列出一或多種 gate type，也可排除指定 type。
```

共用集合規則：`gateTypeFilters` 採 OR 聯集；未指定 include 表示全部 active gates；
`excludedGateTypeFilters` 最後扣除且優先。重複 type 會去重，合法的零結果仍回
`ok=true, hasGateCount=true, gateCount=0`。

### 10.1 CountByGateType

| 設定 | 意思 |
|---|---|
| `query.type = CountByGateType` | 執行 gate type count |
| include 未指定 | 回傳全部未排除 type 統計 |
| `query.gateTypeFilters = {NAND, NOR}` | 統計 NAND 與 NOR 的 OR 聯集 |
| `query.excludedGateTypeFilters = {DFF}` | 排除 DFF |

例子：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::CountByGateType;
query.gateTypeFilters = {GateType::NAND, GateType::NOR};
query.excludedGateTypeFilters = {GateType::NOR};

Netlist::BasicReport report = netlist.runBasicQuery(query);
size_t nandCount = report.gateCount;
```

### 10.2 GatesByType

| 設定 | 意思 |
|---|---|
| `query.type = GatesByType` | 列出 include-minus-exclude gate 集合 |
| include 未指定 | 列出全部 active gates |
| `query.gateTypeFilters = {AND, OR, NOT}` | 列出三種 type |
| `query.excludedGateTypeFilters = {OR}` | 從候選移除 OR |

例子：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::GatesByType;
query.gateTypeFilters = {GateType::NAND, GateType::NOR};
query.includeConnectionDetails = true;

Netlist::BasicReport report = netlist.runBasicQuery(query);
std::vector<std::string> selectedGates = report.gateNames;
std::vector<GateConnectionSummary> selectedConnections = report.gateConnections;
```

不需要 connections 時維持預設 `false`，避免建立重複的 detail records。大量 results 的檔案輸出
由 `tools.cpp` 負責；C++ API 本身回傳完整 structured records，不做隱含截斷。

---

## 11. GatesWithConstantInput

用途：

```text
找出 input 接到 1'b0 或 1'b1 的 gates。
```

寫法：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::GatesWithConstantInput;
query.gateTypeFilters = {GateType::NAND, GateType::NOR};
query.excludedGateTypeFilters = {GateType::NOR};
query.constValue = -1;
query.inputCount = -1;
query.includeConnectionDetails = true;

Netlist::BasicReport report = netlist.runBasicQuery(query);
```

參數表：

| 設定 | 意思 |
|---|---|
| include 未指定 | 不限制 gate type |
| `gateTypeFilters = {NAND, NOR}` | 找 NAND 或 NOR |
| `excludedGateTypeFilters = {NOR}` | 從候選排除 NOR |
| `constValue = -1` | 不限制 constant value |
| `constValue = 0` | 只找接到 `1'b0` 的 input |
| `constValue = 1` | 只找接到 `1'b1` 的 input |
| `inputCount = -1` | 不限制 gate input 數量 |
| `inputCount = 2` | 只找 two-input gates |

`constValue` 只能是 `-1/0/1`，`inputCount` 只能是 `-1` 或非負數。非法值會回傳 `ok=false`，呼叫端不可把它解讀為「找到 0 個 gate」。

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 數量 | `report.gateCount` |
| gate IDs | `report.gateIds` |
| gate names | `report.gateNames` |
| constant 所在 pin、其他 inputs 與 output net | `report.gateConnections` |

不需要連接明細時保留 `includeConnectionDetails=false`，即可維持 names-only 回傳。詳細模式仍只列出
符合 constant filter 的 gates；每筆 input 的 `isConstant` / `constantValue` 可精確指出 constant
接在哪個 pin。

---

## 12. StructuralIssues

用途：

```text
找出可能有結構問題的 nets / gates。
```

寫法：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::StructuralIssues;

Netlist::BasicReport report = netlist.runBasicQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 無 driver nets | `report.undrivenNets` |
| 無 load nets | `report.noLoadNets` |
| floating nets | `report.floatingNets` |
| 有 unconnected pin 的 gates | `report.unconnectedGates` |
| unconnected input/output pin 數量 | `report.unconnectedInputPinCount` / `report.unconnectedOutputPinCount`；先檢查 validity |
| 每個有問題的 pin | `report.unconnectedPins` |
| floating primary-input bit nets | `report.floatingPrimaryInputNets` |
| unconnected primary-output bit nets | `report.unconnectedPrimaryOutputNets` |

目前定義：

| 類型 | 定義 |
|---|---|
| undriven net | 非 PI、非 constant，且沒有雙向一致 active driver |
| no-load net | 非 PO、非 constant，且沒有雙向一致 active load |
| floating net | undriven 與 no-load 的 union |
| unconnected gate | input 或 output 存在無效 / unconnected net ID |

`unconnectedPins` 的 reason 分為 `Unconnected`（負 net ID）、`InvalidNetId`（超出 storage
範圍）與 `RemovedNet`（指向 tombstone net）。只掃描 gate 已儲存的 slots；省略的 optional DFF
control 不會憑空產生紀錄，明確 `.RN()` 會保留 pin 名稱並回報。pin-level record 是結構診斷，
不等同於宣稱該 DFF 在功能上非法。

PI/PO 專用欄位回傳 bit-net names。例如未驅動的 `output [1:0] y` 會回傳 `y[1]`、`y[0]`，不會只回傳 base port name `y`；因此可直接用 vector size 回答 signal 數量。

---

## 13. includeIds / includeNames

`BasicQuery` 可以控制結果是否填 ID 或 name。

| 欄位 | 預設 | 效果 |
|---|---|---|
| `includeIds` | `true` | 填入 `gateIds` / `netIds` |
| `includeNames` | `true` | 填入 `gateNames` / `netNames` / `portNames` |

例子：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::ListGates;
query.includeIds = false;
query.includeNames = true;

Netlist::BasicReport report = netlist.runBasicQuery(query);
```

適合情況：

| 需求 | 建議 |
|---|---|
| 給人看 | `includeNames = true` |
| 給後續 engine 用 | `includeIds = true` |
| 降低回傳資料量 | 關掉不需要的欄位 |

---

## 14. Prompt 對應表

| Prompt | 建議 BasicQueryType | 需要設定 | 主要讀取 |
|---|---|---|---|
| Determine the number of gates. | `Summary` | 無 | `gateCount` |
| Determine the number of wires. | `Summary` | 無 | `logicalWireCount` |
| Count all NAND gates. | `CountByGateType` | `gateType = NAND` | `gateCount` |
| List all gates. | `ListGates` | 無 | `gateNames` |
| List all nets. | `ListNets` | 無 | `netNames` |
| Count/list internal, PI, PO or structural constant nets. | `NetClassification` | 無 | `netClassification` |
| List all primary inputs with their bit widths. | `ListPrimaryInputs` | 無 | `ports` |
| List all primary outputs with their bit widths. | `ListPrimaryOutputs` | 無 | `ports` |
| Report PI/PO port counts and total bit counts. | `Summary` | 無 | four PI/PO port/bit count fields |
| List all DFFs. | `ListDffs` | 無 | `gateNames` |
| List all combinational gates. | `ListCombinationalGates` | 無 | `gateNames` |
| Report gate g1 with its type and pin connections. | `GateInfo` | `name = "g1"` | `typeName`, `gateConnections[0]` |
| Is gate g1 a DFF? | `GateInfo` | `name = "g1"` | `isDff` |
| Is net n1 a PI? | `NetInfo` | `name = "n1"` | `isPrimaryInput` |
| Is output y a bus? | `PortInfo` | `name = "y"` | `isBus`, `portWidth` |
| Is port data declared `[31:0]` or `[0:31]`? | `PortInfo` | `name = "data"` | `ports[0].msb`, `ports[0].lsb` |
| Find gates with constant input. | `GatesWithConstantInput` | 可選 `gateType`, `constValue` | `gateNames`, `gateCount` |
| List constant-input gates with their input/output signals. | `GatesWithConstantInput` | `includeConnectionDetails = true` | `gateConnections`, `gateCount` |
| Find floating nets. | `StructuralIssues` | 無 | `floatingNets` |

---

## 15. 何時不要用 BasicQuery

如果問題包含「直接連線」或「跨多層 traversal」，不要只用 BasicQuery。

| 問題 | 應改用 |
|---|---|
| Report every gate connected to the output of g0. | Direct Connectivity Query |
| Which gate drives net n1? | Direct Connectivity Query |
| Find all gates reachable from n2. | Cone / Fanout Query |
| Find all gates in fanin cone of y. | Cone / Fanin Query |
| Is there a path from a to y? | PathQuery |
| Find shortest path from a to y avoiding g3. | PathQuery |
| What is the maximum depth to output y? | DepthAnalysis |
| What is the critical path? | DepthAnalysis |

---

## 16. 低階 Helper 對照

`BasicQuery` 是高階入口；底層仍可直接使用 helper。

| BasicQueryType | 底層 helper |
|---|---|
| `Summary` | 以 `getGateCount()` / `getNetCount()` 作為掃描邊界，過濾 tombstone，並使用 `getLogicalWireCount()` / `countGatesByType()` |
| `ListGates` | `getAllGateNames()` |
| `ListNets` | `getAllNetNames()` |
| `NetClassification` | 單次 active-net scan；直接讀 `Net.isPI/isPO/isConst/isRemoved` |
| `ListPrimaryInputs` | `getPrimaryInputNames()` |
| `ListPrimaryOutputs` | `getPrimaryOutputNames()` |
| `ListDffs` | `getDffNames()`, `getGatesByType(DFF)` |
| `ListCombinationalGates` | `getCombinationalGateNames()`, `isCombinationalGate()` |
| `GateInfo` | `getGateId()`, `buildGateConnectionSummary()`, `getGateInfo()` legacy fallback、`isDffGate()`, `isCombinationalGate()` |
| `NetInfo` | `getNetId()`, `isPrimaryInputNet()`, `isPrimaryOutputNet()`, `isConstantNet()` |
| `PortInfo` | stored `Port` metadata、`getPortWidth()`、`getPortBitNames()`、`expandNetToBits()` |
| `CountByGateType` | `countGatesByType()`, `getGateCountByType()` |
| `GatesByType` | `getGatesByType()` |
| `GatesWithConstantInput` | `findGatesWithConstInput()`, `getGateNamesWithConstInput()`, `countGatesWithConstInput()` |
| `StructuralIssues` | net issue helpers + 共用 O(active gates + stored pin slots) unconnected-pin collector |

---

## 17. 目前實作與測試狀態

```text
實作檔案：src/analysis/BasicAnalysis.cpp
型別檔案：include/core/NetlistQueries.h
focused regression：mini test/test50/test50.ps1
port-width/range regression：mini test/test15/test15.ps1，17 passed / 0 failed。
GateInfo、PI/PO port/bit counts、net classification、constant、DFF named pins、tombstone 與
pin-level structural diagnostics 全數通過；150 筆 pin issue 會完整寫入 artifact。
official smoke：test76 為 PI 4/4、PO 3/10，且 `port_info n4` 回 range `2/0`；
test91 為 PI 9/109、PO 7/134（ports/bits），`net_classes` 回 active 26,106、PI 109、
PO 134、PI+PO 0、constant 1、internal 25,862，完整 artifact footer 為 `Complete: yes`。
```
