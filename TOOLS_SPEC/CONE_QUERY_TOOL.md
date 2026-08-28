# Cone Query Tool

## 1. 責任

`cone_query` 負責指定 net/gate 的多層 transitive fanin/fanout 集合、cone gate-type breakdown、shared fanin，以及 output cone extrema/ranking/threshold selection。它回答「範圍內有哪些物件」，不證明特定 endpoints 間的 path condition。

完整性規則：要求 cone list 時取得全部 gates/nets，不自行限制筆數；只問數量或 type breakdown
時只回摘要。大型 cone list 由 CLI 自動完整寫入唯一 artifact，terminal 保留 gates/nets、type
breakdown、artifact completeness 與 `output_file`。4096 tokens 仍是正式 response 上限；完整
輸出估算達到 3072 tokens，或 list records 達到 256 筆時會提前寫 artifact。門檻只決定輸出
位置，不限制 cone 大小，也不由 LLM 設定。時間限制依題目指定。詳見
[`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

當答案是由一個或多個 roots 出發，遞迴取得 transitive ancestors/descendants，或比較這些
rooted sets 的 size/filter/rank 時使用本 tool。先辨識 root object（net、gate 或所有 PO），再由
資訊流決定 direction，最後決定 metric、quantifier 與 output shape。

`logic cone of output X` 或未限定方向的 output implementation，語意是 feeding X 的
`net_fanin X`；只有題目要 X 所影響、可到達或驅動的 downstream 集合時才是 fanout。若只需
一層 driver/load adjacency，使用 `structure_query`；若同時指定 start 與 end 並問兩者之間的
path relation，使用 `path_query`。

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
| `output_rank` | `<metric> <highest\|lowest> [options]` 或 `<metric> <nth_highest\|nth_lowest\|top\|bottom> <k> [options]` | 依 cone metric 排名完整 PO-bit population，保留完整 ties；不接受 named candidate subset | ranking counts、`Ranked output cones` |
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

方向依 root 與資訊流判斷，不依英文中是否剛好出現 `input` 或 `output`：

| Prompt 所問集合 | Direction | Mode |
|---|---|---|
| 哪些 logic feeds、implements、determines、can affect root | root 的 ancestors | `net_fanin` / `gate_fanin` |
| root can affect、can reach、drives downstream 的哪些 logic | root 的 descendants | `net_fanout` / `gate_fanout` |
| output 的 logic cone，未明說 downstream/reachable-from | output 的 implementation | `net_fanin` |
| 多個 PO cone 的 extrema/rank/threshold | 每個 PO 的 fanin metric | `output_rank` / `output_filter` |
| 兩個 roots 共同依賴哪些 upstream gates | fanin intersection | `shared_fanin` |

`largest_output`、`output_rank` 與 `output_filter` 的 population 固定是目前設計的全部 PO bits。
若 prompt 明確限定一組具名 candidates，不能把問題擴張成全體 PO ranking；應對每個具名 root
使用相同 single-root cone mode，並比較相同的 envelope scalar。Cone size 是 gate/filtered-gate/net
數量，不是 logic depth；deepest、logic level、arrival depth 使用 `depth_query`。

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

`gates`、`scope gates` 與 `filtered gates` 都是 scalar count metadata，不能代替 `Cone gates`
中的 gate identities。Prompt 要求「所有/哪些 gates」時，若沒有 `output_file`，正式答案必須
交付完整 `Cone gates`；若有 `output_file`，則回答 semantic count/complete 並提供該路徑。

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
- `highest` / `lowest` 不接受 `k`；只有 `nth_highest`、`nth_lowest`、`top`、`bottom` 需要正整數 `k`。
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
