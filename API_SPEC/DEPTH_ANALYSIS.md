# Depth Analysis / DepthQuery API 整理

這份文件整理 `Netlist` 的 depth / timing analysis API。  
這一層負責回答 logic depth、critical path、primary output depth、DFF.D depth、以及 depth 超標 endpoint 類問題。

---

## 1. 核心定位

Depth Analysis 負責回答 timing/depth 類問題。

典型問題：

```text
What is the maximum logic depth to output y?
Report the critical path to n10.
Which outputs have depth greater than 4?
What is the maximum logic depth from any primary input to any DFF D-pin?
Find endpoints exceeding target depth 4.
Find the global critical path.
```

這一層不做：

```text
Boolean constant status -> FunctionQuery
function equivalence -> FunctionQuery
reachable / fanin / fanout cone set -> ConeQuery
arbitrary A-to-B through/avoid path -> PathQuery
direct driver/load/input/output -> DirectConnectivityQuery
optimization rewrite
```

---

## 2. Depth 定義

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

因為穿越 `g1` 和 `g2`。

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

## 5. 基礎 Depth Helper

目前提供以下底層 helper：

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

## 6. DepthReport

```cpp
enum class DepthEndpointType {
    Unknown,
    SpecificNet,
    PrimaryOutput,
    DffD
};

struct DepthReport {
    DepthEndpointType endpointType = DepthEndpointType::Unknown;
    std::string endpointName;
    int endpointNetId = -1;
    int depth = -1;
    CombinationalPath criticalPath;
};
```

欄位語意：

| 欄位 | 意思 |
|---|---|
| `endpointType` | endpoint 來源類型 |
| `endpointName` | 給人看的 endpoint 名稱，例如 `y`、`n10`、`ff1.D` |
| `endpointNetId` | 實際被分析的 net ID |
| `depth` | 到該 endpoint 的最大 combinational depth |
| `criticalPath` | 到該 endpoint 的一條 critical path |

---

## 7. 高階 DepthQuery

目前已提供統一高階入口：

```cpp
DepthReportSet runDepthQuery(const DepthQuery& query) const;
```

支援的 query type：

| Query type | 用途 |
|---|---|
| `SpecificNet` | 分析單一 net 的最大 depth 與 critical path |
| `PrimaryOutputs` | 分析所有 primary output endpoints |
| `DffD` | 分析所有 DFF D-pin endpoints |
| `GlobalCriticalPath` | 找出 PO 與 DFF.D 中最深 timing endpoint |
| `EndpointsExceedingDepth` | 找出所有 depth 大於 threshold 的 timing endpoints |

核心資料結構：

```cpp
struct DepthQuery {
    DepthQueryType type = DepthQueryType::SpecificNet;
    std::string netName;
    int threshold = -1;
    bool includeCriticalPath = true;
};

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

## 8. DepthQuery 輸入欄位

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `DepthQueryType` | `SpecificNet` | 決定要執行哪一種 depth query |
| `netName` | `std::string` | `""` | `SpecificNet` 使用 |
| `threshold` | `int` | `-1` | `EndpointsExceedingDepth` 使用 |
| `includeCriticalPath` | `bool` | `true` | 是否保留 `criticalPath` |

`includeCriticalPath = false` 時：

```text
runDepthQuery() 會清空 reports 與 worst 內的 criticalPath。
這適合只需要 depth / endpoint metadata，不需要完整 path 的情況。
```

---

## 9. DepthReportSet 回傳欄位

| 欄位 | 意思 |
|---|---|
| `ok` | query 是否成功 |
| `message` | debug / LLM response 用的簡短訊息 |
| `type` | query type |
| `reports` | 查詢得到的一個或多個 endpoint report |
| `worst` | `reports` 中 depth 最大的 endpoint；GlobalCriticalPath 的主要結果 |
| `threshold` | query 使用的 depth 門檻 |
| `count` | `reports.size()` |

---

## 10. Query Type 對照

| DepthQueryType | 底層 helper | 主要讀取 |
|---|---|---|
| `SpecificNet` | `analyzeDepthToNet(netName)` | `reports[0]`, `worst.depth`, `worst.criticalPath` |
| `PrimaryOutputs` | `analyzePrimaryOutputDepths()` | `reports`, `worst` |
| `DffD` | `analyzeDffDDepths()` | `reports`, `worst` |
| `GlobalCriticalPath` | `findGlobalCriticalPath()` | `worst`, `reports[0]` |
| `EndpointsExceedingDepth` | `findEndpointsExceedingDepth(threshold)` | `reports`, `count`, `worst` |

---

## 11. 使用範例

### 11.1 單一 net depth

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::SpecificNet;
query.netName = "y";

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

```cpp
int depth = report.worst.depth;
Netlist::CombinationalPath path = report.worst.criticalPath;
```

---

### 11.2 所有 primary outputs depth

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::PrimaryOutputs;
query.includeCriticalPath = false;

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

```cpp
std::vector<Netlist::DepthReport> outputs = report.reports;
Netlist::DepthReport worstOutput = report.worst;
```

---

### 11.3 所有 DFF.D depth

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::DffD;

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

---

### 11.4 全設計 global critical path

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::GlobalCriticalPath;

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

```cpp
Netlist::DepthReport critical = report.worst;
```

---

### 11.5 找 depth 超標 endpoints

```cpp
Netlist::DepthQuery query;
query.type = Netlist::DepthQueryType::EndpointsExceedingDepth;
query.threshold = 4;

Netlist::DepthReportSet report = netlist.runDepthQuery(query);
```

讀取：

```cpp
for (const Netlist::DepthReport& endpoint : report.reports) {
    // endpoint.depth > 4
}
```

---

## 12. Prompt 對應表

| Prompt | 建議 API |
|---|---|
| What is the maximum logic depth to output y? | `DepthQuery::SpecificNet`, `netName = "y"` |
| Report the critical path to n10. | `DepthQuery::SpecificNet`, `includeCriticalPath = true` |
| What is the maximum output depth? | `DepthQuery::PrimaryOutputs`, read `worst` |
| Which outputs have depth greater than 4? | `DepthQuery::EndpointsExceedingDepth` then filter `PrimaryOutput`, or low-level `getPrimaryOutputsWithDepthGreaterThan(4)` |
| What is the maximum logic depth from any PI to any DFF D-pin? | `DepthQuery::DffD`, read `worst` |
| Find the global critical path. | `DepthQuery::GlobalCriticalPath` |
| Find endpoints exceeding target depth 4. | `DepthQuery::EndpointsExceedingDepth`, `threshold = 4` |

---

## 13. 和 PathQuery 的關係

`PathQuery` 和 `DepthQuery` 都可能出現 depth 字樣，但語意不同。

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
For each output depth > 4 optimize cone.
```

適合：

```text
DepthQuery::EndpointsExceedingDepth
```

---

## 14. 和 FunctionQuery 的關係

DepthQuery 不做功能常數判斷。

| 問題 | 正確 API |
|---|---|
| Is output n16 always 0? | `FunctionQuery::AlwaysZero` |
| Is output n16 non-constant? | `FunctionQuery::TruthStatus` |
| What is output n16 depth? | `DepthQuery::SpecificNet` |
| Which endpoints exceed depth 4? | `DepthQuery::EndpointsExceedingDepth` |

---

## 15. 和 Optimization 的關係

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

DepthQuery 只負責第 1 步與第 5 步的分析。

---

## 16. 目前限制

第一版 DepthAnalysis / DepthQuery 先不處理：

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

因為 Problem A 目前是 restricted primitive gate-level netlist，先採 unit-depth model 較適合。

---

## 17. 目前完成狀態

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
mini test 覆蓋所有 DepthQueryType
```

測試狀態：

```text
runDepthQuery() 已通過 mini tester。
後續編譯或連結測試時，要記得把 src/analysis/DepthAnalysis.cpp 加入編譯命令。
```
