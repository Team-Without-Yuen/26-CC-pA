# Startpoint-to-Endpoint PathQuery 使用說明

這份文件只負責說明 `PathQuery` 怎麼使用。  
設計背景與整理筆記請看：

```text
API_SPEC/STARTPOINT_ENDPOINT_PATH_ANALYSIS.md
```

---

## 1. 基本概念

`PathQuery` 用來回答「從某些起點走到某些終點」的組合邏輯路徑問題。

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
| `EnumerateAll` | 列出所有合法路徑 | `result.paths` | 大型電路可能爆量 |
| `MinDepth` | 找最短 logic depth 路徑 | `result.path`, `result.depth` | depth = combinational gate 數量 |
| `MaxDepth` | 找最長 logic depth 路徑 | `result.path`, `result.depth` | 可用於 critical path 類問題 |
| `EveryPathThrough` | 所有既有路徑是否都經過 requiredNodes？ | `result.exists` | 沒有任何原始路徑時回 false |
| `EveryPathAvoids` | 所有既有路徑是否都避開 avoidedNodes？ | `result.exists` | 沒有任何原始路徑時回 false |

常用寫法：

```cpp
query.mode = Netlist::PathQueryMode::Exists;
query.mode = Netlist::PathQueryMode::FindAny;
query.mode = Netlist::PathQueryMode::EnumerateAll;
query.mode = Netlist::PathQueryMode::MinDepth;
query.mode = Netlist::PathQueryMode::MaxDepth;
query.mode = Netlist::PathQueryMode::EveryPathThrough;
query.mode = Netlist::PathQueryMode::EveryPathAvoids;
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
| `exists` | 是否找到路徑，或 every-path 條件是否成立 |
| `depth` | `path` 的 logic depth；沒找到時為 `-1` |
| `path` | 單一路徑，供 `FindAny` / `MinDepth` / `MaxDepth` 使用 |
| `paths` | 多條路徑，供 `EnumerateAll` 使用 |

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

## 7. 使用範例

### 7.1 是否存在 a 到 y 的路徑

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

### 7.2 找一條 PI a 到 PO y 的路徑

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::FindAny;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::PrimaryInput, "a"));
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::PrimaryOutput, "y"));

Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

### 7.3 找所有 PI 到 ff1.D 的最大 logic depth

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::MaxDepth;
query.startpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::PrimaryInput, ""));
query.endpoints.push_back(Netlist::PathEndpoint(
    Netlist::PathEndpointType::DffD, "ff1"));

Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

### 7.4 找 a 到 y 且避開 n3 的最短路徑

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

### 7.5 所有 a 到 y 的路徑是否都經過 g1

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

### 7.6 所有 a 到 y 的路徑是否都避開 n3

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

---

## 8. 注意事項

```text
1. PathEndpoint 和 PathNode 都會自動解析 ID。
2. 名稱不存在時，通常回傳空結果或 false。
3. GateInput 若使用 primitive gate，通常要指定 pinIndex。
4. EnumerateAll 目前沒有 maxPaths / timeout 保護。
5. EveryPathThrough / EveryPathAvoids 不採用 vacuous truth；沒有原始路徑時回 false。
6. 目前 combinationalOnly=false 尚未支援。
```

---

## 9. 目前實作與測試狀態

```text
實作檔案：src/analysis/PathAnalysis.cpp
型別檔案：include/core/PathTypes.h
tester：mini test/tester.cpp
目前 regression：Summary: 45 passed, 0 failed.
```

目前 `runPathQuery()` 已覆蓋：

```text
Exists
FindAny
MinDepth
MaxDepth
EveryPathThrough
EveryPathAvoids
```
