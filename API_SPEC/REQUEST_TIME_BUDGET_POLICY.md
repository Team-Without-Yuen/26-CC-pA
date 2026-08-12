# Request Time Budget Policy

## 1. 官方限制

| Request 類別 | 官方 wall-clock 上限 | 目前 backend 預設工作預算 |
|---|---:|---:|
| Basic operation：read、write | 60 秒 | 55 秒 |
| Query、analysis、transformation、optimization | 300 秒 | 290 秒 |

專案中的 `BasicQuery` 是電路分析 query，屬於 300 秒類別，不是官方所稱的 basic
operation。55/290 秒會保留時間給 report serialization、artifact 收尾與 request terminator。

共用常數定義於 `include/core/RequestTimeBudget.h`。高階 request 不得再各自硬編碼
30、55 或 240 秒作為預設值。

同一請求的實際 wall-clock deadline 由同檔案的 `RequestDeadline` 表示。facade 只建立
一次，nested backend 讀取 `remainingSeconds()`；不可在每一層重新開始完整預算。

## 2. 分配規則

1. 同一個高階 request 的多次 SAT、candidate search 或 endpoint diagnosis 共用一個總 deadline。
2. 使用者明確指定較短正數時，採用該值；未指定時使用對應的 backend 預設工作預算。
3. sequential functional analysis 的 per-DFF limit 為 `0` 時，由剩餘總預算與剩餘 DFF 數量自動公平分配；正數仍代表明確的單顆上限。
4. optimizer 內部 5/40/60/120 秒等數值若是 strategy cap，只能限制特定演算法階段，且仍須受 request 剩餘 deadline 約束；它們不是官方 request limit。
5. timeout、UNKNOWN、partial 或 skipped 結果不可視為完整否定答案，也不可作為 transformation commit 的等價證明。

### LLM 與 runner 的責任邊界

- 官方 wall-clock 上限由正式 runner / request watchdog 強制，不由 LLM 自行分配秒數。
- `RequestDeadline` 管理一次高階 API 內部的前置分析、backend、SAT 與 rollback；它不跨越多個獨立 CLI command。
- 一個 edit/optimization prompt 預期只呼叫一次主要 `edit_apply` 或 `opt_apply`。必要分析與驗證應由該高階 API 內部完成。
- 修改後的追問優先使用 `report_query` 或其他輕量 query，不得對同一 prompt 原樣重複執行昂貴 apply。
- 除非 prompt 明確指定較短限制，LLM 不應自行估算剩餘時間或傳入自訂 time-limit。

在上述競賽 routing 約束下，目前不另設跨 command 的 prompt-level deadline。若未來允許一個
prompt 串接多個昂貴 apply/search，應由外部 orchestrator 建立 prompt-level watchdog，而不是
把時間計算責任交給 LLM。

## 3. 修改與驗證

調整時限時需同步檢查：高階 request 預設、CLI parser 預設、共享 deadline、report
欄位、TOOLS_SPEC 與 API_SPEC。Regression 使用極小的顯式 budget 測試 timeout，不以等待
290 秒的方式測試。

目前 `VerilogReader::read()` 與 `VerilogWriter::write()` 仍為不可搶占的同步呼叫；正式
60 秒 hard limit 由 request/process watchdog 保證。mockturtle 單次 primitive 同樣無
cooperative cancellation，optimizer 會在 primitive 返回後檢查 deadline 並丟棄逾時候選。
