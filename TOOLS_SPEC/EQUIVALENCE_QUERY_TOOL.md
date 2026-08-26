# Equivalence Query Tool

## 1. 責任

`equiv_query` 比較 current design 與 session 保存的 original 或 previous-edit baseline，在所有同名 PO 與 DFF.D combinational boundaries 上執行 whole-design SAT equivalence。

完整性規則：不得自行限制 compared boundaries；只有 complete result 才能當成完整 proof。
時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

當 proof object 是兩份 design states 在同名 PO 與 DFF.D boundaries 上的 whole-design
equivalence 時使用本 tool。baseline identity 決定 mode：最近一次成功 `read` 的 snapshot 使用
`original`；最近一次 edit/optimization transaction 之前的 snapshot 使用 `previous_edit`。

同一份 current design 內兩條已知 nets 的 Boolean equivalence 使用 `func_query equivalence`；
上一筆 operation 的 certificate/delta 使用 `report_query last_edit`。不能只因 prompt 出現
`equivalent` 或 `previous` 就選本工具，必須確認比較單位是 design state，而不是 signals 或 report。

`edit_apply`/`opt_apply` 自己的 validation report 用於判斷該 transaction 是否可 commit；
只有 prompt 額外要求 whole-design baseline comparison 時才另呼叫 `equiv_query`。

## 3. Command Grammar

```text
equiv_query original [time_budget_seconds]
equiv_query previous_edit [time_budget_seconds]
```

時間預設 290 秒，必須是有限且大於 0 的數字。prompt 沒有指定 budget 時省略此參數，
不要自行縮短。

| Mode | Baseline | 適用問題 |
|---|---|---|
| `original` | 最近一次成功 `read` 建立的 snapshot | 最終 current 是否仍等價於原始題目設計 |
| `previous_edit` | 最近一次 `edit_apply` 或 `opt_apply` 前 snapshot | 上一步 edit/optimization 是否保持功能 |

## 4. 輸出判讀

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

只有 envelope `status:ok`、`complete:true` 且 `equivalent:true` 時，才能宣稱已完整證明等價。
`equivalent:false` 且 complete=true 表示已找到 mismatch。timeout/partial/unsupported 不能
當成 proof；若文字題仍要求 yes/no，依 `LLM_NOTES.md` 的 Competition Answer Policy 選擇
候選答案並記錄推定。若此結果是 transformation 的 commit validation，則禁止猜測，
驗證未完成必須 rollback 或保留 original。

interface mismatch 是完整且明確的不等價結果，因此可能仍為 `status:ok, complete:true, equivalent:false`；應回報 missing/extra names。

## 5. Prompt Examples

```text
Prompt: Is the current design equivalent to the original design?
Command: equiv_query original
Read: complete, equivalent, mismatch/interface fields
```

```text
Prompt: Did the previous edit preserve functionality?
Command: equiv_query previous_edit
Read: complete, equivalent
```

## 6. 限制

- `original` 需要先成功 `read`；`previous_edit` 需要先執行 `edit_apply` 或 `opt_apply`。
- `previous_edit` 對應最近一次 edit/optimization 前的 snapshot；若最近一次操作失敗、rollback 或 timeout，應先讀該次 `report_query last_edit`，不可拿更早一次成功 edit 的 delta 或 equivalence 結論回答本題。
- 目前比較 PO 與 DFF.D next-state combinational boundary，不包含 DFF initial state 或 multi-cycle sequential equivalence。
- 同一 design 內兩條 internal nets 的 equivalence 使用 `func_query equivalence`。
