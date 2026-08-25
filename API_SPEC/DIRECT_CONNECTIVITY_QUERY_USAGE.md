# Direct Connectivity Query 使用說明

> 對外請使用 `structure_query <mode> [args]`。本文件中的 `DirectConnectivityQuery` 是 `structure_query` 內部 dispatch 的 C++ API；舊 `conn_query` CLI 僅保留相容性。

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

公開 CLI grammar 必須嚴格遵守：

```text
structure_query net_driver <net> [--with-pins]
structure_query net_loads <net> [--with-pins]
structure_query fanout_load <net>
structure_query fanout_report <net>
structure_query global_fanout [non_negative_limit]
structure_query pi_fanout [non_negative_limit]
structure_query fanout_violations <non_negative_limit>
structure_query fanout_filter <all|pi> <eq|ne|gt|ge|lt|le|between> <value> [upper]
structure_query fanout_rank <scope> <highest|lowest|nth_highest|nth_lowest|top|bottom> [k]
structure_query gate_inputs <gate>
structure_query gate_output <gate>
structure_query gate_fanin <gate>
structure_query gate_fanout <gate>
structure_query is_connected <gate> <net-or-bus>
```

名稱數量必須完全相符；fanout limit 只接受 `0..INT_MAX` 整數。缺值、未知 option、
非法數字或 trailing token 都回 `status:error, complete:false`，不會用預設欄位繼續查詢。
`fanout_filter` 的 value/upper 則使用 `size_t` 非負十進位整數，不套 `INT_MAX` 人工上限。
這是 CLI adapter 契約；直接建立 C++ `DirectConnectivityQuery` 時，必要欄位仍由 backend 的
`report.ok/message` 驗證。

---

## 2. Query Type 總表

| Query type | 問題類型 | 需要設定 | 主要讀取欄位 |
|---|---|---|---|
| `NetDriverGates` | 誰 drive 這條 net / bus；可選 output-pin detail | `netName`, optional `includePinDetails` | `gateIds`, `gateNames`, `count`, `pinConnections` |
| `NetLoadGates` | 這條 net / bus load 到哪些 gates；可選逐 input-pin detail | `netName`, optional `includePinDetails` | `gateIds`, `gateNames`, `count`, `pinConnections` |
| `FanoutLoadReport` | 依 Problem A QA 定義統計 pin-level fanout loads | `netName` | `fanoutLoadReport`, `count` |
| `GlobalFanoutReport` | 全設計或 PI-only fanout max / violations / predicate matches | `fanoutLimit`, `primaryInputsOnly`, optional predicate/bounds | `globalFanoutReport` |
| `FanoutRankingReport` | scoped distinct-level fanout ranking | `fanoutScope`, `fanoutRankMode`, optional positive rank/count | `fanoutRankingReport`, `count` |
| `GateInputs` | 這顆 gate 的 input nets | `gateName` | `netIds`, `netNames`, `count` |
| `GateOutput` | 這顆 gate 的 output net | `gateName` | `netId`, `netName`, `netIds`, `netNames` |
| `GateFanin` | 哪些 gates 直接餵進這顆 gate | `gateName` | `gateIds`, `gateNames`, `count` |
| `GateFanout` | 這顆 gate output 直接接到哪些 gates | `gateName` | `gateIds`, `gateNames`, `count` |
| `DirectlyConnected` | gate 和 scalar net / bus 是否直接相連 | `gateName`, `netName` | `connected`, `count`, `netIds`, `netNames` |

---

## 3. DirectConnectivityQuery 輸入欄位

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `DirectConnectivityQueryType` | `NetDriverGates` | 決定查詢類型 |
| `gateName` | `std::string` | `""` | gate 相關 query 使用 |
| `netName` | `std::string` | `""` | net 相關 query 使用 |
| `fanoutLimit` | `int` | `-1` | `GlobalFanoutReport` 的 violation threshold；-1 表示未指定 |
| `primaryInputsOnly` | `bool` | `false` | `GlobalFanoutReport` 是否只掃 PI bit nets |
| `includeZeroFanout` | `bool` | `false` | 是否在完整 `netReports` 明細保留 0-fanout nets；不影響 extrema candidates |
| `fanoutPredicate` | `FanoutPredicate` | `None` | optional fanout 比較條件；不取代 limit/extrema |
| `fanoutValue` | `size_t` | `0` | 單值條件，或 `BetweenInclusive` 下界 |
| `fanoutUpperValue` | `size_t` | `0` | `BetweenInclusive` 的 inclusive 上界 |
| `fanoutScope` | `FanoutScope` | `All` | ranking candidate scope |
| `fanoutRankMode` | `FanoutRankMode` | `Highest` | ranking 方向與 single/multi-level 選取方式 |
| `fanoutRankValue` | `size_t` | `1` | nth rank 或 top/bottom distinct-level count；必須大於 0 |
| `includeIds` | `bool` | `true` | 是否填 `gateIds` / `netIds` |
| `includeNames` | `bool` | `true` | 是否填 `gateNames` / `netNames` |
| `includePinDetails` | `bool` | `false` | `NetDriverGates` / `NetLoadGates` 是否填逐 pin records |

注意：

```text
includeIds / includeNames 只控制 payload 是否填入，不改變 count。
例如 GateInputs 即使 includeIds=false 且 includeNames=false，count 仍是完整 input net 數。
```

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
| `count` | 結果數量；`DirectlyConnected` 時為實際相連的 active scalar/bit nets 數量 |
| `gateIds` | 查詢結果中的 gate IDs |
| `netIds` | 查詢結果中的 net IDs |
| `gateNames` | 查詢結果中的 gate names |
| `netNames` | 查詢結果中的 net names |
| `pinDetailsIncluded` | 本次是否要求 pin detail；即使結果為空仍為 true |
| `pinConnectionCount` | 實際 driver/load pin edge 數，不等同去重 gate count |
| `pinConnections` | 完整 `ConnectivityPinRecord` records |
| `fanoutLoadReport` | `FanoutLoadReport` 查詢的分類結果 |
| `globalFanoutReport` | `GlobalFanoutReport` 查詢的全域彙整結果 |
| `fanoutRankingReport` | `FanoutRankingReport` 的 scope/rank/result 與 compact ranked entries |

通用錯誤與 tombstone 語意：

```text
名稱缺失或 active object 不存在時，ok=false、exists=false。
removed gate（GateType::UNKNOWN）視為 Gate not found。
removed net（isRemoved=true）視為 Net not found。
bus query 只有至少一個 active bit net 時才算存在；全部 bit 都 removed 時視為 Net not found。
```

連線一致性語意：

```text
driver edge 必須同時滿足 net.driverGateId == gateId 與 gate.outputNetId == netId。
load edge 必須同時滿足 net.loadGateIds 含 gateId，且 gate.inputNetIds 實際含 netId。
若 edit 後留下 stale cached edge，DirectConnectivityQuery 不會回報該 edge。
GateOutput 找不到有效且一致的 output edge 時，gate 本身仍存在，因此 ok=true、exists=true，
但 count=0、netId=-1，且不會填入 output payload。
```

---

## 5. NetDriverGates

用途：

```text
查某個 net / bus 的直接 driver gate。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::NetDriverGates;
query.netName = "n1";
query.includePinDetails = true;

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| driver gate names | `report.gateNames` |
| driver gate IDs | `report.gateIds` |
| driver 數量 | `report.count` |
| driver output pin records | `report.pinConnections` |

語意：

```text
removed scalar net 回 ok=false。
bus 會展開 active bit nets，忽略 removed bits。
```

---

## 6. NetLoadGates

用途：

```text
查某個 net / bus 直接 load 到哪些 gates。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::NetLoadGates;
query.netName = "n1";
query.includePinDetails = true;

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| load gate names | `report.gateNames` |
| load gate IDs | `report.gateIds` |
| load 數量 | `report.count` |
| load pin 數量 | `report.pinConnectionCount` |
| gate type、pin name 與 DFF role | `report.pinConnections` |

注意：

```text
NetLoadGates 預設只回答「這條 net 接到哪些 gate/DFF instance」。設定 `includePinDetails=true` 後，
會區分 combinational input 與 DFF.D / DFF.CK / DFF.RN / DFF.SN。
它不會把 primary output connection 算成 gate pin load。
若題目問 fanout load count，應使用 FanoutLoadReport。
同一顆 gate 的多個 input pin 接同一條 net 時，`count` 仍只計一次 gate，但 `pinConnections` 會逐 pin
保留，因此 `pinConnectionCount` 可能大於 `count`。
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
| primitive gate input load 數 | `report.fanoutLoadReport.combinationalGateLoadCount` |
| DFF D-pin loads | `report.fanoutLoadReport.dffDataLoads` |
| DFF D-pin load 數 | `report.fanoutLoadReport.dffDataLoadCount` |
| DFF clock loads | `report.fanoutLoadReport.dffClockLoads` |
| DFF clock load 數 | `report.fanoutLoadReport.dffClockLoadCount` |
| DFF reset/set loads | `report.fanoutLoadReport.dffResetSetLoads` |
| DFF reset/set load 數 | `report.fanoutLoadReport.dffResetSetLoadCount` |
| DFF other-pin load 數 | `report.fanoutLoadReport.dffOtherLoadCount` |
| 所有不重複的直接 load gates | `report.fanoutLoadReport.distinctGateLoadIds` |
| 不重複的直接 load gate 數 | `report.fanoutLoadReport.distinctGateLoadCount` |
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

同樣地，若一般 gate 兩個 input pins 都接同一條 net，FanoutLoadReport 會計成兩個
pin-level loads；這和 NetLoadGates 的 gate-level 去重不同。

若同一個 prompt 同時問「fanout 是多少」與「直接驅動哪些 gates」，只需一次
`FanoutLoadReport`：前者讀 `totalLoadCount`，後者讀 `distinctGateLoadCount` 與
`distinctGateLoadIds`。不需要再組合第二個 `NetLoadGates` query。

五個 category count 永遠保留在 public report。即使完整名稱清單因 4096-token policy 寫入
artifact，呼叫端仍可直接回答各類 load 數量，不需要讀取 artifact 重新計數。

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
| 實際參與 extrema/limit 的 active candidate 數 | `report.globalFanoutReport.checkedNetCount` |
| predicate 命中數 | `report.globalFanoutReport.matchedNetCount` |
| predicate 命中 nets 與各自 fanout | `report.globalFanoutReport.matchedReports` |

所有 scope candidates 都會參與 maximum 計算，即使 `includeZeroFanout=false`。因此當所有 PI
fanout 都是 0 時，`maxFanout=0`，且 `maxFanoutReports` 仍完整保留所有並列的 PI；
`includeZeroFanout` 只決定這些零值是否也出現在完整 `netReports` 明細。

Predicate 範例：

```cpp
query.primaryInputsOnly = true;
query.fanoutPredicate = Netlist::FanoutPredicate::BetweenInclusive;
query.fanoutValue = 4;
query.fanoutUpperValue = 8;
```

`matchedNetCount` 是 filter matches；外層 `DirectConnectivityReport.count` 仍是完整 scope 的
`checkedNetCount`。`between 4 8` 包含 4 與 8。若無命中，query 仍成功且 matched count 為 0。

---

## 9. FanoutRankingReport

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::FanoutRankingReport;
query.fanoutScope = Netlist::FanoutScope::PrimaryInputs;
query.fanoutRankMode = Netlist::FanoutRankMode::NthHighest;
query.fanoutRankValue = 2;

const auto report = netlist.runDirectConnectivityQuery(query);
```

主要欄位：`checkedNetCount`、`distinctFanoutLevelCount`、`selectedFanoutLevelCount`、
`resultNetCount`、`requestedRankExists` 與 `rankedReports`。每個 `FanoutRankEntry` 含
`netId/netName/fanout/rank`。rank 是 distinct fanout value 的名次；ties 全部保留並穩定排序。
`Top/Bottom K` 回前 K 個 levels，因此結果 net 數可大於 K。若要求的第 K level 不存在，
single-rank mode 回成功空清單；top/bottom 回現有 levels 並令 `requestedRankExists=false`。

scope：`All` 包含所有 active nets；`Internal` 排除 PI/PO/constant；三種 output scope 要求有效且
一致的 active gate driver，再依任意 gate、非 DFF gate或 DFF 分類。

---

## 10. GateInputs

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

## 11. GateOutput

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

## 12. GateFanin

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

## 13. GateFanout

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

## 14. DirectlyConnected

用途：

```text
判斷指定 gate 和指定 scalar net，或 bus 的任一 active bit，是否直接相連。
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
| 實際相連的 bit 數量 | `report.count` |
| 實際相連的 scalar/bit nets | `report.netIds`, `report.netNames` |
| 查詢是否成功 | `report.ok` |

語意：

```text
scalar net 是 gate 的任一 input 或 output 時，connected=true、count=1。
bus base name 會依 declaration order 展開所有 active bits；`connected` 表示至少一個 bit 相連，
`count/netIds/netNames` 只包含實際相連的 bits。bus 存在但沒有相連 bit 是成功的零結果；
只有 scalar 與所有 bus bits 都不存在時才是 `ok=false`。
```

---

## 15. Prompt 對應表

| Prompt | 建議 Query type | 需要設定 | 主要讀取 |
|---|---|---|---|
| Which gate drives net n1? | `NetDriverGates` | `netName = "n1"` | `gateNames` |
| Report every gate connected to net n1. | `NetLoadGates` | `netName = "n1"` | `gateNames` |
| How many fanout loads does n1 have? | `FanoutLoadReport` | `netName = "n1"` | `fanoutLoadReport.totalLoadCount` |
| What is the fanout of n1? List every gate it drives directly. | `FanoutLoadReport` | `netName = "n1"` | `totalLoadCount`, `distinctGateLoadCount`, `distinctGateLoadIds` |
| List DFF clock/reset loads driven by n1. | `FanoutLoadReport` | `netName = "n1"` | `dffClockLoads`, `dffResetSetLoads` |
| Does n1 directly drive a primary output? | `FanoutLoadReport` | `netName = "n1"` | `drivesPrimaryOutput` |
| Which primary input has the highest fanout? | `GlobalFanoutReport` | `primaryInputsOnly = true` | `maxFanoutReports` |
| Does every signal satisfy max fanout 16? | `GlobalFanoutReport` | `fanoutLimit = 16` | `satisfiesLimit`, `violatingReports` |
| How many inputs have fanout exactly 4? | `GlobalFanoutReport` | PI scope + `Equal`, value 4 | `matchedNetCount`, `matchedReports` |
| List signals with fanout from 4 through 8. | `GlobalFanoutReport` | all scope + inclusive range 4..8 | `matchedNetCount`, `matchedReports` |
| Which signals have the second-highest fanout? | `FanoutRankingReport` | all + `NthHighest`, value 2 | `requestedRankExists`, `rankedReports` |
| List the bottom 3 internal fanout levels. | `FanoutRankingReport` | internal + `Bottom`, value 3 | `selectedFanoutLevelCount`, `rankedReports` |
| What are the input nets of g1? | `GateInputs` | `gateName = "g1"` | `netNames` |
| What is the output net of g1? | `GateOutput` | `gateName = "g1"` | `netName` |
| Which gates feed g1? | `GateFanin` | `gateName = "g1"` | `gateNames` |
| Report every gate connected to the output of g0. | `GateFanout` | `gateName = "g0"` | `gateNames` |
| Is g1 directly connected to n2? | `DirectlyConnected` | `gateName = "g1"`, `netName = "n2"` | `connected` |
| Is g1 connected to any bit of bus data? | `DirectlyConnected` | `gateName = "g1"`, `netName = "data"` | `connected`, `count`, `netNames` |

---

## 16. 何時不要用 DirectConnectivityQuery

| 問題 | 應改用 |
|---|---|
| Find all gates reachable from n2. | Cone / Fanout Query |
| Find all gates in fanin cone of y. | Cone / Fanin Query |
| Is there a path from a to y? | PathQuery |
| Find shortest path from a to y. | PathQuery |
| What is the maximum depth to output y? | DepthAnalysis |

---

## 17. 低階 Helper 對照

| Query type | 底層 helper |
|---|---|
| `NetDriverGates` | `getNetDriverGateIds()`, `getNetDriverGateNames()` |
| `NetLoadGates` | `getNetLoadGateIds()`, `getNetLoadGateNames()`, `getNetLoadGateCount()` |
| `FanoutLoadReport` | `getFanoutLoadReport()`, `getFanoutLoadCount()` |
| `GlobalFanoutReport` | `getGlobalFanoutReport()`, `satisfiesFanoutLimit()` |
| `FanoutRankingReport` | `getFanoutRankingReport()` |
| `GateInputs` | `getGateInputNetIds()`, `getGateInputNetNames()` |
| `GateOutput` | `getGateOutputNetId()`, `getGateOutputNetName()` |
| `GateFanin` | `getGateFaninGateIds()`, `getGateFaninGateNames()`, `getGateFaninGateCount()` |
| `GateFanout` | `getGateFanout()`, `getGateFanoutNames()`, `getGateFanoutCount()` |
| `DirectlyConnected` | `isGateDirectlyConnectedToNet()`, `isNetDirectlyConnectedToGate()` |

---

## 18. 目前實作與測試狀態

```text
實作檔案：src/analysis/ConnectivityAnalysis.cpp
型別檔案：include/core/NetlistQueries.h
tester：mini test/tester.cpp
目前 regression：Summary: 57 passed, 0 failed.
fanout ranking：test55 focused matrix 與 test47 official large artifact regression 已建立，待手動編譯驗證。
```
