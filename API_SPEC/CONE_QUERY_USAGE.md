# Cone Query 使用說明

這份文件只負責說明 `ConeQuery` 怎麼使用。  
設計背景與分類整理請看：

```text
API_SPEC/CONE_QUERY_API.md
```

---

## 1. 基本概念

`ConeQuery` 用來回答「從某個 net 或 gate 出發，跨多層往 fanin / fanout 方向能到哪些 nets/gates」，也能掃描所有 output 找出 fanin cone 最大者。

基本形式：

```text
query.type + optional netName/gateName -> runConeQuery() -> ConeReport
```

使用流程：

```text
1. 建立 Netlist::ConeQuery query
2. 設定 query.type
3. 視 query.type 設定 query.netName 或 query.gateName
4. 視需求設定 query.includeIds / query.includeNames / query.includeLocalPaths
5. 呼叫 netlist.runConeQuery(query)
6. 從 report.netNames / report.gateNames / report.netCount / report.gateCount 讀結果
```

注意：

```text
ConeQuery 是 transitive traversal。
DirectConnectivityQuery 只查直接相連。
PathQuery 查指定 startpoint 到 endpoint 的路徑。
DFF 是 sequential boundary，ConeQuery 不穿越 DFF。
```

---

## 2. Query Type 總表

| Query type | 問題類型 | 需要設定 | 主要讀取欄位 |
|---|---|---|---|
| `NetTransitiveFanin` | 從 net 往回找能影響它的 cone | `netName` | `netNames`, `gateNames`, `netCount`, `gateCount` |
| `NetTransitiveFanout` | 從 net 往前找能被它影響的 cone | `netName` | `netNames`, `gateNames`, `netCount`, `gateCount` |
| `GateTransitiveFanin` | 從 gate output 往回找 fanin cone | `gateName` | `netNames`, `gateNames`, `netCount`, `gateCount` |
| `GateTransitiveFanout` | 從 gate output 往前找 fanout cone | `gateName` | `netNames`, `gateNames`, `netCount`, `gateCount` |
| `LargestOutputCone` | 找 fanin cone gateCount 最大的 primary output | 無 | `sourceName`, `gateCount`, `gateNames`, `checkedOutputCount` |
| `SharedFaninGates` | 找兩個 net fanin cones 的共有 gates | `netName`, `secondNetName` | `gateNames`, `gateCount`, `gateTypeCounts` |

---

## 3. ConeQuery 輸入欄位

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `ConeQueryType` | `NetTransitiveFanin` | 決定 cone 來源與方向 |
| `netName` | `std::string` | `""` | net 類 query 使用 |
| `secondNetName` | `std::string` | `""` | `SharedFaninGates` 的第二個 net |
| `gateName` | `std::string` | `""` | gate 類 query 使用 |
| `includeIds` | `bool` | `true` | 是否填 `rootNetIds`, `netIds`, `gateIds` |
| `includeNames` | `bool` | `true` | 是否填 `rootNetNames`, `netNames`, `gateNames` |
| `includeLocalPaths` | `bool` | `false` | 是否附帶 cone 內 longest/shortest net path |

---

## 4. ConeReport 回傳欄位

| 欄位 | 意思 |
|---|---|
| `ok` | 查詢是否成功 |
| `exists` | 指定來源是否存在並成功建立 cone |
| `message` | debug / LLM response 用的簡短訊息 |
| `type` | query type |
| `sourceName` | 使用者指定的來源名稱 |
| `sourceId` | 來源 net ID 或 gate ID |
| `secondSourceName` / `secondSourceId` | shared fanin query 的第二個來源 |
| `cone` | 原始 `ConeResult` |
| `netCount` | cone 內有效 net 數量 |
| `gateCount` | cone 內有效 combinational gate 數量 |
| `gateTypeCounts` | cone 或 shared gate 集合內各 gate type 數量 |
| `checkedOutputCount` | `LargestOutputCone` 掃描的 primary output bit 數量 |
| `rootNetIds` | cone root net IDs |
| `rootNetNames` | cone root net names |
| `netIds` | cone 內 net IDs |
| `gateIds` | cone 內 gate IDs |
| `netNames` | cone 內 net names |
| `gateNames` | cone 內 gate names |
| `longestDepth` | cone 內 local longest path depth |
| `shortestDepth` | cone 內 local shortest path depth |
| `longestPathNetNames` | cone 內 local longest path 的 net names |
| `shortestPathNetNames` | cone 內 local shortest path 的 net names |

通用存在性與連線規則：

```text
removed gate/net 視為不存在，不會出現在 root、count 或 payload。
bus 只有至少一個 active bit 時才存在；partial-removed bus 只走訪 active bits。
cone 只沿著 net cache 與 gate pin 兩端一致的 driver/load edge traversal。
includeIds/includeNames 只控制 payload，不改變 count、ok 或 exists。
```

---

## 5. NetTransitiveFanin

用途：

```text
從指定 net 往 fanin 方向追，找所有可能影響它的 upstream nets/gates。
```

寫法：

```cpp
Netlist::ConeQuery query;
query.type = Netlist::ConeQueryType::NetTransitiveFanin;
query.netName = "y";

Netlist::ConeReport report = netlist.runConeQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| fanin cone gate names | `report.gateNames` |
| fanin cone net names | `report.netNames` |
| gate 數量 | `report.gateCount` |
| net 數量 | `report.netCount` |

---

## 6. NetTransitiveFanout

用途：

```text
從指定 net 往 fanout 方向追，找所有可能被它影響的 downstream nets/gates。
```

寫法：

```cpp
Netlist::ConeQuery query;
query.type = Netlist::ConeQueryType::NetTransitiveFanout;
query.netName = "n2";

Netlist::ConeReport report = netlist.runConeQuery(query);
```

這可以回答：

```text
Determine all gates reachable from n2.
```

---

## 7. GateTransitiveFanin

用途：

```text
從指定 gate 的 output net 往 fanin 方向追。
```

寫法：

```cpp
Netlist::ConeQuery query;
query.type = Netlist::ConeQueryType::GateTransitiveFanin;
query.gateName = "g1";

Netlist::ConeReport report = netlist.runConeQuery(query);
```

注意：

```text
這裡的 gate fanin cone 是以 gate output net 作為 root。
```

---

## 8. GateTransitiveFanout

用途：

```text
從指定 gate 的 output net 往 fanout 方向追。
```

寫法：

```cpp
Netlist::ConeQuery query;
query.type = Netlist::ConeQueryType::GateTransitiveFanout;
query.gateName = "g1";

Netlist::ConeReport report = netlist.runConeQuery(query);
```

---

## 9. includeLocalPaths

若想同時取得 cone 內 local longest / shortest net path：

```cpp
Netlist::ConeQuery query;
query.type = Netlist::ConeQueryType::NetTransitiveFanin;
query.netName = "y";
query.includeLocalPaths = true;

Netlist::ConeReport report = netlist.runConeQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 最長路徑深度 | `report.longestDepth` |
| 最長路徑 net names | `report.longestPathNetNames` |
| 最短路徑深度 | `report.shortestDepth` |
| 最短路徑 net names | `report.shortestPathNetNames` |

注意：

```text
這是 cone 內 local path。
若要指定 A 到 B 的路徑，使用 PathQuery。
```

---

## 10. LargestOutputCone

用途：

```text
掃描所有 primary output bit，找出 fanin cone gateCount 最大的 output。
```

寫法：

```cpp
Netlist::ConeQuery query;
query.type = Netlist::ConeQueryType::LargestOutputCone;

Netlist::ConeReport report = netlist.runConeQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 哪個 output cone 最大 | `report.sourceName` |
| 最大 cone 有幾個 gate | `report.gateCount` |
| 最大 cone 內有哪些 gates | `report.gateNames` |
| 總共掃了多少個 PO bit | `report.checkedOutputCount` |

注意：

```text
LargestOutputCone 是用 fanin cone 的 combinational gateCount 判斷大小。
如果題目問 deepest fanin logic cone，應使用 DepthQuery::DeepestOutputCone。
```

### 10.1 SharedFaninGates

用途：直接取得兩個 fanin cones 的 gate intersection，不需要呼叫端自行比對兩份 gate list。

```cpp
Netlist::ConeQuery query;
query.type = Netlist::ConeQueryType::SharedFaninGates;
query.netName = "n16";
query.secondNetName = "n17";

Netlist::ConeReport report = netlist.runConeQuery(query);
```

`report.ok=true`、`report.exists=true` 且 `report.gateCount=0` 表示兩個合法 cones 沒有共有 gate；名稱不存在才是 query error。即使結果為空，`includeIds/includeNames` 仍只控制相應 root/gate payload。

---

## 11. Prompt 對應表

| Prompt | 建議 Query type | 需要設定 | 主要讀取 |
|---|---|---|---|
| Determine all gates reachable from n2. | `NetTransitiveFanout` | `netName = "n2"` | `gateNames` |
| Find all gates in the fanin cone of y. | `NetTransitiveFanin` | `netName = "y"` | `gateNames` |
| List gates that can affect output y. | `NetTransitiveFanin` | `netName = "y"` | `gateNames` |
| Count gates reachable from g0. | `GateTransitiveFanout` | `gateName = "g0"` | `gateCount` |
| Find the fanin cone of gate g1. | `GateTransitiveFanin` | `gateName = "g1"` | `gateNames`, `netNames` |
| Find the fanout cone of gate g1. | `GateTransitiveFanout` | `gateName = "g1"` | `gateNames`, `netNames` |
| What is the longest path inside the fanin cone of y? | `NetTransitiveFanin` | `netName = "y"`, `includeLocalPaths = true` | `longestDepth`, `longestPathNetNames` |
| Which output has the largest fanin cone? | `LargestOutputCone` | 無 | `sourceName`, `gateCount` |
| Report all gates shared between the fanin cones of n16 and n17. | `SharedFaninGates` | `netName = "n16"`, `secondNetName = "n17"` | `gateNames`, `gateCount` |
| Report the number of each gate type in the cone of n8. | `NetTransitiveFanin` | `netName = "n8"` | `gateTypeCounts` |

---

## 12. 何時不要用 ConeQuery

| 問題 | 應改用 |
|---|---|
| How many gates are in the design? | BasicQuery |
| Which gate directly drives n1? | DirectConnectivityQuery |
| What are g1's input nets? | DirectConnectivityQuery |
| Is there a path from a to y? | PathQuery |
| Find shortest path from a to y avoiding g3. | PathQuery |
| What is the maximum depth to output y? | DepthAnalysis |
| Which output bit has the deepest fanin logic cone? | DepthQuery::DeepestOutputCone |

---

## 13. 低階 Helper 對照

| Query type | 底層 helper |
|---|---|
| `NetTransitiveFanin` | `getTransitiveFaninCone()` |
| `NetTransitiveFanout` | `getTransitiveFanoutCone()` |
| `GateTransitiveFanin` | `getGateTransitiveFaninCone()` |
| `GateTransitiveFanout` | `getGateTransitiveFanoutCone()` |
| `LargestOutputCone` | `getPrimaryOutputNetIds()` + `getTransitiveFaninCone()` + `getConeGateCount()` |
| result net names/count | `getConeNetNames()`, `getConeNetCount()` |
| result gate names/count | `getConeGateNames()`, `getConeGateCount()` |
| local longest/shortest path | `findLongestPathInCone()`, `findShortestPathInCone()` |

---

## 14. 目前實作與測試狀態

```text
實作檔案：src/analysis/ConeAnalysis.cpp
型別檔案：include/core/NetlistQueries.h
tester：mini test/tester.cpp, mini test/test6/test6.cpp
目前 test6：Summary: 16 passed, 0 failed.
覆蓋 active/tombstone、partial/all-removed bus、stale driver、SharedFanin flags、
reconvergent/multi-root/cycle 與 100000-level iterative longest path。
```
