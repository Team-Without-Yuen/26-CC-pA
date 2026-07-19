# Equivalence Query Tool

## 1. 責任

`equiv_query` 比較 current design 與 session 保存的 original 或 previous-edit baseline，在所有同名 PO 與 DFF.D combinational boundaries 上執行 whole-design SAT equivalence。

## 2. Command Grammar

```text
equiv_query original [time_budget_seconds]
equiv_query previous_edit [time_budget_seconds]
```

時間預設 240 秒，必須是有限且大於 0 的數字。

| Mode | Baseline | 適用問題 |
|---|---|---|
| `original` | 最近一次成功 `read` 建立的 snapshot | 最終 current 是否仍等價於原始題目設計 |
| `previous_edit` | 最近一次 `edit_apply` 前 snapshot | 上一步 edit 是否保持功能 |

## 3. 輸出判讀

| 欄位 | 意義 |
|---|---|
| `report_ok` | checker 是否完成可判定結果 |
| `equivalent` | 所有已比較 boundaries 是否等價 |
| `method` | 應為 `WholeDesignSat` |
| `time_budget_exceeded` | 是否超過整體 budget |
| `compared_output_count`, `compared_dff_d_count` | 已比較數量 |
| `mismatched_output_names`, `mismatched_dff_d_names` | 明確不等價 boundaries |
| `skipped_output_names`, `skipped_dff_d_names` | 未完成比較 boundaries |
| `missing_*`, `extra_*` | 兩份 design interface 差異 |
| `unsupported_reasons`, `warnings` | 無法完成的原因 |

只有 envelope `status:ok`、`complete:true` 且 `equivalent:true` 時，才能宣稱已完整證明等價。`equivalent:false` 且 complete=true 表示已找到 mismatch。timeout/partial/unsupported 不能判成等價或不等價。

interface mismatch 是完整且明確的不等價結果，因此可能仍為 `status:ok, complete:true, equivalent:false`；應回報 missing/extra names。

## 4. Prompt Examples

```text
Prompt: Is the current design equivalent to the original design?
Command: equiv_query original 240
Read: complete, equivalent, mismatch/interface fields
```

```text
Prompt: Did the previous edit preserve functionality?
Command: equiv_query previous_edit 60
Read: complete, equivalent
```

## 5. 限制

- `original` 需要先成功 `read`；`previous_edit` 需要先執行 `edit_apply`。
- 目前比較 PO 與 DFF.D next-state combinational boundary，不包含 DFF initial state 或 multi-cycle sequential equivalence。
- 同一 design 內兩條 internal nets 的 equivalence 使用 `func_query equivalence`。
