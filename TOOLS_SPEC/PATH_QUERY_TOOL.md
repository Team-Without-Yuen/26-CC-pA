# Path Query Tool

## 1. 責任

`path_query` 負責明確 startpoint 與 endpoint 之間的 combinational path、路徑限制、register-to-register path、mandatory nodes 與 separator/cut。對外不使用 legacy `reg_path_query` 或 `graph_query`。

## 2. 選擇條件

prompt 出現 `path from A to B`、`through`、`avoid`、`every path`、`shortest`、`longest between endpoints`、`register-to-register`、`mandatory`、`separator` 或 `cut` 時使用本 tool。

只問某物件可到達的完整範圍使用 `cone_query`；全設計 maximum depth/critical endpoint 使用 `depth_query`。

## 3. Endpoint 與 Node

一般語法：

```text
path_query <mode> <start_endpoint> <end_endpoint> [options]
```

endpoint tokens：

| Token | 意義 |
|---|---|
| `net:<n>` 或 bare net | 指定 net |
| `pi:<p>`, `po:<p>` | 指定 PI/PO |
| `all_pi`, `all_po` | 所有 PI/PO |
| `dff_q:<ff>`, `dff_d:<ff>` | 指定 DFF Q/D boundary |
| `all_dff_q`, `all_dff_d` | 所有 DFF Q/D boundaries |
| `dff_clk:<ff>[:pin]` | 指定 DFF clock pin |
| `dff_reset:<ff>[:pin]` | 指定 DFF reset/set pin |
| `gate_out:<g>` | gate output |
| `gate_in:<g>:<index_or_pin>` | gate input pin |

required/avoided nodes 使用 `gate:<g>`、`net:<n>` 或 bare net；bare token 預設視為 net。

## 4. Modes

| Mode | 語法 | 用途 | 主要 data |
|---|---|---|---|
| `exists` | `<start> <end> [-req ...] [-avoid ...]` | 是否至少存在一條符合限制的 path | `Yes` / `No` |
| `find_any` | `<start> <end> [constraints]` | 回傳任一條 path | `Depth`, `Nets`, `Gates` |
| `enumerate` | `<start> <end> [constraints/options]` | 列舉或計數所有 paths | `Total paths`, `Complete enumeration`, stop fields |
| `min_depth` | `<start> <end> [constraints]` | 最短 path | path `Depth`, `Nets`, `Gates` |
| `max_depth` | `<start> <end> [constraints]` | 最長 path | path `Depth`, `Nets`, `Gates` |
| `every_through` | `<start> <end> -req <node...>` | 每條 path 是否都通過 required nodes | `Yes` / `No` |
| `every_avoids` | `<start> <end> -avoid <node...>` | 每條 path 是否都避開指定 nodes | `Yes` / `No` |
| `mandatory_nodes` | `<start> <end>` | 所有 path 都經過的 internal nets | `Path exists`, `Mandatory internal nets` |
| `is_separator` | `<start> <end> <candidate_net>` | candidate 是否切斷指定 endpoints | `Is separator`, witness fields |
| `pi_po_cut` | `<candidate_net>` | candidate 是否為 PI-to-PO directed cut | `Is separator`, witness fields |
| `direct_pi_po` | `[-max_print N]` | 所有 depth-0 PI-to-PO direct connections | `Total direct PI-to-PO connections` |

## 5. Enumeration Options

| Option | 用途 |
|---|---|
| `-req <node...>` | path 必須經過的 nodes；遇到下一個 option 結束 |
| `-avoid <node...>` | path 必須避開的 nodes |
| `-out <file>` | 將列舉 paths 寫入檔案 |
| `-max_print <N>` | 最多在 terminal data 顯示 N 條 |
| `-max_paths <N>` | 最多列舉 N 條後停止 |
| `-time_limit <seconds>` | enumeration 時間上限 |
| `-count_only` | 只計數，不保存/顯示每條 path |

對 `enumerate`，只有 envelope `complete:true` 且 data `Complete enumeration: yes` 時，`Total paths` 才是精確完整總數。`partial`、`timeout` 或 path-limit 結果只能回報已找到數量與停止原因。

## 6. Prompt Examples

```text
Prompt: Is there a path from input a to output y that avoids n8?
Command: path_query exists pi:a po:y -avoid net:n8
Read: Yes/No；先確認 complete=true
```

```text
Prompt: Count all register-to-register paths.
Command: path_query enumerate all_dff_q all_dff_d -count_only -time_limit 30
Read: Total paths；只有 Complete enumeration=yes 才是完整答案
```

```text
Prompt: Find the longest path from ff1.Q to ff2.D.
Command: path_query max_depth dff_q:ff1 dff_d:ff2
Read: Depth, Nets, Gates
```

```text
Prompt: Is n10 a PI-to-PO cut net?
Command: path_query pi_po_cut n10
Read: Is separator
```

## 7. 限制

- 大型設計的 all-path enumeration 可能指數成長，優先設定 `-time_limit`、`-max_paths` 或 `-count_only`。
- `max_depth` 是指定 endpoints 間的最長 path；全域 critical path 應使用 `depth_query global_critical`。
- 空的 mandatory list 可能表示 path 存在但沒有 mandatory internal net；必須同時讀 `Path exists`。
