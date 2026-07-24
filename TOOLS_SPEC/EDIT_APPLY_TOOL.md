# Edit Apply Tool

## 1. 責任

`edit_apply` 執行使用者已指定的高階 transformation，並建立統一 `NetlistEditReport`。public parser 不開放 unchecked rewiring、driver replacement 或任意 net merge；functional duplicate merge 只能走 SAT search、cycle-safe apply 與 whole-design rollback 的專用 mode。每次 public edit 預設要求 equivalence certificate。

## 2. 選擇條件

prompt 明確指定 rename、cleanup、constant propagation、buffer insertion、gate replacement、basis conversion，或要求找出並合併 functionally equivalent gates 時使用本 tool。若要求 `minimize`、`best depth`、`best cost` 或自動搜尋最佳 transformation，改用 `opt_apply critical_path_depth`，不是 `edit_apply`。

## 3. Rename/Cleanup Modes

| Mode | 參數 | 用途 |
|---|---|---|
| `rename_gate` | `<old> <new>` | rename gate instance |
| `rename_net` | `<old> <new>` | rename net/wire |
| `cleanup_buffers` | 無 | bypass 可安全移除的 BUF |
| `collapse_double_inverter` | 無 | collapse internal back-to-back NOT；不清除既有 BUF |
| `local_simplification_fixpoint` | 無 | local simplification 到 fixpoint |
| `safe_cleanup_fixpoint` | 無 | 多個安全 cleanup/simplification passes 到 fixpoint |
| `trim_dead_logic` | 無 | 移除不影響 boundary 的 dead logic |
| `remove_dangling_logic` | 無 | 移除 dangling gates |
| `remove_unused_nets` | 無 | 移除 unused internal nets |
| `remove_net_if_unused` | `<net>` | 只嘗試移除指定 unused net |
| `merge_structurally_equivalent_gates` | 無 | 只合併 structural duplicates |
| `merge_functionally_equivalent_gates` | `<scope> [scope_name] [--gate-type type] [--patterns N] [--time-limit seconds]` | SAT-proven functional duplicate merge |
| `simplify_constants` | `[gate_type\|all] [0\|1\|any] [--inputs N]` | typed constant propagation |
| `simplify_same_input` | 無 | same-input Boolean simplification |

`merge_equivalent_gates` 不是 public command；它是名稱容易誤導的 legacy structural alias。functional merge 固定使用 `merge_functionally_equivalent_gates`。

## 4. Buffer Modes

| Mode | 參數 | 用途 |
|---|---|---|
| `insert_buffers_for_fanout` | `<max_fanout>` | 全設計 fanout buffering |
| `insert_buffers_for_net` | `<net> <max_fanout>` | 指定 net fanout buffering |
| `insert_buffers_for_dff_control` | `<max_fanout> -clock\|-reset` | DFF clock/reset control buffering |
| `insert_buffers_on_each_load` | `<net>` | 每個 load 前插 dedicated BUF |
| `insert_buffer_at_driver` | `<net>` | driver side 插 BUF |
| `insert_buffer_before_gate` | `<net> <gate>` | 指定 gate load 前插 BUF |
| `insert_buffers_by_gate_type` | `<type> [-inputs] [-outputs] [-both]` | 依 gate type input/output 插 BUF |

## 5. Technology Mapping Modes

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

## 6. 輸出判讀

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

`status:no_change` 是成功但沒有修改機會。`ok:true` 不代表 `report_changed:true`。若 envelope `complete:false`，不能宣稱已取得完整 equivalence certificate。

`insert_buffers_for_net` 的 `fanout_change` scope 是指定 net/bus 與本次建立的 buffer tree；全設計是否都低於限制只能由 `insert_buffers_for_fanout` 判斷。

題目問「移除了多少 gates」時使用 active count delta；題目問「多少 OR gates 被 constant-1 propagation 消除」時使用 `constant_simplification.eliminated_target_gate_count`，不要使用較寬泛的 `simplified_count`。

## 7. Prompt Examples

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
Command: edit_apply merge_functionally_equivalent_gates whole --time-limit 30
Read: functional_merge.merged_gate_count, merge records, validation.equivalence_method
Require: report_success=true, rolled_back=false, whole_design_equivalent=true
```

## 8. 組合流程與限制

修改後回答成果可再呼叫 `report_query last_edit`。`merge_functionally_equivalent_gates` 已強制執行 whole-design SAT；其他 edit 若題目明確要求 whole-design 功能不變，可再呼叫 `equiv_query previous_edit`，最終流程可使用 `equiv_query original`。

- structural duplicate merge 與 functional duplicate merge 是不同 mode；後者成本較高且可能 timeout。
- fixed basis conversion 不會自動找到 minimum depth/best cost implementation。
- depth/cost optimization 的公開契約見 [`OPTIMIZATION_TOOL.md`](OPTIMIZATION_TOOL.md)。
- public parser 不提供可能任意改變功能的低階 rewiring commands。
