# Direct Connectivity Query 使用說明

這份文件只負責說明 `DirectConnectivityQuery` 怎麼使用。  
設計背景與分類整理請看：

```text
API_SPEC/DIRECT_CONNECTIVITY_QUERY_API.md
```

---

## 1. 基本概念

`DirectConnectivityQuery` 用來回答 gate/net 的直接連線問題。

基本形式：

```text
query.type + gateName/netName -> runDirectConnectivityQuery() -> DirectConnectivityReport
```

使用流程：

```text
1. 建立 Netlist::DirectConnectivityQuery query
2. 設定 query.type
3. 視 query.type 設定 query.gateName / query.netName
4. 視需求設定 query.includeIds / query.includeNames
5. 呼叫 netlist.runDirectConnectivityQuery(query)
6. 從 report.ok / report.connected / report.gateNames / report.netNames 讀結果
```

注意：

```text
DirectConnectivityQuery 只查 immediate connection。
不會往前或往後跨多層 traversal。
如果要找所有 reachable gates，應使用 Cone Query。
如果要找 startpoint 到 endpoint 的路徑，應使用 PathQuery。
```

---

## 2. Query Type 總表

| Query type | 問題類型 | 需要設定 | 主要讀取欄位 |
|---|---|---|---|
| `NetDriver` | 誰 drive 這條 net / bus | `netName` | `gateIds`, `gateNames`, `count` |
| `NetLoads` | 這條 net / bus load 到哪些 gates | `netName` | `gateIds`, `gateNames`, `count` |
| `GateInputs` | 這顆 gate 的 input nets | `gateName` | `netIds`, `netNames`, `count` |
| `GateOutput` | 這顆 gate 的 output net | `gateName` | `netId`, `netName`, `netIds`, `netNames` |
| `GateFanin` | 哪些 gates 直接餵進這顆 gate | `gateName` | `gateIds`, `gateNames`, `count` |
| `GateFanout` | 這顆 gate output 直接接到哪些 gates | `gateName` | `gateIds`, `gateNames`, `count` |
| `DirectlyConnected` | gate 和 net 是否直接相連 | `gateName`, `netName` | `connected`, `count` |

---

## 3. DirectConnectivityQuery 輸入欄位

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `DirectConnectivityQueryType` | `NetDriver` | 決定查詢類型 |
| `gateName` | `std::string` | `""` | gate 相關 query 使用 |
| `netName` | `std::string` | `""` | net 相關 query 使用 |
| `includeIds` | `bool` | `true` | 是否填 `gateIds` / `netIds` |
| `includeNames` | `bool` | `true` | 是否填 `gateNames` / `netNames` |

---

## 4. DirectConnectivityReport 回傳欄位

| 欄位 | 意思 |
|---|---|
| `ok` | 查詢是否成功 |
| `exists` | 指定 gate/net 是否存在 |
| `connected` | `DirectlyConnected` 的主要布林結果 |
| `message` | debug / LLM response 用的簡短訊息 |
| `gateName` | 查詢指定或解析出的 gate name |
| `netName` | 查詢指定或解析出的 net name |
| `gateId` | 單一 gate ID 結果，沒有唯一 gate 時為 -1 |
| `netId` | 單一 net ID 結果，沒有唯一 net 時為 -1 |
| `count` | 結果數量 |
| `gateIds` | 查詢結果中的 gate IDs |
| `netIds` | 查詢結果中的 net IDs |
| `gateNames` | 查詢結果中的 gate names |
| `netNames` | 查詢結果中的 net names |

---

## 5. NetDriver

用途：

```text
查某個 net / bus 的直接 driver gate。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::NetDriver;
query.netName = "n1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| driver gate names | `report.gateNames` |
| driver gate IDs | `report.gateIds` |
| driver 數量 | `report.count` |

---

## 6. NetLoads

用途：

```text
查某個 net / bus 直接 load 到哪些 gates。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::NetLoads;
query.netName = "n1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| load gate names | `report.gateNames` |
| load gate IDs | `report.gateIds` |
| load 數量 | `report.count` |

---

## 7. GateInputs

用途：

```text
查某個 gate 的直接 input nets。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::GateInputs;
query.gateName = "g1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| input net names | `report.netNames` |
| input net IDs | `report.netIds` |
| input 數量 | `report.count` |

---

## 8. GateOutput

用途：

```text
查某個 gate 的 output net。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::GateOutput;
query.gateName = "g1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| output net name | `report.netName` 或 `report.netNames` |
| output net ID | `report.netId` 或 `report.netIds` |

---

## 9. GateFanin

用途：

```text
查直接驅動某個 gate input nets 的上一層 gates。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::GateFanin;
query.gateName = "g1";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

注意：

```text
這是 immediate fanin，不是 transitive fanin cone。
PI 或 constant input 沒有 driver gate，會被略過。
```

---

## 10. GateFanout

用途：

```text
查某個 gate output net 直接 fanout 到哪些 gates。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::GateFanout;
query.gateName = "g0";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

這可以回答：

```text
Report every gate connected to the output of g0.
```

---

## 11. DirectlyConnected

用途：

```text
判斷指定 gate 和指定 net 是否直接相連。
```

寫法：

```cpp
Netlist::DirectConnectivityQuery query;
query.type = Netlist::DirectConnectivityQueryType::DirectlyConnected;
query.gateName = "g1";
query.netName = "n2";

Netlist::DirectConnectivityReport report =
    netlist.runDirectConnectivityQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否直接相連 | `report.connected` |
| 查詢是否成功 | `report.ok` |

語意：

```text
若 net 是 gate 的任一 input 或 output，connected=true。
```

---

## 12. Prompt 對應表

| Prompt | 建議 Query type | 需要設定 | 主要讀取 |
|---|---|---|---|
| Which gate drives net n1? | `NetDriver` | `netName = "n1"` | `gateNames` |
| Report every gate connected to net n1. | `NetLoads` | `netName = "n1"` | `gateNames` |
| What are the input nets of g1? | `GateInputs` | `gateName = "g1"` | `netNames` |
| What is the output net of g1? | `GateOutput` | `gateName = "g1"` | `netName` |
| Which gates feed g1? | `GateFanin` | `gateName = "g1"` | `gateNames` |
| Report every gate connected to the output of g0. | `GateFanout` | `gateName = "g0"` | `gateNames` |
| Is g1 directly connected to n2? | `DirectlyConnected` | `gateName = "g1"`, `netName = "n2"` | `connected` |

---

## 13. 何時不要用 DirectConnectivityQuery

| 問題 | 應改用 |
|---|---|
| Find all gates reachable from n2. | Cone / Fanout Query |
| Find all gates in fanin cone of y. | Cone / Fanin Query |
| Is there a path from a to y? | PathQuery |
| Find shortest path from a to y. | PathQuery |
| What is the maximum depth to output y? | DepthAnalysis |

---

## 14. 低階 Helper 對照

| Query type | 底層 helper |
|---|---|
| `NetDriver` | `getNetDriverGateIds()`, `getNetDriverGateNames()` |
| `NetLoads` | `getNetLoadGateIds()`, `getNetLoadGateNames()`, `getNetLoadGateCount()` |
| `GateInputs` | `getGateInputNetIds()`, `getGateInputNetNames()` |
| `GateOutput` | `getGateOutputNetId()`, `getGateOutputNetName()` |
| `GateFanin` | `getGateFaninGateIds()`, `getGateFaninGateNames()`, `getGateFaninGateCount()` |
| `GateFanout` | `getGateFanout()`, `getGateFanoutNames()`, `getGateFanoutCount()` |
| `DirectlyConnected` | `isGateDirectlyConnectedToNet()`, `isNetDirectlyConnectedToGate()` |
