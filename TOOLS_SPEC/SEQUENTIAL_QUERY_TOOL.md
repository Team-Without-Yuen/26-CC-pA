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

`--summary-only` 不可搭配 offset/limit。全設計 `all` 不允許 `--verify-sat`，避免超過 testcase 時限。

## 5. 輸出判讀

摘要欄位：

| 欄位 | 意義 |
|---|---|
| `total_dff_count` | query scope 的 DFF 總數 |
| `analyzed_dff_count` | 成功分析數 |
| `matched_dff_count` | 有 confirmed pattern 的 DFF 數 |
| `candidate_dff_count` | 包含未確認 candidate 的 DFF 數 |
| `records_truncated` | detail page 是否未列完 |
| `next_record_offset` | 下一頁 offset |

每個 `dff_record` 主要包含 `dff_name`、D/Q net、`matched`、`pattern_count`。每個 `pattern` 主要包含：

```text
kind
detection_method
confirmed
semantics_pending
enable_net_name
active_level_name
data_net_name
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

## 7. 限制

- canonical MUX hold pattern可確認；AND-only candidate 的完整官方語意仍需保守判讀。
- detail pagination 的 envelope 會在尚有下一頁時回 `partial`；需取完所有頁面才能宣稱完整列表。
- 本 tool 不進行 multi-cycle sequential equivalence。
