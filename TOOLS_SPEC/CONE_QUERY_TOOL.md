# Cone Query Tool

## 1. 責任

`cone_query` 負責指定 net/gate 的多層 transitive fanin/fanout 集合、cone gate-type breakdown、shared fanin，以及 output cone extrema/ranking/threshold selection。它回答「範圍內有哪些物件」，不證明特定 endpoints 間的 path condition。

完整性規則：要求 cone list 時取得全部 gates/nets，不自行限制筆數；只問數量或 type breakdown
時只回摘要。大型 cone list 由 CLI 自動完整寫入唯一 artifact，terminal 保留 gates/nets、type
breakdown、artifact completeness 與 `output_file`。4096-token 保守估算門檻只決定輸出位置，不限制 cone
大小，也不由 LLM 設定。時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

prompt 出現 `transitive fanin`、`transitive fanout`、`cone`、`reachable`、`can affect`、`shared fanin`、`largest cone`、`smallest cone`、`second-largest cone`、`top/bottom cone`，或要求 outputs 的 cone size exact/comparison/range 時使用本 tool。`logic cone of output X`、`cone of X` 未明說方向時預設為 feeding X 的 `net_fanin X`；只有 `affected by X`、`reachable from X`、`downstream of X` 等語意使用 `net_fanout X`。

只問一層 driver/load 使用 `structure_query`；指定 A 到 B 的 path/through/avoid 使用 `path_query`；depth/critical 使用 `depth_query`。

## 3. Command Grammar

```text
cone_query <mode> [name] [options]
```

options 可任意排列：

```text
with_paths | --with-paths
--gate-types <type> [type ...]
--with-pins
```

`--gate-types` 支援 `AND OR NOT NAND NOR XOR XNOR BUF DFF`，不分大小寫；多個 type
採 OR semantics，重複 type 會自動去重。沒有此 option 表示所有 gate type。`MUX`、
`UNKNOWN` 或其他名稱會回 `status:error`，不可自行替換成近似 type。

需要 `<net>` 或 `<gate>` 的 mode 必須先提供完整 object name；option 不能代替必填名稱。
例如 `cone_query net_fanin --with-pins` 會回 `status:error` 與
`net_fanin requires <net>`。`shared_fanin` 的兩個 net 名稱都遵守相同規則。

## 4. Modes

| Mode | 參數 | 用途 | 主要 data |
|---|---|---|---|
| `net_fanin` | `<net> [options]` | net 的 transitive fanin | scope/filter counts、nets、type counts、names/details |
| `net_fanout` | `<net> [options]` | net 的 transitive fanout | 同上 |
| `gate_fanin` | `<gate> [options]` | gate 的 transitive fanin | 同上 |
| `gate_fanout` | `<gate> [options]` | gate 的 transitive fanout | 同上 |
| `largest_output` | `[options]` | legacy 單一 representative；只供舊 command 相容 | `source`、checked outputs、scope/filter counts |
| `output_rank` | `<metric> <mode> [k] [options]` | 依 cone metric 排名所有 PO bits，保留完整 ties | ranking counts、`Ranked output cones` |
| `output_filter` | `<metric> <predicate> <value> [upper] [options]` | 依 cone metric threshold/range 篩選所有 PO bits | matched count、`Matched output cones` |
| `shared_fanin` | `<net_a> <net_b> [options]` | 兩個 nets 的 shared fanin gates | sources、scope/filter counts、names/details |

`output_rank` 的 metric：

```text
gates            完整 combinational gate count（同 scope_gates）
filtered_gates   --gate-types 套用後的 gate count
nets             完整 cone net count
```

mode 為 `highest`、`lowest`、`nth_highest`、`nth_lowest`、`top` 或 `bottom`。
`nth_*`、`top`、`bottom` 必須提供正整數 `k`；`highest/lowest` 不提供 `k`。
rank 採 distinct metric levels，所有 boundary ties 都保留；Top/Bottom K 不是限制成 K 筆 output。
同 level 依 output name、net ID 穩定排序。

`output_filter` 使用相同三種 metric，predicate 為 `eq`、`ne`、`gt`、`ge`、`lt`、`le`
或 `between`。`between` 必須提供 inclusive lower/upper；其餘 predicate 只提供一個非負整數。
所有 matches 依 output name、net ID 穩定排序。這是 summary-only batch query，只允許
`--gate-types`；`with_paths` 與 `--with-pins` 會明確拒絕。

`with_paths` 額外要求 local longest/shortest path 摘要，主要欄位為 `longest local path depth`、`shortest local path depth` 與 longest path nets。

`--with-pins` 回傳 filter 後每顆 gate 的名稱、type、input pin/net、output pin/net 與
PI/PO/constant 標記。只有 prompt 明確要求 connection、pin 或每顆 gate 的詳細資訊時使用。

## 5. 輸出判讀

| 題目 | 應讀欄位 |
|---|---|
| 完整 cone 有多少 gates | `scope gates` |
| 指定 type 後有多少 gates | `filtered gates`（相容欄位 `gates` 相同） |
| cone 內某 gate type 數量 | 傳 `--gate-types <type>`，讀 `filtered gates` |
| cone 內各 gate type breakdown | 不傳 filter，讀 `Gate type counts` |
| 列出符合 type 的 gates | 傳 filter，讀 `Cone gates` |
| 列出 gate pin/net details | 加 `--with-pins`，讀 `Gate connection details` |
| 哪個/哪些 output cone 最大 | `output_rank gates highest`，讀全部 `Ranked output cones` ties |
| 哪個/哪些 output cone 最小 | `output_rank gates lowest`，讀全部 `Ranked output cones` ties |
| 指定 type 後的最大/最小 cone | `output_rank filtered_gates highest/lowest --gate-types ...` |
| 第 N 大/小 cone | `output_rank ... nth_highest/nth_lowest N`，讀 `requested rank exists` 與 ranking list |
| 前/後 K 個 cone-size levels | `output_rank ... top/bottom K`；完整保留每個 level 的 ties |
| cone metric 等於/不等於/大於/小於門檻 | `output_filter <metric> <eq|ne|gt|ge|lt|le> <value>` |
| cone metric 位於 inclusive range | `output_filter <metric> between <lower> <upper>` |
| 兩 cone 共用哪些 gates | `Cone gates` 或 filter 後 details |

大型 cone 由 envelope 讀 `list artifact complete`、semantic counts 與 `output_file`；只有
envelope `complete:true` 且 artifact complete 為 `yes` 時，才可把該路徑描述為完整名單。
LLM 不開啟 artifact，也不使用 `list entry count` 代替 cone gate/net count。

## 6. Prompt Examples

```text
Prompt: How many NOR gates are in the fanin cone of n15?
Command: cone_query net_fanin n15 --gate-types NOR
Read: filtered gates
```

```text
Prompt: List every NAND or NOT gate in n8's fanin cone with its pin connections.
Command: cone_query net_fanin n8 --gate-types NAND NOT --with-pins
Read: Gate connection details；大型結果回報 semantic count、complete 與 output_file
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

```text
Prompt: Which primary output has the largest fanin logic cone?
Command: cone_query output_rank gates highest
Read: Ranked output cones；即使 prompt 使用單數，也要回答全部最大 ties，不要只讀 source
```

```text
Prompt: Report the number of each gate type in the logic cone of output n6[0].
Command: cone_query net_fanin n6[0]
Read: Gate type counts；output logic cone 未指定方向時是 fanin，不能改成 net_fanout
```

```text
Prompt: List outputs in the second-largest NOR/NOT cone-size level.
Command: cone_query output_rank filtered_gates nth_highest 2 --gate-types NOR NOT
Read: Ranked output cones
```

```text
Prompt: How many outputs have more than 100 gates in their fanin cones?
Command: cone_query output_filter gates gt 100
Read: matched output count
```

```text
Prompt: List outputs whose fanin cones contain between 5 and 10 NOR gates.
Command: cone_query output_filter filtered_gates between 5 10 --gate-types NOR
Read: matched output count；大型完整 records 交付於 output_file
```

## 7. 限制

- `with_paths` 是 cone 內 local path 摘要，不等於列出所有 paths。
- `largest_output` 是 legacy 單一 representative：gate count 同分時再比 net count，仍同分保留先遇到者。
  它不能證明 winner 唯一，LLM 不得用它回答自然語言 extrema prompt。Largest/smallest、單數/複數、
  ties、第 N 與 top/bottom 一律使用 `output_rank`。
- `output_rank` 的 `source` 只代表 selected ranking entries 的第一筆及其 cone payload；完整排名
  必須讀 `Ranked output cones`。要求的 Nth level 不存在時是 `status:ok`、空 list、
  `requested rank exists:no`。
- `output_filter` 不填代表性 source/cone；完整答案只能讀 `matched output count` 與
  `Matched output cones`。沒有 active PO 或沒有 match 都是 `status:ok` 的完整零結果。
- DFF.Q 是 combinational sequential boundary；`cone_query net_fanin <dff_q_net>` 不會回傳任何 combinational gate（`gates: 0`），也不會穿透到同一顆 DFF 的 D input。report 仍可能保留 query root 本身，因此 `nets` 可為 1。
- `DFF` 是合法 filter，但 cone traversal 不包含 sequential gate；因此只篩 DFF 時通常是
  `status:ok`、`filtered gates: 0`，不能把 valid zero 誤判為工具失敗。
- cone traversal 不會跨越 sequential state；DFF 的 D/clock/reset 等 pin 不可由 Q 的 fanin query 反推。
- 若題目只問數量或 gate-type breakdown，只讀 `gates`、`nets` 或 `Gate type counts`；不要把
  完整 `Cone gates` / `Cone nets` 複製進答案。題目明確要求列出物件時，必須輸出全部
  confirmed entries；大型結果使用工具自動產生的 artifact，不得用固定筆數、省略號或摘要
  取代完整清單。
- 若題目要求「所有 A-to-B paths 是否都經過某點」，必須改用 `path_query`。
