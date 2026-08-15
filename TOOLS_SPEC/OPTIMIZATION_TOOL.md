# Depth Optimization Tool

## 1. 責任

`opt_query` 與 `opt_apply` 負責 cost/objective-driven 的 critical-path depth 最佳化。它們與 `depth_query` 的差異是會搜尋並嘗試提交重構後的 design；與 `edit_apply` 的差異是使用者指定的是目標與限制，而不是固定 transformation。

完整性規則：不得自行加入題目未要求的 candidate/result 數量上限；depth target、gate basis 與
fanout limit 是題目 constraint，不是輸出截斷。時間限制依題目指定。詳見
[`LLM_NOTES.md`](LLM_NOTES.md)。

公開 tools layer 只開放 `CriticalPathDepth` 高階 transaction，不直接暴露 `DepthOptimizer`、mockturtle 或 unchecked rewrite。候選必須通過 scope、gate-type constraint 與 target depth 檢查才會 commit。

**等價驗證不在本 tool 內執行。** 候選來自 mockturtle 的 balancing / cut_rewriting /
resubstitution，這些是同一顆網路上的 Boolean-preserving 改寫；輸出的 gate basis 由
polarity-DP lowering 在建構時保證。兩者都是結構保證，不是事後驗證。需要 SAT
等價證明時請另外呼叫 `equiv_query`。

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
          [--basis-scope <scope> [scope_name]]
          [--basis-name <scope_name>]
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
basis scope = whole      (gate 限制套用於整張 netlist)
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

## 4. 兩個獨立的作用域

這是本 tool 最容易用錯的地方。prompt 通常包含兩句不同的話，各自對應一組參數：

| prompt 裡的句子 | 對應參數 | 語意 |
|---|---|---|
| `The cost function is ...` | `--scope` + `--objective` | 要最小化「哪裡」的 depth |
| `ensuring ... only ... gates` | `--basis-scope` + `--allowed`/`--banned` | gate 限制套用在「哪裡」 |

兩者互相獨立，可以指向不同範圍。

### Cost scope（`--scope` / `--objective`）

| 參數 | 語意 |
|---|---|
| `--objective global` | cost 是全設計最大 PO/DFF.D depth |
| `--objective cone` | cost 是 resolved fanin root depth；`--scope` 必須是 `net_fanin` 或 `gate_fanin` |
| `--scope whole` | 搭配 global objective |
| `--scope net_fanin n10` | 指定 net 的 fanin cone |
| `--scope gate_fanin g10` | 指定 gate output 的 fanin cone |
| `--scope net_fanout n10` / `gate_fanout g10` | 只可搭配 global objective |

scope name 可直接放在 `--scope` 後，也可寫成 `--name`：

```text
--scope net_fanin n10
--scope net_fanin --name n10
```

### Basis scope（`--basis-scope` / `--basis-name`）

| 情況 | 寫法 |
|---|---|
| `ensuring the netlist remains NAND and NOT only` | 省略 `--basis-scope`（預設全域） |
| `ensuring the cone of n10 maintains only NOR and NOT` | `--basis-scope net_fanin n10 --allowed NOR NOT` |

`--basis-scope` 指定 cone 時，cone 以外的邏輯不受 gate 限制，optimizer 可自由使用任何 gate 壓深度。

**未指定 `--basis-scope` 時的相容推導**：若 `--objective global` 且 `--scope` 是 cone 且有 gate 限制，
會把 `--scope` 當成 basis scope 並附上 warning。這是為了相容舊呼叫方式；新的呼叫請一律明確指定。

### DFF.Q 的特殊情形

若 `--scope net_fanin <net>` 的 `<net>` 是 DFF.Q，該 fanin 視為 empty combinational cone；流程會保留 original，回傳 no-op / already-optimal 類結果，不會穿透到同一顆 DFF 的 D input。這和 `--scope gate_fanin <dff_gate>` 不同：後者是明確以 gate input-side cone 為 scope。

## 5. Modes

| Command / Mode | 用途 | 主要 data |
|---|---|---|
| `opt_query critical_path_depth` | 確認 pass-level candidate 可用；不修改 design | `candidate_count` |
| `opt_apply critical_path_depth` | 搜尋、驗證並嘗試提交 depth candidate | `depth_change`, `depth_optimization` |

`opt_query` 不保證之後一定找到較佳 implementation，只表示此高階最佳化流程可執行。

對 `--objective cone`，transaction 會先檢查可保守證明的 lower bound。目前可
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
| `status:timeout` | transaction 預算在 optimizer core 前或中耗盡 |
| `status:error` | scope、constraint、target 或 structure 驗證失敗 |
| `complete:true` | transaction 成功完成 |

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
| `scope`, `requested_scope_name` | cost 作用域 |
| `basis_scope`, `basis_scope_name` | gate 限制作用域 |
| `resolved_root_net_name` | 實際最佳化 root |
| `allowed_gate_types`, `banned_gate_types` | hard gate-type constraints |
| `baseline_constraints_satisfied` | 原設計是否已符合 constraints |
| `final_constraints_satisfied` | 候選是否符合 constraints |
| `candidate_generated`, `candidate_accepted` | core 是否產生候選、transaction 是否提交 |
| `time_budget_seconds`, `elapsed_seconds` | 預算與耗時 |

`whole_design_equivalence_checked`、`whole_design_equivalent`、`whole_design_timed_out`、
`compared_output_count`、`compared_dff_d_count` 這幾個欄位保留自舊版，本 pass 不再填值，
一律為 `false` / `0`。**不要據此回答等價相關的問題**——要等價結論請呼叫 `equiv_query`。

回答時必須直接報告 before/after depth。不可只因 `status:ok` 就說「depth 已降低」：
若原設計違反明確 gate-type hard constraint，流程會接受合規但 depth 沒改善的候選，
此時輸出 `improved:false`、`baseline_constraints_satisfied:false` 與 warning。

面積通常會增加。深度最佳化以 depth 為唯一主要目標，面積只在同 depth 時當 tie-break，
所以 `gate_count_delta` 為正是預期行為，不是錯誤。回報時可一併說明。

## 7. Prompt Examples

```text
Prompt: Reduce the critical path depth through restructuring. Make sure nothing changes functionally.
Command: opt_apply critical_path_depth --scope whole --objective global
Read: before_depth, after_depth, improved, candidate_accepted
```

```text
Prompt: Reduce critical path depth, ensuring the cone of n10 maintains only NOR and NOT gates.
        The cost function is the maximum logic depth of the final design.
Command: opt_apply critical_path_depth --objective global
         --basis-scope net_fanin n10 --allowed NOR NOT
Read: depth_change, basis_scope_name, final_constraints_satisfied
```

```text
Prompt: Minimize the maximum logic depth, ensuring the netlist remains AND and NOT only.
Command: opt_apply critical_path_depth --objective global --allowed AND NOT
Read: before_depth, after_depth, final_constraints_satisfied
```

```text
Prompt: Optimize the logic cone of output n14 while ensuring the netlist remains NAND and
        NOT only. The cost function is the depth of the cone of n14.
Command: opt_apply critical_path_depth --scope net_fanin n14 --objective cone --allowed NAND NOT
Read: objective_metric, before_depth, after_depth, basis_scope, final_constraints_satisfied
Note: cost 是 n14 的 cone，但 gate 限制是整張 netlist，所以不加 --basis-scope。
```

```text
Prompt: Reduce the maximum logic depth to at most 5.
Command: opt_apply critical_path_depth --target-depth 5
Read: meets_target, report_success, rolled_back
```

## 8. 組合流程

最佳化前可用 `depth_query global_critical` 取得 current baseline。

本 pass **不**執行 whole-design SAT。若 prompt 要求驗證功能等價，或需要在提交後
確認設計未被改壞，請另外呼叫：

```text
equiv_query original           # 對照最初載入的 netlist
equiv_query previous_edit      # 對照本次 opt_apply 之前的狀態
```

最佳化後的完整 report 會存入同一個 edit cache，可用：

```text
report_query last_edit
```

## 9. 限制

- 目前只公開 `critical_path_depth`，不公開 optimizer internal passes。
- 演算法是 best-effort，不保證取得數學上的 global optimum；只有明確命中的
  lower-bound pattern 才能宣稱 already optimal。
- `--target-depth` 是 commit requirement；未達成時 candidate rollback。
- `--allow-no-improvement` 允許提交同 depth 的合規候選。若 optimizer 沒有造成
  graph change，回 `candidate_generated:false`、`candidate_accepted:false` 與
  `EquivalenceMethod:StructuralIdentity`。
- 本 pass 不執行 whole-design SAT；等價結論一律以 `equiv_query` 為準。
- `--basis-scope` 目前只支援單一 cone。同時對兩個不同 cone 施加不同 gate 限制尚未支援。
- `--time-limit` 會依前一輪實測時間預測下一輪，時間不夠時停在目前最佳候選，
  但單次 mockturtle primitive 仍無 cooperative cancellation，超時最多會多跑一輪。
- 深度最佳化通常使 gate count 增加，面積只在同 depth 時作為 tie-break。
- `--verbose` 可能產生 optimizer diagnostic output，LLM 一般不應開啟。
- 若後續 prompt 要求延續前一題的 gate-type 或 fanout constraint，LLM 必須在本次 `opt_apply` 重新傳入對應 `--allowed` / `--banned` / `--basis-scope`；工具不會自動從自然語言上下文保存 constraint。
