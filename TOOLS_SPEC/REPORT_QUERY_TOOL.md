# Report Query Tool

## 1. 責任

`report_query` 讀取 session 中最近一次 `edit_apply` 或 `opt_apply` 保存的完整 `NetlistEditReport`，用於回答「上一步改了什麼」、「移除了多少」或「最佳化後 depth 如何」等 follow-up prompt。它不重新執行 operation，也不重新計算 report。

完整性規則：不得把 changed-name sample 當成全部；只問數量時讀 delta/count 欄位並回摘要。
時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

當 requested state 是最近一次 edit/optimization transaction，且 metric 是該 operation 的
before/after delta、專屬 pass count、commit/rollback 狀態、constraint validation 或 changed
identities 時使用本 tool。它回答的是 operation provenance，不是重新分析 current design。

若問題只要 current design 現況，即使緊接在修改後，也應重新呼叫 owning query；若要比較
current 與 original/previous design 的 Boolean equivalence，使用 `equiv_query`；若 operation 尚未
發生或要求再次執行，使用 `edit_apply`/`opt_apply`。不能只因 prompt 說「現在」「之前」或
「多少」就選擇本工具，必須確認答案來源是 cached last-edit report。

## 3. Command Grammar

```text
report_query last_edit
```

在任何 `edit_apply`/`opt_apply` 前呼叫會回 `NO_LAST_EDIT_REPORT`。新的 edit 或 optimization apply 會覆蓋上一筆 cached report；read-only query、equivalence 與 write 不會覆蓋它。新的 `read` 會清除 cache。

## 4. 輸出判讀

輸出欄位與 [`EDIT_APPLY_TOOL.md`](EDIT_APPLY_TOOL.md) 的 edit report 相同，主要包括：

```text
report_success
report_changed
rolled_back
before_stats / after_stats
diff
validation
mapping_delta
constant_simplification
fanout_change
cost_change
optimization
cleanup_fixpoint / dead_logic / redundancy_removal
structural_merge / dff_merge / functional_merge
changed_gate_names / changed_net_names
warnings
```

## 5. Prompt Examples

```text
Prompt: What changed in the previous step?
Command: report_query last_edit
Read: report_changed, diff, changed names, warnings
```

```text
Prompt: How many gates were removed by the last cleanup?
Command: report_query last_edit
Read: 依 operation_name 讀專屬 summary（例如 dead_logic.removed_gate_count 或
      redundancy_removal.removed_gate_count）；只有沒有專屬欄位時才使用 diff.active_gate_count_delta
```

```text
Prompt: Did the previous mapping leave any banned gate types?
Command: report_query last_edit
Read: mapping_delta.final_gate_count_by_type 與 validation
```

```text
Prompt: How did the previous depth optimization perform?
Command: report_query last_edit
Read: cost_change.metric_name/before_value/after_value/improved，以及
      optimization.candidate_generated/candidate_accepted/final_constraints_satisfied；
      不讀 legacy whole_design_equivalent 作為等價結論
```

## 6. 限制

- 只能取得最近一筆 edit/optimization apply，沒有歷史 report list。
- `report_query` 的成功只代表 cache 可讀，不表示上一筆 operation 一定成功；仍需讀 `report_success`、`rolled_back`。
- edit report 的 local/structural certificate 不取代題目要求的 whole-design equivalence 時，應再呼叫 `equiv_query`。
