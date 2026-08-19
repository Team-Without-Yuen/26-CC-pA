# Depth Query Tool

## 1. 責任

`depth_query` 負責量測 current design 的 combinational arrival depth、PO/DFF.D endpoint depth、critical path 與 threshold count。它是 read-only analysis，不會降低或最佳化 depth。

完整性規則：all/threshold mode 不限制 endpoint 筆數。record 數或預估字元量超過 terminal 門檻時，工具自動
完整寫入 artifact；response 只回 count、worst summary、完整性、格式與 `output_file`。
詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

prompt 出現 `logic depth`、`level`、`critical path`、`maximum depth`、`deepest output`、`depth greater than N` 或 `gate on critical path` 時使用本 tool。

若動詞是 `reduce`、`minimize`、`optimize`、`best`，或題目提供 cost function，屬於 optimization，不是 `depth_query`。

## 3. Command Grammar

```text
depth_query <mode> [args]
```

## 4. Modes

| Mode | 參數 | 用途 | 主要 data |
|---|---|---|---|
| `net` | `<net>` | 指定 net 的 depth 與代表性 critical path | `Report count`, endpoint depth, path |
| `all_po` | 無 | 所有 PO depths | endpoint/depth list, `Worst endpoint` |
| `all_dff_d` | 無 | 所有 DFF.D depths | endpoint/depth list, `Worst endpoint` |
| `global_critical` | 無 | PO 與 DFF.D 中的全域最大 depth | `Worst endpoint`, `depth`, path |
| `exceeding` | `<depth>` | 超過 threshold 的 PO 與 DFF.D endpoints | `Report count`, endpoint/depth list |
| `po_exceeding` | `<depth>` | 只計算超過 threshold 的 primary outputs | `Report count`, endpoint/depth list |
| `gate_on_critical` | `<gate>` | gate 是否位於任一條 global maximum-depth path | `Gate on critical path`；顯示的 worst path 仍只是一條代表路徑 |
| `deepest_output` | 無 | fanin depth 最大的 primary output | `Worst endpoint`, depth, path |

## 5. 輸出判讀

| 題目 | 應讀欄位 |
|---|---|
| How many outputs have depth greater than 4? | `po_exceeding 4` 的 `Report count` |
| How many endpoints exceed depth 4? | `exceeding 4` 的 `Report count` |
| Maximum logic depth | `global_critical` 的 `Worst endpoint ... depth=` |
| Critical path gates/nets | `global_critical` 的 path `Gates` / `Nets` |
| Specific net depth | `net` report 中對應 endpoint depth |

`exceeding` 包含 PO 與 DFF.D；題目明確說 outputs 時必須使用 `po_exceeding`。

小型結果直接完整列出。大型 endpoint list 或 critical path 使用
`QUERY_LIST_ARTIFACT_V1`；artifact 的 `Endpoint depth reports` 保存每個 endpoint 與 depth，
`Worst path nets/gates in start-to-end order` 保存代表性 critical path。`Complete: yes` 才能
把檔案視為完整結果。

## 6. Prompt Examples

```text
Prompt: How many outputs have a logic depth greater than 4?
Command: depth_query po_exceeding 4
Read: Report count
```

```text
Prompt: Report the current maximum logic depth and critical path.
Command: depth_query global_critical
Read: Worst endpoint, depth, Nets, Gates
```

```text
Prompt: Is gate g10 on the critical path?
Command: depth_query gate_on_critical g10
Read: Gate on critical path
```

若 prompt 將 startpoint 明確限制為 PI，例如「PI 到 PO 的最長路徑」或「PI 到 DFF.D
的最大 depth」，不可直接使用 timing-global `depth_query global_critical`，因為後者同時允許
DFF.Q start boundary。應改用：

```text
path_query max_depth all_pi all_po
path_query max_depth all_pi all_dff_d
```

## 7. 限制

- 同一 maximum depth 可能有多條 paths；`global_critical` 回傳一條代表性 path，不代表唯一
  critical path。`gate_on_critical` 則判斷任一條 maximum-depth path，不只檢查該代表路徑。
- depth traversal 以 combinational boundary 處理 DFF，DFF.Q 為 start boundary，DFF.D 為 endpoint boundary。
- `deepest_output` 只比較 PO；全域最差 endpoint 使用 `global_critical`。
