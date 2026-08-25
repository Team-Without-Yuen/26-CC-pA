# Depth Query API 整理

這份文件整理 Depth Analysis / DepthQuery 的內部設計、depth model、底層 helper 與限制。

外部使用手冊請看：

```text
API_SPEC/DEPTH_QUERY_USAGE.md
```

---

## 1. 核心定位

Depth Analysis 負責回答 timing/depth 類問題：

```text
logic depth
primary output depth
DFF.D depth
global critical path
depth 超標 endpoint
gate 是否位在任一 maximum-depth path
列出位在任一 global maximum-depth path 上的所有 combinational gates
最深 primary output fanin cone
```

它不處理：

```text
Boolean constant status -> FunctionQuery
function equivalence -> FunctionQuery
reachable / fanin / fanout cone set -> ConeQuery
arbitrary A-to-B through/avoid path -> PathQuery
direct driver/load/input/output -> DirectConnectivityQuery
optimization rewrite
```

---

## 2. Depth Model

目前採用簡化的 unit-delay gate model：

```text
每個 combinational gate 的 depth cost = 1
```

一條 path 的 depth：

```text
path 中穿越的 combinational gate 數量。
```

例子：

```text
a -> g1 -> n1 -> g2 -> y
```

depth 為：

```text
2
```

---

## 3. Boundary 規則

Depth analysis 只分析 combinational region。

level 0 的來源：

```text
Primary Input
DFF.Q
constant 1'b0 / 1'b1
```

DFF 規則：

```text
DFF 是 sequential boundary。
不從 DFF input 穿越到 DFF Q。
DFF.Q 可視為 timing startpoint。
DFF.D 可視為 timing endpoint。
```

---

## 4. Level 與 Critical Path

net level：

```text
從最近的 combinational startpoint 到該 net 的最大 gate 數。
```

若多條 fanin path 匯合到同一個 net：

```text
net level = max(所有 fanin path depth)
```

critical path：

```text
到某 endpoint 的一條最大 depth path。
若多條 path 同樣最長，回傳任意一條即可。
```

---

## 5. 底層 Helper

目前提供：

```cpp
std::vector<int> computeNetLevels() const;
std::vector<int> computeGateLevels() const;
int getMaxDepthToNet(const std::string& netName) const;
CombinationalPath findCriticalPathToNet(const std::string& netName) const;
DepthReport analyzeDepthToNet(const std::string& netName) const;
std::vector<DepthReport> analyzePrimaryOutputDepths() const;
std::vector<std::string> getPrimaryOutputsWithDepthGreaterThan(int limit) const;
std::vector<DepthReport> analyzeDffDDepths() const;
std::vector<std::string> getDffsWithDDepthGreaterThan(int limit) const;
DepthReport findGlobalCriticalPath() const;
std::vector<DepthReport> findEndpointsExceedingDepth(int maxDepth) const;
```

用途：

| API | 用途 |
|---|---|
| `computeNetLevels()` | 計算每個 net 的 combinational level |
| `computeGateLevels()` | 計算每個 combinational gate output level |
| `getMaxDepthToNet(net)` | 查單一 net 的最大 fanin depth |
| `findCriticalPathToNet(net)` | 找到單一 net 的一條 critical path |
| `analyzeDepthToNet(net)` | 回傳單一 net 的 `DepthReport` |
| `analyzePrimaryOutputDepths()` | 分析所有 primary output endpoints |
| `getPrimaryOutputsWithDepthGreaterThan(limit)` | 列出 depth 大於 limit 的 PO names |
| `analyzeDffDDepths()` | 分析所有 DFF D-pin endpoints |
| `getDffsWithDDepthGreaterThan(limit)` | 列出 D-pin depth 大於 limit 的 DFF names |
| `findGlobalCriticalPath()` | 找出 PO 與 DFF.D 中最深 endpoint |
| `findEndpointsExceedingDepth(maxDepth)` | 找出所有 depth 大於 maxDepth 的 timing endpoints |

---

## 6. Data Types

型別位置：

```text
include/core/PathTypes.h
```

`DepthEndpointType`：

```cpp
enum class DepthEndpointType {
    Unknown,
    SpecificNet,
    PrimaryOutput,
    DffD
};
```

`DepthFilterScope` 與 `DepthPredicate`：

```cpp
enum class DepthFilterScope {
    AllTimingEndpoints, PrimaryOutputs, DffD
};

enum class DepthPredicate {
    None, Equal, NotEqual, GreaterThan, GreaterOrEqual,
    LessThan, LessOrEqual, BetweenInclusive
};

enum class DepthStatus {
    Unknown, Available, NoTimingPath,
    GraphInconsistent, AnalysisFailure
};
```

`DepthReport`：

```cpp
struct DepthReport {
    DepthEndpointType endpointType = DepthEndpointType::Unknown;
    std::string endpointName;
    int endpointNetId = -1;
    int depth = -1;
    DepthStatus depthStatus = DepthStatus::Unknown;
    CombinationalPath criticalPath;
};
```

`CriticalGateReport`：

```cpp
struct CriticalGateReport {
    std::string gateName;
    int gateId = -1;
    std::string gateTypeName;
    std::string outputNetName;
    int outputNetId = -1;
    int arrivalDepth = -1;
    int remainingDepth = -1;
};
```

`DepthQuery`：

```cpp
struct DepthQuery {
    DepthQueryType type = DepthQueryType::SpecificNet;
    std::string netName;
    std::string gateName;
    int threshold = -1;
    int upperThreshold = -1;
    DepthFilterScope filterScope = DepthFilterScope::AllTimingEndpoints;
    DepthPredicate predicate = DepthPredicate::None;
    bool includeCriticalPath = true;
};
```

`DepthReportSet`：

```cpp
struct DepthReportSet {
    bool ok = false;
    bool exists = false;
    bool complete = true;
    std::string message;
    DepthQueryType type = DepthQueryType::SpecificNet;
    std::vector<DepthReport> reports;
    DepthReport worst;
    int threshold = -1;
    int upperThreshold = -1;
    size_t count = 0;
    bool filterApplied = false;
    DepthFilterScope filterScope = DepthFilterScope::AllTimingEndpoints;
    DepthPredicate predicate = DepthPredicate::None;
    size_t checkedEndpointCount = 0;
    size_t matchedEndpointCount = 0;
    size_t definedDepthEndpointCount = 0;
    size_t noTimingPathEndpointCount = 0;
    size_t graphInconsistentEndpointCount = 0;
    size_t analysisFailureEndpointCount = 0;
    size_t unavailableEndpointCount = 0;
    std::string gateName;
    int gateId = -1;
    bool gateOnCriticalPath = false;
    bool criticalGateBatchApplied = false;
    size_t checkedGateCount = 0;
    size_t analyzableGateCount = 0;
    size_t criticalGateCount = 0;
    size_t noTimingPathGateCount = 0;
    size_t graphInconsistentGateCount = 0;
    size_t analysisFailureGateCount = 0;
    std::map<GateType, size_t> criticalGateTypeCounts;
    std::vector<CriticalGateReport> criticalGates;
};
```

---

## 7. High-Level Entry

統一高階入口：

```cpp
DepthReportSet runDepthQuery(const DepthQuery& query) const;
```

dispatch 對照：

| DepthQueryType | 底層 helper | 主要讀取 |
|---|---|---|
| `SpecificNet` | shared net levels + single-net report builder | `reports[0]`, `worst.depth`, `worst.criticalPath` |
| `PrimaryOutputs` | shared net levels + active PO collector | `reports`, `worst` |
| `DffD` | shared net levels + active DFF.D collector | `reports`, `worst` |
| `GlobalCriticalPath` | single level pass + endpoint metadata selection | `worst`, `reports[0]` |
| `EndpointsExceedingDepth` | single level pass + all-endpoint threshold filter | `reports`, `count`, `worst` |
| `PrimaryOutputsExceedingDepth` | single level pass + PO-only threshold filter | `reports`, `count`, `worst` |
| `EndpointDepthFilter` | single level pass + scope/predicate filter | `checkedEndpointCount`, `matchedEndpointCount`, `unavailableEndpointCount`, `reports`, `complete` |
| `GateOnCriticalPath` | net level + iterative remaining-depth DP | `gateOnCriticalPath`, `exists`, `worst` |
| `CriticalGateBatch` | one forward level pass + one iterative reverse remaining-depth pass | `criticalGates`, gate counts, `worst`, `complete` |
| `DeepestOutputCone` | shared net levels + active PO collector | `worst`, `reports`, `count` |

`analyzePrimaryOutputDepths()` 與 `analyzeDffDDepths()` 會先計算一次全設計 net levels，再讓所有 endpoint 共用；不會為每個 output/DFF.D 重跑整張 graph。critical path 也從同一份 level vector 重建。

`EndpointDepthFilter` 支援 `Equal / NotEqual / GreaterThan / GreaterOrEqual /
LessThan / LessOrEqual / BetweenInclusive`。`BetweenInclusive` 使用閉區間；filter 是
summary-only，不建立 matched endpoint critical paths。`AllTimingEndpoints` 的 stable order 為
所有 active PO bits 後接 active DFF.D。`count == matchedEndpointCount == reports.size()`；
沒有 timing path 的 endpoint 不參與數值 predicate，但會計入 `noTimingPathEndpointCount`。
`complete` 表示 traversal 沒有 graph inconsistency 或 backend failure；因此合法的
`NoTimingPath` 不會使結果變成 partial。`unavailableEndpointCount` 是三種非 `Available`
狀態的相容總和，不能單獨用來判斷 `complete`。

`GateOnCriticalPath` 的語意：

```text
判斷指定 combinational gate 是否位在任一 global maximum-depth path 上。
它不是只檢查 findGlobalCriticalPath() 回傳的單一路徑，而是用：
fanin depth to gate + gate cost + remaining depth to timing endpoint
是否等於 global maximum depth。
```

`CriticalGateBatch` 使用相同 membership 定義，但一次分類所有 active combinational gates：

```text
arrivalDepth(gate.output) + remainingDepth(gate.output) == globalMaximumDepth
```

其中 `remainingDepth` 是 gate output 到任一可達 PO 或 DFF.D timing endpoint 的最大剩餘
combinational gate 數。結果依 gate ID 穩定排序、每顆 gate 最多出現一次，且是所有同深度
maximum paths 的聯集，不是 `GlobalCriticalPath` 代表性單一路徑上的 gate 集合。DFF 本身是
boundary，不列入 `checkedGateCount` 或 `criticalGates`。

Batch report 契約：

```text
count == criticalGateCount == criticalGates.size()
sum(criticalGateTypeCounts) == criticalGateCount
checkedGateCount == 所有 active non-DFF gates
analyzableGateCount == 同時具有 arrivalDepth 與 remainingDepth 的 gates
complete == endpoint summary 與所有 checked gates 均無 graph/backend failure
```

合法但無法到達任何 timing endpoint 的 gate 計入 `noTimingPathGateCount`，不會令
`complete=false`。graph inconsistency 或 analysis failure 會令結果 partial，但已確認的
`criticalGates` 仍保留。
`criticalGateTypeCounts` 固定包含 AND、OR、NAND、NOR、NOT、BUF、XOR、XNOR 八種
combinational type，沒有 match 的 type 也保留明確零值。統計在同一次 batch traversal 中完成，
不需要再逐 gate 掃描 artifact 或重跑 `GateOnCriticalPath`。

實作規則：

```text
1. level、endpoint collection 與 critical-path reconstruction 只接受 active gate/net。
2. traversal edge 必須同時存在於 net.loadGateIds 與 gate.inputNetIds；driver 也必須雙向一致。
   combinational gate 的每一條不同 input net 都必須有反向 load edge，否則整顆 gate 不參與 levelization；
   tied inputs 即使 load list 已去重仍視為合法。
3. includeCriticalPath=false 時不建立稍後會被丟棄的 path。
4. Global/threshold query 先用同一份 netLevels 收集 metadata，只替必要 endpoint 建 path。
5. GateOnCriticalPath 與 CriticalGateBatch 在 combinational DAG 上共用 iterative reverse-depth DP，
   避免深鏈遞迴 stack overflow；batch 不會對每顆 gate 重跑全圖。
6. request-local status pass 以非遞迴方式傳播 `NoTimingPath`；缺 driver 是合法 no-path，
   stale/missing reverse edge 是 `GraphInconsistent`，有效 graph 卻未 levelize 才是
   `AnalysisFailure`。
```

---

## 8. 與其他 Query 的邊界

`DepthQuery` 和 `PathQuery` 都可能出現 depth 字樣，但語意不同。

| API | 語意 |
|---|---|
| `PathQuery::MaxDepth` | 指定 startpoints 到 endpoints 之間的最長 path |
| `DepthQuery` | timing startpoints 到 timing endpoints 的 arrival depth / critical path |

例子：

```text
What is the max depth from input a to output y?
```

適合：

```text
PathQuery::MaxDepth
```

但：

```text
Which outputs have depth greater than 4?
```

適合：

```text
DepthQuery::EndpointsExceedingDepth
```

---

## 9. 與 Optimization 的關係

Depth optimization 的高階流程通常是：

```text
1. DepthQuery 找出超標 timing endpoints。
2. OptimizationCandidate 建立最佳化候選。
3. 對 candidate critical path / fanin cone 做 target selection。
4. 執行 transformation / rewrite。
5. 重新跑 DepthQuery。
6. 用 FunctionQuery / equivalence check 驗證功能。
7. accept 或 rollback。
```

DepthQuery 只負責第 1 步與第 5 步的分析，不直接修改 netlist。

---

## 10. 目前限制

第一版 DepthQuery 不處理：

```text
real cell delay
library timing arc
load capacitance
slew
arrival / required time / slack
multi-cycle path
false path
clock domain timing constraints
sequential timing across multiple cycles
```

因為 Problem A 目前是 restricted primitive gate-level netlist，unit-depth model 較適合。

---

## 11. 實作與測試狀態

實作位置：

```text
include/core/PathTypes.h
src/analysis/DepthAnalysis.cpp
```

已完成：

```text
computeNetLevels()
computeGateLevels()
getMaxDepthToNet()
findCriticalPathToNet()
analyzeDepthToNet()
analyzePrimaryOutputDepths()
getPrimaryOutputsWithDepthGreaterThan()
analyzeDffDDepths()
getDffsWithDDepthGreaterThan()
findGlobalCriticalPath()
findEndpointsExceedingDepth()
runDepthQuery(GateOnCriticalPath)
runDepthQuery(DeepestOutputCone)
runDepthQuery(EndpointDepthFilter)
DepthQuery / DepthReportSet / runDepthQuery()
```

既有測試記錄：

```text
mini test/tester.cpp 已覆蓋 computeNetLevels / computeGateLevels /
findCriticalPathToNet / runDepthQuery(SpecificNet, DffD, GlobalCriticalPath)。
mini test/test5/test5.cpp 已覆蓋 GateOnCriticalPath / DeepestOutputCone、七種 depth predicates、
all/po/dff_d scopes、zero match、legacy gt compatibility、undriven/floating no-timing-path、
graph inconsistency partial、
includeCriticalPath=false、driver/input stale edge、tombstone、tied input、
reconvergent depth 與 30000-level deep-chain regression。
```

後續建議補：

```text
real timing model
target-depth optimization apply flow
```
