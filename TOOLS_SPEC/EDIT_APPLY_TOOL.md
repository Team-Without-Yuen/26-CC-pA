# Edit Apply Tool

## 1. 責任

`edit_apply` 執行使用者已指定的高階 transformation，並建立統一 `NetlistEditReport`。public parser 不開放 unchecked rewiring、driver replacement 或任意 net merge；functional duplicate merge 只能走 SAT search、cycle-safe apply 與 whole-design rollback 的專用 mode。每次 public edit 預設要求 equivalence certificate。

完整性規則：不得因 report records 很多而省略 confirmed changes；若 prompt 只問數量，正式
答案只回 delta 摘要。時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

prompt 明確指定 rename、cleanup、constant propagation、buffer insertion、gate replacement、basis conversion，或要求找出並合併 functionally equivalent gates 時使用本 tool。若要求 `minimize`、`best depth`、`best cost` 或自動搜尋最佳 transformation，改用 `opt_apply critical_path_depth`，不是 `edit_apply`。

trim、prune、sweep、dangling、unused、floating、eliminate unused logic、not contributing to any primary output 等語意一律使用 remove_dead_logic。cleanup_buffers、local_simplification_fixpoint、safe_cleanup_fixpoint 不對應這類 prompt。remove_unused_nets 只回收 net、不移除 gate,除非 prompt 明確只問 net,否則不要選它。remove_dead_logic 預設不移除 register,只有 prompt 明確要求連無人使用的 flip-flop 一併移除時才加 --include-sequential。

prompt 出現 redundant gates、可以移除而不改變功能的邏輯時,使用 remove_redundant_logic。它涵蓋 remove_dead_logic 的全部效果,並額外做 SAT-proven 常數傳播與單路徑 stuck-at 分析,成本較高且結果可能不完整。單純的 dangling/unused 類 prompt 不要用它取代 remove_dead_logic。

prompt 指名具體 net(例如 "insert buffers on the reset signal n1")時使用 insert_buffers_for_net <net> <max_fanout>,即使該 net 的角色是 clock 或 reset。insert_buffers_for_dff_control 只用於 prompt 要求處理所有 clock 或 reset 網路而未指名特定 net 的情況。

## 3. Command Grammar

```text
edit_apply <simple_mode> [required_args]

edit_apply merge_functionally_equivalent_gates <scope> [scope_name]
           [--gate-type type] [--patterns 1..4096]
           [--time-limit seconds]

edit_apply convert_basis <scope> [scope_name]
           -allow <type...> [-ban <type...>]
           [--validate_equivalence]

edit_apply replace_type <scope> [scope_name] <target_type>
           -allow <type...> [--validate_equivalence]

edit_apply remove_dead_logic [--include-sequential]

edit_apply remove_redundant_logic [--time-limit seconds]
```

`simple_mode` 的 required arguments 見下方 mode tables。`scope_name` 對 `whole` 省略，
其他 scope 必填。`convert_basis`/`replace_type` 的 `-allow` 必填且可接受空白或逗號分隔；
`-ban` 只適用 `convert_basis`。

| 輸入 | Required when | Default/規則 |
|---|---|---|
| old/new name | `rename_gate`, `rename_net` | 兩者皆必填，不自行改寫名稱 |
| net | net-specific buffer/remove modes | 必填，bus bit 保留完整 `[]` 名稱 |
| max fanout | fanout buffer modes | 必填正整數，直接使用 prompt limit |
| gate type | typed buffer/simplification/mapping | 使用 public primitive type token |
| constant value | `simplify_constants` | 未指定時才使用 `any`；明確 0/1 不得放寬 |
| input count | `simplify_constants --inputs N` | 只有 prompt 明確指定 N-input 時使用 |
| scope/scope name | functional merge/mapping | `whole` 不需 name；其他 scope 必填 name |
| time limit | functional merge | 題目 budget；未指定時不自行縮短 |

## 4. Rename/Cleanup Modes

| Mode | 參數 | 用途 |
|---|---|---|
| `rename_gate` | `<old> <new>` | rename gate instance |
| `rename_net` | `<old> <new>` | rename net/wire |
| `cleanup_buffers` | 無 | bypass 可安全移除的 BUF |
| `collapse_double_inverter` | 無 | collapse internal back-to-back NOT；不清除既有 BUF |
| `local_simplification_fixpoint` | 無 | local simplification 到 fixpoint |
| `safe_cleanup_fixpoint` | 無 | 多個安全 cleanup/simplification passes 到 fixpoint |
|remove_dead_logic|	[--include-sequential]	|移除所有無法從 PO / DFF input endpoint 反向到達的 gates 與 internal nets。trim_dead_logic、remove_dangling_logic 是 alias |
| `remove_unused_nets` | 無 | 移除 unused internal nets |
| `remove_net_if_unused` | `<net>` | 只嘗試移除指定 unused net |
| remove_redundant_logic |	[--time-limit seconds]	| 移除可證明不影響功能的 redundant logic:SAT-proven 常數傳播 + 單路徑 untestable stuck-at fault,最後以 dead logic cleanup 收尾 |
| `merge_structurally_equivalent_gates` | 無 | 只合併 structural duplicates |
| `merge_functionally_equivalent_gates` | `<scope> [scope_name] [--gate-type type] [--patterns N] [--time-limit seconds]` | SAT-proven functional duplicate merge |
| `simplify_constants` | `[gate_type\|all] [0\|1\|any] [--inputs N]` | typed constant propagation |
| `simplify_same_input` | 無 | same-input Boolean simplification |

`merge_equivalent_gates` 不是 public command；它是名稱容易誤導的 legacy structural alias。functional merge 固定使用 `merge_functionally_equivalent_gates`。

## 5. Buffer Modes

| Mode | 參數 | 用途 |
|---|---|---|
| `insert_buffers_for_fanout` | `<max_fanout>` | 涵蓋 gate output、DFF Q 與 primary input net;cascaded 結構 |
| `insert_buffers_for_net` | `<net> <max_fanout>` | 指定 net fanout buffering。prompt 指名具體 net 時一律使用本 mode,不論該 net 的角色 |
| `insert_buffers_for_dff_control` | `<max_fanout> -clock\|-reset` | 全設計 DFF clock/reset(含 SN)control buffering。只在 prompt 未指名特定 net 時使用;指名 net 請改用 insert_buffers_for_net |
| `insert_buffers_on_each_load` | `<net>` | 每個 load 前插 dedicated BUF |
| `insert_buffer_at_driver` | `<net>` | driver side 插 BUF |
| `insert_buffer_before_gate` | `<net> <gate>` | 指定 gate load 前插 BUF |
| `insert_buffers_by_gate_type` | `<type> [-inputs] [-outputs] [-both]` | 依 gate type input/output 插 BUF |

## 6. Technology Mapping Modes

```text
edit_apply convert_basis <scope> [scope_name]
           -allow <type...> [-ban <type...>] [--validate_equivalence]

edit_apply replace_type <scope> [scope_name] <target_type>
           -allow <type...> [--validate_equivalence]
```

scope：

```text
whole
net_fanin <net>
net_fanout <net>
gate_fanin <gate>
gate_fanout <gate>
```

`convert_basis` 確保 scope 最終只使用允許且未被禁止的 combinational gate types。`replace_type` 只替換 scope 中指定 `target_type`。

## 7. 輸出判讀

先讀 envelope，再讀 edit report：

| 欄位 | 意義 |
|---|---|
| `report_success` | edit transaction 是否成功 |
| `report_changed` | design 是否真的發生修改 |
| `rolled_back` | 失敗修改是否已 rollback |
| `before_stats`, `after_stats` | 修改前後 active 統計 |
| `diff.active_gate_count_delta` | active gate 總數變化 |
| `diff.gate_type_count_delta` | 各 gate type 變化 |
| `validation.equivalence_checked` | 是否取得 equivalence certificate |
| `validation.functionally_equivalent` | certificate 是否證明等價 |
| `validation.equivalence_method` | `StructuralIdentity`、`LocalRewriteRule` 或其他方法 |
| `mapping_delta` | mapping removed/added/final type counts |
| `constant_simplification` | constant pass candidate/simplified/eliminated count |
| `functional_merge` | search/class/SAT 統計、merged/skipped gates、whole-design SAT 與逐筆 merge records |
| `fanout_change` | before/after max fanout 與 constraint result |
| `changed_gate_names`, `changed_net_names` | 受影響物件 |
| dead_logic |	移除的 gate/net/DFF 數與是否 timeout |
| redundancy_removal |	移除的 gate/net 數、已證明的 redundant pin 數、完整性 |

`status:no_change` 是成功但沒有修改機會。`ok:true` 不代表 `report_changed:true`。若 envelope `complete:false`，不能宣稱已取得完整 equivalence certificate。

`insert_buffers_for_net` 的 `fanout_change` scope 是指定 net/bus 與本次建立的 buffer tree；全設計是否都低於限制只能由 `insert_buffers_for_fanout` 判斷。若 insert_buffers_for_net 的目標 net 不存在,回報 report_success:false 與明確訊息,不會產生 fanout_change;此時不可回答已完成 buffer 插入。

題目問「移除了多少 gates」時使用 active count delta；題目問「多少 OR gates 被 constant-1 propagation 消除」時使用 `constant_simplification.eliminated_target_gate_count`，不要使用較寬泛的 `simplified_count`。simplified_count 是所有被化簡的候選數(含輸入減少但型別未變的情況);eliminated_target_gate_count 只算「化簡後不再是該 target type」的數量,才是「多少顆 X gate 被消除」的正確答案。常數傳播會跑到 fixpoint,因此結果包含被連鎖化簡的下游 gate。

題目問「移除了多少 dangling / unused / dead gates」時,正式答案讀 dead_logic.removed_gate_count,不要用 diff.active_gate_count_delta 反推。問移除多少 net 時讀 dead_logic.removed_net_count。dead_logic.timed_out:true 代表 design 完全未改動,必須回報 timeout 而非數量。

題目問「移除了多少 redundant gates」時,讀 redundancy_removal.removed_gate_count。redundancy_removal.complete:false 或 envelope 為 status:timeout 時,回答必須說明這是在時間預算內找到的數量,不是設計中全部的 redundancy,且不得宣稱已完成移除。proven_redundant_pin_count 是已用 SAT 證明為 untestable 的 stuck-at fault 數,供需要區分「常數傳播」與「真正 ATPG redundancy」的題目使用;一般題目只需要 removed_gate_count。

## 8. Prompt Examples

```text
Prompt: Collapse all back-to-back inverter pairs.
Command: edit_apply collapse_double_inverter
Read: report_changed, diff.active_gate_count_delta, validation
```

```text
Prompt: How many 2-input OR gates were eliminated by constant-1 propagation?
Command: edit_apply simplify_constants OR 1 --inputs 2
Read: constant_simplification.eliminated_target_gate_count
```

```text
Prompt: Reconstruct the design using only AND and NOT gates.
Command: edit_apply convert_basis whole -allow AND NOT
Read: mapping_delta, final_gate_count_by_type, validation
```

```text
Prompt: Replace XOR gates in the fanin cone of n10 using NAND gates.
Command: edit_apply replace_type net_fanin n10 XOR -allow NAND
Read: mapping_delta.removed_count_by_type / added_count_by_type
```

```text
Prompt: Find and merge all gate pairs that are functionally equivalent.
Command: edit_apply merge_functionally_equivalent_gates whole
Read: functional_merge.merged_gate_count, merge records, validation.equivalence_method
Require: report_success=true, rolled_back=false, whole_design_equivalent=true
```

## 9. 組合流程與限制

修改後回答成果可再呼叫 `report_query last_edit`。`merge_functionally_equivalent_gates` 已強制執行 whole-design SAT；其他 edit 若題目明確要求 whole-design 功能不變，可再呼叫 `equiv_query previous_edit`，最終流程可使用 `equiv_query original`。

- structural duplicate merge 與 functional duplicate merge 是不同 mode；後者成本較高且可能 timeout。
- fixed basis conversion 不會自動找到 minimum depth/best cost implementation。
- depth/cost optimization 的公開契約見 [`OPTIMIZATION_TOOL.md`](OPTIMIZATION_TOOL.md)。
- public parser 不提供可能任意改變功能的低階 rewiring commands。
- 只有 `status:ok|no_change`、`complete:true`、`report_success:true`，且題目要求等價時 `validation.functionally_equivalent:true`，才能回答 edit 已成功完成。timeout/error/partial 或 rollback 不得沿用前一次 edit 的 delta。
- 若 prompt 要求保留前題 constraint，LLM 必須在新的 `edit_apply` command 重新傳入 basis/fanout 等條件，或在 write 前主動用對應 query 驗證；session 不會自動保存自然語言 constraint。
- remove_redundant_logic 刻意排除 DFF 的 CK/RN/SN fanin cone。這些路徑即使 Boolean 上等價,改寫仍可能引入 glitch,不在本 pass 的處理範圍。
- redundancy 候選之間並非彼此獨立:移除其中一個之後,另一個可能變成必要的。因此本 pass 每次只套用一個已證明的 stuck-at redundancy,其餘留待下一次呼叫。若題目要求盡可能移除,可重複呼叫直到 proven_redundant_pin_count 為 0。