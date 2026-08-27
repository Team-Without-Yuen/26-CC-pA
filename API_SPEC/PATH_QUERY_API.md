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

Path traversal 的 edge 是 structural `source net -> gate instance -> output net`。底層
`Net::loadGateIds` 為了 Direct Connectivity 可按 input pin 保留重複 gate ID；若同一 net
同時接到同一 gate 的多個 input pins，Path count/list 仍只穿越該 gate instance 一次。
不同 gate instances 即使功能相同，仍各自形成不同 structural paths。

Path query 只解析 current active named netlist：active net 必須 `!isRemoved`，active gate 必須
`type != UNKNOWN`。明確指定 tombstone endpoint 或 required/avoided node 會進 unresolved 欄位並
使 query 失敗；不能把它解讀成合法的 no-path。PI/PO/DFF aggregate endpoints 只展開 active nets。

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
| `RankedPaths` | startpoints、endpoints、rank window | 回傳 Top-K/Nth shortest 或 longest paths |
| `MinDepth` | startpoints、endpoints | 最短合法 path |
| `MaxDepth` | startpoints、endpoints | 最長合法 path |
| `EveryPathThrough` | endpoints、requiredNodes | 每條既有 path 是否都通過 required nodes |
| `EveryPathAvoids` | endpoints、avoidedNodes | 每條既有 path 是否都避開 avoided nodes |
| `FindMandatoryNodes` | startpoints、endpoints | 所有 path 共用的 internal nets |
| `IsSeparator` | candidate；可選 endpoints | candidate 是否為 directed separator/cut |
| `DirectPiPoConnections` | 無 | PI 與 PO 共用同一 net 的 depth-0 connections |

`EnumerateAll` 與 `RankedPaths` 的控制欄位：

| 欄位 | 預設 | Contract |
|---|---:|---|
| `writePathsToFile` | `true` | 非 count-only 時 streaming 寫檔 |
| `outputFilePath` | 空 | core fallback 為 `path_enumeration_output.txt` |
| `maxPrintedPaths` | `20` | 只限制 report/sample 保存，不截斷完整列舉 |
| `maxEnumeratedPaths` | legacy | 保留相容，不作為截斷條件 |
| `enumerationTimeLimitSeconds` | `290.0` | backend 預設工作預算；兩個 mode 都必須為有限正數 |
| `countOnly` | `false` | `EnumerateAll` 只計數；`RankedPaths` 不接受 |
| `minimumAcceptedDepth` | `-1` | inclusive lower bound；`-1` 表示不限制 |
| `maximumAcceptedDepth` | `-1` | inclusive upper bound；`-1` 表示不限制 |

depth bounds 適用 `EnumerateAll` 與 `RankedPaths`。`minimumAcceptedDepth == maximumAcceptedDepth == N`
表示 exactly N；只有 lower/upper 分別表示 at least/at most。兩者皆有時表示 inclusive range。
任一欄位小於 `-1`、lower 大於 upper，或其他 mode 帶入 bounds，都回 `INVALID_ARGUMENT`。

`RankedPaths` 另使用：

| 欄位 | 預設 | Contract |
|---|---:|---|
| `rankingOrder` | `ShortestFirst` | shortest 採 depth ascending；longest 採 descending |
| `firstRank` | `1` | 1-based 第一個要求的 global rank |
| `resultCount` | `1` | 從 firstRank 起要回傳的 path 數量 |

`firstRank/resultCount` 必須為正數，且 rank window 不可 overflow。Top-K 對應 `(1, K)`；Nth
對應 `(N, 1)`。多 start/end 的所有合法 paths 共同形成一個 global ranked population。

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
| `paths` | EnumerateAll samples、RankedPaths rank window 或 DirectPiPoConnections 完整集合 |
| `pathCount` | EnumerateAll/DirectPiPoConnections 的數量 |
| `minimumAcceptedDepth` | EnumerateAll/RankedPaths 實際套用的 inclusive lower bound |
| `maximumAcceptedDepth` | EnumerateAll/RankedPaths 實際套用的 inclusive upper bound |

### 4.3 Ranked Path

| 欄位 | 語意 |
|---|---|
| `rankingOrder` | shortest-first 或 longest-first |
| `requestedFirstRank/requestedResultCount` | normalized 1-based rank window |
| `returnedRankedPathCount` | 實際回傳的 paths 數量 |
| `rankExists` | requested first rank 是否存在 |
| `rankingComplete` | 是否在 deadline 內完成並證明排名 |
| `populationExhausted` | backend 是否已證明沒有更多合法 paths |
| `firstReturnedRankDepth/lastReturnedRankDepth` | rank window 首筆與末筆的 logic depth；沒有回傳 rank 時為 `-1` |
| `rankedDepthRanges` | 將連續且同 depth 的 global ranks 壓縮成 exact `(firstRank,lastRank,depth)` records |

若 Top-K 的 population 少於 K，回傳全部可用 paths 並設 `populationExhausted=true`。Nth 超過
population 時回 `ok=true, rankExists=false`；只有 `rankingComplete=true` 時這個不存在結論才成立。
timeout 不回傳未證明的 partial rank window。

`rankedDepthRanges` 不重新計算或近似 depth，而是由已證明的 `paths` rank window 建立。ranges
依 rank 遞增、彼此相鄰且不重疊，涵蓋的 rank 數總和必須等於
`returnedRankedPathCount`。由於 primary ranking key 是 depth，相同 depth 在 shortest/longest
排序中必定連續，因此此表示可精確還原每個 returned rank 的 depth，不保存第二份 literal path。

### 4.4 完整性與檔案

| 欄位 | 語意 |
|---|---|
| `wrotePathsToFile` | 是否成功建立 enumeration file |
| `outputFilePath` | 實際輸出路徑 |
| `completeEnumeration` | 是否未被截斷 |
| `enumerationTimedOut` | 是否因 wall-clock limit 停止 |
| `enumerationPathLimitReached` | legacy；目前應維持 false |
| `countOnly` | 是否只保留 count |
| `enumerationStopReason` | partial result 的停止原因 |

### 4.5 完整 Path Artifact 格式

非 count-only 的 `EnumerateAll` 使用 `LITERAL_PATH_V1` 文字格式。API 的 query/report 欄位不變，
artifact 以 streaming 逐條輸出完整具名路徑：

1. header 記錄 exact `Total paths`、格式版本、depth predicate（若有）與 expected count。
2. 每筆 record 使用 `Path <index>: <net> -> <gate>(<type>) -> <net> ...`。
3. net、gate instance 與 gate type 均直接使用 current named netlist 名稱，不需要 dictionary 或 decoder。
4. footer 記錄 `Written paths`、`Complete` 與 `Timed out`。

每一筆 record 對應一條可直接閱讀的 start-to-end path。只有
`Written paths == Expected paths` 且 `Complete: yes` 才能將 artifact 視為完整。

RankedPaths 的小型結果直接回傳；保守估算達到 3072 response tokens，或 list records 達到
256 筆時，CLI 提前使用 `QUERY_LIST_ARTIFACT_V1`。每筆包含原始 rank、depth 與完整 literal
path；artifact 同時記錄
ranking order、requested window、completion、population exhaustion 與完整 rank-depth ranges。
terminal envelope 固定保留 first/last returned depth、range count；Nth 查詢另保留
`requested_rank_depth`。壓縮 ranges 在 token-safe 時即使 literal paths 已外移仍會留在 envelope。

只有 `ok=true` 且 `completeEnumeration=true` 時，`pathCount` 才可視為完整計數。目前
`pathCount` 使用 `size_t`；超過 `2^64-1` 的正式規格正在等待官方確認，詳見
`Blup/OFFICIAL_QUESTIONS.md`。

### 4.6 Mandatory 與 Separator

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
| EnumerateAll | reverse reachability pruning + DFS + literal streaming；無 required-node artifact flow 另做 exact pre-count，所有 stage 共用同一 request deadline |
| Count-only | 無 depth predicate 時使用 endpoint-based scalar DAG DP；有 predicate 時 memo state 納入 remaining inclusive depth range |
| RankedPaths | DAG K-best DP；每個 state 只保留 requested last rank 所需候選，suffix 以共享 backpointer 保存，最後才重建 paths |
| MaxDepth | multi-source/multi-endpoint DAG longest-path DP |
| Mandatory/Separator | directed dominator analysis，不列舉所有 paths |
| Direct PI-to-PO | PI/PO net ID intersection |

## 6. 與其他 API 的責任界線

| Prompt 類型 | 使用 API |
|---|---|
| A 到 B 是否有 path、所有 paths、最短/最長或 Top-K/Nth path | `PathQuery` |
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
- depth predicate 屬 `EnumerateAll/RankedPaths`。一般存在性用 filtered count-only 的
  `exists/pathCount` 判讀；`MinDepth/MaxDepth` 的 extrema witness 語意不變。
- RankedPaths 的 deterministic tie-break 為 depth、start net name、end net name、gate instance
  name sequence、net name sequence；不使用 ID 或 adjacency insertion order。

## 8. 實作與測試狀態

| 類型 | 路徑 |
|---|---|
| Public types | `include/core/PathTypes.h` |
| Netlist declarations/aliases | `include/core/Netlist.h` |
| Implementation | `src/analysis/PathAnalysis.cpp` |
| CLI integration | `tools.cpp` |
| API regression | `mini test/tester.cpp`、`mini test/test7`、`mini test/test8`、`mini test/test35`、`mini test/test60`、`mini test/test61` |

目前所有 `PathQueryMode` 均已 dispatch。已知未完成事項：

- `combinationalOnly=false` 不支援。
- 超過 64-bit 的 path count 等待官方回覆。
- literal path artifact 以 footer 記錄 expected/written/complete/stop reason；呼叫端仍須同時核對 report envelope。
- `EnumerateAll` facade 建立單一 `RequestDeadline`；exact pre-count、reverse reachability、count/DFS
  與 streaming enumeration 不得重新取得完整 budget。同步 filesystem flush 仍由外部 watchdog
  提供 hard cutoff。
- 深鏈遞迴、max-depth full-path memo 與多 endpoint O(V x endpoints) working memory 尚待
  scalability regression，詳見 `PATH-SCALE-001/002`。
