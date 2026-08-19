# Depth Query 使用說明

這份文件只負責說明 `DepthQuery` 怎麼使用。  
內部 depth model、helper 與實作狀態請看：

```text
API_SPEC/DEPTH_QUERY_API.md
```

---

## 1. 基本概念

`DepthQuery` 用來回答 timing/depth 類問題，例如 output depth、DFF.D depth、global critical path、depth 超標 endpoint、gate 是否在 maximum-depth path 上、以及最深 output cone。

基本形式：

```text
query.type + optional netName / gateName / threshold -> runDepthQuery() -> DepthReportSet
```

使用流程：

```text
1. 建立 Netlist::DepthQuery query
2. 設定 query.type
3. 視 query.type 設定 query.netName / query.gateName / query.threshold
4. 視需求設定 query.includeCriticalPath
5. 呼叫 netlist.runDepthQuery(query)
6. 從 report.ok / report.reports / report.worst / report.count 讀結果
```

注意：

```text
DepthQuery 只處理 timing/depth。
指定 startpoint 到 endpoint 的路徑深度，應使用 PathQuery::MaxDepth。
Boolean constant status 或 equivalence，應使用 FunctionQuery。
fanin/fanout cone 集合，應使用 ConeQuery。
```

基本範例：

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::EndpointsExceedingDepth;
query.threshold = 4;
query.includeCriticalPath = false;

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
size_t endpointCount = report.count;
```

---

## 2. DepthQueryType 總表

| Query type | 問題類型 | 需要設定 | 主要讀取欄位 |
|---|---|---|---|
| `SpecificNet` | 單一 net/output 的最大 depth | `netName` | `reports[0]`, `worst.depth`, `worst.criticalPath` |
| `PrimaryOutputs` | 所有 primary output depth | 無 | `reports`, `worst` |
| `DffD` | 所有 DFF D-pin depth | 無 | `reports`, `worst` |
| `GlobalCriticalPath` | 全設計最深 timing endpoint | 無 | `worst`, `worst.criticalPath` |
| `EndpointsExceedingDepth` | 找 depth 大於 threshold 的 endpoints | `threshold` | `reports`, `count`, `worst` |
| `PrimaryOutputsExceedingDepth` | 只找 depth 大於 threshold 的 primary outputs | `threshold` | `reports`, `count`, `worst` |
| `GateOnCriticalPath` | gate 是否位在任一 global maximum-depth path 上 | `gateName` | `exists`, `gateOnCriticalPath`, `worst` |
| `DeepestOutputCone` | 找出 fanin logic cone depth 最深的 primary output | 無 | `worst`, `reports`, `count` |

---

## 3. DepthQuery 輸入欄位

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `DepthQueryType` | `SpecificNet` | 決定要執行哪一種 depth query |
| `netName` | `std::string` | `""` | `SpecificNet` 使用 |
| `gateName` | `std::string` | `""` | `GateOnCriticalPath` 使用 |
| `threshold` | `int` | `-1` | `EndpointsExceedingDepth` / `PrimaryOutputsExceedingDepth` 使用 |
| `includeCriticalPath` | `bool` | `true` | 是否保留 `criticalPath` |

`includeCriticalPath = false` 適合只需要數量或 endpoint metadata 的問題；backend 會略過 path reconstruction，而不只是建立後再清空。

---

## 4. DepthReportSet 回傳欄位

| 欄位 | 意思 |
|---|---|
| `ok` | query 是否成功 |
| `exists` | yes/no 型 query 的主要答案；一般 list query 表示是否有結果 |
| `message` | debug / LLM response 用的簡短訊息 |
| `type` | query type |
| `reports` | 查詢得到的一個或多個 endpoint report |
| `worst` | `reports` 中 depth 最大的 endpoint |
| `threshold` | query 使用的 depth 門檻 |
| `count` | `reports.size()` |
| `gateName` | `GateOnCriticalPath` 的查詢目標 |
| `gateId` | `GateOnCriticalPath` 的 gate ID |
| `gateOnCriticalPath` | gate 是否在任一 global maximum-depth path 上 |

`DepthReport` 主要欄位：

| 欄位 | 意思 |
|---|---|
| `endpointType` | `SpecificNet` / `PrimaryOutput` / `DffD` |
| `endpointName` | endpoint 名稱，例如 `n12`、`out[0]`、`ff1.D` |
| `endpointNetId` | 實際被分析的 net ID |
| `depth` | 到該 endpoint 的最大 combinational depth |
| `criticalPath` | 到該 endpoint 的一條 critical path |

公開 CLI 的大型 `reports` 或 `worst.criticalPath` 不會全部塞進 terminal。當 endpoint、net 與
gate records 的總量超過內部門檻時，`tools.cpp` 自動輸出 `QUERY_LIST_ARTIFACT_V1`，並在
response 提供 `list artifact complete`、`list entry count` 與 `output_file`。這個門檻只決定
呈現方式，不限制 `DepthReportSet` 的完整計算；LLM 不控制檔名或 sample 數。

---

## 5. SpecificNet

用途：

```text
分析單一 net 或 output 的最大 fanin depth。
```

寫法：

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::SpecificNet;
query.netName = "n12";

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 最大 depth | `report.worst.depth` |
| endpoint 名稱 | `report.worst.endpointName` |
| critical path | `report.worst.criticalPath` |

---

## 6. PrimaryOutputs

用途：

```text
分析所有 primary outputs 的 depth。
```

寫法：

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::PrimaryOutputs;
query.includeCriticalPath = false;

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 所有 output depth | `report.reports` |
| 最深 output | `report.worst` |
| output 數量 | `report.count` |

---

## 7. DffD

用途：

```text
分析所有 DFF D-pin 的 combinational depth。
```

寫法：

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::DffD;

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 所有 DFF.D depth | `report.reports` |
| 最深 DFF.D endpoint | `report.worst` |

---

## 8. GlobalCriticalPath

用途：

```text
找出所有 primary output 與 DFF.D endpoints 中最深的一條 timing path。
```

寫法：

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::GlobalCriticalPath;

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| critical depth | `report.worst.depth` |
| critical endpoint | `report.worst.endpointName` |
| critical path | `report.worst.criticalPath` |

---

## 9. EndpointsExceedingDepth

用途：

```text
找出 depth 大於指定 threshold 的 timing endpoints。
```

寫法：

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::EndpointsExceedingDepth;
query.threshold = 4;
query.includeCriticalPath = false;

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 超標 endpoint 數量 | `report.count` |
| 超標 endpoints | `report.reports` |
| 最深超標 endpoint | `report.worst` |

若 prompt 明確問 primary outputs，直接使用 PO-only mode：

```text
How many outputs have a logic depth greater than 4?
```

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::PrimaryOutputsExceedingDepth;
query.threshold = 4;
query.includeCriticalPath = false;
Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

此時 `report.count` 可直接作為 output bit 數量，不會混入 DFF.D。

## 10. GateOnCriticalPath

用途：

```text
判斷指定 gate 是否位在任一 global maximum-depth path 上。
```

寫法：

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::GateOnCriticalPath;
query.gateName = "g0";

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| g0 是否在任一 maximum-depth path 上 | `report.gateOnCriticalPath` 或 `report.exists` |
| 全域 critical depth | `report.worst.depth` |
| 全域 critical endpoint | `report.worst.endpointName` |

注意：

```text
這個 query 不是只檢查 findGlobalCriticalPath() 回傳的一條 path。
它會判斷 gate 是否可能出現在任一條 global maximum-depth path 上。
```

---

## 11. DeepestOutputCone

用途：

```text
找出 fanin logic cone depth 最深的 primary output。
```

寫法：

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::DeepestOutputCone;

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 最深 output 名稱 | `report.worst.endpointName` |
| 最深 output depth | `report.worst.depth` |
| 所有 output depth | `report.reports` |

---

## 12. Prompt 對應表

| Prompt | 建議 DepthQueryType | 需要設定 | 主要讀取 |
|---|---|---|---|
| What is the maximum logic depth to output n12? | `SpecificNet` | `netName = "n12"` | `worst.depth` |
| Compute the maximum logic depth of the fanin cone of output n3. | `SpecificNet` | `netName = "n3"` | `worst.depth` |
| Report the critical path to n10. | `SpecificNet` | `netName = "n10"`, `includeCriticalPath = true` | `worst.criticalPath` |
| What is the maximum output depth? | `PrimaryOutputs` | 無 | `worst` |
| How many outputs have a logic depth greater than 4? | `PrimaryOutputsExceedingDepth` | `threshold = 4` | `count` |
| What is the maximum logic depth from any primary input to any DFF D-pin? | `PathQuery::MaxDepth` | `all_pi -> all_dff_d` | Path Query 的 maximum depth |
| What is the global critical path? | `GlobalCriticalPath` | 無 | `worst`, `worst.criticalPath` |
| Find endpoints exceeding target depth 4. | `EndpointsExceedingDepth` | `threshold = 4` | `reports`, `count` |
| Determine whether gate g0 lies on any maximum-depth path of the design. | `GateOnCriticalPath` | `gateName = "g0"` | `gateOnCriticalPath`, `exists` |
| Which output bit has the deepest fanin logic cone? | `DeepestOutputCone` | 無 | `worst.endpointName`, `worst.depth` |

---

## 13. 何時不要用 DepthQuery

| 問題 | 應改用 |
|---|---|
| What is the max depth from input n2 to output n12? | `PathQuery::MaxDepth` |
| Does a path from n2 to n12 exist while avoiding g0? | `PathQuery` |
| Find all gates in the fanin cone of n12. | `ConeQuery` |
| Is output n16 always 0? | `FunctionQuery::AlwaysZero` |
| Are signals n1 and n2 equivalent? | `FunctionQuery::Equivalence` |
| What type of gate is g0? | `BasicQuery::GateInfo` |

---

## 14. 測試狀態

目前文件化完成：

```text
DepthQuery / DepthReportSet / runDepthQuery()
SpecificNet
PrimaryOutputs
DffD
GlobalCriticalPath
EndpointsExceedingDepth
GateOnCriticalPath
DeepestOutputCone
```

目前既有測試文件記錄：

```text
mini test/tester.cpp 已覆蓋 computeNetLevels / computeGateLevels /
findCriticalPathToNet / runDepthQuery(SpecificNet, DffD, GlobalCriticalPath)。
mini test/test5/test5.cpp 已覆蓋 GateOnCriticalPath / DeepestOutputCone、
includeCriticalPath=false、tombstone/stale edge 與 30000-level deep-chain regression。
```

`EndpointsExceedingDepth` 與 `PrimaryOutputsExceedingDepth` 也已納入
`includeCriticalPath=false` matrix regression；官方 testcase 另由 tools 層持續驗證。
