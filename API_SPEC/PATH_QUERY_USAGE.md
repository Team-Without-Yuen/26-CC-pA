# Path Query 使用說明

這份文件只負責說明 `PathQuery` 怎麼使用。  
設計背景與整理筆記請看：

```text
API_SPEC/PATH_QUERY_API.md
```

---

## 1. 基本概念

`PathQuery` 用來回答 endpoints 間的組合邏輯 path、reachability、mandatory node 與 separator/cut。register-to-register 使用 DffQ/DffD endpoint preset；cut/articulation 不再要求 LLM 選擇另一個 Graph tool。

基本形式：

```text
startpoints -> combinational graph -> endpoints
```

使用流程：

```text
1. 建立 Netlist::PathQuery query
2. 設定 query.mode
3. 設定 query.startpoints
4. 設定 query.endpoints
5. 視需求設定 query.requiredNodes / query.avoidedNodes
6. 呼叫 netlist.runPathQuery(query)
7. 從 result.exists / result.depth / result.path / result.paths 讀結果
```

注意：

```text
目前 PathQuery 只支援 combinationalOnly=true。
DFF 是 sequential boundary，路徑不會從 DFF input 穿越到 DFF Q。
```

---

## 2. Startpoint 類型

Startpoint 放在：

```cpp
query.startpoints
```

可放一個或多個。每個 startpoint 都會自動解析成實際 net ID。

| Startpoint type | 寫法 | name 的意思 | pinName / pinIndex | 用途 |
|---|---|---|---|---|
| `SpecificNet` | `PathEndpoint(SpecificNet, "n1")` | net name | 不使用 | 從指定 net 出發 |
| `PrimaryInput` | `PathEndpoint(PrimaryInput, "a")` | PI port name | 不使用 | 從指定 PI 出發 |
| `PrimaryInput` | `PathEndpoint(PrimaryInput, "")` | 空字串代表全部 PI | 不使用 | 從所有 PI 出發 |
| `DffQ` | `PathEndpoint(DffQ, "ff1")` | DFF instance name | 不使用 | 從 DFF Q-pin 出發 |
| `DffQ` | `PathEndpoint(DffQ, "")` | 空字串或 `"*"` | 不使用 | 從所有 DFF Q-pin 出發 |
| `GateOutput` | `PathEndpoint(GateOutput, "g1")` | gate instance name | 不使用 | 從 gate output net 出發 |
| `DffClock` | `PathEndpoint(DffClock, "ff1")` | DFF instance name | 預設 `CK` | 從 DFF clock pin net 出發，偏 control path |
| `DffReset` | `PathEndpoint(DffReset, "ff1", "RN")` | DFF instance name | 指定 `RN` 或 `SN` | 從 reset/set pin net 出發，偏 control path |

常用例子：

```cpp
// 從 net n1 出發
Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "n1")

// 從 primary input a 出發
Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryInput, "a")

// 從所有 primary inputs 出發
Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryInput, "")

// 從 DFF ff1 的 Q 出發
Netlist::PathEndpoint(Netlist::PathEndpointType::DffQ, "ff1")

// 從 gate g1 的 output net 出發
Netlist::PathEndpoint(Netlist::PathEndpointType::GateOutput, "g1")
```

---

## 3. Endpoint 類型

Endpoint 放在：

```cpp
query.endpoints
```

可放一個或多個。每個 endpoint 也會自動解析成實際 net ID。

| Endpoint type | 寫法 | name 的意思 | pinName / pinIndex | 用途 |
|---|---|---|---|---|
| `SpecificNet` | `PathEndpoint(SpecificNet, "y")` | net name | 不使用 | 到指定 net 結束 |
| `PrimaryOutput` | `PathEndpoint(PrimaryOutput, "y")` | PO port name | 不使用 | 到指定 PO 結束 |
| `PrimaryOutput` | `PathEndpoint(PrimaryOutput, "")` | 空字串代表全部 PO | 不使用 | 到所有 PO 中任一個 |
| `DffD` | `PathEndpoint(DffD, "ff1")` | DFF instance name | 預設 `D` | 到 DFF D-pin 結束 |
| `DffD` | `PathEndpoint(DffD, "")` | 空字串或 `"*"` | 預設 `D` | 到所有 DFF D-pin 結束 |
| `DffClock` | `PathEndpoint(DffClock, "ff1")` | DFF instance name | 預設 `CK` | 到 DFF clock pin 結束 |
| `DffReset` | `PathEndpoint(DffReset, "ff1")` | DFF instance name | 空字串代表嘗試 `RN` 和 `SN` | 到 DFF reset/set pin 結束 |
| `DffReset` | `PathEndpoint(DffReset, "ff1", "SN")` | DFF instance name | 指定 `SN` | 到指定 reset/set pin 結束 |
| `GateInput` | `PathEndpoint(GateInput, "g1", "", 0)` | gate instance name | input index | 到 gate 的指定 input pin net |
| `GateInput` | `PathEndpoint(GateInput, "ff1", "D")` | gate/DFF instance name | named pin | 到 named input pin net |
| `GateOutput` | `PathEndpoint(GateOutput, "g1")` | gate instance name | 不使用 | 到 gate output net 結束 |

常用例子：

```cpp
// 到 net y
Netlist::PathEndpoint(Netlist::PathEndpointType::SpecificNet, "y")

// 到 primary output y
Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryOutput, "y")

// 到所有 primary outputs 中任一個
Netlist::PathEndpoint(Netlist::PathEndpointType::PrimaryOutput, "")

// 到 DFF ff1 的 D pin
Netlist::PathEndpoint(Netlist::PathEndpointType::DffD, "ff1")

// 到 gate g1 的第 0 個 input
Netlist::PathEndpoint(Netlist::PathEndpointType::GateInput, "g1", "", 0)
```

---

## 4. Mode 類型

Mode 放在：

```cpp
query.mode
```

| Mode | 回答的問題 | 主要輸出 | 注意事項 |
|---|---|---|---|
| `Exists` | 是否存在至少一條合法路徑？ | `result.exists` | 最快的存在性檢查 |
| `FindAny` | 找任意一條合法路徑 | `result.path`, `result.depth` | 底層用 BFS，通常會找到較短的一條 |
| `EnumerateAll` | 列出所有合法路徑 | `result.paths`, `result.pathCount` | 預設會將完整結果寫檔 |
| `MinDepth` | 找最短 logic depth 路徑 | `result.path`, `result.depth` | depth = combinational gate 數量 |
| `MaxDepth` | 找最長 logic depth 路徑 | `result.path`, `result.depth` | 可用於 critical path 類問題 |
| `EveryPathThrough` | 所有既有路徑是否都經過 requiredNodes？ | `result.exists` | 沒有任何原始路徑時回 false |
| `EveryPathAvoids` | 所有既有路徑是否都避開 avoidedNodes？ | `result.exists` | 沒有任何原始路徑時回 false |
| `DirectPiPoConnections` | 找 PI/PO 共用同一 net 的 depth-0 連線 | `result.paths`, `result.pathCount` | 不需要 startpoints/endpoints，也不做全路徑列舉 |

常用寫法：

```cpp
query.mode = Netlist::PathQueryMode::Exists;
query.mode = Netlist::PathQueryMode::FindAny;
query.mode = Netlist::PathQueryMode::EnumerateAll;
query.mode = Netlist::PathQueryMode::MinDepth;
query.mode = Netlist::PathQueryMode::MaxDepth;
query.mode = Netlist::PathQueryMode::EveryPathThrough;
query.mode = Netlist::PathQueryMode::EveryPathAvoids;
query.mode = Netlist::PathQueryMode::FindMandatoryNodes;
query.mode = Netlist::PathQueryMode::IsSeparator;
query.mode = Netlist::PathQueryMode::DirectPiPoConnections;
```

---

## 5. requiredNodes 與 avoidedNodes

`requiredNodes` 和 `avoidedNodes` 使用 `PathNode`。

```cpp
Netlist::PathNode(Netlist::PathNodeType::Net, "n3")
Netlist::PathNode(Netlist::PathNodeType::Gate, "g5")
```

| 欄位 | 用途 | 語意 |
|---|---|---|
| `requiredNodes` | 指定必經點 | 路徑必須經過所有 required nodes |
| `avoidedNodes` | 指定避開點 | 路徑不能碰到任何 avoided node |

例子：

```cpp
query.requiredNodes.push_back(Netlist::PathNode(
    Netlist::PathNodeType::Gate, "g1"));

query.avoidedNodes.push_back(Netlist::PathNode(
    Netlist::PathNodeType::Net, "n3"));
```

注意：

```text
PathNode 進入 API 後會自動解析成 net ID 或 gate ID。
使用者不需要自己先呼叫 getNetId() 或 getGateId()。
```

---

## 6. PathQueryResult

`runPathQuery()` 回傳：

```cpp
Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

| 欄位 | 用途 |
|---|---|
| `ok` | request 是否有效並完成 dispatch |
| `unsupported` | request 合法但設定目前不支援 |
| `message` | success、no-path 或 validation error 語意 |
| `status` | separator/mandatory mode 的精確分類 |
| `exists` | 是否找到路徑，或 every-path 條件是否成立 |
| `pathExists` | mandatory/separator endpoints 是否原本相連 |
| `isSeparator` | 指定 candidate 是否為 directed separator |
| `mandatoryNetIds` / `mandatoryNetNames` | endpoints 間所有 paths 共同經過的 internal nets |
| `witnessStartpoint` / `witnessEndpoint` | PI-to-PO cut 成立時的 witness pair |
| `depth` | `path` 的 logic depth；沒找到時為 `-1` |
| `path` | 單一路徑，供 `FindAny` / `MinDepth` / `MaxDepth` 使用 |
| `paths` | 多條路徑，供 `EnumerateAll` 使用 |
| `pathCount` | `EnumerateAll` 的完整路徑數 |
| `wrotePathsToFile` | 是否已將完整路徑列表寫到檔案 |
| `outputFilePath` | 實際輸出檔案路徑 |
| `completeEnumeration` | 是否完整列舉，未截斷 |
| `enumerationTimedOut` | `EnumerateAll` 是否因 wall-clock time limit 停止 |
| `enumerationPathLimitReached` | `EnumerateAll` 是否因 `maxEnumeratedPaths` 停止 |
| `countOnly` | 是否只計數、不保存每條 path |
| `enumerationStopReason` | 若列舉被截斷，記錄停止原因 |
| `unresolvedStartpoints` / `unresolvedEndpoints` | 無法解析的 endpoint descriptions |
| `unresolvedRequiredNodes` / `unresolvedAvoidedNodes` | 無法解析的 constraint nodes |

名稱錯誤時 `ok=false` 並填 unresolved 欄位；名稱都合法但沒有 path 時 `ok=true, exists=false`。呼叫端應先檢查 `ok`，不能直接把所有 `exists=false` 都翻成「沒有路徑」。

`path` 裡面存的是 ID：

```cpp
path.netIds
path.gateIds
```

輸出名稱時要轉：

```cpp
for (int netId : result.path.netIds) {
    std::cout << netlist.getNet(netId).name << "\n";
}

for (int gateId : result.path.gateIds) {
    std::cout << netlist.getGate(gateId).instName << "\n";
}
```

---

## 7. EnumerateAll 自動寫檔與時間限制

大型 testcase 若要求完整列出所有 paths，`EnumerateAll` 會預設寫出完整 path list，避免 terminal 輸出過大。
若沒有指定 `outputFilePath`，預設輸出到 `path_enumeration_output.txt`。

官方時限中 basic operation request 為 60 秒，其他 request 為 300 秒。因此 `EnumerateAll` 目前主要依靠 wall-clock time limit 控制執行時間：

```text
query.enumerationTimeLimitSeconds = 55.0;
query.countOnly = false;
```

語意如下：

| 欄位 | 預設值 | 用途 |
|---|---:|---|
| `maxEnumeratedPaths` | legacy | 保留相容欄位；目前不作為 `EnumerateAll` 截斷條件 |
| `enumerationTimeLimitSeconds` | `55.0` | `EnumerateAll` 的 wall-clock 上限；`<=0` 表示不限制 |
| `countOnly` | `false` | true 時只更新 `pathCount`，不保存/輸出每條 path |

若列舉被時間限制停止：

```text
result.completeEnumeration = false
result.enumerationTimedOut = true
result.enumerationStopReason 會說明停止原因
```

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::EnumerateAll;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::PrimaryInput, "n0"));
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::PrimaryOutput, "n12"));
query.outputFilePath = "paths_output.txt";  // 可省略；省略時使用預設檔名
query.maxPrintedPaths = 20;                 // 只影響 CLI / report 顯示，不影響完整檔案
query.enumerationTimeLimitSeconds = 55.0;   // basic operation 建議保守設 55 秒

Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

結果讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 完整 path 數 | `result.pathCount` |
| 是否寫檔成功 | `result.wrotePathsToFile` |
| 輸出檔案 | `result.outputFilePath` |
| 是否完整 enumerate | `result.completeEnumeration` |
| 是否因時間停止 | `result.enumerationTimedOut` |
| 是否因數量上限停止 | `result.enumerationPathLimitReached`，目前應維持 false |
| 截斷原因 | `result.enumerationStopReason` |

輸出檔格式：

```text
Total paths: 123

Path 0
  depth: 3
  nets: a -> n1 -> y
  gates: g1 -> g2
```

目前版本若 `writePathsToFile=true` 且 `countOnly=false`，會在 DFS 過程中 streaming 寫入檔案，`result.paths` 只保留 terminal 顯示所需的前 `maxPrintedPaths` 條 sample。若 prompt 只問數量，建議設定 `countOnly=true`，可避免輸出大型 path file。

Enumeration 內部會對每個 end net 先做 reverse reachability pruning：從 endpoint 反向標記所有能在 combinational boundary 內抵達 endpoint 的 nets，DFS 只會進入這些可達分支。這不改變 path 定義或輸出格式，但可避免在大型 fanout cone 中反覆探索不可能到達目標 endpoint 的分支。

若 `countOnly=true` 且沒有 required nodes，會啟用 memoized path-count DP：對固定 end net 計算每個 upstream net 到 endpoint 的 path count，並在同一個 query 中跨多個 startpoint 重用結果。若偵測到 combinational cycle，會退回原本 DFS count，以維持 simple-path enumeration 語意。

已驗證案例：

```text
NewTestCase/test14, net:n0[0] -> net:n63[1]
count_only: Total paths = 289366, complete = true, 約 0.4 秒
streaming -out: Total paths = 289366, complete = true, 約 1.7 秒，輸出檔約 226 MB

NewTestCase/test37, all_dff_q -> all_dff_d
count_only: Total paths = 16548172, complete = true, 約 1.2-1.7 秒
```

---

## 8. 使用範例

### 8.0 找所有 depth-0 PI-to-PO direct connections

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::DirectPiPoConnections;
Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

CLI：

```text
path_query direct_pi_po
```

每筆 `result.paths[i]` 只含一個同時為 PI 與 PO 的 net，`gateIds` 為空，因此 depth 為 0。

### 8.1 全域 register-to-register path

如果問題是「所有 register-to-register paths」、「任意 DFF.Q 到任意 DFF.D」、
「最長 register-to-register path」，`PathQuery` 可直接使用 all-DFF endpoint，
不需要手動列出所有 DFF pair。

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::MaxDepth;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::DffQ, "")); // all DFF.Q
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::DffD, "")); // all DFF.D

Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

CLI 寫法：

```text
path_query max_depth all_dff_q all_dff_d
path_query enumerate all_dff_q all_dff_d -out reg_paths.txt -max_print 0
```

也可使用 `dff_q:*` / `dff_d:*`：

```text
path_query max_depth dff_q:* dff_d:*
```

無 `requiredNodes` / `avoidedNodes` 的 `MaxDepth` 會使用 multi-source /
multi-endpoint DP-style longest-path engine。全域 register-to-register
max-depth 不會逐一列舉所有 DFF pair，因此可用來回答大型 testcase 的
「maximum combinational depth on any register-to-register path」類問題。

`RegisterPathQuery` 僅保留為 legacy/internal convenience wrapper。對外一律使用 `PathQuery` endpoint resolver：`DffQ("")` 表示所有 DFF.Q，`DffD("")` 表示所有 DFF.D。
`RegisterPathQuery::EnumerateAll` 也支援與 `PathQuery` 相同的安全欄位：

```text
query.maxEnumeratedPaths
query.enumerationTimeLimitSeconds
query.countOnly
```

這些欄位會直接轉傳到底層 `PathQuery`，結果可從 `report.pathResult.completeEnumeration`、
`report.pathResult.enumerationPathLimitReached`、`report.pathResult.enumerationTimedOut`
與 `report.pathResult.enumerationStopReason` 讀取。

### 8.2 是否存在 a 到 y 的路徑

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::Exists;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::SpecificNet, "a"));
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::SpecificNet, "y"));

Netlist::PathQueryResult result = netlist.runPathQuery(query);
bool answer = result.exists;
```

### 8.3 找一條 PI a 到 PO y 的路徑

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::FindAny;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::PrimaryInput, "a"));
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::PrimaryOutput, "y"));

Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

### 8.4 找所有 PI 到 ff1.D 的最大 logic depth

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::MaxDepth;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::PrimaryInput, ""));
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::DffD, "ff1"));

Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

### 8.5 找 a 到 y 且避開 n3 的最短路徑

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::MinDepth;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::SpecificNet, "a"));
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::SpecificNet, "y"));
query.avoidedNodes.push_back(Netlist::PathNode(
    Netlist::PathNodeType::Net, "n3"));

Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

### 8.6 所有 a 到 y 的路徑是否都經過 g1

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::EveryPathThrough;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::SpecificNet, "a"));
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::SpecificNet, "y"));
query.requiredNodes.push_back(Netlist::PathNode(
    Netlist::PathNodeType::Gate, "g1"));

Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

### 8.7 所有 a 到 y 的路徑是否都避開 n3

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::EveryPathAvoids;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::SpecificNet, "a"));
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::SpecificNet, "y"));
query.avoidedNodes.push_back(Netlist::PathNode(
    Netlist::PathNodeType::Net, "n3"));

Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

### 8.8 找出 endpoints 間的 mandatory nodes

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::FindMandatoryNodes;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::SpecificNet, "n2"));
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::SpecificNet, "n14"));

const auto result = netlist.runPathQuery(query);
```

CLI：

```text
path_query mandatory_nodes net:n2 net:n14
```

底層使用 dominator，不列舉所有 paths。`NO_PATH` 與「connected 但沒有 mandatory internal net」會用不同 status 回報。

### 8.9 檢查 separator / PI-to-PO cut

指定 endpoints：

```text
path_query is_separator net:n2 net:n14 n10
```

掃描是否切斷至少一組 PI-to-PO pair：

```text
path_query pi_po_cut n55104
```

對應 C++：

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::IsSeparator;
query.separatorCandidateNetName = "n55104";

const auto result = netlist.runPathQuery(query);
```

沒有設定 endpoints 時採 PI-to-PO cut 語意；若指定 endpoints，必須各自解析成一條 scalar net。

---

## 9. 注意事項

```text
1. PathEndpoint 和 PathNode 都會自動解析 ID。
2. 名稱不存在時回 `ok=false` 並列在 unresolved 欄位；合法的 zero-result 才是 `ok=true, exists=false`。
3. GateInput 若使用 primitive gate，通常要指定 pinIndex。
4. EnumerateAll 目前以 `enumerationTimeLimitSeconds` / `countOnly` 控制成本；完整列舉需求若被截斷，需回報 `completeEnumeration=false` 與停止原因。
5. EveryPathThrough / EveryPathAvoids 不採用 vacuous truth；沒有原始路徑時回 false。
6. 目前 combinationalOnly=false 尚未支援。
```

---

## 10. 目前實作與測試狀態

```text
實作檔案：src/analysis/PathAnalysis.cpp
型別檔案：include/core/PathTypes.h
tester：mini test/tester.cpp
目前 regression：Summary: 58 passed, 0 failed.
timeout guard：mini test/test7/test7.cpp，Summary: 5 passed, 0 failed.
register path timeout guard：mini test/test8/test8.cpp，Summary: 3 passed, 0 failed.
```

目前 `runPathQuery()` 已覆蓋：

```text
Exists
FindAny
MinDepth
MaxDepth
EveryPathThrough
EveryPathAvoids
DirectPiPoConnections
```
