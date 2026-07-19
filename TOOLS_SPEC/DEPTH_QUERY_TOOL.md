# Depth Query Tool

## 1. 責任

`depth_query` 負責量測 current design 的 combinational arrival depth、PO/DFF.D endpoint depth、critical path 與 threshold count。它是 read-only analysis，不會降低或最佳化 depth。

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
| `gate_on_critical` | `<gate>` | gate 是否位於目前代表性 global critical path | `Gate on critical path` |
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

## 7. 限制

- 同一 maximum depth 可能有多條 paths；`global_critical` 回傳一條代表性 path，不代表唯一 critical path。
- depth traversal 以 combinational boundary 處理 DFF，DFF.Q 為 start boundary，DFF.D 為 endpoint boundary。
- `deepest_output` 只比較 PO；全域最差 endpoint 使用 `global_critical`。
