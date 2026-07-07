# Depth Query API

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

`DepthReport`：

```cpp
struct DepthReport {
    DepthEndpointType endpointType = DepthEndpointType::Unknown;
    std::string endpointName;
    int endpointNetId = -1;
    int depth = -1;
    CombinationalPath criticalPath;
};
```

`DepthQuery`：

```cpp
struct DepthQuery {
    DepthQueryType type = DepthQueryType::SpecificNet;
    std::string netName;
    int threshold = -1;
    bool includeCriticalPath = true;
};
```

`DepthReportSet`：

```cpp
struct DepthReportSet {
    bool ok = false;
    std::string message;
    DepthQueryType type = DepthQueryType::SpecificNet;
    std::vector<DepthReport> reports;
    DepthReport worst;
    int threshold = -1;
    size_t count = 0;
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
| `SpecificNet` | `analyzeDepthToNet(netName)` | `reports[0]`, `worst.depth`, `worst.criticalPath` |
| `PrimaryOutputs` | `analyzePrimaryOutputDepths()` | `reports`, `worst` |
| `DffD` | `analyzeDffDDepths()` | `reports`, `worst` |
| `GlobalCriticalPath` | `findGlobalCriticalPath()` | `worst`, `reports[0]` |
| `EndpointsExceedingDepth` | `findEndpointsExceedingDepth(threshold)` | `reports`, `count`, `worst` |

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
DepthQuery / DepthReportSet / runDepthQuery()
```

既有測試記錄：

```text
mini test/tester.cpp 已覆蓋 computeNetLevels / computeGateLevels /
findCriticalPathToNet / runDepthQuery(SpecificNet, DffD, GlobalCriticalPath)。
```

後續建議補：

```text
runDepthQuery(PrimaryOutputs)
runDepthQuery(EndpointsExceedingDepth)
getPrimaryOutputsWithDepthGreaterThan()
```
