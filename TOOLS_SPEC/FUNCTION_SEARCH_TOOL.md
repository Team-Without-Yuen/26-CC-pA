# Function Search Tool

## 1. 責任

`func_search` 負責候選名稱未知、必須掃描 current design 才能找到 witness 的 Boolean search：

```text
1. 找 existing signal pair，使 NAND(a,b) 與 target 等價
2. 找 output function 相同的 active combinational gate pairs 或 groups
```

本工具只搜尋並回報 SAT-proven 結果，不修改 netlist。已知兩個 net 名稱時改用
`func_query equivalence`；需要實際合併時使用 `edit_apply merge_functionally_equivalent_gates`。

## 2. 選擇條件

Prompt 出現下列語意時使用本工具：

```text
find any pair / does there exist a pair
find two existing signals whose NAND equals a target
list all functionally equivalent gate pairs or groups
find gates implementing the same function in a cone
```

指定 `NAND(a,b)==target` 時選 `nand_pair`；要求任意 gate outputs 功能相同時選
`equivalent_pairs`。Prompt 只問是否存在時維持預設 FindAny；出現 `all`、`every`、`list`
等完整列舉語意時加入 `--all`。

## 3. Command Grammar

```text
func_search nand_pair <target_net>
    [--find-any | --all]
    [--max-results N]
    [--patterns 1..4096]
    [--time-limit seconds]
    [--allow-same]
    [--include-boundary-signals]

func_search equivalent_pairs <scope> [scope_name]
    [--find-any | --all]
    [--gate-type type]
    [--max-results N]
    [--patterns 1..4096]
    [--time-limit seconds]
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
| `nand_pair` | target net | 找 `NAND(a,b)==target` 的 signal pair | `found`, `match_count`, match 或 `output_file` |
| `equivalent_pairs` | scope、選用 scope name | 找功能等價的 gate pairs/classes | class/pair counts、match 或 `output_file` |

Options：

| Option | 預設 | 使用規則 |
|---|---:|---|
| `--find-any` | 是 | 找到第一組 SAT-proven witness 即停止 |
| `--all` | 否 | 完整搜尋，並自動把全部 records 寫到唯一檔案 |
| `--max-results N` | 不限制 | 只有 prompt 明確要求前 N 筆時才使用 |
| `--patterns N` | 256 | simulation prefilter patterns；通常不需指定 |
| `--time-limit seconds` | 290 | simulation、enumeration、SAT 共用 backend 預算；prompt 未指定時不要自行加入較短值 |
| `--allow-same` | 否 | 僅 NAND mode；允許 `(a,a)` |
| `--include-boundary-signals` | 否 | 僅 NAND mode；納入 PI/PO/boundary candidates |
| `--gate-type type` | ANY | 僅 equivalent-pair mode；限制兩成員的 gate type |

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

`nand_pair` 的 match 讀取 `net_a`、`net_b`、`proof_method: SAT_UNSAT_MITER` 與
`solver_status: UNSAT`。`equivalent_pairs` 另讀 `equivalence_class_count`、
`equivalent_pair_count`、scope 與 gate-type filter。

對 `--all` 的自然語言回答應提供總數與 `output_file` 路徑，不要把 artifact 全文重新貼入回答。

## 6. Prompt Examples

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

## 7. 組合流程

若 prompt 要求「找出並合併所有 functionally equivalent gates」，不要先把搜尋結果交給 LLM
逐筆 rewiring，直接使用：

```text
edit_apply merge_functionally_equivalent_gates <scope> [scope_name]
```

該 edit command 內部使用緊湊 equivalence classes，並執行 mandatory whole-design SAT 與失敗
rollback。修改後若 prompt 再要求證明，可使用相應的 equivalence query。

## 8. 限制

- simulation 只做候選過濾；公開 matches 均需 SAT UNSAT proof。
- DFF 不可作為 equivalent-pair candidate，DFF.Q 只作為 function boundary。
- time limit、solver unknown 或 unsupported logic 仍可能使 FindAll 不完整。
- equivalent output function 不等於 observability redundancy；本工具不刪除任意 redundant gate。
- Boolean/AIG 共用 index 與更一般化 functional decomposition 屬後期重構，本階段沿用 named-netlist simulation + SAT。
