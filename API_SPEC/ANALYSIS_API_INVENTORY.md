# High-Level API 責任與功能劃分

這份文件整理目前 `Netlist` 裡已建立或規劃中的 read-only query、edit 與 optimization 高階 API，目標是：

對外 facade 與 legacy command 的最終邊界另見 `API_SPEC/PUBLIC_API_CONSOLIDATION.md`。

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
| `StructureQuery`（公開 tool facade） | gate/net/port 基本資訊、統計、structural issue，以及一層 driver/load/fanout/pin connectivity | transitive cone、多層路徑、Boolean function、修改 |
| `ConeQuery` | transitive fanin/fanout 的 net/gate 集合與集合摘要 | 指定 A-to-B path、全域 critical path、功能證明 |
| `PathQuery` | endpoint path、DFF.Q-to-DFF.D path、mandatory nodes、separator 與 PI-to-PO cut | 全設計 timing endpoint 掃描、Boolean function |
| `DepthQuery / DepthAnalysis` | timing arrival depth、PO/DFF.D depth、global critical path、depth threshold | arbitrary through/avoid path、Boolean function |
| `FunctionQuery` | 已指定 net/bus 的 Boolean 性質：equivalence、constant、dependence、symmetry、expression | 未知候選搜尋、cross-design equivalence、修改 |
| `FunctionSearchQuery` | 未知 internal signal 候選搜尋；目前支援 SAT-proven `NAND(a,b)==target` | 修改 netlist、任意 functional duplicate/redundancy removal |
| `SequentialPatternQuery` | 從 DFF D-input logic 推導 canonical 或 SAT-proven functional enable/hold 等 register-control pattern | 一般 DFF 列表、clock/reset 直接連線、修改 |
| `WholeDesignEquivalence` | current 與 original/previous snapshot 的 PO+DFF.D 等價證明 | 同一 design 中兩條 internal nets 的比較 |
| `EditApply` | 執行使用者指定且已有安全規則/certificate 的 transformation | cost-driven 最佳化搜尋、未知功能候選搜尋 |
| `OptimizationCandidate / OptApply` | 建立候選並依 depth cost function transactional apply；驗證 basis、target 與 whole-design equivalence | 一般 read-only query、固定指定 mapping 的語意 |

`FunctionSearchQuery` 負責「候選未知、需要從大量 signals/gates 中搜尋」的 Boolean discovery；它不修改 netlist。第一版 `NandEquivalentInputPairs` 已完成，其他 functional pair/redundancy mode 仍不得用 `FunctionQuery` 或 structural merge 假裝覆蓋。

`StructureQuery` 是 tools/LLM-facing facade，內部依 mode dispatch 到 `BasicQuery` 或 `DirectConnectivityQuery`。`PathQuery` 對外統一 endpoint connectivity；register path endpoint 展開及 Graph dominator 演算法仍保留為內部元件。

目前 source ownership：

| 高階 API | 主要實作檔案 |
|---|---|
| `BasicQuery` | `src/analysis/BasicAnalysis.cpp` |
| `DirectConnectivityQuery` | `src/analysis/ConnectivityAnalysis.cpp` |
| `FunctionQuery` | `src/analysis/FunctionAnalysis.cpp`（含 equivalence、dependence、symmetry、constant/truth） |
| `FunctionSearchQuery` | `src/analysis/FunctionAnalysis.cpp`（共用 bit-parallel simulation + SAT proof） |
| `SequentialPatternQuery` | `src/analysis/SequentialPatternAnalysis.cpp`；非 canonical 搜尋使用 `FunctionalPatternEngine.cpp` 與共用 simulation signatures |
| `GraphQuery`（PathQuery 內部 engine） | `src/analysis/GraphAnalysis.cpp` |
| `ConeQuery` | `src/analysis/ConeAnalysis.cpp` |
| `PathQuery` | `src/analysis/PathAnalysis.cpp` |
| `DepthQuery / DepthAnalysis` | `src/analysis/DepthAnalysis.cpp` |
| `WholeDesignEquivalence` | `src/analysis/WholeDesignEquivalence.cpp` |
| `OptimizationCandidate` | `src/analysis/OptimizationAnalysis.cpp` |
| `EditApply` | `src/transformation/EditFlow.cpp` + operation owner |
| `OptApply` | `src/optimization/OptimizationFlow.cpp` + `DepthOptimizer.cpp` |

目前共用 query/report 型別位置：

| Header | 內容 |
|---|---|
| `include/core/NetlistQueries.h` | Basic / DirectConnectivity / Function / SequentialPattern / Graph / Cone query/report |
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
Public high-level Query API
    |
    +-- StructureQuery facade
    +-- ConeQuery
    +-- PathQuery
    +-- DepthQuery / DepthAnalysis
    +-- FunctionQuery
    +-- SequentialPatternQuery
    +-- WholeDesignEquivalence

Internal query engines / adapters
    |
    +-- BasicQuery
    +-- DirectConnectivityQuery
    +-- GraphQuery dominator engine
    +-- RegisterPathQuery legacy wrapper
    |
Search / planning layer
    |
    +-- FunctionSearchQuery (read-only discovery; NAND pair mode completed)
    +-- OptimizationCandidate
    |
Mutation / optimization layer
    |
    +-- EditApply (specified transformation)
    +-- OptApply (cost-driven search and apply)
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

### 5.1 SequentialPatternQuery

定位：

```text
組合既有 DFF pin、cone、path 與 Function/SAT helper，推導 D-input 的 register-control semantics。
```

高階入口：

```cpp
SequentialPatternReportSet runSequentialPatternQuery(
    const SequentialPatternQuery& query) const;
```

目前只公開 `DffEnableHold`，回答：

```text
哪些 DFF 具有 canonical 或功能等價的 feedback-MUX enable/hold structure？
Enable/data/Q-feedback 分別是哪條 net？
Enable 是 active-high 或 active-low？
符合條件的 unique DFF 數量是多少？
```

它不重做 `ListDffs`、clock/reset load、fanin cone 或 SAT encoding，而是重用既有 API。預設只跑 canonical fast path；C++ caller 可開啟有 candidate/time/completeness 限制的 functional cofactor fallback。AND-only D-input 目前只回報 semantics-pending candidate，不計入 confirmed `matchedDffCount`。

文件：

```text
API_SPEC/SEQUENTIAL_PATTERN_API.md
API_SPEC/SEQUENTIAL_PATTERN_USAGE.md
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
回答 endpoint 間路徑、register boundary path，以及 mandatory/separator 類 connectivity 問題。
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
List mandatory internal nets between A and D.
Does candidate C separate A from D?
Is C a directed PI-to-PO cut net?
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
| 求 source/target 間 mandatory internal nets | `FindMandatoryNodes` |
| 判斷指定 candidate 是否為 separator | `IsSeparator` |
| register-to-register path | 上述 path mode + `DffQ` / `DffD` endpoint kind |

邊界：

```text
一般 PathQuery mode 必須有 startpoints/endpoints；`DirectPiPoConnections` 與 PI-to-PO cut preset 不需要一般 endpoints。
PathQuery 的 MaxDepth 是指定 startpoint-to-endpoint 範圍內的最長路徑。
全設計 worst endpoint / critical path 屬於 DepthAnalysis。
mandatory/separator 由 `GraphAnalysis.cpp` 的 dominator engine 計算，不使用大量 path enumeration。
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
API_SPEC/PATH_QUERY_API.md
API_SPEC/PATH_QUERY_USAGE.md
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
| `PrimaryOutputsExceedingDepth` | 只找 depth 大於 threshold 的 PO bits | `threshold` | `reports`, `count`, `worst` |

`DepthQuery` 輸入欄位：

| 欄位 | 用途 |
|---|---|
| `type` | 決定執行哪一種 depth query |
| `netName` | `SpecificNet` 使用，指定要分析的 net/output |
| `threshold` | exceeding 類 query 使用，指定 depth 門檻 |
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
| 有幾個 outputs depth 大於 N | `DepthQuery::PrimaryOutputsExceedingDepth` | `count` |
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
| depth 超標 primary outputs | `DepthQuery::PrimaryOutputsExceedingDepth` 或 `getPrimaryOutputsWithDepthGreaterThan()` |

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

應使用 `DepthQuery::PrimaryOutputsExceedingDepth`；底層亦可用 `getPrimaryOutputsWithDepthGreaterThan(4)`。

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

### 10.6 FunctionQuery vs WholeDesignEquivalence

| API | 比較範圍 |
|---|---|
| `FunctionQuery::Equivalence` | 同一張 netlist 中，使用者已指定的兩條 net/bus |
| `WholeDesignEquivalence` | 兩個 design snapshots 的所有對應 PO 與 DFF.D next-state boundary |

prompt 出現 `current/original/previous/transformed design` 時必須使用 whole-design checker，不能逐條猜 internal net。

---

### 10.7 PathQuery facade vs GraphQuery engine

| 公開 Path mode / 內部 engine | 語意 |
|---|---|
| `PathQuery::EveryPathThrough` | 指定 startpoint/endpoints 的所有路徑是否經過指定 node |
| `PathQuery::IsSeparator` | 判斷指定 net 是否為 source/target separator，或使用 PI-to-PO cut preset |
| `PathQuery::FindMandatoryNodes` | 一次求出指定 source/target 間的 mandatory internal nets |
| internal `GraphQuery` | 提供上述 mode 使用的 directed dominator/cut 演算法與 cycle/status report |

公開 routing 一律進入 `PathQuery`；`GraphQuery` 是 authoritative algorithm owner，但不再是獨立的 LLM-facing command。舊 `graph_query` 只保留相容性。

---

### 10.8 EditApply vs OptApply

```text
使用者已指定要做哪種 transformation
  -> EditApply

使用者指定 objective/cost function，要求工具搜尋較佳結果
  -> OptApply
```

例子：

| Prompt | Owner |
|---|---|
| Convert cone n8 to NAND/NOT | `EditApply::ConvertToBasis` |
| Replace every XOR with NAND implementation | `EditApply::ReplaceGateType` |
| Minimize depth while remaining NAND/NOT | `OptApply::CriticalPathDepth` |
| Find best depth / report original if optimal | `OptApply::CriticalPathDepth` |

`EditApply` 可以提供 before/after depth，但不能因為 report 有 depth 欄位就宣稱已做最佳化搜尋。

---

### 10.9 FunctionQuery vs FunctionSearchQuery

```text
候選名稱已知，只需證明一個 Boolean property
  -> FunctionQuery

候選未知，需要 enumeration/filter/SAT/time budget/completeness
  -> FunctionSearchQuery
```

例如 `Are n1 and n2 equivalent?` 屬 FunctionQuery；`Does any pair (a,b) satisfy NAND(a,b)==n25?` 屬 FunctionSearchQuery。搜尋 report 必須有 `complete/timedOut/candidateCount`，不能只沿用 FunctionReport 的單一 `exists`。

---

### 10.10 Cleanup Specific Pass vs SafeCleanupFixpoint

| Prompt | Owner |
|---|---|
| collapse double inverters | 指定的 `CollapseDoubleInverter` |
| remove dangling logic | 指定的 `RemoveDanglingLogic` |
| prune/clean unused logic（廣義） | `SafeCleanupFixpoint` umbrella |
| remove arbitrary functionally redundant logic | 未來 observability-aware redundancy flow |

`SafeCleanupFixpoint` 只組合已知安全 cleanup 規則，不代表 arbitrary SAT redundancy removal。

---

### 10.11 BasicQuery::GateInfo vs DirectConnectivityQuery

`GateInfo` 可以附帶單一 gate 的 pins，因為它是 object snapshot；但涉及關係集合、分類 load、fanout constraint 或「所有相連 gate」時，authoritative owner 是 DirectConnectivityQuery。tools 不應從 GateInfo 字串重新解析 connectivity。

---

### 10.12 SequentialPatternQuery 的組合責任

SequentialPatternQuery 會重用 DFF pin lookup、Cone 與 Function SAT，但輸出的是 register-control semantics，因此不是重複 API。`FunctionalPatternEngine` 是其內部可擴充 matcher registry，不形成另一個公開 query family。一般 `List all DFFs` 仍用 BasicQuery；`DFFs driven by clock n0` 仍用 DirectConnectivityQuery；只有 enable/hold/feedback-MUX pattern 使用 SequentialPatternQuery。

---

## 11. Prompt Routing 規則

| Prompt 關鍵語意 | 應使用 API |
|---|---|
| count / list / type / is PI / is DFF / structural issue | public `StructureQuery` -> internal `BasicQuery` |
| directly connected / driver / load / input nets / output net / immediate fanin/fanout | public `StructureQuery` -> internal `DirectConnectivityQuery` |
| same-design named nets equivalent / constant / dependence / symmetry / expression | `FunctionQuery` |
| unknown pair/candidate search / exists any pair | `FunctionSearchQuery`；目前公開 NAND pair mode |
| DFF enable / hold / feedback MUX / register control pattern | `SequentialPatternQuery` |
| reachable / transitive fanin / transitive fanout / cone / can affect | `ConeQuery` |
| path from A to B / through / avoid / every path / shortest path / longest path between endpoints | `PathQuery` |
| register-to-register path | `PathQuery` with `DffQ` / `DffD` endpoints |
| depth / level / critical path / endpoints exceeding depth / PO depth / DFF.D depth | `DepthQuery / DepthAnalysis` |
| cut / articulation / mandatory nodes / separator | public `PathQuery` -> internal `GraphQuery` engine |
| current vs original/previous design equivalence | `WholeDesignEquivalence` |
| rename / cleanup / specified rewrite / basis conversion | `EditApply` |
| minimize / optimize / best / depth cost function | `OptApply::CriticalPathDepth` |

Routing precedence：

```text
1. 先判斷是否修改 current design。
2. 修改且有 objective/cost function -> OptApply；指定操作 -> EditApply。
3. read-only 且比較 design snapshots -> WholeDesignEquivalence。
4. read-only 且候選未知 -> FunctionSearchQuery 或其他 search API。
5. 其餘依 function / graph / path / depth / cone / connectivity / basic 語意選 owner。
```

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
runFunctionSearchQuery()
runSequentialPatternQuery()
runConeQuery()
runPathQuery()
runDepthQuery()
少量 mutation primitive
```

目前驗證結果：

```text
Sequential pattern mini test: 15 passed, 0 failed.
Sequential edit-flow mini test: 13 passed, 0 failed.
Sequential CLI mini test: 12 passed, 0 failed.
Functional pattern engine mini test: 12 passed, 0 failed.
NewTestCase/test40 original: 2585 DFF analyzed, 1583 canonical matches.
NewTestCase/test40 post-edit: 1590 confirmed matches, 1975 candidates, query about 0.050 s.
Full mapping/edit/PO+DFF.D-equivalence/query flow: about 33.7 s.
```

Windows 目前建議使用：

```powershell
make libs
$sources = Get-ChildItem -Path src -Recurse -Filter *.cpp |
    Where-Object { $_.Name -notin @('MockturtleConverter.cpp', 'DepthOptimizer.cpp') } |
    ForEach-Object { $_.FullName }
g++ -std=c++20 -I. -Iinclude -Iinclude/lib `
    -Iinclude/lib/cadical/src -Iinclude/lib/abc/src `
    "mini test/test19/test19.cpp" $sources `
    -Wl,--start-group ./include/lib/abc/libabc.a `
    ./include/lib/cadical/build/libcadical.a -Wl,--end-group `
    -lpthread -lm -o "mini test/test19/test19.exe"
& ".\mini test\test19\test19.exe"
```

新版 Makefile 已整合 mockturtle、ABC 與 CaDiCaL；以上 focused 驗證命令仍排除 optimization 模組，以縮短 analysis API 回歸測試時間。

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
AnalysisQuery -> dispatch 到既有 runBasicQuery / runDirectConnectivityQuery / runFunctionQuery / runFunctionSearchQuery / runConeQuery / runPathQuery / runDepthQuery
```

不能讓 `AnalysisQuery` 自己新增另一套重複語意。
