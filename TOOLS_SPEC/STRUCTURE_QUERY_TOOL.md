# Structure Query Tool

## 1. 責任

`structure_query` 負責 current active netlist 的基本數量、物件列表、物件資訊、structural issue 與一層直接 connectivity。它不處理 transitive cone、endpoint path、logic depth 或 Boolean property。

完整性規則：list mode 預設取得全部 active objects，不自行限制筆數；只問 count 時只回摘要。
當所有 list sections 的 record 數或預估 serialized characters 超過工具內部顯示門檻時，CLI 會自動將完整資料寫入唯一 artifact，
terminal 只回 count、artifact completeness 與 `output_file`。這個門檻只決定輸出位置，不會
截斷結果，也不由 LLM 設定。時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

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
| `port_info` | `<port>` | port direction、width/bus flag 與 bit nets | `object`, `width`, `is_bus`, `is_primary_input`, `is_primary_output`, `Net names`；不輸出無意義的負 ID |
| `count_by_type` | `[type] [--gate-types <type...>] [--exclude-gate-types <type...>]` | 統計 include-minus-exclude type；省略 filters 時列全部 | `gates`, `Gate type counts`, filter metadata |
| `gates_by_type` | `[type] [--gate-types <type...>] [--exclude-gate-types <type...>] [--with-pins]` | 列 include-minus-exclude gates；旗標要求 pin/net 明細 | names-only：`Gate names`；detail：`Gate connection details` artifact |
| `const_input_gates` | `[type\|all] [0\|1\|any] [--gate-types <type...>] [--exclude-gate-types <type...>] [--const <0\|1\|any>] [--with-pins]` | 先找直接接 constant 的 gates，再套 type filters | names-only：`Gate names`；detail：`Gate connection details` artifact |
| `structural_issues` | 無 | 結構問題摘要 | 一般 issue lists，以及 `Floating primary-input nets`、`Unconnected primary-output nets` 精確分類 |

gate 數量皆指 current active objects，不應使用底層 raw storage count 回答修改後的 gate 數量。

Gate-type filter 規則：`--gate-types` 內多個 type 採 OR；未指定 include 表示全部；
`--exclude-gate-types` 從 include 結果扣除且優先。舊 positional 單型別語法仍可使用，但不可和
`--gate-types` 同時使用。支援 `AND OR NOT NAND NOR XOR XNOR BUF DFF`，重複與大小寫會正規化。

固定 arity 規則：`summary`、所有 `list_*` 與 `structural_issues` 不接受任何參數；
`gate_info`、`net_info`、`port_info` 必須且只能提供一個對應名稱。缺值或 trailing token 都回
`status:error, complete:false`，不得被忽略，也不得誤報為 unknown mode。

## 5. Connectivity Modes

| Mode | 參數 | 用途 | 主要 data |
|---|---|---|---|
| `net_driver` | `<net> [--with-pins]` | 直接 driver gates；可選 output-pin detail | names-only：`count`, `Gate names`；detail：`pin connection count`, `Pin connection details` |
| `net_loads` | `<net> [--with-pins]` | 直接 load gates；可選逐 input-pin detail | names-only：`count`, `Gate names`；detail：`pin connection count`, `Pin connection details` |
| `fanout_load` / `fanout_report` | `<net>` | QA 定義的 pin-level fanout 分類 | `fanout load count`, 各 load 分類 |
| `global_fanout` | `[limit]` | 全設計最大 fanout；有 limit 時也檢查 violations | `max fanout`, `Max-fanout nets`, `Violating nets` |
| `pi_fanout` | `[limit]` | 只掃 PI nets 的最大 fanout | 同上 |
| `fanout_violations` | `<limit>` | 列超過 fanout limit 的 nets | `satisfies limit`, `Violating nets` |
| `gate_inputs` | `<gate>` | gate 直接 input nets | `Net names` |
| `gate_output` | `<gate>` | gate output net | `Net names` |
| `gate_fanin` | `<gate>` | 直接驅動 inputs 的上一層 gates | `Gate names` |
| `gate_fanout` | `<gate>` | gate output 的直接 load gates | `Gate names` |
| `is_connected` | `<gate> <net>` | gate 與 net 是否直接相連 | `connected` |

Connectivity parser 採 fail-closed：上述必要名稱不可省略，也不可附加未定義的 trailing token。
`net_driver` / `net_loads` 只接受單一可選旗標 `--with-pins`；其他 option、重複旗標均為錯誤。
`global_fanout` / `pi_fanout` 的 limit 可省略，`fanout_violations` 的 limit 必填；提供時只能是
`0..INT_MAX` 的十進位整數。recognized mode 的參數錯誤會回
`status:error, complete:false`，不會誤報成 unknown mode，也不會沿用預設 limit 執行查詢。

`fanout_load` 的 `totalLoadCount` 包含 combinational input pins、DFF D/clock/reset/other input pins及 PO connections。同一 gate 多個 input pins 接同一 net 時會按 pin 計數。

## 6. 輸出判讀

先確認 envelope `status:ok` 與 `complete:true`，再依 mode 讀取 data。不存在的 object、
parser error 或不完整結果不能當成 count 0 或 empty list。

count query 或 list/filter query 即使沒有 matching object，也會明確輸出有效的零值，例如
`gates: 0`。成功但不適用於該 mode 的 count 欄位不會輸出；LLM 不得自行把缺少欄位解讀為零。

| 題目 | 應讀欄位 |
|---|---|
| current active gate 總數與 type breakdown | `summary` 的 `gates` 與 `Gate type counts` |
| 指定或排除 type 後的 active gate 數量 | `count_by_type` 的 `gates` / `filtered gates` |
| PI/PO port 數量 | `summary` 的 `primary inputs` / `primary outputs` |
| PI/PO bit 或 signal 數量 | `list_pi` / `list_po` 的 `Port summaries` width 加總 |
| driver/load gate 數量 | `net_driver` / `net_loads` 的 count 與 `Gate names` |
| 某 net 的 exact driver/load pins | `net_driver <net> --with-pins` / `net_loads <net> --with-pins` 的 `Pin connection details` |
| pin-level fanout load | `fanout_load` / `fanout_report` 的 total load 與分類 |
| 一或多種 type 的 gates 及其 input/output signals | `gates_by_type --gate-types <type...> --with-pins` 的 `Gate connection details` |
| constant-input gates 及 constant 所在 pin、其他 inputs、output | `const_input_gates [type\|all] [0\|1\|any] --with-pins` 的 `Gate connection details` |
| floating/unconnected 結構問題 | `structural_issues` 的精確分類 lists |

修改後的 gate count 必須使用 active fields；tombstone storage 中的 removed gate/net 不得計入。
`const_input_gates` 只證明 input pin 直接連到 constant，不代表 output Boolean function 為常數。

大型 list query 另讀：

```text
list artifact format: QUERY_LIST_ARTIFACT_V1
list artifact complete: yes/no
list entry count
wrote list to file
output_file
```

只有 envelope `complete:true`、`list artifact complete:yes`，且 artifact footer 為
`Complete: yes` 時，檔案才是完整答案。artifact 建立失敗時工具會回退成 terminal 全量輸出。

`gates_by_type [type filters]` 維持 names-only；只有 prompt 明確要求 pins、input/output signals 或
connections 時才加 `--with-pins`。detail artifact 每筆格式為：

```text
gate=g103 type=NAND inputs=[IN1=n29328, IN2=n29163] output=OUT=n30174
```

DFF 會保留具名 input pins（例如 D、CK、RN、SN），output pin 為 Q。`<unconnected>` 表示該 pin
沒有 active net；`(PI)`、`(PO)` 與 `1'b0`/`1'b1` 標記是 net 屬性，不是額外 net 名稱。

`const_input_gates` 預設仍只回傳 names。prompt 明確要求 constant 所在 pin、其他 input 或 output
signal 時才加 `--with-pins`。例如：

```text
gate=g12 type=NAND inputs=[IN1=1'b1, IN2=n8] output=OUT=n15
```

既有 `--inputs N` 只保留作相容選項；本題 gate arity 已固定，不需用它回答一般 prompt。

`net_driver` / `net_loads` 的 `count` 是去重 gate instance 數；`pin connection count` 是實際 edge
數。同一 net 接到同一 gate 的兩個 inputs 時，detail 會保留兩筆。record 格式為：

```text
net=n10 direction=load gate=g12 type=NAND pin=IN1 role=combinational_input
net=n0 direction=load gate=g20 type=DFF pin=CK role=clock
net=n8 direction=driver gate=g7 type=NOR pin=OUT role=output
```

這兩個 mode 只列 gate pins，不含 primary-output connection；QA fanout load count 仍使用
`fanout_load` / `fanout_report`。

## 7. Prompt Examples

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
Prompt: List every gate pin connected to n10, including gate type and pin role.
Command: structure_query net_loads n10 --with-pins
Read: count, pin connection count；完整 records 在 `Pin connection details` 或 artifact
```

```text
Prompt: How many OR gates have a constant-1 input?
Command: structure_query const_input_gates OR 1
Read: Gate names 的列表長度
```

```text
Prompt: List all NAND gates with a constant-1 input and show where the constant is connected, the other input, and the output signal.
Command: structure_query const_input_gates NAND 1 --with-pins
Read: gate count, list artifact complete, output_file；完整 records 在 artifact
```

```text
Prompt: Which primary input has the highest fanout?
Command: structure_query pi_fanout
Read: max fanout, Max-fanout nets
```

```text
Prompt: List all NAND gates with their input and output signals.
Command: structure_query gates_by_type NAND --with-pins
Read: gate count, list artifact complete, output_file；完整 records 在 artifact
```

```text
Prompt: List all AND, OR, and NOT gates, but exclude OR gates.
Command: structure_query gates_by_type --gate-types AND OR NOT --exclude-gate-types OR
Read: filtered gates, gate type filters, excluded gate type filters, Gate names/output_file
```

```text
Prompt: Count all combinational gates except buffers.
Command: structure_query count_by_type --exclude-gate-types DFF BUF
Read: filtered gates, Gate type counts
```

## 8. 限制

- `const_input_gates` 是結構連線查詢，不證明 output function 為 constant。
- `gate_fanout` 只回一層 immediate loads，不代表所有 reachable gates。
- `gates_by_type --with-pins` 是全設計依 type include/exclude 篩選，不會自動限制在某個 cone；有 cone scope 的
  prompt 仍需使用 `cone_query`，不能假裝這個旗標支援 scope。
- `summary` 的 PI/PO 數量是 port count；bus width 請讀 `list_pi`/`list_po` 的 `Port summaries`。
- prompt 說 input/output bits 或 signals 時，必須加總 `Port summaries` 的 width，不能直接使用
  port count。
- 大型 gate/net/load/structural-issue 名單會自動寫 artifact；正式回答提供總數與
  `output_file`，不要把 artifact 全文重新貼回自然語言答案。
- `Floating primary-input nets` 是沒有 active load 的 PI bit nets；
  `Unconnected primary-output nets` 是沒有 active driver 的 PO bit nets。兩者都是 bit-net
  name，不是 bus base port name。
