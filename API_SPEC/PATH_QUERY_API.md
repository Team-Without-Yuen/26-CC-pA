# Path Query API 整理

這份文件整理 `Netlist::PathQuery` 的設計、責任與內部 contract。

使用方式與 prompt 範例請看：

```text
API_SPEC/PATH_QUERY_USAGE.md
```

---

## 1. 核心定位

`PathQuery` 統一處理明確 startpoint 與 endpoint 之間的組合路徑問題：

- connectivity 與任一路徑
- 完整列舉或精確計數
- 最短與最長 logic depth path
- required/avoided node constraints
- every-path、mandatory node 與 directed separator/cut
- PI、PO、DFF pin、gate pin 與多 endpoint sets

對外不再建立新的 `GraphQuery` 或 `RegisterPathQuery` 功能；register-to-register query 由
`DffQ`/`DffD` endpoints 表示。`RegisterPathQuery` 只保留 legacy/internal wrapper。

以下問題不屬於本 API：

- 單一物件的完整 fanin/fanout cone：`ConeQuery`
- 全設計 critical endpoint/depth：`DepthQuery`
- Boolean equivalence、constant、expression：`FunctionQuery`
- sequential enable/hold pattern：`SequentialPatternQuery`

## 2. 資料模型與邊界

Traversal 使用 gate-level named netlist：

```text
Net -> combinational Gate -> Net
```

`CombinationalPath.netIds` 包含 start/end net，`gateIds` 保存真正穿越的 combinational
gates，logic depth 等於 `gateIds.size()`。NOT、BUF 與其他 gate 一樣各算一層。

目前只支援 `combinationalOnly=true`：

- DFF 是 sequential boundary。
- 路徑不從 DFF.D 穿越到 DFF.Q。
- register-to-register 從前一顆 DFF.Q net 出發，到後一顆 DFF.D net 結束。
- DFF.Q 作為 fanin endpoint 時視為空的 sequential boundary。

正式題目中的 combinational graph 為 DAG。max-depth 與 count-only DP 依此 contract 使用
memoization；不承諾 combinational cycle 下的 longest-path 語意。

## 3. Query 型別

### 3.1 Endpoint

| `PathEndpointType` | `name` | 解析結果 |
|---|---|---|
| `SpecificNet` | net name | 指定 scalar net |
| `PrimaryInput` | PI port；空或 `*` 表示全部 | 一個或多個 PI bit nets |
| `PrimaryOutput` | PO port；空或 `*` 表示全部 | 一個或多個 PO bit nets |
| `DffQ` | DFF instance；空或 `*` 表示全部 | DFF output/Q net |
| `DffD` | DFF instance；空或 `*` 表示全部 | named D input net |
| `DffClock` | DFF instance | `pinName` 或預設 CK net |
| `DffReset` | DFF instance | 指定 pin，或未指定時嘗試 RN/SN |
| `GateOutput` | gate instance | gate output net |
| `GateInput` | gate instance | `pinName` 或 `pinIndex` 指定的 input net |

Bus port endpoint 會展開成各 bit net。Primitive gate 通常使用 positional input，應提供
`pinIndex`；DFF/control pin 使用 `pinName`。

### 3.2 Constraint Node

`PathNode` 可表示具名 `Net` 或 `Gate`：

```cpp
PathNode(PathNodeType::Net, "n3")
PathNode(PathNodeType::Gate, "g5")
```

`requiredNodes` 中每個節點都必須經過；`avoidedNodes` 中任一節點都不能經過。

### 3.3 Mode

| `PathQueryMode` | 必要輸入 | 主要語意 |
|---|---|---|
| `Exists` | startpoints、endpoints | 是否至少存在一條合法 path |
| `FindAny` | startpoints、endpoints | 回傳任一條合法 path |
| `EnumerateAll` | startpoints、endpoints | 完整列舉或 count-only |
| `MinDepth` | startpoints、endpoints | 最短合法 path |
| `MaxDepth` | startpoints、endpoints | 最長合法 path |
| `EveryPathThrough` | endpoints、requiredNodes | 每條既有 path 是否都通過 required nodes |
| `EveryPathAvoids` | endpoints、avoidedNodes | 每條既有 path 是否都避開 avoided nodes |
| `FindMandatoryNodes` | startpoints、endpoints | 所有 path 共用的 internal nets |
| `IsSeparator` | candidate；可選 endpoints | candidate 是否為 directed separator/cut |
| `DirectPiPoConnections` | 無 | PI 與 PO 共用同一 net 的 depth-0 connections |

`EnumerateAll` 的控制欄位：

| 欄位 | 預設 | Contract |
|---|---:|---|
| `writePathsToFile` | `true` | 非 count-only 時 streaming 寫檔 |
| `outputFilePath` | 空 | core fallback 為 `path_enumeration_output.txt` |
| `maxPrintedPaths` | `20` | 只限制 report/sample 保存，不截斷完整列舉 |
| `maxEnumeratedPaths` | legacy | 保留相容，不作為截斷條件 |
| `enumerationTimeLimitSeconds` | `290.0` | backend 預設工作預算；EnumerateAll 必須為有限正數，不提供 unlimited 語意 |
| `countOnly` | `false` | 只計數，不保存或輸出每條 path |

`tools.cpp` 的 CLI 會在未指定 `-out` 時建立唯一自動檔名；這是 CLI policy，不改變 core
facade 的固定 fallback contract。

## 4. Report Contract

### 4.1 共用狀態

| 欄位 | 語意 |
|---|---|
| `ok` | request 是否通過 validation 並完成 dispatch |
| `unsupported` | request 合法，但設定目前不支援 |
| `message` | success、no-path 或 validation error 訊息 |
| `status` | mode-specific 狀態，例如 `NO_PATH`、`SEPARATOR` |
| `exists` | path/every-path 類 mode 的主要 Boolean 結果 |

名稱解析失敗為 `ok=false`，並填入對應 `unresolved*` 欄位。名稱合法但沒有 path 為
`ok=true, exists=false`，不可把兩者混為同一種「沒有路徑」。

| Validation 欄位 | 內容 |
|---|---|
| `unresolvedStartpoints` | 無法解析的 start endpoint descriptions |
| `unresolvedEndpoints` | 無法解析的 end endpoint descriptions |
| `unresolvedRequiredNodes` | 無法解析的 required constraint nodes |
| `unresolvedAvoidedNodes` | 無法解析的 avoided constraint nodes |

### 4.2 Path 與深度

| 欄位 | 使用 mode |
|---|---|
| `depth` | FindAny、MinDepth、MaxDepth 的代表 path depth |
| `path` | 單一代表 path |
| `paths` | EnumerateAll samples 或 DirectPiPoConnections 完整集合 |
| `pathCount` | EnumerateAll/DirectPiPoConnections 的數量 |

### 4.3 完整性與檔案

| 欄位 | 語意 |
|---|---|
| `wrotePathsToFile` | 是否成功建立 enumeration file |
| `outputFilePath` | 實際輸出路徑 |
| `completeEnumeration` | 是否未被截斷 |
| `enumerationTimedOut` | 是否因 wall-clock limit 停止 |
| `enumerationPathLimitReached` | legacy；目前應維持 false |
| `countOnly` | 是否只保留 count |
| `enumerationStopReason` | partial result 的停止原因 |

### 4.4 完整 Path Artifact 格式

非 count-only 的 `EnumerateAll` 使用 `COMPACT_PATH_V3` 文字格式。API 的 query/report 欄位不變，
但檔案不再對每條 path 重複輸出完整 net/gate 名稱：

1. header 記錄 exact `Total paths`、格式版本與 expected count。
2. `net_dictionary`、`gate_dictionary` 各輸出一次 ID/name/output-net 對照。
3. dictionary 與 path records 的 ID 統一使用 unsigned base36。
4. 完整 token sequence 定義為 `S=[start_net_id, gate_id_1, ..., gate_id_N]`。
5. 第一條 path 使用完整 `S`；後續每條 path 記錄相對前一條 `S` 的共同 prefix、共同 suffix 與中間差異 IDs。prefix/suffix count 包含 token 0 的 start net。
6. footer 記錄 `Written paths`、`Complete` 與 `Timed out`。

每一筆 record 仍對應一條明確 path。解碼後的第一個 ID 是 start net，其餘為 gate IDs；完整 net
sequence 由 start net 加上每個 gate 的 output net 無損重建。只有
`Written paths == Expected paths` 且 `Complete: yes` 才能將 artifact 視為完整。

只有 `ok=true` 且 `completeEnumeration=true` 時，`pathCount` 才可視為完整計數。目前
`pathCount` 使用 `size_t`；超過 `2^64-1` 的正式規格正在等待官方確認，詳見
`Blup/OFFICIAL_QUESTIONS.md`。

### 4.5 Mandatory 與 Separator

| 欄位 | 語意 |
|---|---|
| `pathExists` | 指定 endpoints 是否原本相連 |
| `isSeparator` | candidate 是否切斷原本存在的 directed connectivity |
| `mandatoryNetIds/Names` | 所有 paths 共用的 internal nets，不含 source/target |
| `separatorCandidateNetName/Id` | 實際解析的 candidate |
| `witnessStartpoint/Endpoint` | PI-to-PO cut 成立時的 witness pair |
| `checkedStartpointCount/EndpointCount` | cut 掃描範圍 |
| `combinationalCycleDetected` | dominator backend 是否遇到不支援的 cycle |

若 endpoints 原本沒有 path，mandatory mode 回 `NO_PATH`，separator 不因 vacuous truth 成立。

## 5. 高階入口與執行流程

公開入口：

```cpp
Netlist::PathQueryResult Netlist::runPathQuery(
    const Netlist::PathQuery& query) const;
```

主要流程：

```text
validate combinationalOnly
-> resolve endpoints / constraints
-> dispatch mode
-> execute BFS、DFS、DAG DP 或 dominator
-> populate PathQueryResult
-> finalize streaming file and completeness fields
```

主要演算法：

| 功能 | 實作策略 |
|---|---|
| Exists/FindAny/MinDepth | BFS/constraint-aware traversal |
| EnumerateAll | exact pre-count + reverse reachability pruning + DFS + compact delta streaming；無 constraints 的官方 DAG 使用低開銷 fast path |
| Count-only | endpoint-based memoized DAG path-count DP |
| MaxDepth | multi-source/multi-endpoint DAG longest-path DP |
| Mandatory/Separator | directed dominator analysis，不列舉所有 paths |
| Direct PI-to-PO | PI/PO net ID intersection |

## 6. 與其他 API 的責任界線

| Prompt 類型 | 使用 API |
|---|---|
| A 到 B 是否有 path、所有 paths、最短/最長 path | `PathQuery` |
| 所有 register-to-register paths | `PathQuery(DffQ -> DffD)` |
| A 的完整 transitive fanin/fanout cone | `ConeQuery` |
| 全設計 maximum depth、critical endpoint、depth threshold | `DepthQuery` |
| net driver/load、gate/net 基本資訊 | `BasicQuery` |
| Boolean constant/equivalence/expression | `FunctionQuery` |

Reachable cone 不是 start-to-end path；全域 critical depth 也不應用所有 PI/PO pair 的
`MaxDepth` 取代專用 `DepthQuery`。

## 7. 限制與安全規則

- 只支援 `combinationalOnly=true`。
- 正式 contract 假設 combinational graph 為 DAG；cycle 下 max-depth 不保證。
- 完整 enumeration 的輸出數量可能呈指數成長；只問數量時使用 `countOnly=true`。
- timeout result 必須保留 `completeEnumeration=false` 與 stop reason，不得把 partial count 當精確答案。
- `maxEnumeratedPaths` 不限制完整列舉，完成性只看 completion/stop fields。
- DFF named pin 查詢依 `inputPinNames` 與 `inputNetIds` 同位置對應；disconnect 應保留索引並設為 `-1`。
- overflow count 的正式處理等待官方確認；目前不能把 `SIZE_MAX` 解讀成真實精確數量。

## 8. 實作與測試狀態

| 類型 | 路徑 |
|---|---|
| Public types | `include/core/PathTypes.h` |
| Netlist declarations/aliases | `include/core/Netlist.h` |
| Implementation | `src/analysis/PathAnalysis.cpp` |
| CLI integration | `tools.cpp` |
| API regression | `mini test/tester.cpp`、`mini test/test7`、`mini test/test8` |
| 專項盤查 | `Blup/PATH_QUERY_ISSUES_AND_CHANGES.md` |

目前所有 `PathQueryMode` 均已 dispatch。已知未完成事項：

- `combinationalOnly=false` 不支援。
- 超過 64-bit 的 path count 等待官方回覆。
- partial path file 本身尚未包含 completion metadata；completion 目前由 report envelope 提供。
