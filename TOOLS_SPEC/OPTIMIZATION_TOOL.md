# Depth Optimization Tool

## 1. 責任

`opt_query` 與 `opt_apply` 負責 cost/objective-driven 的 critical-path depth 最佳化。它們與 `depth_query` 的差異是會搜尋並嘗試提交重構後的 design；與 `edit_apply` 的差異是使用者指定的是目標與限制，而不是固定 transformation。

完整性規則：不得自行加入題目未要求的 candidate/result 數量上限；depth target、gate basis 與
fanout limit 是題目 constraint，不是輸出截斷。時間限制依題目指定。詳見
[`LLM_NOTES.md`](LLM_NOTES.md)。

公開 tools layer 只開放 `CriticalPathDepth` 高階 transaction，不直接暴露 `DepthOptimizer`、mockturtle 或 unchecked rewrite。graph 完全不變時以 `StructuralIdentity` 證明；其餘 changed candidate 必定嘗試 whole-design SAT。SAT 證明 mismatch 時不提交，但 SAT UNKNOWN/inconclusive 且沒有 mismatch 時，現行 scoring-oriented policy 可能信任 mockturtle 的 function-preserving rewrite 並提交，因此「執行 SAT」不等於「取得 SAT proof」。

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
time limit = 290 seconds
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
| `status:timeout` | transaction 預算耗盡；若發生在 core 前通常不提交，但 SAT inconclusive/timeout 且未找到 mismatch 時，現行 policy 可能已接受 candidate，必須再讀 `changed`、`candidate_accepted` 與 warnings |
| `status:error` | scope、constraint、target、structure 或 equivalence 驗證失敗 |
| `complete:true` | CLI 已完整回傳本次 transaction 結果；現行 legacy bool 可能把 trusted-but-unproven 視為 accepted，不能據此宣稱 SAT proof |

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
| `whole_design_equivalence_checked` | 是否呼叫 whole-design checker；表示 SAT attempt，不表示一定得到 proof；StructuralIdentity no-op 為 `false` |
| `whole_design_equivalent` | 現行 acceptance 相容欄位；inconclusive 但被接受時也可能為 `true`，不可單獨視為 proof |
| `whole_design_timed_out` | SAT 是否超時 |
| `compared_output_count`, `compared_dff_d_count` | 實際比較的 sequential boundaries |
| `time_budget_seconds`, `elapsed_seconds` | 預算與耗時 |

不可只因 `status:ok` 就回答「depth 已降低」。若原設計違反明確 gate-type hard constraint，流程可接受等價且合規、但 depth 沒改善的候選；此時會輸出 `improved:false`、`baseline_constraints_satisfied:false` 與 warning。最終答案必須直接報告 before/after depth。

等價結果必須分三類：

| 類別 | 判讀方式 | 回答方式 |
|---|---|---|
| Structural identity | `changed:false`、`EquivalenceMethod:StructuralIdentity` | 說明 design 未改變，沒有執行 SAT |
| SAT-proven | changed candidate、`WholeDesignSat`、等價欄位為 true，且 warnings 沒有 `This has not been proven by SAT` | 可說所比較的 PO/DFF.D 已由 whole-design SAT 證明等價 |
| Trusted but unproven | candidate accepted，warnings 包含 `This has not been proven by SAT` | 只能說工具依 function-preserving rewrite 假設接受；不可稱為 SAT-proven |

因此，LLM 不可只看 `status`、`complete`、`whole_design_equivalent` 或
`functionally_equivalent` 任一單獨欄位回答「已證明等價」。

timeout 判讀需搭配 `core_status`：`core_status:TIMEOUT` 且
`candidate_generated:false` 表示預算在 optimizer core 前已耗盡，因此
`whole_design_timed_out:false`；若 SAT 階段超時，才會看到
`whole_design_timed_out:true`。

## 7. Prompt Examples

```text
Prompt: Reduce the critical path depth through restructuring. Make sure nothing changes functionally.
Command: opt_apply critical_path_depth --scope whole --objective global
Read: before_depth, after_depth, improved, changed, candidate_accepted, equivalence method, whole_design_equivalent, whole_design_timed_out, warnings
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

最佳化前可用 `depth_query global_critical` 取得 current baseline。`opt_apply` 已內建 mandatory equivalence attempt：graph identity 使用 `StructuralIdentity`，其餘 changed candidate 嘗試 whole-design SAT。一般情況不需為了提交而額外呼叫 `equiv_query`；但若 warnings 表示 `This has not been proven by SAT`，而 prompt 明確要求 proof，必須再使用 `equiv_query previous_edit` 取得獨立 follow-up report。若 follow-up 仍為 UNKNOWN，不可把結果描述成已證明等價。

最佳化後的完整 report 會存入同一個 edit cache，可用：

```text
report_query last_edit
```

## 9. 限制

- 目前只公開 `critical_path_depth`，不公開 optimizer internal passes。
- 演算法是 best-effort，不保證取得數學上的 global optimum；只有明確命中的
  lower-bound pattern 才能宣稱 already optimal。
- `--target-depth` 是 commit requirement；未達成時 candidate rollback。
- `--allow-no-improvement` 允許提交同 depth 的合規候選，但仍要求進入等價驗證流程；若 optimizer
  沒有造成 graph change，回 `candidate_generated:false`、`candidate_accepted:false`、
  `whole_design_equivalence_checked:false` 與 `EquivalenceMethod:StructuralIdentity`，不啟動 SAT。
- whole-design equivalence 比較所有 PO 與 DFF.D；DFF initial state 尚未納入。
- changed candidate 的 whole-design SAT 若為 UNKNOWN/inconclusive，目前可能在沒有 mismatch
  的情況下被接受。這種結果會帶有 `This has not been proven by SAT` warning；即使
  `complete:true`、`whole_design_equivalent:true`，也不可稱為 SAT proof。
- `--time-limit` 目前能限制 transaction 後段與 whole-design SAT，但單次
  mockturtle primitive 尚無 cooperative cancellation；大型 scoped cone 可能超出
  此時間。LLM 必須保留足夠的整體時間預算，避免在 deadline 前啟動無法安全完成的最佳化。
- `--verbose` 可能產生 optimizer diagnostic output，LLM 一般不應開啟。
- 若後續 prompt 要求延續前一題的 gate-type 或 fanout constraint，LLM 必須在本次 `opt_apply` 重新傳入對應 `--allowed` / `--banned` 等限制；工具不會自動從自然語言上下文保存 constraint。
