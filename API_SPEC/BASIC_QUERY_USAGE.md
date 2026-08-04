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
3. 視 query.type 設定 query.name / query.gateType / query.constValue / query.inputCount
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
| `Summary` | current active design 規模與統計 | 無 | `gateCount`, `netCount`, `logicalWireCount`, `primaryInputCount`, `primaryOutputCount`, `gateTypeCounts` |
| `ListGates` | 列出所有 active gate | 無 | `gateIds`, `gateNames`, `gateCount` |
| `ListNets` | 列出所有 active net | 無 | `netIds`, `netNames`, `netCount` |
| `ListPrimaryInputs` | 列出所有 PI ports 與 width/range | 無 | `portNames`, `ports`, `primaryInputCount` |
| `ListPrimaryOutputs` | 列出所有 PO ports 與 width/range | 無 | `portNames`, `ports`, `primaryOutputCount` |
| `ListDffs` | 列出所有 DFF | 無 | `gateIds`, `gateNames`, `gateCount` |
| `ListCombinationalGates` | 列出所有組合邏輯 gate | 無 | `gateIds`, `gateNames`, `gateCount` |
| `GateInfo` | 查單一 gate 基本資訊 | `name` = gate instance name | `exists`, `objectId`, `objectName`, `typeName`, `isDff`, `isCombinational`, `formattedInfo` |
| `NetInfo` | 查單一 net 基本資訊 | `name` = net name | `exists`, `objectId`, `objectName`, `typeName`, `isPrimaryInput`, `isPrimaryOutput`, `isConstant` |
| `PortInfo` | 查單一 PI/PO port | `name` = port name | `exists`, `objectName`, `portWidth`, `isBus`, `isPrimaryInput`, `isPrimaryOutput`, `netNames`, `netIds` |
| `CountByGateType` | 統計 gate type 數量 | `gateType` 可選 | `gateTypeCounts`, `gateCount`, `typeName` |
| `GatesByType` | 列出指定 gate type | `gateType` | `gateIds`, `gateNames`, `gateCount`, `typeName` |
| `GatesWithConstantInput` | 找 constant input gates | `gateType`、`constValue`、`inputCount` 可選 | `gateIds`, `gateNames`, `gateCount`, `typeName` |
| `StructuralIssues` | 找結構問題 | 無 | `undrivenNets`, `noLoadNets`, `floatingNets`, `unconnectedGates` |

---

## 3. BasicQuery 輸入欄位

`BasicQuery` 的欄位如下：

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `BasicQueryType` | `Summary` | 決定要執行哪一種 basic query |
| `name` | `std::string` | `""` | `GateInfo`, `NetInfo`, `PortInfo` 使用 |
| `gateType` | `GateType` | `UNKNOWN` | `CountByGateType`, `GatesByType`, `GatesWithConstantInput` 使用 |
| `constValue` | `int` | `-1` | `GatesWithConstantInput` 使用；`-1` 不限制、`0` 找 `1'b0`、`1` 找 `1'b1` |
| `inputCount` | `int` | `-1` | `GatesWithConstantInput` 使用；`-1` 不限制，非負數限制 gate input 數量 |
| `includeIds` | `bool` | `true` | 是否填入 `gateIds` / `netIds` |
| `includeNames` | `bool` | `true` | 是否填入 `gateNames` / `netNames` / `portNames`；不控制 structured `ports` |

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
| `gateTypeCounts` | 各 gate type 統計 |

### 4.3 單一物件資訊

| 欄位 | 意思 |
|---|---|
| `objectId` | `GateInfo` / `NetInfo` 的 gate ID 或 net ID |
| `objectName` | 查詢到的 gate/net/port 名稱 |
| `typeName` | gate type 或 net/port 類型描述 |
| `formattedInfo` | 人類可讀的格式化資訊，目前主要由 `GateInfo` 使用 |
| `isDff` | gate 是否 DFF |
| `isCombinational` | gate 是否 combinational gate |
| `isPrimaryInput` | `NetInfo` 的 net 是否 PI，或 `PortInfo` 的 port 是否 input |
| `isPrimaryOutput` | `NetInfo` 的 net 是否 PO，或 `PortInfo` 的 port 是否 output |
| `isConstant` | net 是否 constant |
| `isBus` | port 是否 bus |
| `portWidth` | port bit width |
| `ports` | batch PI/PO summaries；每筆含 name、width、msb/lsb、bus 與 direction flags |

### 4.4 List 類欄位

| 欄位 | 意思 |
|---|---|
| `gateIds` | 查詢結果中的 gate IDs |
| `netIds` | 查詢結果中的 net IDs |
| `gateNames` | 查詢結果中的 gate names |
| `netNames` | 查詢結果中的 net names |
| `portNames` | 查詢結果中的 port names |

### 4.5 Structural issue 欄位

| 欄位 | 意思 |
|---|---|
| `undrivenNets` | 非 PI、非 constant，且沒有合法 driver 的 nets |
| `noLoadNets` | 非 PO、非 constant，且沒有 load gate 的 nets |
| `floatingNets` | `undrivenNets` 和 `noLoadNets` 的 union |
| `unconnectedGates` | input 或 output 有無效 / unconnected net ID 的 gates |
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
| 各 gate type 數量 | `report.gateTypeCounts` |

`gateCount` / `netCount` 是 current design 的 active object 數，不包含 edit 留下的 `UNKNOWN` gate 或 `isRemoved` net slot。底層 `getGateCount()` / `getNetCount()` 則保留 physical slot count，不應由 LLM 直接當作題目答案。

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

`ListPrimaryInputs` / `ListPrimaryOutputs` 的 `ports` 一律包含 declaration-order metadata。若設定 `includeNames=false`，只有重複的 `portNames` 會省略，`ports` 仍可用來取得各 port 的 name、width、range 與 direction。

---

## 7. GateInfo

用途：

```text
查單一 gate 是否存在、ID、type、是否 DFF、是否 combinational。
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
| 格式化文字 | `report.formattedInfo` |

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

## 9. PortInfo

用途：

```text
查 PI/PO port 是否存在、方向、是否 bus、bit width，以及展開後的 bit net names/IDs。
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
| bit net names | `report.netNames` |
| bit net IDs | `report.netIds` |

`netNames` 與 `netIds` 都依 Verilog port declaration order 排列，且相同 index 對應同一個 bit。例如 `input [3:0] data_in` 會依序回傳 `data_in[3]` 到 `data_in[0]`。關閉 `includeIds` 或 `includeNames` 只會讓對應 vector 保持空白，不會改變另一個 vector 的順序。

---

## 10. Gate Type Query

用途：

```text
統計或列出指定 gate type。
```

### 10.1 CountByGateType

| 設定 | 意思 |
|---|---|
| `query.type = CountByGateType` | 執行 gate type count |
| `query.gateType = GateType::UNKNOWN` | 回傳全部 type 統計 |
| `query.gateType = GateType::NAND` | 只統計 NAND |

例子：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::CountByGateType;
query.gateType = GateType::NAND;

Netlist::BasicReport report = netlist.runBasicQuery(query);
int nandCount = report.gateCount;
```

### 10.2 GatesByType

| 設定 | 意思 |
|---|---|
| `query.type = GatesByType` | 列出指定 gate type |
| `query.gateType = GateType::DFF` | 列出所有 DFF |

例子：

```cpp
Netlist::BasicQuery query;
query.type = Netlist::BasicQueryType::GatesByType;
query.gateType = GateType::NAND;

Netlist::BasicReport report = netlist.runBasicQuery(query);
std::vector<std::string> nandGates = report.gateNames;
```

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
query.gateType = GateType::UNKNOWN;
query.constValue = -1;
query.inputCount = -1;

Netlist::BasicReport report = netlist.runBasicQuery(query);
```

參數表：

| 設定 | 意思 |
|---|---|
| `gateType = GateType::UNKNOWN` | 不限制 gate type |
| `gateType = GateType::NAND` | 只找 NAND gates |
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
| floating primary-input bit nets | `report.floatingPrimaryInputNets` |
| unconnected primary-output bit nets | `report.unconnectedPrimaryOutputNets` |

目前定義：

| 類型 | 定義 |
|---|---|
| undriven net | 非 PI、非 constant，且沒有雙向一致 active driver |
| no-load net | 非 PO、非 constant，且沒有雙向一致 active load |
| floating net | undriven 與 no-load 的 union |
| unconnected gate | input 或 output 存在無效 / unconnected net ID |

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
| List all primary inputs with their bit widths. | `ListPrimaryInputs` | 無 | `ports` |
| List all primary outputs with their bit widths. | `ListPrimaryOutputs` | 無 | `ports` |
| List all DFFs. | `ListDffs` | 無 | `gateNames` |
| List all combinational gates. | `ListCombinationalGates` | 無 | `gateNames` |
| Report gate g1. | `GateInfo` | `name = "g1"` | `formattedInfo`, `typeName` |
| Is gate g1 a DFF? | `GateInfo` | `name = "g1"` | `isDff` |
| Is net n1 a PI? | `NetInfo` | `name = "n1"` | `isPrimaryInput` |
| Is output y a bus? | `PortInfo` | `name = "y"` | `isBus`, `portWidth` |
| Find gates with constant input. | `GatesWithConstantInput` | 可選 `gateType`, `constValue` | `gateNames`, `gateCount` |
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
| `ListPrimaryInputs` | `getPrimaryInputNames()` |
| `ListPrimaryOutputs` | `getPrimaryOutputNames()` |
| `ListDffs` | `getDffNames()`, `getGatesByType(DFF)` |
| `ListCombinationalGates` | `getCombinationalGateNames()`, `isCombinationalGate()` |
| `GateInfo` | `getGateId()`, `getGateInfo()`, `isDffGate()`, `isCombinationalGate()` |
| `NetInfo` | `getNetId()`, `isPrimaryInputNet()`, `isPrimaryOutputNet()`, `isConstantNet()` |
| `PortInfo` | `getPortWidth()`, `isBusPort()`, `getPortBitNames()`, `expandNetToBits()` |
| `CountByGateType` | `countGatesByType()`, `getGateCountByType()` |
| `GatesByType` | `getGatesByType()` |
| `GatesWithConstantInput` | `findGatesWithConstInput()`, `getGateNamesWithConstInput()`, `countGatesWithConstInput()` |
| `StructuralIssues` | `getUndrivenNetNames()`, `getNoLoadNetNames()`, `getFloatingNetNames()`, `getUnconnectedGateNames()` |

---

## 17. 目前實作與測試狀態

```text
實作檔案：src/analysis/BasicAnalysis.cpp
型別檔案：include/core/NetlistQueries.h
tester：mini test/tester.cpp
CLI integration regression test9-test16：142 passed, 0 failed.
```
