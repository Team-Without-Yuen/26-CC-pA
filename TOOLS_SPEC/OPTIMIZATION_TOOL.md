# Depth Optimization Tool

## 1. 責任

`opt_query` 與 `opt_apply` 負責 cost/objective-driven 的 critical-path depth 最佳化。它們與 `depth_query` 的差異是會搜尋並嘗試提交重構後的 design；與 `edit_apply` 的差異是使用者指定的是目標與限制，而不是固定 transformation。

完整性規則：不得自行加入題目未要求的 candidate/result 數量上限；depth target、gate basis 與
fanout limit 是題目 constraint，不是輸出截斷。時間限制依題目指定。詳見
[`LLM_NOTES.md`](LLM_NOTES.md)。

公開 tools layer 只開放 `CriticalPathDepth` 高階 transaction，不直接暴露 `DepthOptimizer`、mockturtle 或 unchecked rewrite。候選只有在 scope、gate-type constraint、target depth 與 whole-design SAT 全部通過後才會 commit。

## 2. 選擇條件

prompt 出現 `reduce critical path`、`minimize maximum logic depth`、`best depth`、`smaller is better` 或明確的 depth cost function 時使用本 tool。

只想量測目前 depth 或列出 critical path 時使用 `depth_query`。明確要求固定 mapping，例如「convert to NOR/NOT」但沒有最佳化 objective 時使用 `edit_apply convert_basis`。

## 3. Command Grammar

```text
opt_query critical_path_depth

opt_apply critical_path_depth
          [--scope <scope> [scope_name]]
          [--name <scope_name>]
          [--objective global|cone]
          [--allowed <type...>]
          [--banned <type...>]
          [--target-depth N]
          [--time-limit seconds]
          [--allow-no-improvement]
          [--verbose]
```

預設值：

```text
scope = whole
objective = global
target depth = 未指定
time limit = 240 seconds
require depth improvement = true
```

gate-type list 可用空白或逗號：

```text
--allowed NOR NOT
--allowed nor,not
--banned XOR,XNOR
```

## 4. Scope 與 Objective

| 參數 | 語意 |
|---|---|
| `--scope whole` | 允許重寫全設計 |
| `--scope net_fanin n10` | 只重寫指定 net 的 fanin cone |
| `--scope gate_fanin g10` | 只重寫指定 gate output 的 fanin cone |
| `--scope net_fanout n10` | fanout rewrite scope；只可搭配 global objective |
| `--scope gate_fanout g10` | fanout rewrite scope；只可搭配 global objective |
| `--objective global` | cost 是全設計最大 PO/DFF.D depth |
| `--objective cone` | cost 是 resolved fanin root depth；scope 必須是 `net_fanin` 或 `gate_fanin` |

scope name 可直接放在 `--scope` 後，也可寫成 `--name`：

```text
--scope net_fanin n10
--scope net_fanin --name n10
```

若 `--scope net_fanin <net>` 的 `<net>` 是 DFF.Q，該 fanin 視為 empty combinational cone；流程會保留 original，回傳 verified no-op / already-optimal 類結果，不會穿透到同一顆 DFF 的 D input。這和 `--scope gate_fanin <dff_gate>` 不同：後者是明確以 gate input-side cone 為 scope。

## 5. Modes

| Command / Mode | 用途 | 主要 data |
|---|---|---|
| `opt_query critical_path_depth` | 確認安全 pass-level candidate 可用；不修改 design | `candidate_count`, `requires_equivalence_check` |
| `opt_apply critical_path_depth` | 搜尋、驗證並嘗試提交 depth candidate | `depth_change`, `depth_optimization`, `validation` |

`opt_query` 不保證之後一定找到較佳 implementation，只表示此高階最佳化流程可執行。

對 `ScopedFaninCone`，transaction 會先檢查可保守證明的 lower bound。目前可
直接證明並回傳 original 的情況包括：

```text
resolved root depth = 0
NAND/NOT-only basis 中，兩個獨立 boundary signals 的 NOT(NAND(a,b)) depth = 2
```

這類結果回 `status:no_change`、`EquivalenceMethod:StructuralIdentity`，
不呼叫昂貴的 heuristic optimizer。

## 6. 輸出判讀

先讀 envelope：

| Envelope | 意義 |
|---|---|
| `status:ok` | 候選已接受並修改 design |
| `status:no_change` | 原設計被保留，沒有接受的改善 |
| `status:timeout` | mandatory whole-design equivalence 超時，候選未提交 |
| `status:error` | scope、constraint、target、structure 或 equivalence 驗證失敗 |
| `complete:true` | 成功結果具有 equivalence certificate |

再讀 `depth_change`：

| 欄位 | 意義 |
|---|---|
| `before_depth`, `after_depth` | objective 修改前後的實際 depth |
| `target_depth` | 未指定時為 `-1` |
| `improved` | `after_depth < before_depth` |
| `meets_target` | 是否達到明確 target；未指定 target 時為 `false` |

最後讀 `depth_optimization`：

| 欄位 | 意義 |
|---|---|
| `objective_metric` | `global_maximum_depth` 或 `scoped_fanin_cone_depth` |
| `scope`, `requested_scope_name` | 使用者要求的 rewrite scope |
| `resolved_root_net_name` | 實際最佳化 root |
| `allowed_gate_types`, `banned_gate_types` | hard gate-type constraints |
| `baseline_constraints_satisfied` | 原設計是否已符合 constraints |
| `final_constraints_satisfied` | 候選是否符合 constraints |
| `candidate_generated`, `candidate_accepted` | core 是否產生候選、transaction 是否提交 |
| `whole_design_equivalence_checked` | 是否完成 whole-design SAT |
| `whole_design_equivalent` | PO 與 DFF.D 是否全部等價 |
| `whole_design_timed_out` | SAT 是否超時 |
| `compared_output_count`, `compared_dff_d_count` | 實際比較的 sequential boundaries |
| `time_budget_seconds`, `elapsed_seconds` | 預算與耗時 |

不可只因 `status:ok` 就回答「depth 已降低」。若原設計違反明確 gate-type hard constraint，流程可接受等價且合規、但 depth 沒改善的候選；此時會輸出 `improved:false`、`baseline_constraints_satisfied:false` 與 warning。最終答案必須直接報告 before/after depth。

## 7. Prompt Examples

```text
Prompt: Reduce the critical path depth through restructuring. Make sure nothing changes functionally.
Command: opt_apply critical_path_depth --scope whole --objective global
Read: before_depth, after_depth, improved, candidate_accepted, whole_design_equivalent
```

```text
Prompt: Reduce maximum depth while the cone of n10 contains only NOR and NOT gates.
Command: opt_apply critical_path_depth --scope net_fanin n10 --objective global --allowed NOR NOT
Read: depth_change, final_constraints_satisfied, resolved_root_net_name, validation
```

```text
Prompt: Minimize the depth of the fanin cone of n15 using only AND, OR, and NOT.
Command: opt_apply critical_path_depth --scope net_fanin n15 --objective cone --allowed AND OR NOT
Read: objective_metric, before_depth, after_depth, final_constraints_satisfied
```

```text
Prompt: Reduce the maximum logic depth to at most 5.
Command: opt_apply critical_path_depth --target-depth 5
Read: meets_target, report_success, rolled_back
```

## 8. 組合流程

最佳化前可用 `depth_query global_critical` 取得 current baseline。`opt_apply` 已內建 mandatory whole-design SAT，不需要再呼叫 `equiv_query` 才能提交；但可使用 `equiv_query previous_edit` 取得獨立 follow-up report。

最佳化後的完整 report 會存入同一個 edit cache，可用：

```text
report_query last_edit
```

## 9. 限制

- 目前只公開 `critical_path_depth`，不公開 optimizer internal passes。
- 演算法是 best-effort，不保證取得數學上的 global optimum；只有明確命中的
  lower-bound pattern 才能宣稱 already optimal。
- `--target-depth` 是 commit requirement；未達成時 candidate rollback。
- `--allow-no-improvement` 允許提交同 depth 的合規候選，但仍要求 whole-design equivalence。
- whole-design equivalence 比較所有 PO 與 DFF.D；DFF initial state 尚未納入。
- `--time-limit` 目前能限制 transaction 後段與 whole-design SAT，但單次
  mockturtle primitive 尚無 cooperative cancellation；大型 scoped cone 可能超出
  此時間。LLM 必須保留足夠的整體時間預算，避免在 deadline 前啟動無法安全完成的最佳化。
- `--verbose` 可能產生 optimizer diagnostic output，LLM 一般不應開啟。
- 若後續 prompt 要求延續前一題的 gate-type 或 fanout constraint，LLM 必須在本次 `opt_apply` 重新傳入對應 `--allowed` / `--banned` 等限制；工具不會自動從自然語言上下文保存 constraint。
