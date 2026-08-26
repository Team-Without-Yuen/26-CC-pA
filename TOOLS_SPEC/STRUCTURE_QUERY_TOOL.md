# Structure Query Tool

## 1. 責任

`structure_query` 負責 current active netlist 的基本數量、物件列表、物件資訊、structural issue 與一層直接 connectivity。它不處理 transitive cone、endpoint path、logic depth 或 Boolean property。

完整性規則：list mode 預設取得全部 active objects，不自行限制筆數；只問 count 時只回摘要。
CLI 以單一 prompt 的 4096-token 上限為基準，保守估算完整 response；估算超過門檻時會自動將完整資料寫入唯一 artifact，
terminal 只回 count、artifact completeness 與 `output_file`。這個門檻只決定輸出位置，不會
截斷結果，也不由 LLM 設定。時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

當題目所需資料可由 current named netlist 的物件屬性或一層 adjacency 直接決定時使用本 tool：

- design/object inventory：active gate/net、gate type、PI/PO/DFF、port width/range、net class。
- structural state：是否 undriven、no-load、floating、unconnected，以及 exact unconnected pins。
- direct adjacency：某 net 的 driver/load gates、某 gate 的 immediate input/output/fanin/fanout。
- pin-level load metric：QA fanout、fanout extrema/rank/threshold，以及 clock/reset net 的 DFF pin loads。

判斷 owner 的核心不是 `count` 或 `list` 字樣，而是答案集合是否能由單一物件或一層 edge 得到。
需要遞迴走訪 ancestors/descendants 時使用 `cone_query`；需要兩個 endpoints 間的 path relation 時使用
`path_query`；需要 Boolean function proof 時使用 `func_query`。

## 3. Command Grammar

```text
structure_query <mode> [args]
```

## 4. Basic Modes

| Mode | 參數 | 用途 | 主要 data |
|---|---|---|---|
| `summary` | 無 | 全設計 active summary | `gates`, `nets`, `logical wires`、PI/PO port counts、PI/PO bit counts、`Gate type counts` |
| `list_gates` | 無 | 列 active gates | `Gate names` |
| `list_nets` | 無 | 列 active nets | `Net names` |
| `net_classes` | 無 | 一次分類所有 active nets | active、PI、PO、PI+PO、constant、internal counts；各分類完整 names/artifact |
| `list_pi` | 無 | 列 PI ports、width/range 與 aggregate bits | PI port/bit counts、`Port names`, `Port summaries` |
| `list_po` | 無 | 列 PO ports、width/range 與 aggregate bits | PO port/bit counts、`Port names`, `Port summaries` |
| `list_dffs` | 無 | 列 active DFF instances | `Gate names` |
| `list_comb` | 無 | 列 active combinational gates | `Gate names` |
| `gate_info` | `<gate>` | gate type、ID 與 ordered input/output pin-net snapshot | `object`, `id`, `type`, `Gate connection details` |
| `net_info` | `<net>` | net 類型、ID 與基本資訊 | `object`, `id`, `type` |
| `port_info` | `<port>` | port direction、width、declaration bounds 與 ordered bit nets | object fields、固定 scalar `declaration_left_bound/right_bound`、`Net names`、唯一一筆 `Port summaries`；不輸出無意義的負 ID |
| `count_by_type` | `[type] [--gate-types <type...>] [--exclude-gate-types <type...>]` | 統計 include-minus-exclude type；省略 filters 時列全部 | `gates`, `Gate type counts`, filter metadata |
| `gates_by_type` | `[type] [--gate-types <type...>] [--exclude-gate-types <type...>] [--with-pins]` | 列 include-minus-exclude gates；旗標要求 pin/net 明細 | names-only：`Gate names`；detail：`Gate connection details` artifact |
| `const_input_gates` | `[type\|all] [0\|1\|any] [--gate-types <type...>] [--exclude-gate-types <type...>] [--const <0\|1\|any>] [--with-pins]` | 先找直接接 constant 的 gates，再套 type filters | names-only：`Gate names`；detail：`Gate connection details` artifact |
| `structural_issues` | 無 | 結構問題摘要與 exact unconnected pins | 六類 semantic object counts、input/output pin counts、完整 issue lists / artifact 與 pin details |

gate 數量皆指 current active objects，不應使用底層 raw storage count 回答修改後的 gate 數量。

Gate-type filter 規則：`--gate-types` 內多個 type 採 OR；未指定 include 表示全部；
`--exclude-gate-types` 從 include 結果扣除且優先。舊 positional 單型別語法仍可使用，但不可和
`--gate-types` 同時使用。支援 `AND OR NOT NAND NOR XOR XNOR BUF DFF`，重複與大小寫會正規化。

固定 arity 規則：`summary`、`net_classes`、所有 `list_*` 與 `structural_issues` 不接受任何參數；
`gate_info`、`net_info`、`port_info` 必須且只能提供一個對應名稱。缺值或 trailing token 都回
`status:error, complete:false`，不得被忽略，也不得誤報為 unknown mode。

## 5. Connectivity Modes

| Mode | 參數 | 用途 | 主要 data |
|---|---|---|---|
| `net_driver` | `<net> [--with-pins]` | 直接 driver gates；可選 output-pin detail | names-only：`count`, `Gate names`；detail：`pin connection count`, `Pin connection details` |
| `net_loads` | `<net> [--with-pins]` | 直接 load gates；可選逐 input-pin detail | names-only：`count`, `Gate names`；detail：`pin connection count`, `Pin connection details` |
| `fanout_load` / `fanout_report` | `<net>` | QA pin-level fanout，並列出不重複的直接 load gates | `fanout load count`, `distinct direct-load gate count`, `Direct load gates`, 各 pin 分類 |
| `global_fanout` | `[limit]` | 全設計最大 fanout；有 limit 時也檢查 violations | `max fanout`, `Max-fanout nets`, `Violating nets` |
| `pi_fanout` | `[limit]` | 只掃 PI nets 的最大 fanout | 同上 |
| `fanout_violations` | `<limit>` | 列超過 fanout limit 的 nets | `satisfies limit`, `Violating nets` |
| `fanout_filter` | `<all|pi> <eq|ne|gt|ge|lt|le|between> <value> [upper]` | 依 QA pin-level fanout 篩選 nets | `matched net count`, `Matched nets` |
| `fanout_rank` | `<scope> <highest|lowest|nth_highest|nth_lowest|top|bottom> [k]` | scoped distinct-level fanout ranking | rank metadata, `Ranked nets` |
| `gate_inputs` | `<gate>` | gate 直接 input nets | `Net names` |
| `gate_output` | `<gate>` | gate output net | `Net names` |
| `gate_fanin` | `<gate>` | 直接驅動 inputs 的上一層 gates | `Gate names` |
| `gate_fanout` | `<gate>` | gate output 的直接 load gates | `Gate names` |
| `is_connected` | `<gate> <net-or-bus>` | gate 與 scalar net / bus active bits 是否直接相連 | `connected`, `count`, `Net names` |

Connectivity parser 採 fail-closed：上述必要名稱不可省略，也不可附加未定義的 trailing token。
`net_driver` / `net_loads` 只接受單一可選旗標 `--with-pins`；其他 option、重複旗標均為錯誤。
`global_fanout` / `pi_fanout` 的 limit 可省略，`fanout_violations` 的 limit 必填；提供時只能是
`0..INT_MAX` 的十進位整數。recognized mode 的參數錯誤會回
`status:error, complete:false`，不會誤報成 unknown mode，也不會沿用預設 limit 執行查詢。

`fanout_filter` 的 `all` 掃全部 active nets，`pi` 只掃 PI bit nets。`between lower upper` 為
inclusive range；其他 predicate 不可多給 upper。值採非負十進位整數且不設人工筆數上限。
`count`/`checked nets` 仍是完整 scope candidate 數，題目所問的篩選數量必須讀
`matched net count`。大型 `Matched nets` 會自動寫入完整 artifact，回答時保留 count 與 `output_file`。

`fanout_rank` scope 可用 `all/pi/po/internal/gate_output/comb_output/dff_output`。`highest/lowest`
不帶 K；其餘 mode 必須提供正整數 K。名次以不同 fanout 數值分層，同值 nets 完整保留；
`top/bottom K` 的 K 是 levels，因此結果 net 數可能大於 K。`requested rank exists:no` 表示
第 K level 不存在，但 query 仍成功且完整。scope 為空也是成功零結果。大型 `Ranked nets`
自動寫入 artifact，回答讀 `result net count` 並提供 `output_file`。

`fanout_load` 的 `totalLoadCount` 包含 combinational input pins、DFF D/clock/reset/other input pins及 PO connections。同一 gate 多個 input pins 接同一 net 時會按 pin 計數。
工具固定在 envelope 輸出 `combinational gate input load count`、`DFF D-pin load count`、
`DFF clock-pin load count`、`DFF reset/set-pin load count` 與 `DFF other-pin load count`。
同時固定輸出 `distinct direct-load gate count`，並以 `Direct load gates` 回傳去重後的 gate
instance names。即使完整名稱清單寫入 artifact，所有數量仍直接留在 envelope；只有列舉名稱時
才提供 `output_file`。

prompt 同時問 `fanout` 與 `every gate driven directly` 時，只呼叫一次 `fanout_load`：fanout
讀 QA pin-level count，gate 名單讀 `Direct load gates`。不得以 distinct gate count 代替
pin-level fanout，也不需要再呼叫 `net_loads`。

`global_fanout` / `pi_fanout` 的 maximum 會比較 scope 內所有 active candidates，包括 fanout=0
的 nets。若所有 PI 都未被使用，`max fanout` 為 0，`Max-fanout nets` 仍會列出全部並列 PI，
不得把空負載誤解為 object 不存在。`checked nets` 是實際參與 extrema/limit 計算的 candidate 數。

`is_connected` 的 scalar query 維持 `count=0/1`。若第二個名稱是 bus base，工具會展開所有
active bits；`connected` 表示是否至少一個 bit 相連，`count` 與 `Net names` 則列出實際相連的
bit 數量與名稱。bus 存在但沒有相連 bit 是成功的零結果，不可解讀為 object 不存在。

## 6. 輸出判讀

先確認 envelope `status:ok` 與 `complete:true`，再依 mode 讀取 data。不存在的 object、
parser error 或不完整結果不能當成 count 0 或 empty list。

count query 或 list/filter query 即使沒有 matching object，也會明確輸出有效的零值，例如
`gates: 0`。成功但不適用於該 mode 的 count 欄位不會輸出；LLM 不得自行把缺少欄位解讀為零。

`primary inputs` / `primary outputs` 是 Verilog port counts；`primary input bits` /
`primary output bits` 是 declaration widths 的總和。兩者不得互換。

| 題目 | 應讀欄位 |
|---|---|
| current active gate 總數與 type breakdown | `summary` 的 `gates` 與 `Gate type counts` |
| 指定或排除 type 後的 active gate 數量 | `count_by_type` 的 `gates` / `filtered gates` |
| PI/PO port 數量 | `summary` 的 `primary inputs` / `primary outputs` |
| PI/PO bit 或 signal 數量 | `summary` 或 `list_pi` / `list_po` 的 `primary input bits` / `primary output bits` |
| internal、PI、PO、PI+PO、structural constant net 數量/名稱 | `net_classes` 的分類 counts 與對應 list/artifact sections |
| 單一 port 的 `[left:right]` 與 bit ordering | `port_info <port>` 的 `Port summaries` msb/lsb 與 `Net names` 順序 |
| 單一 gate 的 type、pins、nets、constant/PI/PO flags | `gate_info <gate>` 的一筆 `Gate connection details` |
| driver/load gate 數量 | `net_driver` / `net_loads` 的 count 與 `Gate names` |
| 某 net 的 exact driver/load pins | `net_driver <net> --with-pins` / `net_loads <net> --with-pins` 的 `Pin connection details` |
| pin-level fanout load | `fanout_load` / `fanout_report` 的 total load 與分類 |
| pin-level fanout 並列出所有直接 load gates | 單次 `fanout_load`：讀 total load、distinct direct-load gate count 與 `Direct load gates` / artifact |
| 一或多種 type 的 gates 及其 input/output signals | `gates_by_type --gate-types <type...> --with-pins` 的 `Gate connection details` |
| constant-input gates 及 constant 所在 pin、其他 inputs、output | `const_input_gates [type\|all] [0\|1\|any] --with-pins` 的 `Gate connection details` |
| floating/unconnected 結構問題數量 | `structural_issues` 的 `undriven nets`、`no-load nets`、`floating nets`、`unconnected gates`、`floating primary-input nets`、`unconnected primary-output nets` |
| floating/unconnected 結構問題名稱 | `structural_issues` 的精確分類 lists / artifact |
| exact unconnected input/output pins | `structural_issues` 的 input/output pin counts 與 `Unconnected pin details` / artifact |

修改後的 gate count 必須使用 active fields；tombstone storage 中的 removed gate/net 不得計入。
`const_input_gates` 只證明 input pin 直接連到 constant，不代表 output Boolean function 為常數。
同樣地，`net_classes` 的 `constant nets` 只代表 parser/named-netlist 明確標記的 constants；
若題目問任意 signal 是否在所有輸入下恆為 0/1，必須使用 `func_query constant/truth_status`。

預估超過 4096 tokens 的大型 list query 另讀：

```text
list artifact format: QUERY_LIST_ARTIFACT_V1
list artifact complete: yes/no
list entry count
wrote list to file
output_file
```

LLM 只有在 envelope `complete:true` 且 `list artifact complete:yes` 時，才把 `output_file`
描述為完整答案，不開檔檢查 footer。Producer 仍寫入 `Complete: yes` footer 供檔案接收端驗證；
artifact 建立失敗時工具會回退成 terminal 全量輸出。

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

`port_info` 的 `msb/lsb` 是歷史欄位名稱，輸出保存 Verilog declaration left/right bounds，
不依數值大小重排。因此 `[31:0]` 為 `31/0`，`[0:31]` 為 `0/31`；`Net names` 也依該方向排列。
同一組值會固定出現在 envelope 的 `declaration_left_bound` / `declaration_right_bound`；大型
bit list 寫入 artifact 時，LLM 仍直接讀這兩欄回答宣告方向，不需要也不應開啟 artifact。

`structural_issues` 即使沒有問題也會明確輸出每個 semantic category 與 input/output pin 的 0
count。大型 lists 寫入 artifact 時，這些 counts 仍保留在 `TOOL_RESULT`，因此純數量題與兩個
design/object 結果的數值比較不需要讀取完整 artifact。pin record 格式為：

```text
gate_id=7 gate=ff0 type=DFF direction=input pin=RN pin_index=2 net_id=-1 net=<unconnected> reason=UNCONNECTED
```

reason 可能為 `UNCONNECTED`、`INVALID_NET_ID`、`REMOVED_NET`。未出現在 instance 的 optional
DFF pin 不會被合成；明確空接的 named pin 會列出。大型 pin details 逐筆寫入 self-contained
artifact，檔內直接包含 gate/type/pin/net，不需額外 ID 對照表。

## 7. Prompt Examples

```text
Prompt: What type of gate is g0? Report its gate type and pin connections.
Command: structure_query gate_info g0
Read: type 與唯一一筆 Gate connection details
```

```text
Prompt: How many primary inputs and primary outputs does this design have?
Command: structure_query summary
Read: primary inputs, primary outputs
```

```text
Prompt: Report both PI/PO port counts and the total number of PI/PO bits.
Command: structure_query summary
Read: primary inputs, primary input bits, primary outputs, primary output bits
```

```text
Prompt: How many internal nets are in the current design? List all constant nets.
Command: structure_query net_classes
Read: internal nets、constant nets；大型清單從 envelope 讀 list artifact complete 並回報 output_file
```

```text
Prompt: Is port data declared [31:0] or [0:31]? List its bits in declaration order.
Command: structure_query port_info data
Read: declaration_left_bound/right_bound；小型 Net names inline，大型完整 bit list 回報 output_file
```

```text
Prompt: List all flip-flops driven by clock n0.
Command: structure_query fanout_load n0
Read: 名稱讀 DFF clock-pin loads；若清單大型化則回報 output_file
```

```text
Prompt: What is the fanout of primary input n0? List every gate that n0 drives directly.
Command: structure_query fanout_load n0
Read: fanout load count (QA definition), distinct direct-load gate count；小型 Direct load gates inline，
大型完整 gate list 以 output_file 交付
```

```text
Prompt: How many flip-flops are driven by clock n0?
Command: structure_query fanout_load n0
Read: DFF clock-pin load count；不需要讀取 artifact
```

```text
Prompt: List every gate pin connected to n10, including gate type and pin role.
Command: structure_query net_loads n10 --with-pins
Read: count, pin connection count；小型 records inline，大型 records 以 output_file 交付
```

```text
Prompt: How many OR gates have a constant-1 input?
Command: structure_query const_input_gates OR 1
Read: gates / filtered gates；不要以 inline Gate names 長度計數，因大型名單可能位於 artifact
```

```text
Prompt: List all NAND gates with a constant-1 input and show where the constant is connected, the other input, and the output signal.
Command: structure_query const_input_gates NAND 1 --with-pins
Read: gate count, list artifact complete, output_file；LLM 不開啟 artifact
```

```text
Prompt: Which primary input has the highest fanout?
Command: structure_query pi_fanout
Read: max fanout, Max-fanout nets
```

```text
Prompt: How many primary-input signals have fanout between 4 and 8, inclusive?
Command: structure_query fanout_filter pi between 4 8
Read: matched net count；若產生 artifact，回報 output_file 交付完整 Matched nets
```

```text
Prompt: Which internal signals have the second-highest fanout?
Command: structure_query fanout_rank internal nth_highest 2
Read: requested rank exists, result net count, Ranked nets；ties 必須全部回答
```

```text
Prompt: List the top three primary-input fanout levels.
Command: structure_query fanout_rank pi top 3
Read: selected fanout levels, result net count, Ranked nets/output_file
```

```text
Prompt: List all NAND gates with their input and output signals.
Command: structure_query gates_by_type NAND --with-pins
Read: gate count, list artifact complete, output_file；LLM 不開啟 artifact
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
- `net_classes` 的 PI/PO counts 是 unique active bit-net counts，不是 Verilog port counts；
  PI 與 PO 集合可能重疊，`primary-input/output nets` 另回交集，所有分類不可直接相加。
  artifact 的 `list entry count` 是各 section entries 的總和；若存在 PI+PO overlap，會因同一
  net 同時出現在 PI、PO 與交集 sections 而大於 `active nets`，答案應讀明確分類 count。
- prompt 說 input/output bits 或 signals 時，必須加總 `Port summaries` 的 width，不能直接使用
  port count。
- 大型 gate/net/load/structural-issue 名單會自動寫 artifact；正式回答提供總數與
  `output_file`，不要把 artifact 全文重新貼回自然語言答案。
- `Floating primary-input nets` 是沒有 active load 的 PI bit nets；
  `Unconnected primary-output nets` 是沒有 active driver 的 PO bit nets。兩者都是 bit-net
  name，不是 bus base port name。
