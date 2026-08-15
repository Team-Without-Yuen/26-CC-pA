# Cone Query Tool

## 1. 責任

`cone_query` 負責指定 net/gate 的多層 transitive fanin/fanout 集合、cone gate-type breakdown、shared fanin 及最大 output cone。它回答「範圍內有哪些物件」，不證明特定 endpoints 間的 path condition。

完整性規則：要求 cone list 時取得全部 gates/nets，不自行限制筆數；只問數量或 type breakdown
時只回摘要。大型 cone list 由 CLI 自動完整寫入唯一 artifact，terminal 保留 gates/nets、type
breakdown、artifact completeness 與 `output_file`。內部門檻只決定輸出位置，不限制 cone
大小，也不由 LLM 設定。時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

prompt 出現 `transitive fanin`、`transitive fanout`、`cone`、`reachable`、`can affect`、`shared fanin` 或 `largest cone` 時使用本 tool。

只問一層 driver/load 使用 `structure_query`；指定 A 到 B 的 path/through/avoid 使用 `path_query`；depth/critical 使用 `depth_query`。

## 3. Command Grammar

```text
cone_query <mode> [name] [with_paths]
```

## 4. Modes

| Mode | 參數 | 用途 | 主要 data |
|---|---|---|---|
| `net_fanin` | `<net> [with_paths]` | net 的 transitive fanin | `gates`, `nets`, `Gate type counts`, cone names |
| `net_fanout` | `<net> [with_paths]` | net 的 transitive fanout | 同上 |
| `gate_fanin` | `<gate> [with_paths]` | gate 的 transitive fanin | 同上 |
| `gate_fanout` | `<gate> [with_paths]` | gate 的 transitive fanout | 同上 |
| `largest_output` | `[with_paths]` | 比較所有 PO fanin cones | `source`, `checked primary outputs`, cone 統計 |
| `shared_fanin` | `<net_a> <net_b> [with_paths]` | 兩個 nets 的 shared fanin gates | `source`, `second source`, `Cone gates` |

`with_paths` 額外要求 local longest/shortest path 摘要，主要欄位為 `longest local path depth`、`shortest local path depth` 與 longest path nets。

## 5. 輸出判讀

| 題目 | 應讀欄位 |
|---|---|
| cone 內有多少 gates/nets | `gates`, `nets` |
| cone 內某 gate type 數量 | `Gate type counts` 對應 type |
| 列出 cone gates/nets | `Cone gates`, `Cone nets` |
| 哪個 output cone 最大 | `source` 與 `gates` |
| 兩 cone 共用哪些 gates | `Cone gates` |

大型 cone 改讀 `list artifact complete`、`list entry count` 與 `output_file`；只有 envelope
`complete:true`、artifact complete 為 `yes`，且檔案 footer 為 `Complete: yes` 時才是完整名單。

## 6. Prompt Examples

```text
Prompt: How many NOR gates are in the fanin cone of n15?
Command: cone_query net_fanin n15
Read: Gate type counts 中的 NOR
```

```text
Prompt: Which gates can be affected by n3?
Command: cone_query net_fanout n3
Read: Cone gates
```

```text
Prompt: Find gates shared by the fanin cones of n10 and n12.
Command: cone_query shared_fanin n10 n12
Read: Cone gates
```

## 7. 限制

- `with_paths` 是 cone 內 local path 摘要，不等於列出所有 paths。
- DFF.Q 是 combinational sequential boundary；`cone_query net_fanin <dff_q_net>` 不會回傳任何 combinational gate（`gates: 0`），也不會穿透到同一顆 DFF 的 D input。report 仍可能保留 query root 本身，因此 `nets` 可為 1。
- cone traversal 不會跨越 sequential state；DFF 的 D/clock/reset 等 pin 不可由 Q 的 fanin query 反推。
- 若題目只問數量或 gate-type breakdown，只讀 `gates`、`nets` 或 `Gate type counts`；不要把
  完整 `Cone gates` / `Cone nets` 複製進答案。題目明確要求列出物件時，必須輸出全部
  confirmed entries；大型結果使用工具自動產生的 artifact，不得用固定筆數、省略號或摘要
  取代完整清單。
- 若題目要求「所有 A-to-B paths 是否都經過某點」，必須改用 `path_query`。
