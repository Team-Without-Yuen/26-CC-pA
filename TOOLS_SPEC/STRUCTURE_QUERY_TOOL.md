# Structure Query Tool

## 1. 責任

`structure_query` 負責 current active netlist 的基本數量、物件列表、物件資訊、structural issue 與一層直接 connectivity。它不處理 transitive cone、endpoint path、logic depth 或 Boolean property。

完整性規則：list mode 預設取得全部 active objects，不自行限制筆數；只問 count 時只回摘要。
時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

下列 prompt 優先使用本 tool：

- `how many`、`list`、`gate type`、`PI/PO/DFF count`。
- `floating`、`undriven`、`no load`、`unconnected`。
- `driver`、`load`、`input pin`、`output net`、`direct fanin/fanout`。
- `fanout load`、`fanout limit`、`DFFs driven by clock/reset net`。

若出現 `transitive`、`cone`、`reachable`，改用 `cone_query`。若出現 `path from A to B`，改用 `path_query`。若問 signal 是否永遠為 0/1，改用 `func_query`。

## 3. Command Grammar

```text
structure_query <mode> [args]
```

## 4. Basic Modes

| Mode | 參數 | 用途 | 主要 data |
|---|---|---|---|
| `summary` | 無 | 全設計 active summary | `gates`, `nets`, `logical wires`, `primary inputs`, `primary outputs`, `Gate type counts` |
| `list_gates` | 無 | 列 active gates | `Gate names` |
| `list_nets` | 無 | 列 active nets | `Net names` |
| `list_pi` | 無 | 列 PI ports 與 width/range | `Port names`, `Port summaries` |
| `list_po` | 無 | 列 PO ports 與 width/range | `Port names`, `Port summaries` |
| `list_dffs` | 無 | 列 active DFF instances | `Gate names` |
| `list_comb` | 無 | 列 active combinational gates | `Gate names` |
| `gate_info` | `<gate>` | gate type、ID 與基本資訊 | `object`, `id`, `type` |
| `net_info` | `<net>` | net 類型、ID 與基本資訊 | `object`, `id`, `type` |
| `port_info` | `<port>` | port direction、width/range | `object`, `width` |
| `count_by_type` | `[type]` | 指定 type 數量；省略時列全部 type 統計 | `gates`, `Gate type counts` |
| `gates_by_type` | `<type>` | 指定 type 的 gate names | `Gate names` |
| `const_input_gates` | `[type\|all] [0\|1\|any] [--inputs N]` | 找 input pin 直接接 constant 的 gates | `Gate names` |
| `structural_issues` | 無 | 結構問題摘要 | `Undriven nets`, `No-load nets`, `Floating nets`, `Unconnected gates` |

gate 數量皆指 current active objects，不應使用底層 raw storage count 回答修改後的 gate 數量。

## 5. Connectivity Modes

| Mode | 參數 | 用途 | 主要 data |
|---|---|---|---|
| `net_driver` | `<net>` | 直接 driver gates | `count`, `Gate names` |
| `net_loads` | `<net>` | 直接 load gates | `count`, `Gate names` |
| `fanout_load` / `fanout_report` | `<net>` | QA 定義的 pin-level fanout 分類 | `fanout load count`, 各 load 分類 |
| `global_fanout` | `[limit]` | 全設計最大 fanout；有 limit 時也檢查 violations | `max fanout`, `Max-fanout nets`, `Violating nets` |
| `pi_fanout` | `[limit]` | 只掃 PI nets 的最大 fanout | 同上 |
| `fanout_violations` | `<limit>` | 列超過 fanout limit 的 nets | `satisfies limit`, `Violating nets` |
| `gate_inputs` | `<gate>` | gate 直接 input nets | `Net names` |
| `gate_output` | `<gate>` | gate output net | `Net names` |
| `gate_fanin` | `<gate>` | 直接驅動 inputs 的上一層 gates | `Gate names` |
| `gate_fanout` | `<gate>` | gate output 的直接 load gates | `Gate names` |
| `is_connected` | `<gate> <net>` | gate 與 net 是否直接相連 | `connected` |

`fanout_load` 的 `totalLoadCount` 包含 combinational input pins、DFF D/clock/reset/other input pins及 PO connections。同一 gate 多個 input pins 接同一 net 時會按 pin 計數。

## 6. Prompt Examples

```text
Prompt: How many primary inputs and primary outputs does this design have?
Command: structure_query summary
Read: primary inputs, primary outputs
```

```text
Prompt: List all flip-flops driven by clock n0.
Command: structure_query fanout_load n0
Read: DFF clock-pin loads
```

```text
Prompt: How many 2-input OR gates have a constant-1 input?
Command: structure_query const_input_gates OR 1 --inputs 2
Read: Gate names 的列表長度
```

```text
Prompt: Which primary input has the highest fanout?
Command: structure_query pi_fanout
Read: max fanout, Max-fanout nets
```

## 7. 限制

- `const_input_gates` 是結構連線查詢，不證明 output function 為 constant。
- `gate_fanout` 只回一層 immediate loads，不代表所有 reachable gates。
- `summary` 的 PI/PO 數量是 port count；bus width 請讀 `list_pi`/`list_po` 的 `Port summaries`。
- prompt 說 input/output bits 或 signals 時，必須加總 `Port summaries` 的 width，不能直接使用
  port count。
