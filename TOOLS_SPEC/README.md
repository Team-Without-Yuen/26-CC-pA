# Tools Specification

這份文件是 `tools.cpp` 的 LLM-facing 唯一入口，負責協助判斷「題目應交給哪個高階 API」。選定 API 後，再閱讀本資料夾對應的 `*_TOOL.md`，取得實際 command、mode、輸入參數、輸出欄位與使用限制。

跨工具的判讀規則、輸出量控制與 session constraint 注意事項見 [`LLM_NOTES.md`](LLM_NOTES.md)。

除 prompt 明確要求前 N 筆外，不得自行設定結果數量上限。官方 read/write 等 basic
operation 上限為 60 秒，其餘 query/edit/optimization 上限為 300 秒；工具內部預設分別
保留 55/290 秒工作預算。prompt 未指定較短 budget 時，不要自行加入 time-limit 參數。
支援完整 file output 的大型結果應直接寫檔，正式答案只回必要摘要，不搬運檔案內容。

## 使用流程

```text
1. 依本文件判斷 prompt 的主要問題類型。
2. 選擇一個或多個 public tool families。
3. 開啟對應的 *_TOOL.md，不自行猜測 mode 或參數。
4. 先判讀 envelope 的 status 與 complete，再讀 data。
5. complete=true 的結果可直接作答；不完整結果依 LLM_NOTES 的 Competition Answer Policy
   改用低成本 query、重試或 best-effort inference，正式答案不得留白或模稜兩可。
```

共同回傳格式：

```text
TOOL_RESULT_BEGIN
ok: true
status: ok
command: structure_query
mode: summary
message: ...
design_revision: 0
complete: true
data:
...
TOOL_RESULT_END
```

`partial`、`timeout`、`unsupported`、`error` 或 `complete:false` 不得直接解讀為
`no`、`false`、`0` 或「不存在」。`ok` 只表示 request 是否成功執行，不一定是題目本身
的 yes/no 答案。競賽正式輸出仍須提供明確候選答案；無法取得完整證明時，依
[`LLM_NOTES.md`](LLM_NOTES.md) 記錄推定依據，不以 `unknown` 或工具錯誤訊息代替答案。

## Prompt Routing 優先規則

選擇 tool/mode 時必須讀完整句子，不能只依第一個 `count`、`cone` 或 `optimize`
關鍵字決定 command。遇到多個可能規則時，依下列優先順序判斷：

```text
明確 scope / endpoint / hard constraint / cost function
    > scoped query 或 transformation
    > whole-design generic query
```

### 高容錯 mode 選擇

多個 public mode 都能產生看似合理的結果時，優先選擇能直接、完整保留 prompt 語意的 mode，
不要依賴 LLM 事後從代表值、sample、舊狀態或多份 records 補推答案：

```text
完整 batch/ranking/filter report
    > 單一 representative 或 legacy compatibility mode
同一 report 同時提供 prompt 所需 scalar + identities
    > 分開查詢後由 LLM 猜測兩者關係
明確 start/end scope 的 path query
    > 可能包含其他 startpoint/endpoint 的 global query
修改後對 current design 重新查詢
    > 沿用修改前的自然語言答案或 summary
```

具體預設：

- output cone 的 largest/smallest/highest/lowest，即使 prompt 使用單數 `which output`，也使用
  `cone_query output_rank ... highest/lowest`，完整保留 ties；不使用 legacy `largest_output`。
- `the logic cone of output X`、`the cone of X`、`logic feeding X` 未明說方向時一律是
  `net_fanin X`；只有 `affected by X`、`reachable from X`、`downstream of X` 才是 `net_fanout X`。
- register-to-register、PI-to-DFF.D 等已指定 start/end scope 的最長或最短 depth 使用
  `path_query max_depth/min_depth`；不能用全域 `depth_query global_critical` 代替。
- 同時問 pin-level fanout count 與直接 load gates 時使用一次 `structure_query fanout_load`，
  同時讀 pin-level count 與 `Direct load gates`；不以 `net_loads` 的去重 gate count 冒充 fanout。
- 每次 RUN 只含一個 command；禁止使用 `;`、`&&`、`||` 或換行拼接多個 command。

這些規則不是關鍵字對照表。選擇 command 前先建立下列 semantic tuple：

```text
operation     要 count、list、compare、prove、search、edit 或 optimize
object        gate、net、port、pin、path、DFF 或整個 design
scope         whole、rooted cone、direct adjacency、explicit endpoints 或 saved baseline
direction     feeding/ancestor (fanin)、affected/descendant (fanout) 或無方向
metric        gate count、pin load、depth、Boolean function、cost 等
quantifier    any、all、exactly、threshold、rank、top-K、ties
state         current、original、previous edit 或 operation delta
output shape  scalar、decision、完整 identities、records 或 artifact
```

Tool family 與 mode 必須共同保留這八個維度。表面詞彙可以改寫，但 semantic tuple 相同時，
command 應保持相同；若 tuple 中任一維度改變，就必須重新判斷，不能只沿用上一個相似 prompt。

### Scoped query 優先於 global query

`in the cone of n8`、`between A and B`、`driven by n0` 等文字會限制答案範圍，
不得因 prompt 同時出現 `how many NAND gates` 就改查全設計。

```text
How many NAND gates are in the cone of n8?
-> cone_query net_fanin n8 --gate-types NAND
   讀 filtered gates

How many NAND gates are in the whole design?
-> structure_query count_by_type NAND
```

### Prompt constraint 必須原樣保留

gate type、constant value、input count、scope、through/avoid node、fanout limit 與
target depth 都是 command 參數。prompt 指定 `constant 0` 或 `constant 1` 時，不得
放寬成 `any`；指定 `only NAND and NOT` 時，兩種 gate type 都必須傳入。

```text
Simplify AND gates with a constant 0 input.
-> edit_apply simplify_constants AND 0

Simplify OR gates with any constant input.
-> edit_apply simplify_constants OR any
```

### Cost function 決定 optimization objective

`cone` 可能只是 rewrite scope 或 gate-basis constraint，不代表 objective 必然是 cone。

```text
Cost is the maximum logic depth of the final design.
-> --cost-scope whole

Cost is the depth of the cone of n8.
-> --scope net_fanin n8 --cost-scope cone
```

例如：

```text
Optimize the design while the cone of n11 uses NAND/NOT only;
cost is the maximum depth of the final design.
-> opt_apply critical_path_depth --basis-scope net_fanin n11
   --allowed NAND NOT
```

### Endpoint 與英文語意詞

`input`、`output`、`gate`、`signal`、`wire`、`node` 是物件種類或語意詞，不是
物件名稱。`cone of output n14` 的 net name 是 `n14`，不是 `output`。

```text
path from primary input n2 to primary output n25
-> path_query exists pi:n2 po:n25

path from n2 to n30 that avoids n4552
-> path_query exists net:n2 net:n30 -avoid net:n4552
```

### Query、指定修改與最佳化

```text
count / list / inspect                         -> query tool
convert/restructure using a specified basis   -> edit_apply
minimize/reduce/best with a cost function      -> opt_apply
```

`restructure the cone using only NAND and NOT` 若沒有最小化目標，是 fixed-basis
conversion；使用 `edit_apply convert_basis`。只有 prompt 明確要求降低或最小化 cost
時才使用 `opt_apply`。

### 一題需要多個 command

若題目要求的資料沒有單一 public mode，可組合多個 query，但每個 command 都必須保持
原 scope。例如「列出所有連到 signal n10 的 gates」需同時取得 driver 與 loads：

```text
structure_query net_driver n10
structure_query net_loads n10
```

送出 command 後必須核對回傳的 mode、resolved object/scope、objective 與 constraints。
成功的 count/filter query 若沒有 matching object，工具會明確輸出 `gates: 0` 等有效零值；缺少
某個 count 欄位代表該 mode 不提供它，不得自行猜成 0。
若 `output` 被回報為不存在的 net，或回傳 scope 與 prompt 不同，代表參數擷取錯誤，
應修正 command 後重試，不能把工具 error 當成題目答案。

### 多結果、比較與 artifact

LLM 預設不能讀取 artifact 內容。若比較的是 count、depth、maximum 等 scalar，分別取得各
operand 相同語意的 complete envelope 後直接比較；artifact 只交付完整 records。若比較的是
集合內容、交集、差集或逐筆屬性，必須使用直接回傳衍生結果的專用高階 query，不能由兩個
counts 猜答案，也不能依賴 LLM 開啟 artifact 後自行運算。

正式答案不能只貼 artifact path：prompt 要求的 scalar、yes/no、winner/tie 與關鍵結論必須直接
回答；prompt 要求的完整大型 payload 則以 count、complete 與 `output_file` 交付。Artifact 不是
LLM 的資料來源，也不是下一個 tools command 可直接引用的 session result；不能把
`list entry count` 當成任意 mode 的 object count。完整規則以
[`LLM_NOTES.md`](LLM_NOTES.md) 的「Result Assembly 與 Artifact」為準。

## 全域參數抽取規則

先從 prompt 原文抽出 object names、object kinds、scope、gate types、數值限制與量詞，
再選 command。不得自行改寫 net、gate、port 或 DFF instance 名稱。

### 名稱與物件種類

| Prompt 中的物件 | Command token |
|---|---|
| named net / signal `n10` | 一般 query 使用 `n10`；path endpoint 使用 `net:n10` |
| bus bit `n10[3]` | 使用完整名稱 `n10[3]`；path endpoint 使用 `net:n10[3]` |
| gate instance `g5` | gate 參數使用 `g5`；path node 使用 `gate:g5` |
| primary input/output port | `pi:<name>` / `po:<name>`，或 StructureQuery 的 port mode |
| DFF instance `ff1` | `dff_q:ff1`、`dff_d:ff1`、`dff_clk:ff1` 或 `dff_reset:ff1` |

若 prompt 沒說物件種類且名稱可能同時像 net/gate/port，先用 `structure_query net_info`、
`gate_info` 或 `port_info` 確認，不要把 DFF instance name 當成 Q-net name。名稱不存在時，
先用對應 list mode 找正式名稱，再重送原 query。

gate type token 不分大小寫，使用 `AND OR NOT NAND NOR XOR XNOR BUF DFF`。多個 allowed/banned
types 以空白或逗號分隔；不得把「NOR and NOT only」解讀為只允許其中一種。

### 數值與量詞

| Prompt 語意 | 參數規則 |
|---|---|
| `greater than N` / `more than N` | strict `> N`，直接使用 threshold `N` |
| `at least N` / `N or more` | integer metric 使用 `> N-1` 的 threshold mode |
| `at most N` / `no more than N` | constraint limit 設為 `N` |
| `exactly N-input gates` | 使用 `--inputs N` |
| `any` / `find one` / `does there exist` | 使用 exists/find-any 類 mode |
| `all` / `every` / `list each` | 使用完整列舉、pagination 或 `--all` |

### Scope 映射

| Prompt scope | Tool scope |
|---|---|
| whole design / entire netlist | `whole` |
| cone of net `n10` / logic feeding `n10` | `net_fanin n10` |
| logic reachable from net `n10` | `net_fanout n10` |
| fanin cone of gate `g5` | `gate_fanin g5` |
| fanout cone of gate `g5` | `gate_fanout g5` |

### 預設語意

- gate count 指 current design 的 active gates；修改後不得使用 raw storage count。
- `count all gates` 包含 DFF，並使用 `Gate type counts` 回報各 type。
- PI/PO count 預設是 port count；prompt 明確說 bits/signals 時，依各 port width 加總。
- `outputs` 指 PO；`endpoints` 才包含 PO 與 DFF.D。
- logic depth 中每個 primitive gate都算一層，包含 NOT 與 BUF。
- direct fanout/load 使用 `StructureQuery`；transitive reachable fanout 使用 `ConeQuery`；
  指定 start/end 間 path 使用 `PathQuery`。
- `all paths`、`all pairs`、`all records` 不得以第一個 witness 或 terminal sample 代替。
- critical path 可能不唯一；`global_critical` 的 path 是一條代表性 maximum-depth path。
- 未指定 baseline 時，query 回答 current design；`original` 與 `previous_edit` 只能透過
  `equiv_query` 的對應 mode 明確選擇。

### Routing Checklist

每個 prompt 在送出 command 前必須逐項核對。不能只根據 `path`、`cone`、`list` 等單一關鍵字
選工具，也不能因 command 回傳 `status:ok` 就假設已回答原題。

| 必查項目 | Routing 規則 |
|---|---|
| 量詞 | existence/one witness 使用 find-any；完整 identities、總數、ranking population、`all/every/list each` 都需要完整搜尋或 batch mode。不能因 count 題沒寫 `all` 就只找一筆 |
| 路徑限制 | `avoid`、`without passing`、`does not traverse` 必須保留為 `-avoid`；`through`、`must pass` 不得降成普通 existence query |
| 名稱 | 完整保留 gate/net/port 名稱；bus bit `n4[0]` 不得改成 `n4` |
| Scope | whole、fanin、fanout、指定 endpoints 與指定 candidate set 不得互換 |
| 比較題 | `A or B which...` 分別查 A、B，核對 identity/complete 後比較相同 semantic metric；集合比較另依 `LLM_NOTES.md` 讀 records 或用專用 query |
| Extrema / ranking | largest/smallest/highest/lowest 預設保留完整 ties；使用 batch ranking mode，不取 legacy representative |
| 修改後追問 | edit/opt 後重新查 current design；詢問修改 delta 才使用 `report_query last_edit` |
| Sequential boundary | DFF.Q fanin 是空 combinational cone；空結果不是 error，也不能回追同一顆 DFF.D |
| 完整性 | 只有 `complete:true` 及對應 artifact complete 才能宣稱 `all` 已完整 |
| 最終答案 | 依 prompt 直接回答數值、名稱、yes/no 或比較結果；不要只貼多份 tool reports 讓評分器自行推導 |

常見 routing 對照：

```text
Does there exist any pair ... NAND(a,b)==n25?
-> func_search nand_pair n25

List all pairs ... NAND(a,b)==n25.
-> func_search nand_pair n25 --all

Find signals a and b such that OR(a,b)==n15.
-> func_search pattern OR n15

Find a signal whose inversion equals n5.
-> func_search pattern NOT n5

Is output n16 always 0?
-> func_query always_zero n16

List all signals that are functionally constant 0 or 1.
-> func_search constant_signals either --all

List all pairs of internal signals that compute complementary functions.
-> func_search complementary_pairs signals whole --all

Does a path from A to B exist that does not traverse X?
-> path_query exists net:A net:B -avoid net:X

What is the maximum logic depth from any primary input to any DFF D-pin?
-> path_query max_depth all_pi all_dff_d
Reason: the prompt explicitly fixes the start/end scopes; do not substitute
        depth_query all_dff_d, which also permits sequential startpoints.

Which output (A or B) has the larger fanin cone?
-> cone_query net_fanin A
-> cone_query net_fanin B
-> compare the two `gates` values and answer A, B, or tie
```

詳細的 error、large-output 與答案組裝規則見 [`LLM_NOTES.md`](LLM_NOTES.md)。

---

## 1. 判斷順序

LLM 應先辨識題目的主要動作：

```text
count / list / inspect direct connection       -> StructureQuery
find transitive fanin/fanout scope             -> ConeQuery
find connectivity/depth between fixed endpoints -> PathQuery
measure endpoint depth or global critical depth  -> DepthQuery
prove a property of known signals               -> FunctionQuery
search unknown signals satisfying a function    -> FunctionSearchQuery
recognize DFF enable/hold semantics              -> SequentialPatternQuery
perform a specified transformation              -> EditApply
inspect the previous transformation result      -> EditReport
compare current design with a saved design       -> WholeDesignEquivalence
minimize / optimize / find best cost             -> Optimization flow
```

一個 prompt 可以同時需要多個 API。應依題目中的每個要求分開 routing，不要強迫由單一 summary query 回答全部內容。

---

## 2. StructureQuery

公開 tool：`structure_query`

負責目前 netlist 的基本結構與單層直接連線：

- gate、net、PI、PO、DFF 的 active 數量與名稱列表。
- active nets 的 PI、PO、PI+PO、structural constant 與 internal 批次分類。
- gate type 的數量與 gate 列表。
- 依 gate type 批次列出每顆 gate 的 input/output pin-net 明細。
- 單一 gate、net、port 的基本資訊。
- floating net、undriven net、unused gate 等 structural issue。
- net 的直接 driver/load、gate 的 input/output、immediate fanin/fanout。
- fanout load 分類，包括由指定 clock/reset net 直接驅動的 DFF。

典型 prompt：

```text
How many primary inputs and primary outputs does this design have?
List all NOR gates.
List all NAND gates with their input and output signals.
How many floating signals were found?
How many internal nets are present? List all structural constant nets.
What gate drives n10?
List all flip-flops driven by clock n0.
Which primary input has the highest direct fanout?
```

責任邊界：只處理物件本身與一層 connectivity。題目若要求 transitive cone，改用 `ConeQuery`；若要求 A 到 B 的完整路徑，改用 `PathQuery`；若問 signal 是否功能常數，不是 structural constant connection，改用 `FunctionQuery`。

大型 object/load/issue 名單會自動完整寫入 list artifact；LLM 只從 envelope 讀取 count、
`list artifact complete` 與 `output_file`，不開啟 artifact，也不需或不能指定輸出門檻或檔名。
自動切換以單一 prompt 的 4096-token 上限為基準，使用保守 token estimate 並保留 envelope
空間；估算達到或超過上限即寫 artifact。門檻只決定呈現位置，不改變查詢結果數量。

一或多種 gate type 的 names-only list 使用 `gates_by_type --gate-types <type...>`；排除型別使用
`--exclude-gate-types <type...>`。未指定 include 代表全部，include 後再套 exclude，且 exclude
優先。單型別仍可使用 `gates_by_type <type>`。prompt 明確要求 pins、connections 或 input/output
signals 時加 `--with-pins`。不要先列 names 再逐顆呼叫
`gate_info`。

constant-input gate 的 names-only list 使用 `const_input_gates [type|all] [0|1|any]`；prompt 還要求
constant 位於哪個 pin、其他 input 或 output signal 時，加上 `--with-pins`。不要先取得 gate names
再逐顆查詢。

net classification 使用無參數的 `net_classes`，一次取得所有分類 counts 與完整 names/artifact。
其中 constant 只表示 named netlist 的 structural constant；題目問 Boolean function 是否恆為
0/1 時仍使用 `FunctionQuery`。

net driver/load 預設回 gate-level names；prompt 明確要求 exact pin、gate type 或 pin role 時使用
`net_driver <net> --with-pins` / `net_loads <net> --with-pins`。其中 `count` 是去重 gate 數，
`pin connection count` 才是逐 pin edge 數。

prompt 同時問某 net 的 QA fanout 數量與它直接驅動的所有 gates 時，只呼叫
`structure_query fanout_load <net>`。同一份結果的 `fanout load count (QA definition)` 是
pin-level load 數，`distinct direct-load gate count` 與 `Direct load gates` 是去重 gate instances；
不得混用兩種 count，也不需要額外呼叫 `net_loads`。

詳細用法：[`STRUCTURE_QUERY_TOOL.md`](STRUCTURE_QUERY_TOOL.md)

---

## 3. ConeQuery

公開 tool：`cone_query`

負責從指定 gate 或 net 出發的多層 transitive fanin/fanout 範圍：

- 找出 cone 內所有 gates、nets、PIs、POs 與 DFF boundary。
- 統計 cone 內 gate 數量或各 gate type 數量。
- 以一種或多種 gate type 篩選 cone，並取得 filter 後 gates 或完整 pin/net details。
- 比較或尋找 shared fanin gates。
- 以 tie-preserving ranking 查詢 largest/smallest/highest/lowest/Nth/top/bottom output cone；
  `largest_output` 只保留作 legacy compatibility。
- 依 gate/net count 的 exact、comparison 或 inclusive range 篩選所有 output cones。

典型 prompt：

```text
Find the transitive fanin cone of n10.
Which gates can affect output y?
How many NOR gates are in the cone of n15?
List all NAND, NOR, and NOT gates in n8's fanin cone with their connections.
Which gates are shared by the fanin cones of n10 and n12?
Which output has the largest fanin cone?
Which outputs tie for the second-largest fanin cone?
How many outputs have more than 100 gates in their fanin cones?
```

責任邊界：`ConeQuery` 回答「範圍中有哪些物件」，不負責證明某一條 A-to-B path，也不計算全域 critical depth。指定 endpoints 的路徑使用 `PathQuery`；logic depth 使用 `DepthQuery`。

大型 cone names 會自動完整寫入 list artifact，terminal 保留 cone counts、gate-type
breakdown、完整性與 `output_file`；小型 cone 維持直接列出全部名稱。

prompt 指定 gate type 時使用 `--gate-types <type...>`；支援 `AND OR NOT NAND NOR XOR
XNOR BUF DFF`，多個 type 是聯集。prompt 要求 pin/net connection 時再加 `--with-pins`。
所有自然語言的 largest/smallest/highest/lowest/Nth/top/bottom 題目都使用 `output_rank`，並從
完整 `Ranked output cones` 判讀全部 ties，不可只取代表性的 `source`。`largest_output` 只供
舊 command 相容，不是 LLM routing 選項。
若題目要求 outputs 的 cone count threshold/range，使用 `output_filter`，讀
`matched output count` 與完整 `Matched output cones`；大型結果由工具自動寫 artifact。

詳細用法：[`CONE_QUERY_TOOL.md`](CONE_QUERY_TOOL.md)

---

## 4. PathQuery

公開 tool：`path_query`

負責明確 startpoint 與 endpoint 之間的路徑關係：

- path existence、任一條 path、所有 paths 與 path count。
- shortest/longest path，以及 through/avoid constraint。
- exactly/at-least/at-most/between logic depth 的 path count 或完整列表。
- Top-K/Nth shortest 或 longest paths，含多 endpoint global ranking。
- PI-to-PO、PI-to-DFF.D、DFF.Q-to-PO 與 DFF.Q-to-DFF.D path。
- register-to-register path。
- every-path condition、mandatory nodes、separator 與 PI-to-PO cut。

典型 prompt：

```text
Is there a path from n1 to n20?
List all paths from input a to output y that avoid n8.
How many paths from a to y have logic depth exactly 5?
Find the longest path from ff1.Q to ff2.D.
List the five shortest paths from input a to output y.
Report the third longest register-to-register path.
Which nodes occur on every path from a to y?
Find all articulation points in the combinational graph between n2 and n14.
Is n10 a separator between the primary inputs and outputs?
```

責任邊界：題目必須關心兩個 endpoints 之間如何連通。只問某個 signal 的完整 fanin/fanout 範圍使用 `ConeQuery`；只問全設計 endpoint depth 或 critical path 使用 `DepthQuery`。題目要求 path depth exactly/at-least/at-most/between 時使用 `path_query enumerate` 的 depth predicate；若同時要求 Top-K/Nth，改用對應 ranked mode 並套用相同 predicate。`min_depth/max_depth` 只找 extrema witness。separator/mandatory-node 題目對外仍由 `PathQuery` 處理，不直接選內部 `GraphQuery`。`articulation points between A and B` 應呼叫 `path_query articulation_between A B`；此處語意為所有 directed A-to-B paths 的共同 internal nets，不是無向圖的全域 articulation vertices。

詳細用法：[`PATH_QUERY_TOOL.md`](PATH_QUERY_TOOL.md)

---

## 5. DepthQuery

公開 tool：`depth_query`

負責量測目前設計的 combinational logic depth：

- 指定 net、PO 或 DFF.D endpoint 的 depth。
- 所有 PO/DFF.D 的 depth report。
- 依 all/PO/DFF.D scope 篩選 depth 等於、不等於、大於、大於等於、小於、小於等於，
  或位於 inclusive range 的 endpoints 與數量。
- global maximum logic depth、critical endpoint 與代表性 critical path。
- 判斷單一 gate，或一次列出/計數所有位於任一 global maximum-depth path 上的 gates。

典型 prompt：

```text
How many outputs have a logic depth greater than 4?
List all DFF D-pins with logic depth between 2 and 5 inclusive.
How many outputs have logic depth at most 4?
What is the logic depth of n15?
Which output has the greatest depth?
Report the critical path and maximum logic depth.
List every gate that lies on at least one maximum-depth path.
```

責任邊界：`DepthQuery` 只量測與回報 current design，不會修改電路。若 prompt 使用 `reduce`、`minimize`、`optimize`、`best depth` 或 cost function，屬於 optimization，不可因為能查到 critical path 就宣稱可以完成最佳化。

詳細用法：[`DEPTH_QUERY_TOOL.md`](DEPTH_QUERY_TOOL.md)

---

## 6. FunctionQuery

公開 tool：`func_query`

負責分析題目已明確指定的 signal/net Boolean 性質：

- 兩條已知 internal nets 是否 functionally equivalent。
- 固定一條 scalar condition net 為 0/1 後，兩條已知 nets 是否 equivalent。
- signal 是否 constant 0、constant 1，或能否取某個值。
- output 是否 functionally dependent on 指定 input。
- 兩個 inputs 對指定 function 是否 symmetric。
- 完整 named-DAG Boolean equation artifact、simplified expression 與 functional support。

典型 prompt：

```text
Are n10 and n12 functionally equivalent?
When en is 0, is d equivalent to q?
Is output n8 always zero?
Does y functionally depend on input a?
Are inputs a and b symmetric with respect to y?
Derive the Boolean expression for n15.
```

責任邊界：候選 signal 名稱必須已知。若題目要求從全設計「找出任意一組」符合條件的 signals，使用 `FunctionSearchQuery`。若比較的是修改前後兩份 design，不是兩條 internal nets，使用 `WholeDesignEquivalence`。

所有 `func_query` mode 必須照表提供完整參數；工具會拒絕缺值、trailing token、非 `0/1`
condition/constant value，以及負數或非整數 expression depth，不會猜測缺少的參數。

`func_query boolean_expression <net>` 會自動把完整 named-net DAG equations 寫入唯一
artifact；LLM 不指定 path 或大小上限。response 只回完整性、equation/boundary counts
與 `output_file`，不得把大型 artifact 全貼進自然語言答案。

`func_query support_pi` 與 `func_query symmetry` 的完整 support/counterexample lists 若過大，
會自動改寫入 `QUERY_LIST_ARTIFACT_V1`；response 保留 counts 與主要 Boolean 結論並回
`output_file`。`boolean_expression` 不會再建立第二個 generic list artifact。

Boolean artifact 使用 `NAMED_DAG_EQUATIONS_V2`。若 fanin 含 DFF.Q，方程式會使用
`state_qN` current-state symbol，並在 `Current-state variables` 區段列出
`state_qN = <DFF>.Q (net <net>)`。這是當前週期的 sequential boundary 對照，不會沿 Q
穿越到 D pin，也不能宣稱是只含 top-level PI 的 expression。

詳細用法：[`FUNCTION_QUERY_TOOL.md`](FUNCTION_QUERY_TOOL.md)

---

## 7. FunctionSearchQuery

公開 tool：`func_search`

負責候選未知、必須掃描設計才能找到答案的 Boolean search。它與 `FunctionQuery` 的差異不在 SAT 與否，而在題目是否已提供待分析的 signal 名稱。

典型 prompt：

```text
Find two existing signals whose NAND is functionally equivalent to n20.
Find two existing signals whose OR is functionally equivalent to n15.
Find a signal whose inversion is equivalent to n5.
Does any signal pair in the design satisfy the requested Boolean relation?
List all functionally equivalent gate pairs in the design.
Find equivalent AND gates in the fanin cone of n10.
List all signals that are functionally constant 0 or 1.
Does any internal signal remain at logic 0 for every input assignment?
```

目前公開支援 BUF/NOT/AND/NAND/OR/NOR/XOR/XNOR operand search、whole design / cone
scope 內的 arbitrary equivalent combinational gate-pair search、functionally constant 0/1 signal
batch search，以及 signal/gate-output complementary-pair search。使用
`func_search pattern <type> <target>`；舊 NAND prompt 仍可用 `nand_pair`。`equivalent_pairs` 可依
gate type 過濾並回傳 SAT-proven equivalence classes，但不代表 gate 已可直接刪除；MUX decomposition、
任意 observability-aware redundancy candidate 或其他尚未列於 Usage 的 Boolean search，不可假設已支援。

已知 net 名稱的單點 constant 問題使用 `func_query always_zero/always_one`；候選名稱未知、要
搜尋或完整列出 constant signals 時使用 `func_search constant_signals <zero|one|either>`。
未知 candidates 的 `f(a)=!f(b)` 關係使用
`func_search complementary_pairs <signals|gates> <scope> [scope_name]`；signals 可選擇加入 PI/PO，
gates 可依 gate type 過濾。FindAll 的 phase-class artifact 以
`positive_members x negative_members` 表示完整 pair 集合。

詳細用法：[`FUNCTION_SEARCH_TOOL.md`](FUNCTION_SEARCH_TOOL.md)

---

## 8. SequentialPatternQuery

公開 tool：`sequential_query`

負責從 DFF 的 D-input combinational logic 推導 register-control semantics：

- enable condition。
- hold condition。
- feedback MUX 或相關 canonical sequential pattern。
- opt-in SAT cofactor fallback，可辨識 XOR restructuring、internal selector 等非 canonical 但功能等價的 MUX-hold。
- 指定 DFF 或全設計 DFF 的 pattern summary。

典型 prompt：

```text
Which flip-flops have an enable condition?
Find the hold condition of register ff1.
Identify DFFs implemented with a feedback MUX pattern.
```

責任邊界：一般 `List all DFFs`，或 `List all flip-flops driven by clock n0`，只是物件與直接 clock connectivity，應使用 `StructureQuery`。只有題目要求 enable、hold、feedback 或 register-control pattern 時才使用 `SequentialPatternQuery`。canonical 與 restructuring 的 functional proof 預設都會執行，並以 `complete`、`timed_out` 與 `confirmed` 判讀結果；candidate counters 只是 optional role mapping 診斷。

詳細用法：[`SEQUENTIAL_QUERY_TOOL.md`](SEQUENTIAL_QUERY_TOOL.md)

---

## 9. Optimization (Depth / Gate Count)

公開 tools：`opt_query`、`opt_apply`

負責 objective/cost-driven 的 critical-path depth 或 gate-count 最佳化：

- 全設計 maximum logic depth 最佳化。
- 指定 fanin cone depth 最佳化。
- 全設計或指定 fanin cone 的 gate-count 最佳化。
- whole design 或 local scope 的 allowed/banned gate-type constraints。
- target depth、time budget、no-improvement policy。
- candidate constraint validation、qualified rewrite certificate 與 transactional commit/rollback。

典型 prompt：

```text
Reduce the critical path depth through restructuring.
Minimize the maximum logic depth while keeping the design AND/NOT only.
Optimize the cone of n15 for depth using only AND, OR, and NOT gates.
Reduce maximum depth to at most 5 without changing functionality.
```

責任邊界：只量測 current depth 使用 `DepthQuery`；沒有 cost objective、只指定 basis conversion 時使用 `EditApply`。`opt_apply critical_path_depth` 的 graph identity 使用 `StructuralIdentity`，changed candidate 使用 `CertifiedRewrite`；本 command 不執行 whole-design SAT。prompt 明確要求獨立 SAT proof 時，再呼叫 `equiv_query previous_edit` 或 `equiv_query original`。

詳細用法：[`OPTIMIZATION_TOOL.md`](OPTIMIZATION_TOOL.md)

---

## 10. EditApply 與 EditReport

公開 tools：`edit_apply`、`report_query`

`EditApply` 負責執行題目已指定的 transformation：

- gate/net rename。
- dangling logic、buffer、double inverter、constant propagation 等 cleanup/simplification。
- structural duplicate 與 SAT-proven functional duplicate gate merge。
- buffer insertion 與 fanout-related specified edit。
- gate replacement、basis conversion 與 technology mapping。
- 修改限制檢查與統一 edit report 建立。

`EditReport` 負責回答上一筆修改的成果：

- 是否真的改變 design。
- 新增、移除或變更了多少 gates/nets。
- before/after 統計、gate-type delta、限制與驗證狀態。

典型 prompt：

```text
Remove dangling logic from the design.
Collapse all back-to-back inverters.
Remove all structurally duplicate gates without changing functionality.
Convert the cone of n10 to NOR and NOT gates.
Find and merge all functionally equivalent gate pairs.
How many gates were removed in the previous step?
What changed after the last transformation?
```

責任邊界：`EditApply` 執行指定操作，不負責搜尋最佳 cost/depth。public edit 依 structural/local/certified rewrite 契約提交，不在 transaction 內執行 final whole-design SAT；`merge_functionally_equivalent_gates` 保留候選 gate 間的 SAT 等價搜尋，但不再執行修改後的 whole-design CEC。只有 prompt 明確要求獨立 baseline comparison 時才使用 `equiv_query`。

`structurally duplicate` 與 general observability/functional redundancy 是不同 proof object；只有前者
可直接 routing 到 `merge_structurally_equivalent_gates`。不能由 structural merge 的結果外推
其他 redundancy 類型，也不能用 broad `redundant` 字樣省略所需 proof semantics。

詳細用法：[`EDIT_APPLY_TOOL.md`](EDIT_APPLY_TOOL.md)、[`REPORT_QUERY_TOOL.md`](REPORT_QUERY_TOOL.md)

---

## 11. WholeDesignEquivalence

公開 tool：`equiv_query`

負責比較 current design 與 CLI session 保存的 design baseline：

- current design 對 original design。
- current design 對上一筆 edit 前的 design。
- 以所有 PO 與 DFF.D combinational boundary 判斷 whole-design equivalence。

典型 prompt：

```text
Is the current design functionally equivalent to the original?
Did the previous transformation preserve functionality?
Make sure nothing changes functionally.
```

責任邊界：同一份設計中兩條已知 nets 的 equivalence 使用 `FunctionQuery`。`WholeDesignEquivalence` 也不是修改命令；它只能驗證 edit 前後或 original/current 的功能結果。

詳細用法：[`EQUIVALENCE_QUERY_TOOL.md`](EQUIVALENCE_QUERY_TOOL.md)

---

## 12. 常見組合題型

| 題目要求 | API 組合 |
|---|---|
| 統計某個 cone 內各 gate type | `ConeQuery`，不是全設計 `StructureQuery` |
| output cone 最大/最小/ties/第 N/top-bottom | `ConeQuery::OutputConeRanking`；logic depth 排名仍是 `DepthQuery` |
| output cone gate/net count exact、comparison、range | `ConeQuery::OutputConeFilter` |
| 找 critical path 並列出其節點 | `DepthQuery` 取得 global critical result；需要額外 endpoint/path constraint 時再用 `PathQuery` |
| 修改後回報移除數量 | `EditApply` → `EditReport` |
| 修改後確認功能不變 | `EditApply` → `WholeDesignEquivalence` |
| 修改、回報成果並確認功能不變 | `EditApply` → `EditReport` → `WholeDesignEquivalence` |
| 最佳化 depth 並回報成果 | `Optimization` → `EditReport`；changed candidate 使用 `CertifiedRewrite`，需要獨立 SAT proof 時再接 `WholeDesignEquivalence` |
| 找出未知候選，再對候選做詳細功能分析 | `FunctionSearchQuery` → `FunctionQuery` |
| 列出某 clock 的 DFF，再分析這些 DFF 的 enable/hold | `StructureQuery` → `SequentialPatternQuery` |

選定高階 API 後，LLM 必須進入表中連結的 `*_TOOL.md`，再決定實際 command mode、
輸入參數與應讀取的 report 欄位。

---

## 13. Public Tool 文件索引

| Public command | 詳細文件 |
|---|---|
| `read`, `write`, `help`, `quit` | [`SESSION_AND_IO_TOOL.md`](SESSION_AND_IO_TOOL.md) |
| `structure_query` | [`STRUCTURE_QUERY_TOOL.md`](STRUCTURE_QUERY_TOOL.md) |
| `cone_query` | [`CONE_QUERY_TOOL.md`](CONE_QUERY_TOOL.md) |
| `path_query` | [`PATH_QUERY_TOOL.md`](PATH_QUERY_TOOL.md) |
| `depth_query` | [`DEPTH_QUERY_TOOL.md`](DEPTH_QUERY_TOOL.md) |
| `func_query` | [`FUNCTION_QUERY_TOOL.md`](FUNCTION_QUERY_TOOL.md) |
| `func_search` | [`FUNCTION_SEARCH_TOOL.md`](FUNCTION_SEARCH_TOOL.md) |
| `sequential_query` | [`SEQUENTIAL_QUERY_TOOL.md`](SEQUENTIAL_QUERY_TOOL.md) |
| `opt_query`, `opt_apply` | [`OPTIMIZATION_TOOL.md`](OPTIMIZATION_TOOL.md) |
| `edit_apply` | [`EDIT_APPLY_TOOL.md`](EDIT_APPLY_TOOL.md) |
| `report_query` | [`REPORT_QUERY_TOOL.md`](REPORT_QUERY_TOOL.md) |
| `equiv_query` | [`EQUIVALENCE_QUERY_TOOL.md`](EQUIVALENCE_QUERY_TOOL.md) |

`basic_query`、`conn_query`、`reg_path_query` 與 `graph_query` 只為 legacy 相容保留，
不是 LLM 應選擇的 public command。
