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

核心資料結構：

```cpp
struct ConeQuery {
    ConeQueryType type;
    std::string netName;
    std::string secondNetName;
    std::string gateName;
    bool includeIds = true;
    bool includeNames = true;
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
    size_t gateCount = 0;
    std::map<GateType, int> gateTypeCounts;
    size_t checkedOutputCount = 0;
    std::vector<int> rootNetIds;
    std::vector<std::string> rootNetNames;
    std::vector<int> netIds;
    std::vector<int> gateIds;
    std::vector<std::string> netNames;
    std::vector<std::string> gateNames;

    int longestDepth = -1;
    int shortestDepth = -1;
    std::vector<int> longestPathNetIds;
    std::vector<int> shortestPathNetIds;
    std::vector<std::string> longestPathNetNames;
    std::vector<std::string> shortestPathNetNames;
};
```

`LargestOutputCone` 回傳規則：

```text
1. sourceName/sourceId 是最大 fanin cone 的 primary output net。
2. cone/rootNetIds/netNames/gateNames 等欄位對應該 output 的 fanin cone。
3. gateCount 使用 getConeGateCount()，只計算有效 combinational gates，不計 DFF boundary。
4. checkedOutputCount 表示掃描了多少個 primary output bit。
5. 若多個 output gateCount 相同，會以 netCount 較大者優先；仍相同時保留先遇到的 output。
```

`SharedFaninGates` 會分別建立兩個 transitive fanin cones，對排序後的 gate IDs 做 intersection，並回傳 shared `gateIds/gateNames/gateTypeCounts`。兩個名稱都合法但沒有交集時是成功的零結果，不是錯誤。

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
Cone local longest/shortest path wrapper
ConeQuery / ConeReport 高階 API
LargestOutputCone
Cone gateTypeCounts
SharedFaninGates
```

測試狀態：

```text
mini test/tester.cpp 已覆蓋 runConeQuery() 的 NetTransitiveFanin / NetTransitiveFanout / GateTransitiveFanin。
mini test/test6/test6.cpp 已覆蓋 LargestOutputCone。
```

後續可補：

```text
1. 在 tester 補 GateTransitiveFanout 的高階 query case。
2. 增加 scope-aware cone query，供 transformation / optimization 限定修改範圍。
3. 若遇到超大 cone，可加入 traversal budget 或 result limit。
```
