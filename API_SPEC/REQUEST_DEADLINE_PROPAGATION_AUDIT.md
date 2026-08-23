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
| Path Enumerate | 單一 enumeration start time | 正確 |
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

## 5. Read/Write 邊界

`VerilogReader::read()` 與 `VerilogWriter::write()` 是同步函式，呼叫期間 tools dispatcher
無法中止。工具內可以量測完成後是否超時，但這不能滿足 hard 60 秒限制。因此正式執行環境
仍必須以單一 request/process watchdog 在 60 秒強制終止；未來若要內部 cooperative timeout，
需把 parser/writer loop 改成可查詢 deadline 的 API，不能只在前後加計時器。
