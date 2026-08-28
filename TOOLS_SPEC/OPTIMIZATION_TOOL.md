# Optimization Tool (Depth / Area)

## 1. 責任

`opt_query` 與 `opt_apply` 負責 cost-driven 的最佳化。`opt_query` 只查詢候選資訊；
`opt_apply` 搜尋並嘗試提交重構後的 design。它們與 `depth_query` / `structure_query` 的差異是
operation 要改變 design cost；與 `edit_apply` 的差異是使用者指定的是**目標與限制**，而不是
固定 transformation identity。

本 tool 公開兩個 pass，兩者用法完全相同，只有「成本量什麼」不同：

| Pass | cost | 典型 prompt 用語 |
|---|---|---|
| `critical_path_depth` | 邏輯深度 | `reduce critical path`, `minimize maximum logic depth` |
| `gate_count_minimization` | 閘數 | `minimize the total gate count`, `smaller area is better` |

**最佳化已經包含 basis 轉換的能力。** 題目要求的 gate 限制直接用 `--allowed` / `--banned` 傳入即可，不需要另外呼叫 `edit_apply convert_basis`。

完整性規則：不得自行加入題目未要求的 candidate/result 數量上限；cost target、gate basis 與 fanout limit 是題目 constraint，不是輸出截斷。時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

---

## 2. 語意拆解與 Command

當 operation 是在 hard constraints 下最小化某個 measurable cost 時使用本工具。先建立互相
獨立的三組語意：

1. objective metric：maximum logic depth 或 gate count，決定 pass。
2. cost scope：評分全設計或指定 fanin cone，決定 `--scope` / `--cost-scope`。
3. basis constraints：允許/禁止 gate types 套用在哪個區域，決定 `--basis-scope` 與
   inside/outside gate-type options。

固定 rewrite 名稱不等於 objective；只要求 conversion、replacement、cleanup 或 buffering 而
沒有較佳 cost 的搜尋語意時，owner 是 `edit_apply`。只量測 current cost 時則使用
`depth_query` / `structure_query`。

實際要求改善並輸出修改後設計時直接使用 `opt_apply`。`opt_query` 只在 prompt 明確要求
inspect/preview optimization candidates 而不修改 current design 時使用；不能先查候選後讓 LLM
自行挑選或逐筆 rewiring 來取代 transactional `opt_apply`。

判讀只需要回答兩個問題，分別對應兩組互相獨立的參數：

| prompt 裡的句子 | 問題 | 參數 |
|---|---|---|
| `The cost function is ...` | 要最小化「哪裡」的 cost | `--scope` + `--cost-scope` |
| `ensuring ... only ... gates` | gate 限制套用在「哪裡」 | `--basis-scope` + `--allowed` / `--banned` |

兩句話要分開讀。**最常見的錯誤**是看到限制句裡的 `the cone of n10` 就去設 `--scope net_fanin n10` —— 若 cost 句寫的是 `the final design`，cost 就是全域，`--scope` 必須維持預設。

### A. 無 gate 限制

```text
Prompt: Reduce the critical path depth through restructuring. The cost function is
        the maximum logic depth of the final design; smaller is better.
Command: opt_apply critical_path_depth
```

### B. 全域 gate 限制（限制句主詞是 `the netlist`）

沒有 cone，用 `--allowed` / `--banned`，**不加** `--basis-scope`。

```text
Prompt: ... ensuring the netlist remains AND and NOT only. ...
Command: opt_apply critical_path_depth --allowed AND NOT
```

```text
Prompt: ... ensuring the netlist contains no AND gates. ...
Command: opt_apply critical_path_depth --banned AND
```

### C. Cone gate 限制（限制句主詞是 `the cone of nXX`）

用 `--basis-scope net_fanin nXX --allowed ...`。cost 句寫 `the final design`，所以 `--scope` / `--cost-scope` 維持預設。

```text
Prompt: ... ensuring the cone of n10 maintains only NOR and NOT gates. ...
        The cost function is the maximum logic depth of the final design.
Command: opt_apply critical_path_depth --basis-scope net_fanin n10 --allowed NOR NOT
```

### D. Cost 本身就是 cone

唯一要動 `--scope` / `--cost-scope` 的情況。

```text
Prompt: Optimize the logic cone of output n14 while ensuring the netlist remains NAND
        and NOT only. The cost function is the depth of the cone of n14.
Command: opt_apply critical_path_depth --scope net_fanin n14 --cost-scope cone
         --allowed NAND NOT
Note: cost 在 n14 的 cone，但限制句主詞是 the netlist（全域），所以不加
      --basis-scope。兩個範圍剛好相反，這正是它們必須分開的理由。
```

### E. 異質 basis（cone 內外各有不同限制）

cone 內用 `--allowed` / `--banned`，cone 以外用 `--outside-allowed` / `--outside-banned`。

```text
Prompt: ... Ensure that the cone of n12 consists exclusively of NAND and NOT gates,
        while the rest of the netlist outside this cone maintains only NOR and NOT
        gates. ...
Command: opt_apply critical_path_depth --basis-scope net_fanin n12
         --allowed NAND NOT --outside-allowed NOR NOT
```

```text
Prompt: ... ensuring the cone of n25 contains only AND, OR, and NOT gates, and
        strictly prohibit the use of XOR and XNOR gates in the logic outside of this
        cone. ...
Command: opt_apply critical_path_depth --basis-scope net_fanin n25
         --allowed AND OR NOT --outside-banned XOR XNOR
```

### F. 面積題

pass 換成 `gate_count_minimization`，其餘規則完全相同。

```text
Prompt: Minimize the total gate count of the design, ensuring the netlist remains
        NAND and NOT only. ...
Command: opt_apply gate_count_minimization --allowed NAND NOT
```

### G. `Report original if already optimal`

**不需要任何額外參數。** 預設就會在找不到改善時保留原設計，回 `status:no_change` 與 `EquivalenceMethod:StructuralIdentity`。

---

## 3. cost function 本身就是最佳化要求

題目**可能不會直接說「請最佳化」**，而是在一段看似只要求修改的 prompt 結尾，用一句 cost function 把最佳化目標帶進來：

```text
Insert buffers wherever needed so that no signal drives more than 16 loads.
Make sure nothing changes functionally.
The cost function is the total gate count of the final design; smaller is better.
```

這**不是**單純的 buffer insertion 題。`The cost function is ... ; smaller is better` 代表評分器會比較最終設計的閘數，因此必須呼叫 `opt_apply gate_count_minimization`。cost function 講 maximum logic depth 時同理，必須呼叫 `opt_apply critical_path_depth`。

### 順序規則：先最佳化，後套用硬性修改

```text
1. opt_apply <cost pass>              # 先壓 cost
2. edit_apply <題目要求的修改>          # 再滿足 hard constraint
3. 量測並回報最終 cost
```

**理由：** cost function 是 soft objective（越小越好），題目明寫的修改要求是 hard constraint（最終設計**必須**成立）。最佳化器不知道 fanout 限制的存在，會自由重構 —— 它可能移除剛插入的 buffer，或把某條 net 的 load 數推回 16 以上。因此**最後一個動作必須是保證 hard constraint 成立的那個動作**。

反過來做（先插 buffer 再最佳化）會讓最終設計違反題目的硬性要求，該題直接判錯，即使 cost 更低也沒有意義。

### 這代表 cost 會被第二步推高

buffer insertion 會把閘數加回去，最終數字高於最佳化剛結束時。**這是正確且預期的結果。** 不要為了讓數字好看而跳過 buffer insertion，也不要在 buffer insertion 之後再跑一次最佳化。回報時同時說明兩件事：hard constraint 已滿足、以及相對於原始設計的 cost 變化。

```text
read design.v
structure_query count_by_type            # 原始閘數 baseline
opt_apply gate_count_minimization        # 步驟 1
edit_apply insert_buffers_for_fanout 16  # 步驟 2
report_query last_edit                   # 讀 fanout_change.meets_constraint
structure_query count_by_type            # 最終閘數，用於回報
write out.v
```

深度版本把步驟 1 換成 `opt_apply critical_path_depth`，最後改用 `depth_query global_critical` 量測。

fanout hard constraint 的權威結果是第二步 Edit report 的
`fanout_change.meets_constraint`。通用 `structure_query global_fanout` 的分析 universe 會包含
constant literal loads，而 buffer mutation 不會對 `1'b0` / `1'b1` 建立 buffer tree，因此不可用
該 query 取代 Edit report 判定這個 mutation constraint 是否成功。

### 什麼時候不需要最佳化

prompt 完全沒有 cost function、`smaller is better`、`minimize`、`best` 這類比較語意時，才是純粹的 transformation 題，直接用 `edit_apply`。只要出現任何一句要比較最終設計優劣的話，就必須有一次對應的 `opt_apply`。

---

## 4. 參數說明

```text
opt_query  <critical_path_depth | gate_count_minimization>

opt_apply  <critical_path_depth | gate_count_minimization>
           [--scope <scope> [scope_name]]
           [--name <scope_name>]
           [--cost-scope whole|cone]
           [--basis-scope <net_fanin|gate_fanin> [scope_name]]
           [--basis-name <scope_name>]
           [--allowed <type...>]
           [--banned <type...>]
           [--outside-allowed <type...>]
           [--outside-banned <type...>]
           [--target-cost N]
           [--time-limit seconds]
           [--allow-no-improvement]
           [--verbose]
```

預設值：

```text
scope        = whole
cost-scope   = whole            (cost 量全設計)
basis scope  = whole            (gate 限制套用於整張 netlist)
target cost  = 未指定 (-1)
time limit   = 290 seconds
```

相容別名（舊 prompt 仍可用，新呼叫請用新名）：

```text
--objective global|cone   ≡  --cost-scope whole|cone
--target-depth N          ≡  --target-cost N
```

gate-type list 可用空白或逗號：`--allowed NOR NOT`、`--allowed nor,not`、`--banned XOR,XNOR`。

### Cost scope

| 參數 | 語意 |
|---|---|
| `--cost-scope whole` | cost 是全設計最大 depth 或全設計閘數（預設） |
| `--cost-scope cone` | cost 是指定 cone 的 depth 或閘數；`--scope` 必須是 `net_fanin` 或 `gate_fanin` |
| `--scope net_fanin n10` | 指定 net 的 fanin cone |
| `--scope gate_fanin g10` | 指定 gate output 的 fanin cone |
| `--scope net_fanout n10` / `gate_fanout g10` | 只可搭配 `--cost-scope whole` |

scope name 可放在 `--scope` 後，也可寫成 `--name`：

```text
--scope net_fanin n14
--scope net_fanin --name n14
```

全域閘數含 DFF；cone 閘數只含 cone 內的組合閘。

### Basis scope：兩個區域

工具永遠把設計分成兩個區域：

```text
cone 內   ← --basis-scope 指定；限制寫在 --allowed / --banned
cone 以外 ← 其餘全部；限制寫在 --outside-allowed / --outside-banned
```

`--outside-*` 永遠指「cone 以外的所有邏輯」。沒有 cone 時「cone 以外」就是整張 netlist，全域限制在內部一律歸到這一組。

| 情況 | 怎麼寫 |
|---|---|
| 只有全域限制 | `--allowed` / `--banned`，**不加** `--basis-scope` |
| 只有 cone 限制、cone 外不受限 | `--basis-scope net_fanin nXX --allowed ...` |
| cone 內外各有限制 | `--basis-scope net_fanin nXX --allowed ...` + `--outside-allowed ...` / `--outside-banned ...` |

`--outside-allowed` / `--outside-banned` **必須搭配 `--basis-scope`** —— 沒有 cone 就沒有「以外」。

> 讀 report 時：沒有 cone 時 `allowed_gate_types` 與 `outside_allowed_gate_types` 會印出**同一份清單**，這是上述正規化的結果，不是重複輸出。有 cone 時前者是 cone 內的、後者是 cone 外的。

`--basis-scope` 只支援單一 fanin cone（`net_fanin` / `gate_fanin`）。

### `--target-cost`

單位隨 pass 改變：`critical_path_depth` 是目標深度，`gate_count_minimization` 是目標閘數。未達成時 candidate rollback，回 `report_success:false`。

### DFF.Q 的特殊情形

若 `--scope net_fanin <net>` 的 `<net>` 是 DFF.Q，該 fanin 視為 empty combinational cone，流程會保留 original 並回傳 already-optimal 類結果，不會穿透到同一顆 DFF 的 D input。`--scope gate_fanin <dff_gate>` 則是明確以 gate input-side cone 為 scope。

---

## 5. 輸出判讀

先讀 envelope：

| Envelope | 意義 |
|---|---|
| `status:ok` | 候選已接受並修改 design |
| `status:no_change` | 原設計被保留，沒有接受的改善 |
| `status:timeout` | 時間預算耗盡 |
| `status:error` | scope、constraint、target 或 structure 驗證失敗 |

再讀 `cost_change`：

| 欄位 | 意義 |
|---|---|
| `metric_name` | **先讀這一欄**，它決定下面的數字是深度還是閘數 |
| `before_value`, `after_value` | 修改前後的實際 cost |
| `target_value` | 未指定 `--target-cost` 時為 `-1` |
| `improved` | `after_value < before_value` |
| `meets_target` | 是否達到明確 target；未指定 target 時為 `false` |

`metric_name` 的四個可能值：

```text
global_maximum_depth              全設計最大深度
scoped_fanin_cone_depth           指定 cone 的深度
global_gate_count                 全設計閘數（含 DFF）
scoped_fanin_cone_gate_count      指定 cone 的組合閘數
```

最後讀 `optimization`：

| 欄位 | 意義 |
|---|---|
| `objective_metric` | 與 `cost_change.metric_name` 相同 |
| `cost_scope`, `cost_scope_name` | cost 作用域 |
| `basis_scope`, `basis_scope_name` | gate 限制作用域 |
| `allowed_gate_types`, `banned_gate_types` | basis scope 那個範圍的限制 |
| `outside_allowed_gate_types`, `outside_banned_gate_types` | cone 以外的限制 |
| `baseline_constraints_satisfied` | 原設計是否已符合 constraints |
| `final_constraints_satisfied` | 候選是否符合 constraints |
| `candidate_generated`, `candidate_accepted` | 是否產生候選、是否提交 |
| `core_status`, `core_message` | 最佳化核心的結果 |
| `time_budget_seconds`, `elapsed_seconds` | 預算與耗時 |

`core_status`：

| 值 | 意義 |
|---|---|
| `SUCCESS` | 產生了有變化的候選 |
| `NO_IMPROVEMENT` | 完成但設計沒有可量測的變化 |
| `TIMEOUT` | 預算耗盡且沒有合規候選可交 |
| `ERROR_INVALID_REQUEST` | request 驗證失敗，或 cost 無法量測 |
| `ERROR_CONSTRAINT_UNSATISFIED` | 無法在要求的 basis 下產出合規電路 |
| `ERROR_NOT_EQUIVALENT` | 等價檢查反證，候選已回滾 |

若候選被拒絕，top-level `report_changed` 必須為 `false`，即使 cost、graph diff
與 `candidate_generated:true` 仍描述該候選；`rolled_back:true` 表示候選未提交，
current design 與 `design_revision` 均維持不變。

`whole_design_equivalence_checked`、`whole_design_equivalent`、`compared_output_count`、`compared_dff_d_count` 是舊版欄位，本 pass 不再填值，一律為 `false` / `0`。等價結論改讀 `validation.equivalence_method`。

### 等價結果分三類

| 類別 | 判讀方式 | 回答方式 |
|---|---|---|
| Structural identity | `changed:false`、`EquivalenceMethod:StructuralIdentity` | 說明 design 未改變 |
| Certified rewrite | changed candidate、`EquivalenceMethod:CertifiedRewrite` | 可說最佳化流程保持功能；**不可**稱為 SAT-proven |
| SAT-proven | 獨立 `equiv_query` 回 `WholeDesignSat` 且 equivalent | 可說所比較的 PO/DFF.D 已由 SAT 證明等價 |

prompt 裡的 `Make sure nothing changes functionally` / `Preserve functional equivalence` **不等於**要求 SAT proof —— 這是常態措辭，`CertifiedRewrite` 已足以回應，不需要額外呼叫 `equiv_query`。只有明確出現 `prove`、`SAT`、`formally verify`、`CEC` 時才另外驗證：

```text
equiv_query original           # 對照最初載入的 netlist
equiv_query previous_edit      # 對照本次 opt_apply 之前的狀態
```

### 回報時的注意事項

必須直接報告 before/after 的實際數值，並註明單位（深度或閘數）。不可只因 `status:ok` 就說「cost 已降低」：若原設計違反 gate-type hard constraint，流程會接受合規但 cost 沒改善的候選，此時輸出 `improved:false`、`baseline_constraints_satisfied:false` 與 warning。

**深度最佳化通常使面積增加。** 深度 pass 以 depth 為唯一主要目標，閘數只在同 depth 時當 tie-break，所以 `gate_count_delta` 為正是預期行為，不是錯誤。面積 pass 反之。回報時可一併說明這個取捨。

---

## 6. 限制

- 目前只公開 `critical_path_depth` 與 `gate_count_minimization`。
- 演算法是 best-effort，不保證數學上的 global optimum；只有工具明確回報 already-optimal 時才能這樣宣稱。
- `--target-cost` 是 commit requirement；未達成時 candidate rollback。
- `--allow-no-improvement` 允許提交同 cost 的合規候選。
- 本 pass 不執行 whole-design SAT；changed candidate 使用 `CertifiedRewrite`，不可描述成 SAT proof。
- `--basis-scope` 只支援單一 fanin cone；`--outside-allowed` / `--outside-banned` 需要 `--basis-scope`。
- `--verbose` 會產生 diagnostic output，LLM 一般不應開啟。
- 連續呼叫兩個 pass 時，第二次的 `before_value` 是第一次的輸出而非原始設計。題目只給一個 cost function 時，就只跑對應的那一個 pass。
- 若後續 prompt 要求延續前一題的 gate-type 或 fanout constraint，必須在本次 `opt_apply` 重新傳入對應參數；工具不會從自然語言上下文保存 constraint。
- 最佳化後的完整 report 可用 `report_query last_edit` 取回。
