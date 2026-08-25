# Request Deadline Propagation Audit

## 1. 目的

本文件盤查 public request 的時間預算是否會在巢狀 backend 呼叫時重新開始。官方限制與
預設值見 `REQUEST_TIME_BUDGET_POLICY.md`。

## 2. 狀態總覽

| 流程 | 目前狀態 | 結論 |
|---|---|---|
| Function Search | 單一 query start time，所有 simulation/SAT 共用 | 正確 |
| Functional Merge | search 後以 `total - elapsed` 傳給 whole-design SAT | 正確 |
| Whole-design Equivalence | global miter 使用剩餘總預算；diagnosis 公平分配 | 正確 |
| Sequential Functional Analysis | query-wide deadline；per-DFF 自動公平分配 | 正確 |
| Path Enumerate/Ranked | facade 建立單一 `RequestDeadline`；pre-count、reverse reachability、enumeration 與 K-best DP 各自共用單一 request budget | 正確；enumeration artifact filesystem flush 仍是不可搶占的同步 I/O |
| OptApply | facade 建立單一 `RequestDeadline`，傳入 scope/lower-bound、optimizer、mapper 與 validation；本 pass 不執行 final SAT | 正確 |
| DepthOptimizer Stage 2 | 120 秒 strategy cap 使用 `min(120, request remaining)` | 正確；單次 mockturtle primitive 不可搶占 |
| ConvertToBasis / ReplaceGateType | EditApply 的同一個 deadline 傳入 `TechMapper` | 正確；timeout 整筆 rollback |
| TechMapper exact synthesis 60/40/5 秒 | 使用 `min(strategy cap, request remaining)` | 正確 |
| read/write | synchronous reader/writer，無 cooperative cancellation | 由外部 60 秒 watchdog 強制 |

## 3. 修改原則

1. public CLI grammar 與既有 report 格式不變。
2. request facade 建立一次 deadline；巢狀流程只讀剩餘時間，不重新取得完整 290 秒。
3. strategy cap 使用 `min(strategy cap, request remaining)`。
4. 無法 cooperative cancellation 的單次第三方 primitive 必須明確記錄限制；facade 不偽造已在 deadline 前停止。
5. mutation 在 deadline 後不得 commit；需要 equivalence 的流程必須 rollback。

共用實作為 `include/core/RequestTimeBudget.h` 的 `RequestDeadline`。它保存一次
`steady_clock` 起點；巢狀 backend 只能接收指標或 reference，不得自行建立新的完整預算。

## 4. 已完成接線

- `Netlist::runEditApply()`：functional merge 使用剩餘時間；technology mapping 共用 deadline。
- `TechMapper`：worklist、rule scan 與 exact-synthesis SAT 均檢查同一 deadline。
- `Netlist::runOptApply()`：scope/lower-bound、`DepthOptimizer`、`TechMapper` 與 validation 共用 deadline；whole-design SAT 僅由獨立 equivalence flow 管理。
- `DepthOptimizer`：超時回 `OptimizationStatus::TIMEOUT` 並恢復進入 core 前的 snapshot。
- `TechMapStatus::TIMEOUT`：不再把 timeout 混為 rule-not-found、not-equivalent 或 simulation failure。

Regression：`mini test/test41` 15/15；`mini test/test31` 24/24，其中包含極小 mapping
budget 的完整 transaction rollback。

## 5. PATH-DEADLINE-001：Path Enumerate/Ranked 已完成

盤查現行 source 後確認，舊文件所稱「pre-count 後才建立外層 clock，因此可能取得兩份完整
290 秒」並不符合修正前程式：外層 `PathEnumerationState` 已在 pre-count 前建立。真正缺口是
計時契約只隱含在相對 `startTime`，recursive pre-count 另建 state，且
`computeReverseReachableNets()` traversal 期間沒有 cooperative timeout check。

目前 public `runPathQuery()` 會為 `EnumerateAll` 與 `RankedPaths` 建立一次 `RequestDeadline`。
EnumerateAll 透過 private
`runPathQueryWithDeadline()` 將同一個物件傳給 recursive exact pre-count。reverse reachability、
memoized count、constrained DFS 與 unconstrained streaming DFS 都檢查同一 deadline；legacy
low-level enumeration helper 未提供 deadline 時仍保留原本行為。既有 EnumerateAll 的 CLI、
`PathQueryResult` 欄位與 `LITERAL_PATH_V1` 格式沒有改變。

RankedPaths 的 multi-end reverse reachability、DAG K-best state expansion、candidate merge 與
reconstruction 也使用同一 deadline。timeout 時不回傳未證明的 partial rank window，並維持
`rankingComplete=false`、`completeEnumeration=false`。

`mini test/test59` 使用 24-level diamond DAG 驗證 exact count `16,777,216`，並以極小預算驗證
unconstrained/required-node streaming 都回傳一致的 partial report 與 self-contained timeout
artifact。production build、Path focused regressions及官方 test14 的 `289,366` / `203,810`
count 均通過。同步 filesystem write/flush 仍無法在單次 OS 呼叫中被 cooperative cancellation；
290 秒 backend budget 保留官方 300 秒限制前的收尾空間，hard cutoff 仍由外部 watchdog 負責。

## 6. Read/Write 邊界

`VerilogReader::read()` 與 `VerilogWriter::write()` 是同步函式，呼叫期間 tools dispatcher
無法中止。工具內可以量測完成後是否超時，但這不能滿足 hard 60 秒限制。因此正式執行環境
仍必須以單一 request/process watchdog 在 60 秒強制終止；未來若要內部 cooperative timeout，
需把 parser/writer loop 改成可查詢 deadline 的 API，不能只在前後加計時器。
