# Function Search Tool

## 1. 責任

`func_search` 負責候選名稱未知、必須掃描 current design 才能找到 witness 的 Boolean search：

```text
1. 找 existing signal(s)，使 BUF/NOT/AND/NAND/OR/NOR/XOR/XNOR 與 target 等價
2. 找 output function 相同的 active combinational gate pairs 或 groups
3. 在名稱未知時搜尋 functionally constant 0/1 signals
4. 找 `f(a)=!f(b)` 的 unknown-candidate signal 或 combinational gate-output pairs
```

本工具只搜尋並回報 AIG/SAT-proven 結果，不修改 netlist。已知兩個 net 名稱時改用
`func_query equivalence`；需要實際合併時使用 `edit_apply merge_functionally_equivalent_gates`。

## 2. 選擇條件

當 Boolean property 的一個或多個 operand identities 未知，答案必須在 current design 的
candidate domain 中搜尋 witness、完整集合或數量時使用本工具。選 mode 前依序確定：

1. target relation：指定 gate function 組合後等價於 target、candidate outputs 彼此等價、
   derived constant，或兩 candidates 功能互補。
2. candidate domain：named signals，或 active combinational gate outputs。
3. structural scope：whole design 或指定 fanin/fanout cone。
4. quantifier：存在任一 witness 使用 FindAny；完整 identities/count 使用 `--all`。
5. filters：gate type、boundary inclusion、same-operand policy，只在 prompt 語意明確要求時加入。

指定 BUF/NOT/AND/NAND/OR/NOR/XOR/XNOR function 與已知 target 時選 `pattern`；`nand_pair`
只是 NAND pattern 的相容入口，不代表 NAND 題型有不同責任。未知 gate outputs 功能相同時選
`equivalent_pairs`；未知 derived constant signals 使用 `constant_signals`；未知互補 candidates
使用 `complementary_pairs`，並由題目要求的 object 決定 `signals` 或 `gates` domain。

若所有 operand identities 都已指定，且 `func_query` 已公開對應 property mode，使用
`func_query`。例如已知 net 是否 constant 屬 `func_query`，而「全設計有哪些 constant nets」
才屬搜尋。已知 pair 的 complement 判定或已知 operands 的任意 gate-relation proof 目前沒有
通用 public checker；不能假造 `func_query` mode，也不能用 unknown-candidate search 冒充精確
pair checker。不能只靠 `find`、`all`、`pair` 等表面字詞判斷 owner；關鍵是 candidate identity、
property 與 public mode 是否一致。

## 3. Command Grammar

```text
func_search pattern <BUF|NOT|AND|NAND|OR|NOR|XOR|XNOR> <target_net>
    [--find-any | --all]
    [--scope <scope> [scope_name]]
    [--max-results N]
    [--allow-same]
    [--include-boundary-signals]

func_search nand_pair <target_net>
    [--find-any | --all]
    [--max-results N]
    [--allow-same]
    [--include-boundary-signals]

func_search equivalent_pairs <scope> [scope_name]
    [--find-any | --all]
    [--gate-type type]
    [--max-results N]

func_search constant_signals <zero|one|either>
    [--find-any | --all]
    [--scope <scope> [scope_name]]
    [--include-boundary-signals]
    [--max-results N]

func_search complementary_pairs <signals|gates> <scope> [scope_name]
    [--find-any | --all]
    [--include-boundary-signals]
    [--gate-type type]
    [--max-results N]
```

Scope：

```text
whole
net_fanin <net>
net_fanout <net>
gate_fanin <gate>
gate_fanout <gate>
```

## 4. Modes

| Mode | 參數 | 用途 | 主要輸出 |
|---|---|---|---|
| `pattern` | gate type、target net | 找指定一元/二元 Boolean pattern 的 operands | `pattern_type`, `operand_arity`, match 或 `output_file` |
| `nand_pair` | target net | 找 `NAND(a,b)==target` 的 signal pair | `found`, `match_count`, match 或 `output_file` |
| `equivalent_pairs` | scope、選用 scope name | 找功能等價的 gate pairs/classes | class/pair counts、match 或 `output_file` |
| `constant_signals` | zero/one/either | 搜尋 functionally constant signals | 0/1 counts、proven record 或 `output_file` |
| `complementary_pairs` | signals/gates、scope | 搜尋 Boolean function 互補的 candidates | phase class/pair counts、match 或 `output_file` |

Options：

| Option | 預設 | 使用規則 |
|---|---:|---|
| `--find-any` | 是 | 找到第一組 SAT-proven witness 即停止 |
| `--all` | 否 | 完整搜尋，並自動把全部 records 寫到唯一檔案 |
| `--max-results N` | 不限制 | 只有 prompt 明確要求前 N 筆時才使用 |
| `--allow-same` | 否 | 二元 pattern/NAND mode；允許 `(a,a)`；BUF/NOT 不可使用 |
| `--include-boundary-signals` | 否 | pattern/NAND/constant 及 complementary signals；額外納入 PI 與有 driver 的 PO port nets。internal DFF.Q 預設已納入；DFF.Q 若同時為 PO，需使用此 option |
| `--scope` | whole | pattern/NAND/constant mode；限制候選於指定 cone |
| `--gate-type type` | ANY | equivalent-pair 或 complementary gates；限制候選 gate type |

Prompt 沒有指定數量時，不得自行加入 `--max-results`。大型結果由工具寫檔，不需要以截斷
來控制 terminal output。

## 5. 輸出判讀

共同先讀：

```text
status
complete
report_status
found
timed_out
truncated
unsupported
match_count
stored_match_count
stored_constant_signal_count
wrote_matches_to_file
output_file
```

判讀規則：

- `status: ok` 且 `complete: true` 才是完整答案。
- FindAny 找到 witness 時 `all_candidates_examined:false` 仍是完整的存在性答案。
- FindAll 會回 `wrote_matches_to_file:true`，完整 records 位於 `output_file`；terminal 只回摘要。
- `match_count` 是結果數，`stored_match_count` 只是 envelope 內保留的 sample 數。
- `NO_MATCH` 只有在 `complete:true` 時才能回答不存在。
- `timeout`、`partial`、`unsupported` 不可被描述為已證明不存在。
- 明確使用 `--max-results N` 後若 `truncated:true`，只能回答前 N 筆，不可宣稱完整列舉。
- 不要同時輸出 `--all` 與 `--find-any`，也不要重複 option；strict parser 會拒絕這些命令。
- `--max-results` 只在 prompt 明確限制結果數且使用 `--all` 時合法。

`pattern` 先讀 `pattern_type` 與 `operand_arity`；match 讀 `operand_count`、
`operand_1/2`、`proof_method` 與 `solver_status: UNSAT`。二元結果仍保留 `net_a/net_b`
相容欄位。`nand_pair` 的 match 讀取 `net_a`、`net_b`、`proof_method` 與
`solver_status: UNSAT`。FindAny 的 proof method 可為 `AIG_LITERAL_EQUALITY` 或
`AIG_INCREMENTAL_SAT`，FindAll 目前可為 `SAT_UNSAT_MITER`；三者都是已證明的 match，
LLM 不得因 method 不同改變結論。`equivalent_pairs` 另讀 `equivalence_class_count`、
`equivalent_pair_count`、scope 與 gate-type filter。

`constant_signals` 另讀 `constant_filter`、`constant_zero_count`、`constant_one_count`、
`proven_non_constant_signal_count` 與 `inconclusive_signal_count`。FindAny 的
`constant_signals` record 讀 net、driver gate/type、`constant_value`、`proven_constant:true`、
proof method 與 `solver_status:UNSAT`。simulation 全 0/全 1 只用於候選篩選，不是 positive proof。
`candidate_signals_rejected_by_simulation` 是「不符合目前 filter」的數量，不可直接改寫成
non-constant 數量；只有 `proven_non_constant_signal_count` 可作此結論。

`complementary_pairs` 另讀 `candidate_domain`、`complementary_class_count`、
`complementary_pair_count` 與 `inconclusive_candidate_count`。FindAny match 必須有
`proven_complementary:true`。FindAll artifact 的每個 class 使用：

```text
positive_members x negative_members
```

也就是兩側 Cartesian product 的每一 pair 都互補；不可只配相同 index。positive/negative 只是
相對 phase，不是 constant 1/0。simulation complementary signature 只負責分桶，正式結果仍由
AIG literal phase 或 incremental SAT 證明。
只有 prompt 明確要求前 N 筆時才加 `--max-results N`；此時 artifact 會改成前 N 筆
`literal_pairs_explicit_limit`，不可把它描述成完整列舉。

對 `--all` 的自然語言回答應提供總數與 `output_file` 路徑，不要把 artifact 全文重新貼入回答。
`output_file` 使用 self-contained 的 `FUNCTION_SEARCH_ARTIFACT_V2`，供 evaluator/使用者取得 query
type、target/pattern、scope、filter、全部 records 與 completion footer。LLM 不讀該檔案；判斷只用
envelope。Artifact 也不是下一個 command 可引用的 session handle；需要批次修改時使用相應高階
Edit command。

## 6. Prompt Examples

```text
Prompt: Find an existing signal whose inversion is functionally equivalent to n5.
Command: func_search pattern NOT n5
Read: found、operand_1、proof_method、solver_status
```

```text
Prompt: Find two existing signals whose OR is functionally equivalent to n15.
Command: func_search pattern OR n15
Read: found、operand_1、operand_2、proof_method、solver_status
```

```text
Prompt: List all XOR operand pairs in the fanin cone of y.
Command: func_search pattern XOR y --all --scope net_fanin y
Read: complete、match_count、output_file
```

```text
Prompt: Does there exist any pair of internal signals (a, b) such that NAND(a, b) is equivalent to n25?
Command: func_search nand_pair n25
Read: found、第一筆 match、proof_method、solver_status
```

```text
Prompt: List all signal pairs whose NAND is equivalent to n25.
Command: func_search nand_pair n25 --all
Read: complete、match_count、output_file
```

```text
Prompt: List all functionally equivalent gate pairs in the design.
Command: func_search equivalent_pairs whole --all
Read: complete、equivalence_class_count、equivalent_pair_count、output_file
```

```text
Prompt: Find equivalent AND gate pairs in the fanin cone of n10.
Command: func_search equivalent_pairs net_fanin n10 --all --gate-type AND
Read: scope、gate_type_filter、match_count、output_file
```

```text
Prompt: Does any internal signal remain at logic 0 for every input assignment?
Command: func_search constant_signals zero
Read: found、第一筆 constant signal、constant_value、proof_method
```

```text
Prompt: List all signals that are functionally constant 0 or 1.
Command: func_search constant_signals either --all
Read: complete、constant_zero_count、constant_one_count、match_count、output_file
```

```text
Prompt: Find any two internal signals that compute complementary Boolean functions.
Command: func_search complementary_pairs signals whole
Read: found、第一筆 match、proven_complementary、proof_method
```

```text
Prompt: List all complementary NAND gate-output pairs in the fanin cone of n10.
Command: func_search complementary_pairs gates net_fanin n10 --all --gate-type NAND
Read: complete、complementary_class_count、complementary_pair_count、output_file
```

## 7. 組合流程

若 prompt 要求「找出並合併所有 functionally equivalent gates」，不要先把搜尋結果交給 LLM
逐筆 rewiring，直接使用：

```text
edit_apply merge_functionally_equivalent_gates <scope> [scope_name]
```

該 edit command 內部使用緊湊 equivalence classes、cycle-safe rewrite 與結構驗證，候選 gate 的 SAT 結果作為 `CertifiedRewrite`；不執行 final whole-design SAT。搜尋不完整、apply timeout、cycle safety 或結構驗證失敗時會 rollback。修改後若 prompt 明確要求獨立 baseline comparison，可使用相應的 equivalence query。

## 8. 限制

- simulation 只做候選過濾；公開 matches 均需 SAT UNSAT proof。
- DFF 不可作為 equivalent-pair candidate。internal DFF.Q 是預設合法 Boolean operand，分析在 Q 停止且不跨 cycle；DFF.Q 若同時為 PO，依 PO policy 預設排除，使用 `--include-boundary-signals` 才納入。
- time limit、solver unknown 或 unsupported logic 仍可能使 FindAll 不完整。
- equivalent output function 不等於 observability redundancy；本工具不刪除任意 redundant gate。
- MUX 不使用三元暴力枚舉；需要獨立 decomposition，因此不屬 `pattern` 支援型態。
- AIG/pattern engine 只是高階 API 內部 backend；named netlist 仍是唯一正式設計狀態，LLM 不選擇 backend。
- `constant_signals` 排除 literal constant、removed、unnamed 與 floating/undriven nets；它回答的是 derived Boolean function，不是 structural constant-input gate。
- `complementary_pairs signals` 預設排除 PI/PO 但包含 internal DFF.Q；`gates` 不包含 DFF。兩者都排除 literal constant、removed、unnamed 與 floating/undriven candidates。
- `complete:false` 的 complementary result 只能引用已證明 classes，不得把缺少結果解讀為不存在其他 pairs。
