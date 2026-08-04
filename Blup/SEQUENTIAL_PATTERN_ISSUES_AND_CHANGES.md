# Sequential Pattern Query API 問題與修改紀錄

## 1. 盤查範圍

- `SequentialPatternQuery`、`SequentialPatternReportSet` 與 public facade
- canonical feedback-MUX structural matcher
- AND-only data-gating non-match diagnostic
- DFF selection、summary/detail、count、pagination、timeout 與大型輸出
- `tools.cpp` parser/printer/envelope 與 NewTestCase coverage
- functional cofactor、任意 Boolean restructuring 與 AIG 重構只記錄，暫不作為本階段完整 coverage 要求

## 2. Prompt 責任

直接由 Sequential Pattern Query 回答：

```text
NewTestCase/test40 prompt 12:
Report the D input logic of the flip-flops to report any existing enable or hold structures implemented through multiplexers or AND gates.

NewTestCase/test40 prompt 13:
How many flip-flops were found to have enable or hold structures in their D input logic?
```

不屬於本 API：

| Prompt | 負責工具 |
|---|---|
| `List all flip-flops driven by clock n0.` | Structure Query |
| register-to-register path | Path Query |
| PI-to-DFF.D / register path depth | Depth Query / Path Query |
| DFF gate count | Basic/Structure Query |

官方已確認 `D = EN & DATA` 沒有同一顆 DFF 的 Q feedback，因此不是 enable/hold；它只能是
data-gating non-match diagnostic，不計入 matched/candidate DFF count。

## 3. 狀態總覽

| ID | 問題 | 優先級 | 狀態 | 已修正 | 驗證狀態 |
|---|---|---:|---|---|---|
| SEQ-000 | canonical public API/CLI baseline | P0 | Verified | 不需修改 | test21 18/18、test40 summary 通過 |
| SEQ-001 | all-DFF detail 依賴 pagination 或大型 terminal envelope | P1 | Verified | 是 | test21 18/18、test40 1583/1583 artifact |
| SEQ-002 | arbitrary Boolean enable/hold decomposition | P2 | Boolean-Deferred | 否 | 保留既有 opt-in fallback，後期 AIG 重構 |
| SEQ-003 | canonical normalization 與 same-DFF feedback 邊界 | P1 | Verified | 不需修改 | test19 matrix、wrapper probe 通過 |
| SEQ-004 | canonical SAT confirmation 與 edit/mapping 後可辨識性 | P1 | Verified | 不需修改 | test20 13/13、CLI SAT smoke 通過 |
| SEQ-005 | count 去重、diagnostic、pagination 與錯誤狀態契約 | P1 | Verified | 不需修改 | test21 18/18 |
| SEQ-006 | NewTestCase/test40 prompt 12/13 最終實測 | P0 | Verified | 不需修改 | 1583/1583 unique records，約 0.2 秒 |

## 4. 問題與修改紀錄

## SEQ-000：canonical public API/CLI baseline

- 狀態：`Verified`
- 優先級：P0
- 類型：Coverage / Report / CLI
- Boolean 相關：canonical structural match 不列入延後範圍

### 現有責任

預設 `sequential_query enable_hold` 只執行 canonical fast path，可辨識四種由 AND/OR、
NAND 或 NOR 組成且包含同一顆 DFF.Q feedback 的 MUX-hold 結構。AND-only data gating 回
non-match diagnostic；指定 DFF、summary、confirmed-only、no-pattern 與 detail pagination 均已公開。

### 驗證結果

| 驗證 | 結果 |
|---|---|
| `mini test/test21/test21.ps1` | 18 passed / 0 failed |
| test40 canonical summary | 2585 DFF 全部分析；1583 matched/candidate DFF；complete |
| test40 summary report elapsed | 約 0.012 秒 |
| test40 first 50 records | 正確回 partial、`records_truncated=true`、`next_record_offset=50` |

### 修改內容

正式程式碼：無。本項建立目前 canonical/public baseline。

## SEQ-001：all-DFF detail 依賴 pagination 或大型 terminal envelope

- 狀態：`Verified`
- 優先級：P1
- 類型：Completeness / CLI / Large Output
- Boolean 相關：否

### 問題

`SequentialPrintOptions::recordLimit` 預設為 50。Prompt 要求完整列出 DFF enable/hold
structures 時，LLM 必須依 `next_record_offset` 逐頁重送；每一頁都會重新執行同一個
Sequential Pattern Query。Parser 雖允許把 `--limit` 提高到完整 record count，但全部 records
會直接進入 terminal envelope。

這和目前大型結果政策不一致：完整內容應由工具寫入 artifact，response 只回 count、完整性與
檔案路徑，不應由 LLM 控制 pagination 或接收數 MB 的重複結構化文字。

### test40 實測

| Query | 結果 | 外部時間 | Envelope 大小 |
|---|---|---:|---:|
| `--summary-only --confirmed-only` | 1583 matches，complete | 約 0.135 秒 | summary only |
| `--confirmed-only --limit 50` | 50/1583，partial，next=50 | 約 0.096 秒 | 第一頁 |
| `--confirmed-only --limit 1583` | 1583/1583，complete | 約 0.886 秒 | 3,226,317 characters |

底層 report elapsed 僅約 0.006 秒；主要成本是 record formatting、terminal transport 與 LLM
context，而不是 canonical analysis。

### 處理決議

```text
1. 保留 single-DFF 與 summary-only 的現有 envelope。
2. all-DFF detail 預設自動寫入不覆寫的 sequential artifact。
3. envelope 只回 available/reported count、complete 與 output_file。
4. offset/limit 保留為 prompt 明確要求特定頁面或前 N 筆時的 compatibility options。
5. 不改 SequentialPatternQuery C++ 參數；file output 可留在 tools.cpp printer 層。
```

### 修改內容

正式程式碼：

| 檔案 | 函式 / 型別 | 修改內容 |
|---|---|---|
| `tools.cpp` | `ToolSession` / artifact helper | 新增 sequential artifact sequence 與 `<design>_sequential_query_<sequence>.txt` 不覆寫命名 |
| `tools.cpp` | `SequentialPrintOptions` / parser | 記錄 offset/limit 是否由使用者明確指定；summary 與 record window 保持既有行為 |
| `tools.cpp` | sequential formatter / dispatch | all-DFF default detail 寫入完整 artifact；terminal 改印 summary、artifact count 與 path |
| `tools.cpp` | public help | 說明 all-DFF 自動寫檔，offset/limit 只供明確 record window |
| `mini test/test21/test21.ps1` | CLI regression | 改驗 artifact count、DFF names、confirmed-only filter，保留 pagination/single-DFF regressions |

文件同步：

```text
API_SPEC/SEQUENTIAL_PATTERN_API.md
API_SPEC/SEQUENTIAL_PATTERN_USAGE.md
API_SPEC/NEW_TESTCASE_TOOLS_COMMAND_COVERAGE.md
API_SPEC/TOOLS_CPP_IMPLEMENTATION_SPEC.md
API_SPEC/TOOLS待更新表.md
TOOLS_SPEC/SEQUENTIAL_QUERY_TOOL.md
TOOLS_SPEC/LLM_NOTES.md
```

### 修改後驗證

| 驗證 | 結果 |
|---|---|
| Build | MSYS2 UCRT64 `TOOLS_SPEC/make all` 成功 |
| test21 | 18 passed / 0 failed |
| small all-DFF default | 2/2 artifact records；terminal 0 detail records；complete |
| explicit `--limit 1` | 維持 terminal pagination、partial、next offset |
| single `ff_high` | 維持 terminal detail、complete |
| test40 all confirmed | 1583/1583 artifact records；complete |
| test40 envelope | 3,226,317 characters 降至 1292 characters |
| unique path | 連跑兩次產生 `_1`、`_2`，不覆寫 |

## SEQ-002：arbitrary Boolean enable/hold decomposition

- 狀態：`Boolean-Deferred`
- 優先級：P2
- 類型：Boolean Analysis / Architecture
- Boolean 相關：是

### 處理決議

既有 `FunctionalPatternEngine` 與 opt-in functional fallback 保留，不移除也不破壞；但任意
Boolean restructuring、Q-free EN/DATA decomposition 的完整性與大型全設計成本，留待 current
named netlist + AIG functional index 重構後統一處理。本階段先保證 canonical structural
answers、non-match diagnostic 與 report correctness。

## SEQ-003：canonical normalization 與 same-DFF feedback 邊界

- 狀態：`Verified`
- 優先級：P1
- 類型：Matcher Correctness / Boundary Cases
- Boolean 相關：否，僅驗證 canonical structural fast path

### 風險

canonical matcher 會穿透 BUF、NOT、`NAND(x,x)`、`NOR(x,x)` 等 wrapper。若 literal
normalization、名稱映射或 feedback ownership 判斷有誤，可能出現 count 正確但 enable/data
名稱錯誤，或把另一顆 DFF.Q 誤認成目前 DFF 的 hold feedback。

### 驗證電路與結果

| 驗證 | 結果 |
|---|---|
| `mini test/test19/test19.v` canonical matrix | 8 DFF、6 matched、6 candidate |
| active-high / active-low | `ff_high`、`ff_low` polarity 與 hold level 正確 |
| NAND/NOR/POS canonical forms | `ff_nand`、`ff_nor`、`ff_pos` 均正確匹配 |
| inverted data branch | `ff_nand_inv` 回報 base data=`data`、branch=`not_data`、`data_inverted=true` |
| AND-only data gating | `ff_and` 為 `DataGatingWithoutHoldFeedback`，不計入 matched |
| no pattern | `ff_plain` 為 `NO_PATTERN` |
| `Blup/sequential_pattern_runs/canonical_wrapper_probe/canonical_wrapper_probe.v` | 3 DFF、2 matched、2 candidate |
| BUF / double-inverter wrapper | `ff_wrap` 還原 enable=`en`、data=`data`、feedback=`q_wrap` |
| NOR self-inversion wrapper | `ff_nor_inv` 正確匹配 |
| wrong-Q feedback | `ff_wrong` 使用別顆 DFF.Q，正確回 `NO_PATTERN` |
| confirmed-only artifact | 只有 `ff_wrap`、`ff_nor_inv` 兩筆，不含 `ff_wrong` |

### 修改內容

正式程式碼：無。現有 `normalizeLiteral()`、`stripBuffers()`、canonical root matcher 與
same-DFF positive-Q feedback 條件符合上述邊界語意。

測試證據保留於：

```text
Blup/sequential_pattern_runs/canonical_matrix/
Blup/sequential_pattern_runs/canonical_wrapper_probe/
```

## SEQ-004：canonical SAT confirmation 與 edit/mapping 後可辨識性

- 狀態：`Verified`
- 優先級：P1
- 類型：SAT Confirmation / Transformation Regression
- Boolean 相關：SAT confirmation 使用 Boolean proof，但不擴張 canonical 搜尋範圍

### 風險

`--verify-sat` 必須只確認已找到的 canonical pattern，不能把 solver timeout/unknown 當成成功；
technology mapping、constant simplification 與 inverter cleanup 後，也必須維持功能等價並讓 canonical
matcher 能辨識允許的等價結構。

### 實作核對

對指定 DFF 的 canonical match，程式分別執行：

```text
hold condition：enable = hold level 時證明 D == Q
load condition：enable = active level 時證明 D == data branch
```

兩者都成立才回 `solver_status=PROVEN` 並改為
`StructuralCanonicalWithSat`。FAILED 會取消 `confirmed`；timeout、unknown、unsupported 會保留明確
solver status 並使 report 不完整。CLI 禁止對 `all` 使用 `--verify-sat`，避免無界全設計 SAT 成本。

### 驗證結果

| 驗證 | 結果 |
|---|---|
| build `mini test/test20/test20.exe` | 成功；只有既有 third-party warnings |
| `mini test/test20/test20.exe` | 13 passed / 0 failed |
| whole-design equivalence | PO 與全部 DFF.D endpoints 均納入比較 |
| NAND/NOT mapping + simplification | 修改成功且 equivalence 通過 |
| mapping 後 canonical detection | 2 個原有 enable/hold pattern 仍可辨識 |
| mapping 後 SAT confirmation | `ff_high`、`ff_low` 均為 `PROVEN` |
| wrapper CLI SAT | `ff_wrap` hold/load proof 均 true，solver 無 timeout/unknown |
| wrong-Q CLI SAT | `ff_wrong` 維持 `NO_PATTERN`，不產生 pattern proof |
| all-DFF SAT | 明確回 argument error，不會隱性啟動大量 SAT |

### 修改內容

正式程式碼：無。現有 conditional-equivalence proof、solver status propagation、CLI scope
限制及 transformation regression 均符合預期。

## SEQ-005：count 去重、diagnostic、pagination 與錯誤狀態契約

- 狀態：`Verified`
- 優先級：P1
- 類型：Report Contract / CLI Error Handling
- Boolean 相關：functional fallback 測試保留現況，本項不擴張 Boolean coverage

### 實作核對

`matchedDffCount` 與 `candidateDffCount` 都在每個 DFF report 完成後至多增加一次，不按
pattern 數量累加，因此同一顆 DFF 的多個 match 不會重複計數。candidate 只接受
`MuxHold`；`DataGatingWithoutHoldFeedback` 只作 diagnostic，不會污染 candidate/matched count。

### 驗證結果

`mini test/test21/test21.ps1`：18 passed / 0 failed，涵蓋：

```text
summary aggregate count 與 diagnostic exclusion
confirmed-only 完整 artifact
explicit offset/limit pagination 與 next offset
指定 DFF SAT detail
AND-only diagnostic 與 no-pattern detail
missing DFF error envelope
all-DFF SAT 與 unknown option rejection
functional match、candidate truncation 與 query timeout propagation
functional option dependency / range validation
help text 與一命令一 envelope
```

### 修改內容

正式程式碼：無。現有 counter 聚合、record filtering、parser validation 與 status propagation
符合 report contract。

## SEQ-006：NewTestCase/test40 prompt 12/13 最終實測

- 狀態：`Verified`
- 優先級：P0
- 類型：Official Prompt Coverage / Performance
- Boolean 相關：只計 canonical confirmed matches

### 實際命令

```text
sequential_query enable_hold all --confirmed-only
sequential_query enable_hold all --summary-only
```

### 驗證結果

| 項目 | 結果 |
|---|---:|
| total/analyzed DFF | 2585 / 2585 |
| matched/candidate DFF | 1583 / 1583 |
| prompt 12 artifact records | 1583 |
| unique DFF names | 1583 |
| artifact size | 3,226,139 bytes |
| 兩個 query 加 read/exit 外部總時間 | 約 0.2 秒 |
| error / partial | 0 / 0 |

測試 artifact：

```text
Blup/sequential_pattern_runs/test40_baseline/test40_sequential_query_3.txt
```

prompt 12 可由完整 artifact 回答每顆 DFF 的 D-input enable/hold structure；prompt 13 可直接
使用 `matched_dff_count=1583` 回答 unique flip-flop 數量。

## 5. 後續順序

1. SEQ-001 已完成，不再要求 LLM 逐頁取得一般 all-DFF detail。
2. SEQ-003 已完成：canonical forms、wrapper、polarity、data inversion 與 feedback ownership 均通過。
3. SEQ-004 已完成：optional SAT confirmation 與 transformation 後 detectability 通過。
4. SEQ-005 已完成：count 去重、diagnostic、pagination、timeout 與 error contract 通過。
5. SEQ-006 已完成：NewTestCase prompt 12/13 的完整性、唯一計數與時間均通過。
6. Sequential canonical 階段完成；SEQ-002 維持 Boolean-Deferred，待後期 AIG 重構。
