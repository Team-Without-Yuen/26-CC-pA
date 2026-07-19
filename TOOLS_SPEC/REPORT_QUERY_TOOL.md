# Report Query Tool

## 1. 責任

`report_query` 讀取 session 中最近一次 `edit_apply` 保存的完整 `NetlistEditReport`，用於回答「上一步改了什麼」或「移除了多少」等 follow-up prompt。它不重新執行 edit，也不重新計算 report。

## 2. Command Grammar

```text
report_query last_edit
```

在任何 `edit_apply` 前呼叫會回 `NO_LAST_EDIT_REPORT`。新的 edit 會覆蓋上一筆 cached report；query、equivalence 與 write 不會覆蓋它。

## 3. 輸出判讀

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
changed_gate_names / changed_net_names
warnings
```

## 4. Prompt Examples

```text
Prompt: What changed in the previous step?
Command: report_query last_edit
Read: report_changed, diff, changed names, warnings
```

```text
Prompt: How many gates were removed by the last cleanup?
Command: report_query last_edit
Read: diff.active_gate_count_delta；負值的絕對值為 active gate reduction
```

```text
Prompt: Did the previous mapping leave any banned gate types?
Command: report_query last_edit
Read: mapping_delta.final_gate_count_by_type 與 validation
```

## 5. 限制

- 只能取得最近一筆 edit，沒有歷史 report list。
- `report_query` 的成功只代表 cache 可讀，不表示上一筆 edit 一定成功；仍需讀 `report_success`。
- edit report 的 local/structural certificate 不取代題目要求的 whole-design equivalence 時，應再呼叫 `equiv_query`。
