# Edit Apply Tool

## 1. 責任

`edit_apply` 執行使用者已指定的高階 transformation，並建立統一 `NetlistEditReport`。public parser 不開放 unchecked rewiring、driver replacement 或任意 net merge；functional duplicate merge 只能走 SAT search、cycle-safe apply 與結構驗證的專用 mode。每次 public edit 預設要求 structural/local/certified rewrite certificate，但不執行 final whole-design SAT。

完整性規則：不得因 report records 很多而省略 confirmed changes；若 prompt 只問數量，正式答案只回 delta 摘要。時間限制依題目指定。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

prompt 明確指定 rename、cleanup、constant propagation、buffer insertion、gate replacement、basis conversion，或要求找出並合併重複 gate 時使用本 tool。若要求 `minimize`、`best depth` 或自動搜尋最佳 transformation，改用 `opt_apply critical_path_depth`，不是 `edit_apply`。

### 縮減類 mode 的涵蓋層級

以下四個 mode 是嚴格的包含關係。**選擇原則：prompt 指定了什麼，就用對應的那一層。**

```
remove_redundant_logic          ⊃ safe_cleanup_fixpoint
  └ 全部結構化簡                      ⊃ local_simplification_fixpoint
    + SAT 常數傳播                       ⊃ remove_dead_logic
    + SAT functional merge
    + untestable stuck-at
```

| Mode | 涵蓋範圍 | 何時使用 |
|---|---|---|
| `remove_dead_logic` | 只移除無法從 PO / DFF 反向到達的邏輯 | prompt 說 dangling、unused、not contributing to any output |
| `local_simplification_fixpoint` | 加上 constant folding、same/complementary input、buffer/inverter 清理 | prompt 說 simplify the logic、apply local simplifications |
| `safe_cleanup_fixpoint` | 再加上 structural duplicate merge | prompt 說 clean up the netlist、apply all safe cleanups |
| `remove_redundant_logic` | 再加上 SAT-proven 常數傳播、SAT-proven functional merge、untestable stuck-at 移除 | prompt 說 redundant、reduce gate count as much as possible，或不確定該用哪一個時 |

**`remove_redundant_logic` 是縮減類的預設選擇。** 遇到以下情況一律使用它：

- prompt 出現 redundant、remove anything that can be removed without changing functionality、minimize the gate count、make the design as small as possible
- prompt 的縮減要求不明確，無法對應到上表的特定一層
- 已呼叫某個特定 mode 但回報 `status:no_change`，而 prompt 要求必須有所縮減

反過來，prompt 明確指定了單一種類的縮減時，**要用對應的專用 mode**，因為只有專用 mode 的 report 才有該種類的精確計數。例如問「移除了多少 dangling gates」必須用 `remove_dead_logic` 讀 `dead_logic.removed_gate_count`；用 `remove_redundant_logic` 得到的是所有階段的總縮減量，那不是題目要的數字。

**`remove_redundant_logic` 只處理組合邏輯**，不會合併或移除任何 flip-flop。register 相關的要求必須使用專用 mode：合併重複 register 用 `merge_duplicate_dffs`，移除 Q 無人使用的 register 用 `remove_dead_logic --include-sequential`。

### Dead logic 的 register 選項

`remove_dead_logic` 預設**不移除 register**。prompt 出現 flip-flop、register、registers whose Q output is not used、registers that do not contribute to any primary output 等語意時，加上 `--include-sequential`；此時 Q 沒有任何 load 且非 primary output 的 DFF 會連同它的 fanin cone 一併移除。prompt 只說 gates、logic 而未提及 register 時不要加這個旗標，否則會移除題目沒有要求移除的 flip-flop。

### Inverter 與 buffer 清理

back-to-back inverter、NOT followed by NOT、collapse into a wire 等語意使用 `collapse_double_inverter`。它不清除既有的 BUF；prompt 若同時要求清 BUF 才需要另外呼叫 `cleanup_buffers`。

### Duplicate merge

prompt 說 structural duplicates、same type and same inputs 時使用 `merge_structurally_equivalent_gates`；說 functionally equivalent、compute the same Boolean function 時使用 `merge_functionally_equivalent_gates`。後者成本高得多且可能 timeout，不要用它處理明確標明 structural 的 prompt。

prompt 說 duplicate flip-flops、duplicate registers、identical D/CK/RN/SN connections 時使用 `merge_duplicate_dffs`。其餘所有 merge mode 都只處理組合邏輯，不會合併 register。

### Scope 選擇

prompt 指名單一 gate 且要求改寫**它本身**（而非它的 cone）時，scope 使用 `gate <gate_name>`；說「the cone of X」時使用 `gate_fanin` 或 `net_fanin`。

### Buffer insertion

prompt 指名具體 net（例如 "insert buffers on the reset signal n1"）時使用 `insert_buffers_for_net <net> <max_fanout>`，即使該 net 的角色是 clock 或 reset。`insert_buffers_for_dff_control` 只用於 prompt 要求處理**所有** clock 或 reset 網路而未指名特定 net 的情況。

`insert_buffers_for_fanout` 使用 cascaded buffer 結構，在滿足 fanout 限制的前提下 BUF 數量已是最小值。prompt 若同時要求 fanout 限制與「the cost function is the total gate count; smaller is better」，直接使用本 command 即可，不需要改用 `opt_apply`，也不需要額外的最佳化步驟；回答時可說明 cascaded 插入已使 BUF 數最小化。

此 mode 的 constraint universe 是 active non-constant nets，包含 PI、gate output 與 DFF.Q。`1'b0` / `1'b1` 是 literal source，不是 gate-driven signal，不會為它建立 buffer tree，也不會使 `fanout_change.meets_constraint` 變成 false。若 prompt 單純詢問 constant literal 的 load 數，仍使用 connectivity query。

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

edit_apply merge_duplicate_dffs
```

`simple_mode` 的 required arguments 見下方 mode tables。`scope_name` 對 `whole` 省略，其他 scope 必填。`convert_basis`/`replace_type` 的 `-allow` 必填且可接受空白或逗號分隔；`-ban` 只適用 `convert_basis`。

| 輸入 | Required when | Default/規則 |
|---|---|---|
| old/new name | `rename_gate`, `rename_net` | 兩者皆必填，不自行改寫名稱 |
| net | net-specific buffer/remove modes | 必填，bus bit 保留完整 `[]` 名稱 |
| max fanout | fanout buffer modes | 必填正整數，直接使用 prompt limit |
| gate type | typed buffer/simplification/mapping | 使用 public primitive type token |
| constant value | `simplify_constants` | 未指定時才使用 `any`；明確 0/1 不得放寬 |
| input count | `simplify_constants --inputs N` | 只有 prompt 明確指定 N-input 時使用 |
| scope/scope name | functional merge/mapping | `whole` 不需 name；其他 scope 必填 name |
| `--include-sequential` | `remove_dead_logic` | 只有 prompt 明確提到 register/flip-flop 時加上 |
| time limit | functional merge, `remove_redundant_logic` | 題目 budget；未指定時不自行縮短 |

## 4. Rename/Cleanup Modes

| Mode | 參數 | 用途 |
|---|---|---|
| `rename_gate` | `<old> <new>` | rename gate instance |
| `rename_net` | `<old> <new>` | rename net/wire |
| `cleanup_buffers` | 無 | bypass 可安全移除的 BUF |
| `collapse_double_inverter` | 無 | collapse internal back-to-back NOT；不清除既有 BUF |
| `simplify_constants` | `[gate_type\|all] [0\|1\|any] [--inputs N]` | typed constant propagation，跑到 fixpoint |
| `simplify_same_input` | 無 | same-input 與 complementary-input Boolean simplification，單輪 |
| `remove_dead_logic` | `[--include-sequential]` | 移除所有無法從 PO / DFF input endpoint 反向到達的 gates 與 internal nets。加旗標時連 Q 無人使用的 DFF 一併移除。`trim_dead_logic`、`remove_dangling_logic` 是 alias |
| `remove_unused_nets` | 無 | 移除 unused internal nets |
| `remove_net_if_unused` | `<net>` | 只嘗試移除指定 unused net |
| `local_simplification_fixpoint` | 無 | 反覆執行 constant propagation、same-input 化簡、buffer/inverter 清理與 dead logic 直到收斂 |
| `safe_cleanup_fixpoint` | 無 | `local_simplification_fixpoint` 再加上 structural duplicate merge，同樣到收斂 |
| `remove_redundant_logic` | `[--time-limit seconds]` | 完整的組合邏輯縮減 pipeline：結構化簡 → SAT-proven 常數傳播 → SAT-proven functional merge → 單路徑 untestable stuck-at → 收尾清理。縮減類的預設選擇；不處理 register |
| `merge_structurally_equivalent_gates` | 無 | 只合併組合邏輯的 structural duplicates |
| `merge_functionally_equivalent_gates` | `<scope> [scope_name] [--gate-type type] [--patterns N] [--time-limit seconds]` | SAT-proven functional duplicate merge |
| `merge_duplicate_dffs` | 無 | 合併 D/CK/RN/SN 四個 pin 全部接到相同 net 的 flip-flop |

`merge_equivalent_gates` 不是 public command；它是名稱容易誤導的 legacy structural alias。functional merge 固定使用 `merge_functionally_equivalent_gates`。

## 5. Buffer Modes

| Mode | 參數 | 用途 |
|---|---|---|
| `insert_buffers_for_fanout` | `<max_fanout>` | 全設計 fanout buffering，涵蓋 gate output、DFF Q 與 primary input net；cascaded 結構，BUF 數為最小 |
| `insert_buffers_for_net` | `<net> <max_fanout>` | 指定 net fanout buffering。prompt 指名具體 net 時一律使用本 mode，不論該 net 的角色 |
| `insert_buffers_for_dff_control` | `<max_fanout> -clock\|-reset` | 全設計 DFF clock/reset（含 SN）control buffering。**只在 prompt 未指名特定 net 時使用**；指名 net 請改用 `insert_buffers_for_net` |
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
gate <gate>
```

`convert_basis` 確保 scope 最終只使用允許且未被禁止的 combinational gate types。`replace_type` 只替換 scope 中指定 `target_type`。

prompt 說 "the cone of X"、"the logic cone of output X" 而未指明方向時，使用 `net_fanin X`。scope name 可以是 internal net、primary output net 或 bus bit（保留完整 `[]` 名稱）。

`-allow` 只給單一 universal gate（例如 `-allow NOR`）是合法的；工具會自行用該 gate 合成所需的 inverter。

`gate <gate>` 只涵蓋指名的那一顆 gate 本身，不含它的 fanin 或 fanout cone。prompt 說「replace gate g123」、「convert only the gate driving n45」時使用本 scope；說「the cone of g123」時使用 `gate_fanin`。DFF 不接受此 scope。

`merge_functionally_equivalent_gates` 與 `opt_apply` 都不支援 `gate` scope —— 前者合併至少需要兩顆候選 gate，後者的 depth 成本函數在單一 gate 上沒有意義。

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
| `validation.structure_baseline_valid` | 載入的設計本身是否通過結構驗證 |
| `validation.structure_regressed` | 這次 edit 是否讓結構問題變多 |
| `validation.equivalence_checked` | 是否取得 equivalence certificate |
| `validation.functionally_equivalent` | certificate 是否證明等價 |
| `validation.equivalence_method` | certificate 的取得方式，見下方對照表 |
| `mapping_delta` | mapping removed/added/final type counts |
| `constant_simplification` | constant pass candidate/simplified/eliminated count |
| `cleanup_fixpoint` | fixpoint 的總改動數、輪數與是否超時 |
| `dead_logic` | 移除的 gate/net/DFF 數與是否 timeout |
| `redundancy_removal` | 完整 pipeline 的總縮減量、已證明的 redundant pin 數、完整性 |
| `structural_merge` | 合併的 gate 數與實際發生合併的結構相同組數 |
| `dff_merge` | 合併的 register 數、組數與被跳過的原因計數 |
| `functional_merge` | search/class/SAT 統計、apply timeout、merged/skipped gates 與逐筆 merge records；legacy whole-design 欄位維持 false |
| `fanout_change` | before/after max fanout 與 constraint result |
| `changed_gate_names`, `changed_net_names` | 受影響物件 |

`status:no_change` 是成功但沒有修改機會。`ok:true` 不代表 `report_changed:true`。若 envelope `complete:false`，不能宣稱已取得完整 equivalence certificate。

### `equivalence_method` 的意義

| 值 | 意義 |
|---|---|
| `StructuralIdentity` | 設計未被修改，或只有命名／結構相同物件的合併 |
| `LocalRewriteRule` | 改動來自單一 Boolean identity（例如 `NOT(NOT(x)) = x`） |
| `CertifiedRewrite` | 改動來自可證明保持功能的多步 pipeline，設計確實被修改 |
| `WholeDesignSat` | 已執行 whole-design SAT 證明 |

前三者都**不代表**跑過 whole-design SAT。需要 SAT certificate 請另外呼叫 `equiv_query`。

### 結構驗證與來源檔案的既有問題

部分設計的來源 Verilog 本身就有結構問題，最常見的是同一條 net 被兩個 gate 驅動。工具會原樣保留這些連接，讓設計能寫回與來源一致的內容，但結構驗證會把它們報成違規。因此 edit 的成功判定看的是「有沒有變糟」而不是「是否乾淨」：

| 情況 | 判讀 |
|---|---|
| `structure_baseline_valid:false` | 載入的設計本身就有結構問題，不是這次 edit 造成的 |
| `structure_regressed:false` | 這次 edit 沒有讓情況變糟，結果有效 |
| `structure_regressed:true` | 這次 edit 引入了新的結構問題，已 rollback |

判斷 edit 是否成功一律看 `report_success`，**不要**看 `structure_valid`。`structure_valid:false` 搭配 `structure_baseline_valid:false` 且 `structure_regressed:false` 是正常且可接受的狀態。

`read` 的回應含 `structure_violation_count` 與 `multi_driver_net_count`；兩者非 0 時，回答中可以說明來源設計有結構問題，但不影響本次操作的有效性。

### 數量問題的欄位分派

題目問「移除／消除／合併／新增了多少 gates」時，依所使用的 mode 讀對應的專屬欄位，**不要一律使用 `diff.active_gate_count_delta`**：

| Mode | 正式答案欄位 |
|---|---|
| `remove_dead_logic` | `dead_logic.removed_gate_count`（net 數用 `removed_net_count`；register 數用 `removed_dff_count`） |
| `remove_redundant_logic` | `redundancy_removal.removed_gate_count` |
| `simplify_constants` | `constant_simplification.eliminated_target_gate_count` |
| `merge_structurally_equivalent_gates` | `structural_merge.merged_gate_count` |
| `merge_duplicate_dffs` | `dff_merge.merged_dff_count` |
| `merge_functionally_equivalent_gates` | `functional_merge.merged_gate_count` |
| `local_simplification_fixpoint` / `safe_cleanup_fixpoint` | `cleanup_fixpoint.changed_count` |
| `convert_basis` / `replace_type` | `mapping_delta.removed_count_by_type` / `added_count_by_type` |
| buffer insertion 各 mode | `diff.gate_type_count_delta[BUF]` |
| 以上皆非（例如 `collapse_double_inverter`） | `diff.active_gate_count_delta` |

**`remove_redundant_logic` 的計數是所有階段的總和。** 若題目問的是某一個特定種類的數量（例如「多少 dangling gates」、「多少 structural duplicates」），必須改用該種類的專用 mode，才能得到只屬於那一類的計數。

### 各欄位的判讀細節

**`constant_simplification`**：`simplified_count` 是所有被化簡的候選數；`eliminated_target_gate_count` 只算「化簡後不再是該 target type」的數量，才是「多少顆 X gate 被消除」的正確答案。常數傳播會跑到 fixpoint，因此結果包含被連鎖化簡的下游 gate。

**`dead_logic`**：`timed_out:true` 代表 design 完全未改動，必須回報 timeout 而非數量。`removed_dff_count` 只有在使用 `--include-sequential` 時才可能非 0。

**`redundancy_removal`**：`removed_gate_count` 是整個 pipeline 的總 gate 縮減量。`complete:false` 或 envelope 為 `status:timeout` 時，回答必須說明這是在時間預算內達成的縮減，不代表設計已無可縮減空間。`proven_redundant_pin_count` 是已用 SAT 證明為 untestable 的 stuck-at fault 數，供需要區分「一般化簡」與「真正 ATPG redundancy」的題目使用；一般題目只需要 `removed_gate_count`。

**`structural_merge`、`dff_merge` 與 `functional_merge`** 是三個不同 mode 的欄位，數字不可混用。`equivalence_class_count` 是實際發生合併的組數，合併掉的物件數通常大於組數。`dff_merge.skipped_po_count` 代表有重複的 register 因為兩顆 Q 都是 primary output 而未合併，這是保護 port 對應的正常行為。

**`cleanup_fixpoint`**：`timed_out:true` 代表 fixpoint 未收斂就因時間限制退出，已套用的改動仍然有效且功能等價，但設計中可能還有未清除的機會。此時不可回答「已完全化簡」或「已清乾淨」，只能說明在時間預算內完成的部分。

**`fanout_change`**：`insert_buffers_for_net` 的 scope 是指定 net/bus 與本次建立的 buffer tree；全設計 active non-constant nets 是否都低於限制只能由 `insert_buffers_for_fanout` 判斷。Constant literal 不屬於可 buffer constraint universe。若 `insert_buffers_for_net` 的目標 net 不存在，回報 `report_success:false` 與明確訊息，不會產生 `fanout_change`；此時不可回答已完成 buffer 插入。

**`mapping_delta`**：`replace_type` 若未能清除 scope 內全部目標 gate，回報失敗並 rollback，不會出現「部分完成」的成功狀態。

二輸入 XOR 的 NAND-only replacement 使用固定的標準 4-NAND DAG。prompt 明確寫
`equivalent 4-NAND circuit` 時，呼叫 `edit_apply replace_type <scope> XOR -allow NAND`；
`removed_count_by_type[XOR]` 是替換組數，`added_count_by_type[NAND]` 應為其四倍。
這項保證只適用此標準 XOR 規則，不可推廣成任意 gate 的 `exact K` 合成能力。

## 8. Prompt Examples

### 8.1 縮減類：選對層級

四個 mode 是包含關係，選錯會讓答案偏大或偏小。

```text
Prompt: Remove all dangling gates that do not contribute to any primary output.
        How many dangling gates were removed?
Command: edit_apply remove_dead_logic
Read: dead_logic.removed_gate_count
Note: 問「多少 dangling gates」必須用這個 mode。用 remove_redundant_logic
      得到的是所有化簡階段的總和，不是題目要的數字。
```

```text
Prompt: Trim unused wires and gates.
Command: edit_apply remove_dead_logic
Read: dead_logic.removed_gate_count, dead_logic.removed_net_count
Note: 「wires and gates」仍是同一個 mode；net 數另外從 removed_net_count 讀。
```

```text
Prompt: Remove flip-flops whose Q output is not used anywhere.
Command: edit_apply remove_dead_logic --include-sequential
Read: dead_logic.removed_dff_count, dead_logic.removed_gate_count
Note: prompt 明確提到 flip-flop 才加旗標。remove_redundant_logic 不處理
      register，這類 prompt 不能用它。
```

```text
Prompt: Simplify the logic as much as possible without changing functionality.
Command: edit_apply local_simplification_fixpoint
Read: cleanup_fixpoint.changed_count, cleanup_fixpoint.timed_out
Note: 「simplify the logic」對應 local simplification。若 prompt 進一步要求
      「minimize the gate count」，改用 remove_redundant_logic。
```

```text
Prompt: Clean up the netlist: remove anything that is not needed.
Command: edit_apply safe_cleanup_fixpoint
Read: cleanup_fixpoint.changed_count
Note: 「clean up」比 simplify 更廣，包含 structural duplicate merge。
```

```text
Prompt: Are there any redundant gates in this design that can be removed without
        changing functionality? Remove them if found. How many were removed?
Command: edit_apply remove_redundant_logic
Read: redundancy_removal.removed_gate_count, redundancy_removal.complete
Note: 「redundant」對應最完整的 pipeline。
```

```text
Prompt: Reduce the total gate count of this design as much as you can while
        keeping it functionally identical.
Command: edit_apply remove_redundant_logic
Read: redundancy_removal.removed_gate_count
Note: 沒有指定縮減種類、只要求「越小越好」時，一律用這個 mode。
```

```text
Prompt: Remove internal wires that are no longer connected to anything.
Command: edit_apply remove_unused_nets
Read: diff.active_net_count_delta
Note: 只回收 net、不移除 gate。prompt 同時提到 gate 時請改用 remove_dead_logic。
```

### 8.2 縮減類：no_change 時的處理

```text
Prompt: Remove any redundant logic from this design.
Command: edit_apply merge_structurally_equivalent_gates
Result: status: no_change
Then:   edit_apply remove_redundant_logic
Read:   redundancy_removal.removed_gate_count
Note: 專用 mode 回報 no_change 而 prompt 要求必須有所縮減時，
      改用涵蓋最廣的 remove_redundant_logic 再試一次。
      不要重複呼叫同一個已回報 no_change 的 mode。
```

### 8.3 Buffer / inverter 清理的分辨

```text
Prompt: Find all back-to-back inverter pairs and collapse them into a wire.
Command: edit_apply collapse_double_inverter
Read: report_changed, diff.active_gate_count_delta
Note: 只處理 NOT-NOT。既有的 BUF 不受影響。
```

```text
Prompt: Remove buffers that can be safely bypassed.
Command: edit_apply cleanup_buffers
Read: diff.gate_type_count_delta[BUF]
Note: 只處理 BUF。與 collapse_double_inverter 是兩件事，
      prompt 同時要求兩者時分兩次呼叫。
```

### 8.4 Constant propagation 與 same-input 化簡

```text
Prompt: How many 2-input OR gates were eliminated by constant-1 propagation?
Command: edit_apply simplify_constants OR 1 --inputs 2
Read: constant_simplification.eliminated_target_gate_count
Note: 有明確的 gate type 與常數值時一律傳入，不要放寬成 all / any。
```

```text
Prompt: Report any NAND gates with constant inputs, then simplify them.
Command: edit_apply simplify_constants NAND any
Read: constant_simplification.eliminated_target_gate_count
Note: prompt 沒指定 0 或 1 時才用 any。
```

```text
Prompt: Simplify gates whose two inputs are the same signal.
Command: edit_apply simplify_same_input
Read: diff.active_gate_count_delta
Note: 同時涵蓋 complementary input（x 與 NOT x）。它只做一輪，
      不傳播產生的常數；要完整傳播請用 local_simplification_fixpoint。
```

### 8.5 Duplicate merge 三選一

三個 mode 涵蓋不同對象，數字來自不同欄位，不可互相替代。

```text
Prompt: Try to merge any pairs of gates that compute the same Boolean function
        on the same inputs (structural duplicates). How many gates were merged?
Command: edit_apply merge_structurally_equivalent_gates
Read: structural_merge.merged_gate_count
Note: prompt 寫明 structural 時用這個。它只看 gate type 與 input net，
      不做 SAT，速度快。
```

```text
Prompt: Find and merge all gate pairs that are functionally equivalent.
Command: edit_apply merge_functionally_equivalent_gates whole
Read: functional_merge.merged_gate_count, validation.equivalence_method
Require: report_success=true, rolled_back=false, validation.equivalence_method=CertifiedRewrite
Note: 「functionally equivalent」才用這個。它對候選 gate 跑 SAT search，成本高且可能 timeout；不執行 final whole-design SAT。
```

```text
Prompt: Find and merge duplicate flip-flops that have identical D, CK, RN, and SN
        connections. How many registers were merged?
Command: edit_apply merge_duplicate_dffs
Read: dff_merge.merged_dff_count
Note: 只有這個 mode 會合併 register；其餘所有 merge mode 與
      remove_redundant_logic 都只處理組合邏輯。
```

```text
Prompt: Merge duplicate registers, then simplify the resulting logic.
Command: edit_apply merge_duplicate_dffs
         edit_apply local_simplification_fixpoint
Read: dff_merge.merged_dff_count（第一步）、cleanup_fixpoint.changed_count（第二步）
Note: 合併 register 後下游會出現 AND(q1, q1) 這類 same-input gate，
      需要第二步才會被化簡掉。
```

### 8.6 Basis conversion 與 gate type replacement 的分辨

```text
Prompt: Reconstruct the design using only AND and NOT gates.
Command: edit_apply convert_basis whole -allow AND NOT
Read: mapping_delta, final_gate_count_by_type
Note: 「using only X」是 basis conversion —— 所有不在白名單內的 gate 都要消滅。
```

```text
Prompt: Convert every XNOR gate in this design to an equivalent NOR-only circuit.
Command: edit_apply convert_basis whole -allow NOR
Read: mapping_delta.removed_count_by_type / added_count_by_type
Note: -allow 只給一個 universal gate 是合法的；工具會自行合成 inverter。
```

```text
Prompt: Replace all 2-input OR gates with equivalent logic built only from
        NAND and NOT gates.
Command: edit_apply replace_type whole OR -allow NAND NOT
Read: mapping_delta.removed_count_by_type / added_count_by_type
Note: 「replace all X gates」只針對 X，其他 gate type 保持不變 ——
      這是與 convert_basis 的關鍵差異。
```

```text
Prompt: Replace XOR gates in the fanin cone of n10 using NAND gates.
        How many NAND gates were added?
Command: edit_apply replace_type net_fanin n10 XOR -allow NAND
Read: mapping_delta.added_count_by_type
```

### 8.7 Scope 的分辨

```text
Prompt: Replace gate g123 with an equivalent NAND-only implementation.
Command: edit_apply replace_type gate g123 XOR -allow NAND
Read: mapping_delta.removed_count_by_type / added_count_by_type
Note: 「replace gate g123」只改寫那一顆。
```

```text
Prompt: Restructure the logic cone of g123 using only NAND gates.
Command: edit_apply convert_basis gate_fanin g123 -allow NAND
Read: mapping_delta, final_gate_count_by_type
Note: 「the cone of g123」是整個 fanin cone，不是單一 gate。
```

```text
Prompt: Convert the logic cone of output n8[0] to use only NOR and NOT gates.
Command: edit_apply convert_basis net_fanin n8[0] -allow NOR NOT
Read: mapping_delta, final_gate_count_by_type
Note: 未指明方向的 cone 一律用 net_fanin；bus bit 保留完整的 [] 名稱。
```

### 8.8 Buffer insertion 的分辨

七個 mode 的目標完全不同，選錯會插在錯的位置。

```text
Prompt: Insert buffers wherever needed so that no gate drives more than 4 loads.
Command: edit_apply insert_buffers_for_fanout 4
Read: fanout_change.after_max_fanout, meets_constraint
Note: 全設計的 fanout 限制。
```

```text
Prompt: Insert buffers so that no signal drives more than 16 loads.
        The cost function is the total gate count of the final design; smaller is better.
Command: edit_apply insert_buffers_for_fanout 16
Read: fanout_change.after_max_fanout, meets_constraint, diff.gate_type_count_delta[BUF]
Note: cascaded 插入的 BUF 數已是最小，不需要再呼叫 opt_apply。
```

```text
Prompt: Try to insert buffers on the reset signal n1 to reduce its fanout to
        at most 4 loads per driver.
Command: edit_apply insert_buffers_for_net n1 4
Read: fanout_change.before_max_fanout / after_max_fanout / meets_constraint
Note: prompt 指名了 net，即使它的角色是 reset 也用這個 mode，
      不要用 insert_buffers_for_dff_control。
```

```text
Prompt: Buffer all clock networks so that no clock net drives more than 16 flip-flops.
Command: edit_apply insert_buffers_for_dff_control 16 -clock
Read: fanout_change, diff.gate_type_count_delta[BUF]
Note: 「all clock networks」未指名 net，才用這個 mode。
```

```text
Prompt: Please insert a BUF gate on signal n2 so that each load of n2 is driven
        through a dedicated buffer. How many BUF gates were added?
Command: edit_apply insert_buffers_on_each_load n2
Read: diff.gate_type_count_delta[BUF]
Note: 「each load ... dedicated buffer」是每個 load 一顆，
      與 fanout 限制無關，不要用 insert_buffers_for_net。
```

```text
Prompt: Insert a buffer at the driver side of net n5.
Command: edit_apply insert_buffer_at_driver n5
Read: diff.gate_type_count_delta[BUF]
Note: 只插一顆，位置在 driver 與 net 之間。
```

```text
Prompt: Insert a buffer between net n5 and gate g20 only.
Command: edit_apply insert_buffer_before_gate n5 g20
Read: diff.gate_type_count_delta[BUF]
Note: 只阻斷指定的那一條連線，n5 的其他 load 不受影響。
```

### 8.9 Rename

```text
Prompt: Change the identifier of gate g0 to renamed_gate and update all references.
Command: edit_apply rename_gate g0 renamed_gate
Read: report_changed, changed_gate_names
Note: 名稱原樣傳入，不自行改寫大小寫或加前綴。
```

```text
Prompt: Update the name of signal n7431 to renamed_wire throughout the netlist.
Command: edit_apply rename_net n7431 renamed_wire
Read: report_changed, changed_net_names
```

## 9. 組合流程與限制

修改後回答成果可再呼叫 `report_query last_edit`。所有 public edit（包含 `merge_functionally_equivalent_gates`）都不在 transaction 內執行 final whole-design SAT。只有 prompt 明確要求獨立 baseline comparison 時才呼叫 `equiv_query previous_edit` 或 `equiv_query original`。

- 縮減類 mode 是包含關係：`remove_dead_logic` ⊂ `local_simplification_fixpoint` ⊂ `safe_cleanup_fixpoint` ⊂ `remove_redundant_logic`。呼叫外層之後不需要再呼叫內層，那不會有額外效果。
- `remove_redundant_logic` 依剩餘時間分階段執行：便宜的結構化簡先跑滿，SAT 相關階段（常數傳播、functional merge、untestable stuck-at）依序用剩餘預算。時間不足時會跳過昂貴階段並回報 `complete:false`，但已完成的縮減仍然有效。
- `remove_redundant_logic` 只處理組合邏輯，不會合併或移除任何 flip-flop。合併重複 register 請用 `merge_duplicate_dffs`；移除 Q 無人使用的 register 請用 `remove_dead_logic --include-sequential`。
- structural duplicate merge、DFF duplicate merge 與 functional duplicate merge 是三個不同 mode；functional merge 成本較高且可能 timeout。
- structural merge 與 DFF merge 都不會合併兩顆輸出皆為 primary output 的物件，因為那會讓兩個 port 指向同一條 net。
- `merge_duplicate_dffs` 合併後，原本讀取不同 Q 的下游 gate 會變成 same-input（例如 `AND(q1, q1)`）。若題目要求盡量化簡，可接著呼叫 `simplify_same_input` 或 `local_simplification_fixpoint`。
- `simplify_same_input` 只做一輪 local 化簡，不傳播產生的常數。需要完整傳播請使用 `local_simplification_fixpoint`。
- fixed basis conversion 不會自動找到 minimum depth/best cost implementation。
- depth/cost optimization 的公開契約見 [`OPTIMIZATION_TOOL.md`](OPTIMIZATION_TOOL.md)。
- public parser 不提供可能任意改變功能的低階 rewiring commands。
- 只有 `status:ok|no_change`、`complete:true`、`report_success:true`，且題目要求等價時 `validation.functionally_equivalent:true`，才能回答 edit 已成功完成。timeout/error/partial 或 rollback 不得沿用前一次 edit 的 delta。
- `validation.equivalence_method` 只有 `WholeDesignSat` 代表跑過 whole-design SAT；其餘三種都是建構上可證的 certificate。需要 SAT certificate 請另外呼叫 `equiv_query`。
- 若 prompt 要求保留前題 constraint，LLM 必須在新的 `edit_apply` command 重新傳入 basis/fanout 等條件，或在 write 前主動用對應 query 驗證；session 不會自動保存自然語言 constraint。
- `remove_redundant_logic` 刻意排除 DFF 的 CK/RN/SN fanin cone。這些路徑即使 Boolean 上等價，改寫仍可能引入 glitch，不在本 pass 的處理範圍。
- redundancy 候選之間並非彼此獨立：移除其中一個之後，另一個可能變成必要的。本 pass 會一次套用所有 fanout cone 互不相交的已證明 redundancy；cone 重疊的候選留待下次呼叫重新證明。若題目要求盡可能移除，可重複呼叫直到 `proven_redundant_pin_count` 為 0。
- 來源檔案若有 multi-driver net（同一條 net 被多個 gate 驅動），工具會保留所有 driver 以確保寫回內容與來源一致。這類設計的 `structure_valid` 會是 `false`，但只要 `structure_regressed:false`，edit 仍然有效。
- `remove_dead_logic` 不會移除「輸出 net 由其他 gate 驅動」的 gate。這類 gate 在反向可達性走訪中永遠到不了，但移除它們會改變寫回的內容，因此保守保留。
