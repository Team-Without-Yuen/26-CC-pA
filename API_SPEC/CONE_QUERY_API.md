# Cone Query API 整理

這份文件整理 `Netlist` 的 cone analysis API，也就是跨多層 reachable / transitive fanin / transitive fanout 查詢。

使用範例與 prompt 對應請看：

```text
API_SPEC/CONE_QUERY_USAGE.md
```

---

## 1. 核心定位

Cone Query 負責回答「從某個 net 或 gate 出發，往 fanin 或 fanout 方向跨多層可到達哪些 nets / gates」。

典型問題：

```text
Determine all gates reachable from n2.
Find all gates in the fanin cone of y.
List gates that can affect output y.
Count gates reachable from g0.
Find the transitive fanout cone of net n3.
Find the transitive fanin cone of gate g1.
Which output has the largest fanin cone?
How many outputs have more than 100 gates in their fanin cones?
```

這一層不做：

```text
單純物件資訊查詢 -> BasicQuery
直接相連查詢 -> DirectConnectivityQuery
指定 startpoint 到 endpoint 的 path search -> PathQuery
global timing depth / critical path -> DepthAnalysis
deepest output cone by logic depth -> DepthQuery
optimization rewrite
```

---

## 2. 圖模型與邊界

Cone traversal 採用：

```text
Net -> Gate -> Net
```

Fanin cone：

```text
從目標 net 往回追 driver gate，再追 driver gate 的 input nets。
```

Fanout cone：

```text
從起點 net 往前追 load gates，再追 load gate 的 output net。
```

Active object 與 edge consistency 規則：

```text
removed gate（GateType::UNKNOWN）與 removed net（isRemoved=true）不屬於 cone。
driver edge 必須同時滿足 net.driverGateId == gateId 與 gate.outputNetId == netId。
load edge 必須同時存在於 net.loadGateIds 與 gate.inputNetIds。
scalar/all-removed bus 來源視為不存在；partial-removed bus 只保留 active bit roots。
```

DFF 規則：

```text
DFF 是 sequential boundary。
Cone traversal 不穿越 DFF。
fanout cone 遇到 DFF input 時不會走到 DFF Q。
fanin cone 遇到 DFF Q 時不會走回 DFF D/CK/RN/SN。
```

---

## 3. ConeResult

底層 cone traversal 回傳 `ConeResult`：

```cpp
struct ConeResult {
    std::unordered_set<int> netIds;
    std::unordered_map<int, std::vector<int>> children;
    std::vector<int> rootNetIds;
};
```

欄位語意：

| 欄位 | 意思 |
|---|---|
| `netIds` | cone 內所有 net IDs |
| `children` | net-to-net traversal edge；`children[a] = {b, c}` 表示 cone traversal 從 `a` 走到 `b/c` |
| `rootNetIds` | cone 起點 net IDs；bus 可能有多個 roots |

注意：

```text
ConeResult 的 children 保存的是 net-to-net 關係，不直接保存 gate edge。
若要取得 cone 內 gate，需要從 net-to-net edge 反推參與的 gate。
```

---

## 4. 低階 Cone 建立 API

```cpp
ConeResult getTransitiveFaninCone(const std::string& netName) const;
ConeResult getTransitiveFanoutCone(const std::string& netName) const;
ConeResult getGateTransitiveFaninCone(const std::string& gateName) const;
ConeResult getGateTransitiveFanoutCone(const std::string& gateName) const;
```

用途：

| API | 用途 |
|---|---|
| `getTransitiveFaninCone(net)` | 從 net 往 fanin 方向追 |
| `getTransitiveFanoutCone(net)` | 從 net 往 fanout 方向追 |
| `getGateTransitiveFaninCone(gate)` | 從 gate output net 往 fanin 方向追 |
| `getGateTransitiveFanoutCone(gate)` | 從 gate output net 往 fanout 方向追 |

---

## 5. ConeResult Helper

目前提供通用 helper，方便把 `ConeResult` 整理成高階回傳格式：

```cpp
std::vector<int> getConeNetIds(const ConeResult& cone) const;
std::vector<std::string> getConeNetNames(const ConeResult& cone) const;
size_t getConeNetCount(const ConeResult& cone) const;

std::vector<int> getConeGateIds(const ConeResult& cone) const;
std::vector<std::string> getConeGateNames(const ConeResult& cone) const;
size_t getConeGateCount(const ConeResult& cone) const;
```

重要語意：

```text
getConeGateIds() 會從 ConeResult 的 net-to-net edge 反推真正參與 cone 的 combinational gates。
它不會把 DFF boundary 誤算進 gate set。
```

---

## 6. 既有 Wrapper

為了支援常見 prompt，保留這些 wrapper：

```cpp
std::vector<std::string> getTransitiveFaninConeGateNames(const std::string& netName) const;
size_t getTransitiveFaninConeGateCount(const std::string& netName) const;
std::vector<std::string> getTransitiveFanoutConeGateNames(const std::string& netName) const;
size_t getTransitiveFanoutConeGateCount(const std::string& netName) const;

std::vector<std::string> getGateTransitiveFaninConeGateNames(const std::string& gateName) const;
size_t getGateTransitiveFaninConeGateCount(const std::string& gateName) const;
std::vector<std::string> getGateTransitiveFanoutConeGateNames(const std::string& gateName) const;
size_t getGateTransitiveFanoutConeGateCount(const std::string& gateName) const;
```

這些 wrapper 目前已改為呼叫 `getConeGateNames()` / `getConeGateCount()`，語意與高階 `ConeQuery` 一致。

---

## 7. Cone 內長短路徑

Cone 內也提供局部長短路徑分析：

```cpp
std::pair<int, std::vector<int>> findLongestPathInCone(const ConeResult& cone) const;
std::pair<int, std::vector<int>> findShortestPathInCone(const ConeResult& cone) const;
```

Wrapper：

```cpp
getTransitiveFaninConeLongestPath(net)
getTransitiveFaninConeShortestPath(net)
getTransitiveFanoutConeLongestPath(net)
getTransitiveFanoutConeShortestPath(net)
getGateTransitiveFaninConeLongestPath(gate)
getGateTransitiveFaninConeShortestPath(gate)
getGateTransitiveFanoutConeLongestPath(gate)
getGateTransitiveFanoutConeShortestPath(gate)
```

注意：

```text
這是 cone 內 local path，不是指定 startpoint-to-endpoint path。
若題目明確要求 A 到 B 的路徑，應使用 PathQuery。
```

---

## 8. 高階 ConeQuery

目前已提供統一高階入口：

```cpp
ConeReport runConeQuery(const ConeQuery& query) const;
```

支援的 query type：

| Query type | 用途 |
|---|---|
| `NetTransitiveFanin` | 從指定 net 往 fanin 方向追 |
| `NetTransitiveFanout` | 從指定 net 往 fanout 方向追 |
| `GateTransitiveFanin` | 從指定 gate output net 往 fanin 方向追 |
| `GateTransitiveFanout` | 從指定 gate output net 往 fanout 方向追 |
| `LargestOutputCone` | 掃描所有 primary output bit，找 fanin cone gateCount 最大者 |
| `SharedFaninGates` | 直接計算兩個 net fanin cones 的 shared gate intersection |
| `OutputConeRanking` | 依完整 gate、filter 後 gate 或 net 數量排名所有 primary output bits |
| `OutputConeFilter` | 依完整 gate、filter 後 gate 或 net 數量的 predicate 篩選所有 primary output bits |

核心資料結構：

```cpp
struct ConeQuery {
    ConeQueryType type;
    std::string netName;
    std::string secondNetName;
    std::string gateName;
    std::vector<GateType> gateTypeFilters;
    ConeRankMetric rankMetric = ConeRankMetric::ScopeGateCount;
    ConeRankMode rankMode = ConeRankMode::Highest;
    size_t rankValue = 1;
    ConeMetricPredicate metricPredicate = ConeMetricPredicate::Equal;
    size_t metricValue = 0;
    size_t metricUpperValue = 0;
    bool includeIds = true;
    bool includeNames = true;
    bool includeGateDetails = false;
    bool includeLocalPaths = false;
};

struct ConeReport {
    bool ok = false;
    bool exists = false;
    std::string message;

    ConeQueryType type;
    std::string sourceName;
    int sourceId = -1;
    std::string secondSourceName;
    int secondSourceId = -1;
    ConeResult cone;

    size_t netCount = 0;
    size_t scopeGateCount = 0;
    size_t gateCount = 0;
    bool gateTypeFilterApplied = false;
    bool gateDetailsIncluded = false;
    std::vector<GateType> appliedGateTypeFilters;
    std::map<GateType, int> gateTypeCounts;
    size_t checkedOutputCount = 0;
    std::vector<int> rootNetIds;
    std::vector<std::string> rootNetNames;
    std::vector<int> netIds;
    std::vector<int> gateIds;
    std::vector<std::string> netNames;
    std::vector<std::string> gateNames;
    std::vector<GateConnectionSummary> gateConnections;
    ConeRankingReport rankingReport;
    ConeFilterReport filterReport;

    int longestDepth = -1;
    int shortestDepth = -1;
    std::vector<int> longestPathNetIds;
    std::vector<int> shortestPathNetIds;
    std::vector<std::string> longestPathNetNames;
    std::vector<std::string> shortestPathNetNames;
};
```

Gate-type filter 契約：

```text
1. gateTypeFilters 空集合表示不篩選；一個或多個 type 採 OR semantics。
2. 重複 type 會去重；GateType::UNKNOWN 是 invalid argument，query 回 ok=false。
3. cone/net payload 與 ConeResult 永遠保存完整 scope。
4. scopeGateCount 是篩選前 gate 數；gateCount、gateIds、gateNames、
   gateTypeCounts、gateConnections 都是篩選後結果。
5. includeGateDetails=true 時，gateConnections 重用 BasicQuery 的
   GateConnectionSummary，pin order、constant、PI/PO 與 bus-bit 語意一致。
6. filter 沒有 match 是成功的 valid-zero result，不是 source 不存在。
```

`LargestOutputCone` 回傳規則：

```text
1. sourceName/sourceId 是最大 fanin cone 的 primary output net。
2. cone/rootNetIds/netNames/gateNames 等欄位對應該 output 的 fanin cone。
3. output 選擇使用完整 cone 的 gate count，不受 gateTypeFilters 影響；回傳後才套 filter。
4. scopeGateCount 是選中 output 的完整 cone gate 數，gateCount 是 filter 後數量。
5. checkedOutputCount 表示實際掃描了多少個 active primary output bit。
6. 若多個 output 完整 cone gate count 相同，會以 netCount 較大者優先；仍相同時保留先遇到的 output。
```

`OutputConeRanking` 契約：

```text
1. rankMetric 支援 ScopeGateCount、FilteredGateCount、NetCount。
2. rankMode 支援 Highest、Lowest、NthHighest、NthLowest、Top、Bottom。
3. rankValue 必須大於 0；Highest/Lowest 固定使用 1。
4. rank 採 distinct metric level。Top/Bottom K 指 K 個 distinct levels，不是 K 筆 output。
5. 同一 level 的 outputs 全部保留，依 output name、net ID 穩定排序。
6. rankedOutputs 每筆只保存 output、rank 與三種 count 摘要，不複製完整 cone。
7. ConeReport 的 source/cone payload 對應 selected entries 的第一筆，方便後續查看代表 cone；
   完整排名必須讀 rankingReport.rankedOutputs。
8. 沒有 active PO 或要求的 Nth level 不存在，仍是 ok=true 的 valid-zero ranking，
   requestedRankExists=false；這不改變 LargestOutputCone 原本的 no-output error。
```

`OutputConeFilter` 契約：

```text
1. rankMetric 沿用 ScopeGateCount、FilteredGateCount、NetCount 三種 output cone metric。
2. metricPredicate 支援 Equal、NotEqual、GreaterThan、GreaterOrEqual、LessThan、
   LessOrEqual、BetweenInclusive。
3. BetweenInclusive 使用 metricValue/metricUpperValue，且 lower 不得大於 upper；其他
   predicate 只使用 metricValue，0 是合法值。
4. filter 會掃描全部 active primary output bits，matchedOutputs 完整保留所有 matches，
   並依 output name、net ID 穩定排序。
5. ConeFilterEntry 保存 output identity、metricValue 與三種 count 摘要，不複製完整 cone。
6. 這是 summary-only batch query；不支援 includeGateDetails/includeLocalPaths，也不填代表性
   source/cone payload。完整答案讀 filterReport.matchedOutputs。
7. 沒有 active PO 或沒有 match 都是 ok=true、exists=true、matchedOutputCount=0 的 valid-zero。
8. gateTypeFilters 只影響 FilteredGateCount；ScopeGateCount/NetCount 的 metric 不受 filter 影響。
9. OutputConeFilter 不是單一 rewrite scope，不得直接交給 edit/optimization engine。
```

`SharedFaninGates` 會分別建立兩個 transitive fanin cones，對排序後的 gate IDs 做 intersection，並回傳 shared `gateIds/gateNames/gateTypeCounts`。兩個名稱都合法但沒有交集時回 `ok=true`、`exists=true`、`gateCount=0`，不是來源不存在。`includeIds/includeNames` 只控制對應 payload，不影響成功狀態與 count。

---

## 9. 目前完成狀態

實作位置：

```text
include/core/NetlistQueries.h
src/analysis/ConeAnalysis.cpp
```

已完成：

```text
ConeResult 建立 API
ConeResult net/gate helper
Cone gate names/count wrapper
Cone local longest/shortest path wrapper（longest 使用 iterative post-order，避免深鏈 call-stack overflow）
ConeQuery / ConeReport 高階 API
LargestOutputCone
OutputConeRanking distinct-level ranking（含 ties、filter metric 與 valid-zero）
OutputConeFilter threshold/range selection（含完整 matches 與 valid-zero）
Cone gateTypeCounts
SharedFaninGates
Gate-type OR filter 與 scope/filter count 分離
GateConnectionSummary structured gate details
```

測試狀態：

```text
mini test/tester.cpp 已覆蓋 runConeQuery() 的 NetTransitiveFanin / NetTransitiveFanout / GateTransitiveFanin。
mini test/test6/test6.cpp 已覆蓋 GateTransitiveFanout、LargestOutputCone、OutputConeRanking、OutputConeFilter、tombstone/bus/stale driver、
none/one/many/duplicate/UNKNOWN gate-type filters、valid zero、structured details、SharedFanin filter、
empty/reconvergent/multi-root/cycle local path、100000-level iterative longest-path chain，並讀取
NewTestCase/test70 驗證大型 cone filter/detail consistency；目前 49/49 PASS。
```

CLI parser/help/printer 已同步 gate-type filters、structured details、output ranking 與 strict
required-operand 檢查；option 不得冒充 net/gate 名稱。`mini test/test49` 另驗證 parser、
299-output ranking/filter artifact、strict predicate grammar 與官方 test76 的 legacy/ranking/filter semantics。
