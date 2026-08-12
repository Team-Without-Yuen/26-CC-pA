# Sequential Query Tool

## 1. 責任

`sequential_query` 從 DFF D-input combinational logic 辨識 enable、hold、feedback MUX 與 AND-gated candidate pattern。它不負責一般 DFF listing 或 clock/reset direct connectivity。

完整性規則：all-DFF detail 預設完整寫入唯一 artifact；functional candidate/match/data caps
不得被當成完整 all-result。時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

prompt 出現 `enable condition`、`hold condition`、`feedback MUX` 或 `register-control pattern` 時使用本 tool。`List all DFFs` 與 `DFFs driven by clock n0` 使用 `structure_query`。

## 3. Command Grammar

```text
sequential_query enable_hold <all|dff_name> [options]
```

## 4. Options

| Option | 用途 |
|---|---|
| `--summary-only` | 只輸出統計，不列 DFF records |
| `--confirmed-only` | 只列 confirmed enable/hold matches，排除 non-match diagnostic |
| `--include-and-candidates` | 包含 AND-gated data-gating non-match diagnostic；目前預設開啟 |
| `--include-no-pattern` | all-DFF query 也列沒有 pattern 的 DFF |
| `--offset <N>` | 題目明確要求 record window 時指定起始 offset，並停用自動 artifact |
| `--limit <N>` | 題目明確要求前 N 筆或頁面時指定大小，並停用自動 artifact |
| `--verify-sat` | 對 canonical match 做 SAT 驗證；只允許指定單一 DFF |
| `--functional-fallback` | canonical 無 confirmed match 時啟用 SAT cofactor search |
| `--max-functional-candidates <N>` | 每顆 DFF 最多檢查的 searchable control candidates |
| `--max-functional-matches <N>` | 每顆 DFF 最多回傳的 functional patterns |
| `--functional-find-any` | 找到第一個 SAT-proven match 即停止 |
| `--resolve-functional-data` | 啟用具名 data net 搜尋 |
| `--no-resolve-functional-data` | 關閉具名 data net 搜尋，適合存在性與計數題 |
| `--max-functional-data-candidates <N>` | 每筆 match 的 data net 搜尋上限 |
| `--no-functional-simulation-filter` | 停用 simulation prefilter，僅供除錯或 A/B 驗證 |
| `--functional-simulation-patterns <N>` | bit-parallel simulation patterns，範圍 1..4096 |
| `--functional-per-dff-time-limit <SEC>` | 明確指定單顆 DFF 上限；未指定時由總預算自動公平分配 |
| `--functional-time-limit <SEC>` | 整次 sequential query 的 functional search 總預算；預設 290 秒 |

`--summary-only` 不可搭配 offset/limit。全設計 `all` 不允許 `--verify-sat`，避免超過總時間限制。
所有 functional cost/options 都必須搭配 opt-in `--functional-fallback`；未指定時維持原本 canonical fast path。

沒有 `--summary-only`、`--offset` 或 `--limit` 的 all-DFF detail 會自動把全部 records 寫入
`<design>_sequential_query_<sequence>.txt`，且不覆寫既有檔案。指定單一 DFF 仍直接在 envelope
回 detail。

## 5. 輸出判讀

摘要欄位：

| 欄位 | 意義 |
|---|---|
| `total_dff_count` | query scope 的 DFF 總數 |
| `analyzed_dff_count` | 成功分析數 |
| `matched_dff_count` | 有 confirmed pattern 的 DFF 數 |
| `candidate_dff_count` | 具有 canonical/functional enable-hold candidate 的 DFF 數；AND-only data gating 不計入 |
| `complete`, `timed_out` | 搜尋是否完整，以及是否耗盡 functional time budget |
| `functional_candidate_count` | functional structural control candidates 總數 |
| `functional_searchable_candidate_count` | 排除 simulation-safe Reject 後仍需檢查的 candidates |
| `functional_candidates_examined` | 實際進入 reachability/SAT loop 的 candidates |
| `functional_unexamined_candidate_count` | 尚未檢查的 searchable candidates |
| `functional_inconclusive_candidate_count` | 已進入但 timeout/unknown/unsupported 的 candidates |
| `functional_simulation_rejected_candidate_count` | simulation 已安全排除且不占 quota 的 candidates |
| `functional_sat_check_count` | functional SAT checks |
| `functional_match_count` | functional patterns 數；同一 DFF 可能不只一筆 |
| `records_truncated` | detail page 是否未列完 |
| `next_record_offset` | 下一頁 offset |
| `wrote_records_to_file` | 是否將 all-DFF detail 寫入 artifact |
| `artifact_record_count` | artifact 中的完整或 partial DFF record 數 |
| `output_file` | artifact 路徑 |

每個 `dff_record` 另包含 functional attempted/complete/timeout、candidate limit 與各類 candidate counters。每個 `pattern` 主要包含：

```text
kind
detection_method
confirmed
semantics_pending
enable_net_name
active_level_name
hold_level
data_net_name
data_function_resolved
data_search_attempted
data_search_complete
data_search_timed_out
feedback_net_name
hold_functionally_proven
load_functionally_proven
solver_status
evidence_gate_names
```

只有 `confirmed:true` 可直接當成已確認 enable/hold pattern。`semantics_pending:true` 是候選，不應改寫成官方語意已確認。

## 6. Prompt Examples

```text
Prompt: How many DFFs have a confirmed enable/hold pattern?
Command: sequential_query enable_hold all --summary-only --confirmed-only
Read: matched_dff_count
```

```text
Prompt: What is the enable and hold behavior of ff1?
Command: sequential_query enable_hold ff1 --confirmed-only --verify-sat
Read: pattern confirmed, enable_net_name, active_level_name, data/feedback fields
```

```text
Prompt: List all recognized DFF patterns.
Command: sequential_query enable_hold all
Read: complete、available_dff_record_count、artifact_record_count、output_file
```

```text
Prompt: Does ff1 have any functionally equivalent enable/hold implementation?
Command: sequential_query enable_hold ff1 --functional-fallback --functional-find-any --no-resolve-functional-data
Read: complete, matched_dff_count, functional_match_count, detection_method
```

```text
Prompt: Identify all proven controls and named data signals for ff1.
Command: sequential_query enable_hold ff1 --functional-fallback --resolve-functional-data --max-functional-matches 8
Read: complete, per-DFF counters, enable/data/active/hold fields
```

## 7. 限制

- canonical MUX hold pattern 可確認。`D = EN & DATA` 沒有同一顆 DFF 的 Q feedback，只能回報 `DataGatingWithoutHoldFeedback` / `DATA_GATING_WITHOUT_HOLD_FEEDBACK` non-match diagnostic；不計入 `matched_dff_count` 或 `candidate_dff_count`。
- functional fallback 是 bounded SAT search；`complete:false` 時，已找到的 matches 只是已證明
  下界。正式競賽答案不可只回 unknown，應依 `LLM_NOTES.md` 的 Competition Answer Policy
  提交最可能數量或名單；推定項目不得混入 confirmed match 欄位。
- 數量題讀 `matched_dff_count`，不要使用可能對同一 DFF 重複計數的 `functional_match_count`。
- all-DFF functional fallback 預設不啟用；啟用後若 prompt 未指定更短時限，沿用 290 秒總預算與自動 per-DFF 公平分配。
- all-DFF detail 的完整 records 位於 `output_file`，正式答案回總數、完整性與路徑，不把
  artifact 全文貼回 response。
- 明確使用 offset/limit 時保留 pagination compatibility；此時 envelope 尚有下一頁會回
  `partial`，不得把單頁內容宣稱為完整名單。
- 本 tool 不進行 multi-cycle sequential equivalence。
