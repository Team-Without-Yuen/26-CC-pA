# Direct Connectivity Query 使用說明

這份文件只負責說明 `DirectConnectivityQuery` 怎麼使用。  
設計背景與分類整理請看：

```text
API_SPEC/DIRECT_CONNECTIVITY_QUERY_API.md
```

---

## 1. 基本概念

`DirectConnectivityQuery` 用來回答 gate/net 的直接連線問題。

基本形式：

```text
query.type + gateName/netName -> runDirectConnectivityQuery() -> DirectConnectivityReport
```

使用流程：

```text
1. 建立 Netlist::DirectConnectivityQuery query
2. 設定 query.type
3. 視 query.type 設定 query.gateName / query.netName
4. 視需求設定 query.includeIds / query.includeNames
5. 呼叫 netlist.runDirectConnectivityQuery(query)
6. 從 report.ok / report.connected / report.gateNames / report.netNames 讀結果
```

注意：

```text
DirectConnectivityQuery 只查 immediate connection。
不會往前或往後跨多層 traversal。
如果要找所有 reachable gates，應使用 Cone Query。
如果要找 startpoint 到 endpoint 的路徑，應使用 PathQuery。
```

---

## 2. Query Type 總表

| Query type | 問題類型 | 需要設定 | 主要讀取欄位 |
|---|---|---|---|
| `NetDriver` | 誰 drive 這條 net / bus | `netName` | `gateIds`, `gateNames`, `count` |
| `NetLoads` | 這條 net / bus load 到哪些 gates | `netName` | `gateIds`, `gateNames`, `count` |
| `FanoutLoadReport` | 依 Problem A QA 定義統計 pin-level fanout loads | `netName` | `fanoutLoadReport`, `count` |
| `GlobalFanoutReport` | 全設計或 PI-only fanout max / violations | `fanoutLimit`, `primaryInputsOnly` | `globalFanoutReport` |
| `GateInputs` | 這顆 gate 的 input nets | `gateName` | `netIds`, `netNames`, `count` |
| `GateOutput` | 這顆 gate 的 output net | `gateName` | `netId`, `netName`, `netIds`, `netNames` |
| `GateFanin` | 哪些 gates 直接餵進這顆 gate | `gateName` | `gateIds`, `gateNames`, `count` |
| `GateFanout` | 這顆 gate output 直接接到哪些 gates | `gateName` | `gateIds`, `gateNames`, `count` |
| `DirectlyConnected` | gate 和 net 是否直接相連 | `gateName`, `netName` | `connected`, `count` |

---

## 3. DirectConnectivityQuery 輸入欄位

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `DirectConnectivityQueryType` | `NetDriver` | 決定查詢類型 |
| `gateName` | `std::string` | `""` | gate 相關 query 使用 |
| `netName` | `std::string` | `""` | net 相關 query 使用 |
| `includeIds` | `bool` | `true` | 是否填 `gateIds` / `netIds` |
| `includeNames` | `bool` | `true` | 是否填 `gateNames` / `netNames` |

---

## 4. DirectConnectivityReport 回傳欄位

| 欄位 | 意思 |
|---|---|
| `ok` | 查詢是否成功 |
| `exists` | 指定 gate/net 是否存在 |
| `connected` | `DirectlyConnected` 的主要布林結果 |
| `message` | debug / LLM response 用的簡短訊息 |
| `gateName` | 查詢指定或解析出的 gate name |
| `netName` | 查詢指定或解析出的 net name |
| `gateId` | 單一 gate ID 結果，沒有唯一 gate 時為 -1 |
| `netId` | 單一 net ID 結果，沒有唯一 net 時為 -1 |
| `count` | 結果數量 |
| `gateIds` | 查詢結果中的 gate IDs |
| `netIds` | 查詢結果中的 net IDs |
| `gateNames` | 查詢結果中的 gate names |
| `netNames` | 查詢結果中的 net names |
| `fanoutLoadReport` | `FanoutLoadReport` 查詢的分類結果 |
| `globalFanoutReport` | `GlobalFanoutReport` 查詢的全域彙整結果 |

---

## 5. NetDriver

用途：

```text
查某個 net / bus 的直接 driver gate。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::NetDriver;
query.netName = "n1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| driver gate names | `report.gateNames` |
| driver gate IDs | `report.gateIds` |
| driver 數量 | `report.count` |

---

## 6. NetLoads

用途：

```text
查某個 net / bus 直接 load 到哪些 gates。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::NetLoads;
query.netName = "n1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| load gate names | `report.gateNames` |
| load gate IDs | `report.gateIds` |
| load 數量 | `report.count` |

注意：

```text
NetLoads 只回答「這條 net 接到哪些 gate/DFF instance」。
它不會把 primary output connection 算成 load，也不會區分 DFF.D / DFF.CK / DFF.RN / DFF.SN。
若題目問 fanout load count，應使用 FanoutLoadReport。
```

---

## 7. FanoutLoadReport

用途：

```text
依照 Problem A QA 的 fanout load 定義，查某條 net 的 pin-level loads。
```

QA fanout load 包含：

```text
primitive gate input pin
DFF D pin
DFF CK pin
DFF RN/SN pin
primary output connection
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::FanoutLoadReport;
query.netName = "n1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| QA fanout load 總數 | `report.fanoutLoadReport.totalLoadCount` 或 `report.count` |
| primitive gate input loads | `report.fanoutLoadReport.combinationalGateLoads` |
| DFF D-pin loads | `report.fanoutLoadReport.dffDataLoads` |
| DFF clock loads | `report.fanoutLoadReport.dffClockLoads` |
| DFF reset/set loads | `report.fanoutLoadReport.dffResetSetLoads` |
| 是否直接 drive PO | `report.fanoutLoadReport.drivesPrimaryOutput` |
| PO load 數 | `report.fanoutLoadReport.primaryOutputLoadCount` |

例子：

```verilog
dff ff1(.D(n1), .CK(clk), .RN(rst_n), .SN(rst_n), .Q(q));
```

```text
FanoutLoadReport("rst_n").dffResetSetLoads.size() == 2
```

因為 `.RN(rst_n)` 和 `.SN(rst_n)` 是兩個不同 sink pins。

---

## 8. GlobalFanoutReport

用途：

```text
掃描全設計或所有 primary inputs 的 fanout loads，找最大 fanout 與 fanout limit violations。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::GlobalFanoutReport;
query.fanoutLimit = 16;          // -1 表示只找 max，不檢查 violation
query.primaryInputsOnly = false; // true 時只掃 PI nets

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 最大 fanout | `report.globalFanoutReport.maxFanout` |
| 最大 fanout 的 nets | `report.globalFanoutReport.maxFanoutReports` |
| 是否符合 limit | `report.globalFanoutReport.satisfiesLimit` |
| 違反 limit 的 nets | `report.globalFanoutReport.violatingReports` |
| 實際檢查 net 數 | `report.globalFanoutReport.checkedNetCount` |

---

## 9. GateInputs

用途：

```text
查某個 gate 的直接 input nets。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::GateInputs;
query.gateName = "g1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| input net names | `report.netNames` |
| input net IDs | `report.netIds` |
| input 數量 | `report.count` |

---

## 10. GateOutput

用途：

```text
查某個 gate 的 output net。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::GateOutput;
query.gateName = "g1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| output net name | `report.netName` 或 `report.netNames` |
| output net ID | `report.netId` 或 `report.netIds` |

---

## 11. GateFanin

用途：

```text
查直接驅動某個 gate input nets 的上一層 gates。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::GateFanin;
query.gateName = "g1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

注意：

```text
這是 immediate fanin，不是 transitive fanin cone。
PI 或 constant input 沒有 driver gate，會被略過。
```

---

## 12. GateFanout

用途：

```text
查某個 gate output net 直接 fanout 到哪些 gates。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::GateFanout;
query.gateName = "g0";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

這可以回答：

```text
Report every gate connected to the output of g0.
```

---

## 13. DirectlyConnected

用途：

```text
判斷指定 gate 和指定 net 是否直接相連。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::DirectlyConnected;
query.gateName = "g1";
query.netName = "n2";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否直接相連 | `report.connected` |
| 查詢是否成功 | `report.ok` |

語意：

```text
若 net 是 gate 的任一 input 或 output，connected=true。
```

---

## 14. Prompt 對應表

| Prompt | 建議 Query type | 需要設定 | 主要讀取 |
|---|---|---|---|
| Which gate drives net n1? | `NetDriver` | `netName = "n1"` | `gateNames` |
| Report every gate connected to net n1. | `NetLoads` | `netName = "n1"` | `gateNames` |
| How many fanout loads does n1 have? | `FanoutLoadReport` | `netName = "n1"` | `fanoutLoadReport.totalLoadCount` |
| List DFF clock/reset loads driven by n1. | `FanoutLoadReport` | `netName = "n1"` | `dffClockLoads`, `dffResetSetLoads` |
| Does n1 directly drive a primary output? | `FanoutLoadReport` | `netName = "n1"` | `drivesPrimaryOutput` |
| Which primary input has the highest fanout? | `GlobalFanoutReport` | `primaryInputsOnly = true` | `maxFanoutReports` |
| Does every signal satisfy max fanout 16? | `GlobalFanoutReport` | `fanoutLimit = 16` | `satisfiesLimit`, `violatingReports` |
| What are the input nets of g1? | `GateInputs` | `gateName = "g1"` | `netNames` |
| What is the output net of g1? | `GateOutput` | `gateName = "g1"` | `netName` |
| Which gates feed g1? | `GateFanin` | `gateName = "g1"` | `gateNames` |
| Report every gate connected to the output of g0. | `GateFanout` | `gateName = "g0"` | `gateNames` |
| Is g1 directly connected to n2? | `DirectlyConnected` | `gateName = "g1"`, `netName = "n2"` | `connected` |

---

## 15. 何時不要用 DirectConnectivityQuery

| 問題 | 應改用 |
|---|---|
| Find all gates reachable from n2. | Cone / Fanout Query |
| Find all gates in fanin cone of y. | Cone / Fanin Query |
| Is there a path from a to y? | PathQuery |
| Find shortest path from a to y. | PathQuery |
| What is the maximum depth to output y? | DepthAnalysis |

---

## 16. 低階 Helper 對照

| Query type | 底層 helper |
|---|---|
| `NetDriver` | `getNetDriverGateIds()`, `getNetDriverGateNames()` |
| `NetLoads` | `getNetLoadGateIds()`, `getNetLoadGateNames()`, `getNetLoadGateCount()` |
| `FanoutLoadReport` | `getFanoutLoadReport()`, `getFanoutLoadCount()` |
| `GlobalFanoutReport` | `getGlobalFanoutReport()`, `satisfiesFanoutLimit()` |
| `GateInputs` | `getGateInputNetIds()`, `getGateInputNetNames()` |
| `GateOutput` | `getGateOutputNetId()`, `getGateOutputNetName()` |
| `GateFanin` | `getGateFaninGateIds()`, `getGateFaninGateNames()`, `getGateFaninGateCount()` |
| `GateFanout` | `getGateFanout()`, `getGateFanoutNames()`, `getGateFanoutCount()` |
| `DirectlyConnected` | `isGateDirectlyConnectedToNet()`, `isNetDirectlyConnectedToGate()` |

---

## 17. 目前實作與測試狀態

```text
實作檔案：src/analysis/ConnectivityAnalysis.cpp
型別檔案：include/core/NetlistQueries.h
tester：mini test/tester.cpp
目前 regression：Summary: 57 passed, 0 failed.
```
