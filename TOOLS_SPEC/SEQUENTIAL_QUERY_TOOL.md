# Sequential Query Tool

## 1. 責任

`sequential_query` 從 DFF D-input combinational logic 辨識 enable、hold、feedback MUX 與 AND-gated candidate pattern。它不負責一般 DFF listing 或 clock/reset direct connectivity。

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
| `--confirmed-only` | 排除 semantics pending 的 AND-gated candidates |
| `--include-and-candidates` | 包含 AND-gated candidates；目前預設開啟 |
| `--include-no-pattern` | all-DFF query 也列沒有 pattern 的 DFF |
| `--offset <N>` | detail records 起始 offset |
| `--limit <N>` | detail records 頁面大小；預設 50 |
| `--verify-sat` | 對 canonical match 做 SAT 驗證；只允許指定單一 DFF |
| `--functional-fallback` | canonical 無 confirmed match 時啟用 SAT cofactor search |
| `--max-functional-candidates <N>` | 每顆 DFF 最多檢查的 searchable control candidates |
| `--max-functional-matches <N>` | 每顆 DFF 最多回傳的 functional patterns |
| `--functional-find-any` | 找到第一個 SAT-proven match 即停止 |
| `--resolve-functional-data` | 啟用具名 data net 搜尋；C++ 預設為開啟 |
| `--no-resolve-functional-data` | 關閉具名 data net 搜尋，適合存在性與計數題 |
| `--max-functional-data-candidates <N>` | 每筆 match 的 data net 搜尋上限 |
| `--no-functional-simulation-filter` | 停用 simulation prefilter，僅供除錯或 A/B 驗證 |
| `--functional-simulation-patterns <N>` | bit-parallel simulation patterns，範圍 1..4096 |
| `--functional-per-dff-time-limit <SEC>` | 單顆 DFF 的 functional search 時間上限 |
| `--functional-time-limit <SEC>` | 整次 sequential query 的 functional search 時間上限 |

`--summary-only` 不可搭配 offset/limit。全設計 `all` 不允許 `--verify-sat`，避免超過 testcase 時限。
所有 functional cost/options 都必須搭配 opt-in `--functional-fallback`；未指定時維持原本 canonical fast path。

## 5. 輸出判讀

摘要欄位：

| 欄位 | 意義 |
|---|---|
| `total_dff_count` | query scope 的 DFF 總數 |
| `analyzed_dff_count` | 成功分析數 |
| `matched_dff_count` | 有 confirmed pattern 的 DFF 數 |
| `candidate_dff_count` | 包含未確認 candidate 的 DFF 數 |
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
Command: sequential_query enable_hold all --offset 0 --limit 50
Follow-up: records_truncated=true 時以 next_record_offset 查下一頁
```

```text
Prompt: Does ff1 have any functionally equivalent enable/hold implementation?
Command: sequential_query enable_hold ff1 --functional-fallback --functional-find-any --no-resolve-functional-data --functional-time-limit 2
Read: complete, matched_dff_count, functional_match_count, detection_method
```

```text
Prompt: Identify all proven controls and named data signals for ff1.
Command: sequential_query enable_hold ff1 --functional-fallback --resolve-functional-data --max-functional-matches 8 --functional-time-limit 5
Read: complete, per-DFF counters, enable/data/active/hold fields
```

## 7. 限制

- canonical MUX hold pattern可確認；AND-only candidate 的完整官方語意仍需保守判讀。
- functional fallback 是 bounded SAT search；`complete:false` 時只能把已找到的 matches 當作已證明下界，不能回答「不存在」或「已完整列出」。
- 數量題讀 `matched_dff_count`，不要使用可能對同一 DFF 重複計數的 `functional_match_count`。
- all-DFF functional search 必須設定合理的 candidate 與時間上限；預設不啟用。
- detail pagination 的 envelope 會在尚有下一頁時回 `partial`；需取完所有頁面才能宣稱完整列表。
- 本 tool 不進行 multi-cycle sequential equivalence。
