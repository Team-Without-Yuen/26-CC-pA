# Depth Query 使用說明

這份文件只負責說明 `DepthQuery` 怎麼使用。  
內部 depth model、helper 與實作狀態請看：

```text
API_SPEC/DEPTH_QUERY_API.md
```

---

## 1. 基本概念

`DepthQuery` 用來回答 timing/depth 類問題，例如 output depth、DFF.D depth、global critical path、以及 depth 超標 endpoint。

基本形式：

```text
query.type + optional netName / threshold -> runDepthQuery() -> DepthReportSet
```

使用流程：

```text
1. 建立 Netlist::DepthQuery query
2. 設定 query.type
3. 視 query.type 設定 query.netName / query.threshold
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

---

## 3. DepthQuery 輸入欄位

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `DepthQueryType` | `SpecificNet` | 決定要執行哪一種 depth query |
| `netName` | `std::string` | `""` | `SpecificNet` 使用 |
| `threshold` | `int` | `-1` | `EndpointsExceedingDepth` 使用 |
| `includeCriticalPath` | `bool` | `true` | 是否保留 `criticalPath` |

`includeCriticalPath = false` 適合只需要數量或 endpoint metadata 的問題，可避免回傳完整 path。

---

## 4. DepthReportSet 回傳欄位

| 欄位 | 意思 |
|---|---|
| `ok` | query 是否成功 |
| `message` | debug / LLM response 用的簡短訊息 |
| `type` | query type |
| `reports` | 查詢得到的一個或多個 endpoint report |
| `worst` | `reports` 中 depth 最大的 endpoint |
| `threshold` | query 使用的 depth 門檻 |
| `count` | `reports.size()` |

`DepthReport` 主要欄位：

| 欄位 | 意思 |
|---|---|
| `endpointType` | `SpecificNet` / `PrimaryOutput` / `DffD` |
| `endpointName` | endpoint 名稱，例如 `n12`、`out[0]`、`ff1.D` |
| `endpointNetId` | 實際被分析的 net ID |
| `depth` | 到該 endpoint 的最大 combinational depth |
| `criticalPath` | 到該 endpoint 的一條 critical path |

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

若 prompt 明確問 primary outputs：

```text
How many outputs have a logic depth greater than 4?
```

可讀取 `report.reports` 後篩選：

```cpp
endpoint.endpointType == Netlist::DepthEndpointType::PrimaryOutput
```

或使用底層 helper：

```cpp
netlist.getPrimaryOutputsWithDepthGreaterThan(4).size();
```

---

## 10. Prompt 對應表

| Prompt | 建議 DepthQueryType | 需要設定 | 主要讀取 |
|---|---|---|---|
| What is the maximum logic depth to output n12? | `SpecificNet` | `netName = "n12"` | `worst.depth` |
| Compute the maximum logic depth of the fanin cone of output n3. | `SpecificNet` | `netName = "n3"` | `worst.depth` |
| Report the critical path to n10. | `SpecificNet` | `netName = "n10"`, `includeCriticalPath = true` | `worst.criticalPath` |
| What is the maximum output depth? | `PrimaryOutputs` | 無 | `worst` |
| How many outputs have a logic depth greater than 4? | `EndpointsExceedingDepth` | `threshold = 4` | filter `PrimaryOutput`, count |
| What is the maximum logic depth from any primary input to any DFF D-pin? | `DffD` | 無 | `worst.depth` |
| What is the global critical path? | `GlobalCriticalPath` | 無 | `worst`, `worst.criticalPath` |
| Find endpoints exceeding target depth 4. | `EndpointsExceedingDepth` | `threshold = 4` | `reports`, `count` |

---

## 11. 何時不要用 DepthQuery

| 問題 | 應改用 |
|---|---|
| What is the max depth from input n2 to output n12? | `PathQuery::MaxDepth` |
| Does a path from n2 to n12 exist while avoiding g0? | `PathQuery` |
| Find all gates in the fanin cone of n12. | `ConeQuery` |
| Is output n16 always 0? | `FunctionQuery::AlwaysZero` |
| Are signals n1 and n2 equivalent? | `FunctionQuery::Equivalence` |
| What type of gate is g0? | `BasicQuery::GateInfo` |

---

## 12. 測試狀態

目前文件化完成：

```text
DepthQuery / DepthReportSet / runDepthQuery()
SpecificNet
PrimaryOutputs
DffD
GlobalCriticalPath
EndpointsExceedingDepth
```

目前既有測試文件記錄：

```text
mini test/tester.cpp 已覆蓋 computeNetLevels / computeGateLevels /
findCriticalPathToNet / runDepthQuery(SpecificNet, DffD, GlobalCriticalPath)。
```

後續建議補測：

```text
runDepthQuery(PrimaryOutputs)
runDepthQuery(EndpointsExceedingDepth)
getPrimaryOutputsWithDepthGreaterThan()
```
