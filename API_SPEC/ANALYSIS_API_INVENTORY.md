# Analysis API 整理與功能劃分

這份文件整理目前 `Netlist` 裡已經建立的分析 API，目標是：

```text
1. 明確劃分每一類高階 API 的責任。
2. 避免高階 API 之間功能重複。
3. 建立 prompt routing 規則，讓 LLM 知道不同問題該呼叫哪一層 API。
4. 保留低階 helper，但讓 LLM-facing 使用盡量集中到高階 Query API。
```

目前原則：

```text
高階 API 之間不互相搶責任。
低階 helper 可以重疊或被多個高階 API 使用。
若一個需求只是「批次套用既有高階 API」，不要急著新增新高階類別。
```

---

## 1. 目前高階 API 總表

| 高階 API | 負責問題 | 不負責問題 |
|---|---|---|
| `BasicQuery` | gate/net/port 的基本資訊、數量、type、PI/PO/DFF、structural issue | 連線、cone、path、depth、function |
| `DirectConnectivityQuery` | 直接相連：net driver/load、gate input/output、immediate fanin/fanout | transitive cone、多層路徑、timing depth |
| `FunctionQuery` | Boolean function：等價、可能為 0/1、always 0/1、truth status | reachable、path、depth、critical path |
| `ConeQuery` | transitive fanin/fanout cone 內有哪些 nets/gates | 指定 A 到 B 的 path、global critical path、function equivalence |
| `PathQuery` | 指定 startpoint 到 endpoint 的路徑 exists/find/enumerate/min/max/through/avoid | 全設計 timing endpoint 掃描、功能常數判斷 |
| `DepthQuery / DepthAnalysis` | logic level、endpoint depth、critical path、PO/DFF.D depth、depth 超標 endpoint | Boolean truth status、一般路徑條件查詢 |
| `OptimizationCandidate` | 根據 depth report 建立最佳化候選與 target gate 偵測 | 純分析總入口、實際 rewrite |

目前 source ownership：

| 高階 API | 主要實作檔案 |
|---|---|
| `BasicQuery` | `src/analysis/BasicAnalysis.cpp` |
| `DirectConnectivityQuery` | `src/analysis/ConnectivityAnalysis.cpp` |
| `FunctionQuery` | `src/analysis/FunctionAnalysis.cpp` |
| `ConeQuery` | `src/analysis/ConeAnalysis.cpp` |
| `PathQuery` | `src/analysis/PathAnalysis.cpp` |
| `DepthQuery / DepthAnalysis` | `src/analysis/DepthAnalysis.cpp` |
| `OptimizationCandidate` | `src/analysis/OptimizationAnalysis.cpp` |

目前共用 query/report 型別位置：

| Header | 內容 |
|---|---|
| `include/core/NetlistQueries.h` | Basic / DirectConnectivity / Function / Cone query/report |
| `include/core/PathTypes.h` | Path / Depth query/report 與 endpoint/path 型別 |
| `include/core/OptimizationTypes.h` | Optimization result / candidate 型別 |

注意：

```text
目前不要新增 EndpointQuery。
Endpoint 類問題若是 depth/timing，歸 DepthAnalysis。
Endpoint 類問題若是 function/truth status，歸 FunctionQuery。
Endpoint 類問題若是 start-to-end path，歸 PathQuery。
```

---

## 2. API 分層圖

目前分析層可以理解成：

```text
Low-level storage / lookup
    |
    +-- Basic helper
    +-- Direct connectivity helper
    +-- Cone helper
    +-- Path helper
    +-- Depth helper
    +-- Function/SAT helper
    |
High-level Query API
    |
    +-- BasicQuery
    +-- DirectConnectivityQuery
    +-- FunctionQuery
    +-- ConeQuery
    +-- PathQuery
    +-- DepthQuery / DepthAnalysis
    |
Optimization-facing analysis
    |
    +-- OptimizationCandidate
```

目前還沒有總入口 `AnalysisQuery`。短期不建議先做總入口，因為各高階 API 邊界還在逐步穩定。

---

## 3. BasicQuery

定位：

```text
只回答 netlist 物件本身的基本資訊。
```

高階入口：

```cpp
BasicReport runBasicQuery(const BasicQuery& query) const;
```

典型問題：

```text
How many gates are in the design?
List all DFFs.
Count all NAND gates.
Is gate g1 a DFF?
Is net n1 a primary input?
Find gates with constant input.
Find floating nets.
```

應使用 `BasicQuery` 的情況：

| 問題類型 | Query type |
|---|---|
| 設計規模 | `Summary` |
| 列 gate/net/PI/PO/DFF | `ListGates`, `ListNets`, `ListPrimaryInputs`, `ListPrimaryOutputs`, `ListDffs` |
| 查單一物件資訊 | `GateInfo`, `NetInfo`, `PortInfo` |
| gate type 統計 | `CountByGateType`, `GatesByType` |
| 直接接 constant input 的 gates | `GatesWithConstantInput` |
| 結構問題 | `StructuralIssues` |

邊界：

```text
BasicQuery 不看多層圖 traversal。
BasicQuery 不回答「誰連到誰」，除非只是物件屬性。
BasicQuery 的 GatesWithConstantInput 是結構查詢，不是功能常數判斷。
```

文件：

```text
API_SPEC/BASIC_QUERY_API.md
API_SPEC/BASIC_QUERY_USAGE.md
```

---

## 4. DirectConnectivityQuery

定位：

```text
只回答一層 immediate connectivity。
```

高階入口：

```cpp
DirectConnectivityReport runDirectConnectivityQuery(
    const DirectConnectivityQuery& query) const;
```

典型問題：

```text
Which gate directly drives net n1?
Report every gate connected to the output of g0.
What are gate g1's input nets?
Which gates directly load net n2?
Is gate g1 directly connected to net n3?
```

應使用 `DirectConnectivityQuery` 的情況：

| 問題類型 | Query type |
|---|---|
| net 的直接 driver | `NetDriverGates` |
| net 的直接 loads | `NetLoadGates` |
| gate 的 input nets | `GateInputs` |
| gate 的 output net | `GateOutput` |
| gate 的 immediate fanin gates | `GateFanin` |
| gate 的 immediate fanout gates | `GateFanout` |
| gate/net 是否直接相連 | `DirectlyConnected` |

邊界：

```text
DirectConnectivityQuery 只查一層。
如果問題出現 reachable、fanin cone、fanout cone、all downstream，改用 ConeQuery。
如果問題指定 A 到 B 是否有路徑，改用 PathQuery。
```

文件：

```text
API_SPEC/DIRECT_CONNECTIVITY_QUERY_API.md
API_SPEC/DIRECT_CONNECTIVITY_QUERY_USAGE.md
```

---

## 5. FunctionQuery

定位：

```text
回答 Boolean function 性質，而不是拓樸連線。
```

高階入口：

```cpp
FunctionReport runFunctionQuery(const FunctionQuery& query) const;
```

典型問題：

```text
Is output n16 always 0 regardless of all inputs?
Is n16 always 1?
Can n16 ever become 1?
Are y and z functionally equivalent?
Classify whether n16 is always 0, always 1, or non-constant.
```

應使用 `FunctionQuery` 的情況：

| 問題類型 | Query type |
|---|---|
| 兩個 net/bus 是否等價 | `Equivalence` |
| net 是否可能為某值 | `CanBeValue` |
| net 是否恆等於某常數 | `ConstantFunction` |
| net 是否 always 0 | `AlwaysZero` |
| net 是否 always 1 | `AlwaysOne` |
| net truth status 分類 | `TruthStatus` |

邊界：

```text
FunctionQuery 不回答 path/reachable/depth。
output constant status 已經屬於 FunctionQuery，不要再另做 EndpointQuery。
若要對所有 primary outputs 做 truth status，只是 batch 套用 FunctionQuery。
```

容易混淆的點：

| 問題 | 正確 API |
|---|---|
| gate input 是否直接接 `1'b0` | `BasicQuery::GatesWithConstantInput` |
| net 是否功能上永遠為 0 | `FunctionQuery::AlwaysZero` |
| y 和 z 是否同一條線直接相連 | `DirectConnectivityQuery` |
| y 和 z 是否功能等價 | `FunctionQuery::Equivalence` |

文件：

```text
API_SPEC/FUNCTION_ANALYSIS_API.md
API_SPEC/FUNCTION_ANALYSIS_USAGE.md
```

---

## 6. ConeQuery

定位：

```text
回答 transitive fanin/fanout cone 中有哪些 nets/gates。
```

高階入口：

```cpp
ConeReport runConeQuery(const ConeQuery& query) const;
```

典型問題：

```text
Determine all gates reachable from n2.
Find all gates in the fanin cone of y.
List gates that can affect output y.
Count gates reachable from g0.
Find the fanout cone of gate g1.
```

應使用 `ConeQuery` 的情況：

| 問題類型 | Query type |
|---|---|
| net transitive fanin | `NetTransitiveFanin` |
| net transitive fanout | `NetTransitiveFanout` |
| gate output transitive fanin | `GateTransitiveFanin` |
| gate output transitive fanout | `GateTransitiveFanout` |

邊界：

```text
ConeQuery 回答集合，不回答指定 A 到 B 的 path。
ConeQuery 的 includeLocalPaths 只能作為 cone 內 local summary。
真正的 critical path 屬於 DepthAnalysis。
指定 startpoint-to-endpoint path 屬於 PathQuery。
```

容易混淆的點：

| 問題 | 正確 API |
|---|---|
| net n2 直接 load 到哪些 gate | `DirectConnectivityQuery::NetLoadGates` |
| 從 net n2 可到達哪些 gate | `ConeQuery::NetTransitiveFanout` |
| a 到 y 是否有路徑 | `PathQuery::Exists` |
| y 的 fanin cone 內有哪些 gate | `ConeQuery::NetTransitiveFanin` |

文件：

```text
API_SPEC/CONE_QUERY_API.md
API_SPEC/CONE_QUERY_USAGE.md
```

---

## 7. PathQuery

定位：

```text
回答指定 startpoint 到 endpoint 的路徑問題。
```

高階入口：

```cpp
PathQueryResult runPathQuery(const PathQuery& query) const;
```

典型問題：

```text
Is there a path from A to D?
Find one path from A to D avoiding C.
Find all paths from A to D through B.
Does every path from A to D pass through B?
Find the shortest path from A to D.
Find the longest path from any PI to DFF D-pin.
```

應使用 `PathQuery` 的情況：

| 問題類型 | Query mode |
|---|---|
| 是否存在路徑 | `Exists` |
| 找一條路徑 | `FindAny` |
| 列出所有路徑 | `EnumerateAll` |
| 最短路徑 | `MinDepth` |
| 最長路徑 | `MaxDepth` |
| 所有路徑是否經過某些節點 | `EveryPathThrough` |
| 所有路徑是否避開某些節點 | `EveryPathAvoids` |

邊界：

```text
PathQuery 必須有 startpoint/endpoints。
PathQuery 的 MaxDepth 是指定 startpoint-to-endpoint 範圍內的最長路徑。
全設計 worst endpoint / critical path 屬於 DepthAnalysis。
```

容易混淆的點：

| 問題 | 正確 API |
|---|---|
| 從 a 到 y 是否有路徑 | `PathQuery::Exists` |
| 從 n2 可到達哪些 gates | `ConeQuery` |
| 最大 critical path endpoint 是誰 | `DepthAnalysis::findGlobalCriticalPath()` |
| 從所有 PI 到某個 DFF.D 的最大路徑 | `PathQuery::MaxDepth` 或現有特定 helper |

文件：

```text
API_SPEC/STARTPOINT_ENDPOINT_PATH_ANALYSIS.md
API_SPEC/STARTPOINT_ENDPOINT_PATH_QUERY_USAGE.md
```

---

## 8. DepthQuery / DepthAnalysis

定位：

```text
回答 timing/depth/critical path 類問題。
```

目前已提供統一高階入口：

```cpp
DepthReportSet runDepthQuery(const DepthQuery& query) const;
```

基本使用流程：

```text
DepthQuery.type + optional netName / threshold / includeCriticalPath
  -> runDepthQuery()
  -> DepthReportSet
```

也保留一組 dedicated helper：

```cpp
computeNetLevels()
computeGateLevels()
getMaxDepthToNet(...)
findCriticalPathToNet(...)
analyzeDepthToNet(...)
analyzePrimaryOutputDepths()
getPrimaryOutputsWithDepthGreaterThan(...)
analyzeDffDDepths()
getDffsWithDDepthGreaterThan(...)
findGlobalCriticalPath()
findEndpointsExceedingDepth(...)
runDepthQuery(...)
getMaximumLogicDepthFromPiToDffD()
```

`DepthQueryType` 總表：

| Query type | 問題類型 | 需要設定 | 主要讀取欄位 |
|---|---|---|---|
| `SpecificNet` | 單一 net/output 的最大 depth | `netName` | `reports[0]`, `worst.depth`, `worst.criticalPath` |
| `PrimaryOutputs` | 所有 primary output depth | 無 | `reports`, `worst`, `count` |
| `DffD` | 所有 DFF D-pin depth | 無 | `reports`, `worst` |
| `GlobalCriticalPath` | 全設計最深 timing endpoint | 無 | `worst`, `worst.criticalPath` |
| `EndpointsExceedingDepth` | 找 depth 大於 threshold 的 endpoints | `threshold` | `reports`, `count`, `worst` |

`DepthQuery` 輸入欄位：

| 欄位 | 用途 |
|---|---|
| `type` | 決定執行哪一種 depth query |
| `netName` | `SpecificNet` 使用，指定要分析的 net/output |
| `threshold` | `EndpointsExceedingDepth` 使用，指定 depth 門檻 |
| `includeCriticalPath` | 是否回傳完整 critical path；只問數量時可設為 `false` |

`DepthReportSet` 回傳欄位：

| 欄位 | 意思 |
|---|---|
| `ok` | query 是否成功 |
| `message` | debug / LLM response 用的簡短訊息 |
| `reports` | 一個或多個 endpoint 的 depth report |
| `worst` | `reports` 中 depth 最大的 endpoint |
| `threshold` | query 使用的 depth 門檻 |
| `count` | `reports.size()`，常用於回答「幾個 endpoint 超標」 |

`DepthReport` 主要欄位：

| 欄位 | 意思 |
|---|---|
| `endpointType` | `SpecificNet` / `PrimaryOutput` / `DffD` |
| `endpointName` | endpoint 名稱，例如 output net 或 DFF.D |
| `endpointNetId` | 實際被分析的 net ID |
| `depth` | 到該 endpoint 的最大 combinational depth |
| `criticalPath` | 到該 endpoint 的一條 critical path |

典型問題：

```text
What is the maximum logic depth to output y?
What is the global critical path?
Which outputs have depth greater than 4?
What is the maximum logic depth from any primary input to any DFF D-pin?
Find endpoints exceeding target depth 4.
```

Prompt 對應：

| Prompt 語意 | 建議 API | 主要讀取 |
|---|---|---|
| 單一 output/net 的最大 logic depth | `DepthQuery::SpecificNet` | `worst.depth` |
| 單一 output/net 的 critical path | `DepthQuery::SpecificNet` + `includeCriticalPath = true` | `worst.criticalPath` |
| 所有 primary outputs 的 depth | `DepthQuery::PrimaryOutputs` | `reports`, `worst` |
| 有幾個 outputs depth 大於 N | `DepthQuery::EndpointsExceedingDepth` 後篩 `PrimaryOutput`，或 `getPrimaryOutputsWithDepthGreaterThan(N)` | `count` 或 filtered count |
| 所有 DFF D-pin depth | `DepthQuery::DffD` | `reports`, `worst` |
| 全設計 global critical path | `DepthQuery::GlobalCriticalPath` | `worst`, `worst.criticalPath` |
| 找出 depth 超過 target 的 timing endpoints | `DepthQuery::EndpointsExceedingDepth` | `reports`, `count`, `worst` |

應使用 `DepthAnalysis` 的情況：

| 問題類型 | API |
|---|---|
| 單一 net 最大 depth | `DepthQuery::SpecificNet` 或 `analyzeDepthToNet()` |
| 單一 net critical path | `DepthQuery::SpecificNet` 或 `findCriticalPathToNet()` |
| 所有 PO depth | `DepthQuery::PrimaryOutputs` 或 `analyzePrimaryOutputDepths()` |
| 所有 DFF.D depth | `DepthQuery::DffD` 或 `analyzeDffDDepths()` |
| 全設計 critical path | `DepthQuery::GlobalCriticalPath` 或 `findGlobalCriticalPath()` |
| depth 超標 endpoints | `DepthQuery::EndpointsExceedingDepth` 或 `findEndpointsExceedingDepth()` |

邊界：

```text
DepthAnalysis 不做 Boolean constant status。
DepthAnalysis 不回答 arbitrary path constraints，例如 through/avoid。
若問題指定 startpoint/endpoints 與 through/avoid 條件，改用 PathQuery。
```

`DepthQuery` 只包 depth/timing，不包 FunctionQuery 或 PathQuery 的責任。

與 PathQuery 的差異：

| API | 語意 |
|---|---|
| `PathQuery::MaxDepth` | 指定 startpoints 到 endpoints 之間的最長 path |
| `DepthQuery` | timing startpoints 到 timing endpoints 的 arrival depth / critical path |

例如：

```text
What is the max depth from input n2 to output n12?
```

應使用 `PathQuery::MaxDepth`。

但：

```text
How many outputs have a logic depth greater than 4?
```

應使用 `DepthQuery::EndpointsExceedingDepth` 或 `getPrimaryOutputsWithDepthGreaterThan(4)`。

文件：

```text
API_SPEC/DEPTH_QUERY_USAGE.md
API_SPEC/DEPTH_QUERY_API.md
```

---

## 9. OptimizationCandidate

定位：

```text
Optimization-facing analysis。
它不是純查詢 API，而是把 DepthAnalysis 結果整理成後續 optimization 可使用的 candidate。
```

代表 API：

```cpp
OptimizationCandidate buildOptimizationCandidate(const DepthReport& endpoint,
                                                 int targetDepth) const;
std::vector<OptimizationCandidate> findOptimizationCandidatesExceedingDepth(
    int targetDepth) const;
std::vector<int> findBufferGatesOnCriticalPath(
    const OptimizationCandidate& candidate) const;
std::vector<int> findRemovableBufferGatesOnCriticalPath(
    const OptimizationCandidate& candidate) const;
```

邊界：

```text
OptimizationCandidate 不直接修改 netlist。
OptimizationCandidate 不應混入純 AnalysisQuery 總入口。
真正 rewrite / rollback / validation 應放在 optimization/transformation API。
```

文件：

```text
API_SPEC/OPTIMIZATION_CANDIDATE.md
API_SPEC/OPTIMIZATION_FLOW.md
API_SPEC/CLEANUP_SIMPLIFICATION_NOTES.md
```

---

## 10. 高階 API 重複檢查

### 10.1 FunctionQuery vs Endpoint Constant Status

結論：

```text
不要新增 EndpointQuery::OutputConstantStatus。
```

原因：

```text
單一 output constant status 已經是 FunctionQuery::TruthStatus / AlwaysZero / AlwaysOne。
所有 PO 的 constant status 只是 batch 套用 FunctionQuery。
```

---

### 10.2 PathQuery::MaxDepth vs DepthAnalysis

結論：

```text
兩者不是重複，但語意要切清楚。
```

差異：

| API | 語意 |
|---|---|
| `PathQuery::MaxDepth` | 指定 startpoints 到 endpoints 之間的最長 path |
| `DepthAnalysis` | timing startpoints 到 timing endpoints 的 arrival depth / critical path |

---

### 10.3 ConeQuery includeLocalPaths vs PathQuery / DepthAnalysis

結論：

```text
ConeQuery 的 local path 只作為 cone 內摘要。
```

不得用來取代：

```text
PathQuery 的 startpoint-to-endpoint path。
DepthAnalysis 的 critical path。
```

---

### 10.4 BasicQuery::GatesWithConstantInput vs FunctionQuery

結論：

```text
不是重複。
```

差異：

| API | 查的是 |
|---|---|
| `BasicQuery::GatesWithConstantInput` | 結構上 gate input 是否直接接 constant net |
| `FunctionQuery::AlwaysZero/AlwaysOne` | 功能上 net 是否在所有輸入下固定為 0/1 |

---

### 10.5 DirectConnectivityQuery vs ConeQuery

結論：

```text
不是重複。
```

差異：

| API | 查的是 |
|---|---|
| `DirectConnectivityQuery` | 一層 immediate edge |
| `ConeQuery` | 多層 transitive reachability |

---

## 11. Prompt Routing 規則

| Prompt 關鍵語意 | 應使用 API |
|---|---|
| count / list / type / is PI / is DFF / structural issue | `BasicQuery` |
| directly connected / driver / load / input nets / output net / immediate fanin/fanout | `DirectConnectivityQuery` |
| equivalent / always 0 / always 1 / can become 0/1 / truth status | `FunctionQuery` |
| reachable / transitive fanin / transitive fanout / cone / can affect | `ConeQuery` |
| path from A to B / through / avoid / every path / shortest path / longest path between endpoints | `PathQuery` |
| depth / level / critical path / endpoints exceeding depth / PO depth / DFF.D depth | `DepthQuery / DepthAnalysis` |
| optimize / candidate / target depth / removable buffer on critical path | `OptimizationCandidate` 或 optimization API |

---

## 12. 目前驗證狀態

目前新版 regression tester：

```text
mini test/tester.cpp
mini test/mini_circuit.v
```

覆蓋：

```text
VerilogReader / VerilogWriter
runBasicQuery()
runDirectConnectivityQuery()
runFunctionQuery()
runConeQuery()
runPathQuery()
runDepthQuery()
少量 mutation primitive
```

目前驗證結果：

```text
Summary: 45 passed, 0 failed.
```

Windows 目前建議使用：

```powershell
g++ -std=c++20 -I. -Ilib "mini test/tester.cpp" src/core/*.cpp src/io/*.cpp src/analysis/*.cpp src/optimization/*.cpp src/transformation/*.cpp -L./include/lib/win/ucrt64 -lcadical -o "mini test/tester.exe"
.\mini test\tester.exe
```

## 13. 下一步建議

目前不要新增 `EndpointQuery`，避免和既有高階 API 重疊。

比較乾淨的下一步：

```text
1. 維持 DepthQuery 只處理 depth/timing 類問題。
2. 不把 output truth status 放進 DepthQuery。
3. 不把 arbitrary path through/avoid 放進 DepthQuery。
4. 補 transformation / optimization 的統一修改結果 report。
5. 補 transformation / cleanup 的 regression test。
6. 等所有高階 API 邊界更穩定後，再考慮總入口 AnalysisQuery。
```

建議未來總入口只做 routing，不重新定義功能：

```cpp
enum class AnalysisQueryType {
    Basic,
    DirectConnectivity,
    Function,
    Cone,
    Path,
    Depth
};
```

總入口如果要做，應該只是包裝：

```text
AnalysisQuery -> dispatch 到既有 runBasicQuery / runDirectConnectivityQuery / runFunctionQuery / runConeQuery / runPathQuery / runDepthQuery
```

不能讓 `AnalysisQuery` 自己新增另一套重複語意。
