# Project Organization Plan

這份文件用來規劃整個專案的整理順序。  
目標不是一次大重構，而是用小步驟把目前分散的 API 逐步整理成穩定架構。

## 1. 整理原則

每一步都遵守：

```text
先整理分類，不急著搬檔。
先補清楚 API 邊界，再補實作。
每次修改後盡量編譯或跑 mini test。
不把 analysis / mutation / optimization 混在同一個新 API 裡。
```

API 歸類規則：

| 類型 | 放置方向 | 判斷方式 |
| --- | --- | --- |
| Construction | `Netlist` core | parser / reader 用來建立圖 |
| Basic Query | Analysis | 只查 ID/name/type/count |
| Connectivity Query | Analysis | 查 driver/load/fanin/fanout/pin |
| Cone / Path / Depth | Analysis | traversal、路徑、depth，不修改 |
| Function / Equivalence | Analysis | SAT / CNF / function checker |
| Candidate Finder | Analysis | 找可修改候選，但不修改 |
| Mutation Primitive | Transformation | rename/reconnect/remove/replace |
| Cleanup Execution | Transformation / Optimization | 真的執行 cleanup pass |
| Optimization Strategy | Optimization | 選目標、建 plan、選 rule |
| Unified Query | High-level API | 給 LLM / prompt dispatch 使用 |
| Edit Report | Shared result format | 修改/最佳化後共同回傳格式 |

## 2. 目前檔案狀態

核心 header：

```text
include/core/Netlist.h
```

目前包含：

```text
data model
construction API
basic query
direct connectivity
function analysis
cone analysis
path analysis
mutation
cleanup
depth analysis
optimization candidate
high-level query structs
```

source 已經完成第一階段 analysis 分檔：

```text
src/core/Netlist.cpp
src/analysis/BasicAnalysis.cpp
src/analysis/ConnectivityAnalysis.cpp
src/analysis/FunctionAnalysis.cpp
src/analysis/ConeAnalysis.cpp
src/analysis/PathAnalysis.cpp
src/analysis/DepthAnalysis.cpp
src/analysis/OptimizationAnalysis.cpp
src/optimization/NetlistOptimization.cpp
src/transformation/NetlistTransformation.cpp
src/io/VerilogReader.cpp
src/io/VerilogWriter.cpp
```

所以目前重點已從「拆 cpp」轉成：

```text
1. 確認 Netlist.h 宣告順序與 source ownership 一致。
2. 補 mini test，確定每一組高階 API 都有基本覆蓋。
3. 暫時不要再把新的 analysis API 塞回單一雜物檔。
```

## 3. 整理階段

### Phase 1：Header 分類整理

目標：

```text
讓 Netlist.h 能一眼看出每個 API 屬於哪一層。
```

工作：

1. 加入 API 分類索引。
2. 統一 section 命名。
3. 標註哪些 API 是 analysis/candidate finder，哪些會修改 netlist。
4. 不改 function signature。
5. 不搬動 nested struct，避免宣告順序風險。

目前狀態：

```text
已完成第一版分類索引與主要 section 標題。
已拆出 include/core/NetlistTypes.h：
  GateType / Net / Port / Gate / ConeResult
已拆出 include/core/NetlistQueries.h 第一批型別：
  BasicQuery / DirectConnectivityQuery / FunctionQuery / ConeQuery
  Netlist.h 內用 using alias 保留 Netlist::Xxx 相容寫法。
已拆出 include/core/PathTypes.h：
  PathNode / CombinationalPath
  DepthReport / DepthQuery / DepthReportSet
  PathEndpoint / PathQuery / PathQueryResult
  Netlist.h 內用 using alias 保留 Netlist::Xxx 相容寫法。
已拆出 include/core/OptimizationTypes.h：
  OptimizationResult / OptimizationCandidate
  Netlist.h 內用 using alias 保留 Netlist::Xxx 相容寫法。
已整理 Netlist.h 的 Low-Level Analysis API 順序：
  1.1 Basic Netlist / ID / Type Query
  1.2 Direct Connectivity Query
  1.3 Boolean / Function Analysis
  1.4 Cone Analysis
  1.5 Combinational Path Analysis
  1.6 Depth Analysis
  同時將 setNetConst / checkEquivalence / expandNetToBits 等零散宣告歸到較合理位置。
已整理 Netlist.h 的 Low-Level Mutation / Cleanup API 順序：
  2.1 Graph Mutation Primitives
  2.2 Buffer Insertion Transformation API
  2.3 Validation / Rollback Helpers
  2.4 Legacy Cleanup Execution Passes
  2.5 Buffer Cleanup Building Blocks
  2.6 Double Inverter Removal Building Blocks
  2.7 Constant Propagation Building Blocks
  2.8 Same-Input Simplification Building Blocks
  2.9 Dangling Logic Removal Building Blocks
  2.10 Structural Hashing Building Blocks
  並標註 finder/checker/executor 的職責差異。
已整理 Netlist.h 的 Optimization Candidate / Strategy API 順序：
  3.1 Optimization Result / Candidate Types
  3.2 Depth-Driven Candidate Builder
  3.3 Optimization Target Selection API
  3.4 Future Optimization Rewrite Pass API
  3.5 Future Optimization Validation / Rollback API
  並移除已過期的 validateStructure / validateProblemAConstraints TODO 宣告。
已整理 Startpoint-to-Endpoint resolver helpers：
  getPrimaryInputNetIds / getPrimaryOutputNetIds
  getDffOutputNetId / getGateOutputNetId / getGateInputNetId
  resolvePathEndpoint / resolvePathEndpoints
  已移到 1.5 Combinational Path Analysis 區塊。
```

### Phase 2：A/B/C 分工 API 對齊

目標：

```text
讓 Team A/B/C 文件與 Netlist.h 分類一致。
```

工作：

1. Team A：只讀 analysis / checker / report building block。
2. Team B：mutation primitive / rollback / cleanup execution。
3. Team C：optimization / mapping strategy / plan。
4. 把只讀 API 從 B/C 文件移回 A。
5. 統一使用 `NetlistEditReport` 作為修改/最佳化結果格式。

目前狀態：

```text
TEAM_A/B/C_LOW_LEVEL_API_GAPS.md 已完成第一版重切。
仍需要之後依 Netlist.h 實際新增 API 再同步更新。
```

### Phase 3：補 Team A 缺的 wrapper

優先補 Analysis wrapper，因為 B/C 都會依賴它。

P0：

```cpp
NetlistEditReport / buildEditReport()
NetlistStats / NetlistDiff
getMaxFanout()
getNetsExceedingFanout()
checkMaxFanoutConstraint()
collectGateIdsInScope()
getGateIdsByTypeInScope()
checkWholeDesignEquivalence()
```

P1：

```cpp
getGateInputPinIndex()
getGateInputPinName()
analyzeControlFanout()
checkAllowedGateTypesInScope()
computeMappingDelta()
evaluateDepthChange()
```

### Phase 4：補 Team B mutation primitives

在 Team A 有 checker/report 後，再補安全修改底層。

P0：

```cpp
makeUniqueGateName()
makeUniqueNetName()
disconnectGateInputPin()
reconnectGateInputPin()
replaceGateWithNet()
replaceGateWithConstant()
replaceGateWithNotOfNet()
createSnapshot()
restoreSnapshot()
```

P1：

```cpp
replaceDriverOfNet()
mergeNetIntoNet()
removeUnusedNets()
runLocalSimplificationFixpoint()
```

### Phase 5：補 Team C planning / strategy

最後補 optimization strategy，因為它需要 A 的 checker 與 B 的 mutation。

P0：

```cpp
buildFanoutBufferPlan()
applyFanoutBufferPlan()
hasMappingRule()
getBestMappingRule()
buildMappingPlan()
validateMappingPlan()
```

P1：

```cpp
applyMappingPlanWithValidation()
findAssociativeChainsInCone()
buildBalancedTreePlan()
optimizeEndpointDepth()
```

## 4. 長期拆檔方向

Header 已先拆出部分共用型別：

```text
NetlistTypes.h
NetlistQueries.h
PathTypes.h
OptimizationTypes.h
Netlist.h
```

後續若 API 邊界更穩，可再考慮拆成：

```text
NetlistAnalysis.h
NetlistMutation.h
NetlistEditReport.h
OptimizationPlanning.h
```

但目前建議先保持單一 `Netlist.h` 作為主要 facade，讓使用者仍然只需要 include 一個核心 header。

## 5. 驗證方式

每完成一個小階段：

```text
1. 編譯 main / tester。
2. 跑 mini test。
3. 若有新增 API，補 tester case。
4. 若只是文件或註解整理，可至少確認 git diff 與 header 結構。
```

## 6. 下一步建議

下一步先做：

```text
1. 依照 PROJECT_SPEC/CPP_FUNCTION_OWNERSHIP.md 檢查 Netlist.h 每個 section 的宣告是否放對位置。
2. 補 mini test 覆蓋 Basic / Connectivity / Function / Cone / Path / Depth 高階 API。
3. 若要進入 transformation/optimization，先設計統一修改結果 report。
```

完成後再開始 transformation / optimization API 的低階能力整理。
