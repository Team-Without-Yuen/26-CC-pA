# Function Search Tool

## 1. 責任

`func_search` 負責候選名稱未知、必須掃描 current design 才能找到 witness 的 Boolean search。

完整性規則：prompt 要求 all 時不得接受預設 `maxResults` 作為完整答案；應以 candidate count
推導 pair 上界後重試。時間限制依題目指定。目前沒有 pagination/file output，屬待改善架構。
詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

公開支援兩類問題：

```text
1. 找 signal pair，使 NAND(a,b) 與指定 target 等價
2. 找 output function 相同的 active combinational gate pairs / groups
```

已知兩個 net 名稱、只需驗證是否等價時，應使用 `func_query equivalence`。

## 2. Prompt 選擇

使用本 tool 的線索：

```text
find any pair
find two existing signals
does there exist a pair
list functionally equivalent gates
find gates implementing the same function
equivalent gate pairs in the fanin/fanout cone
```

若 prompt 指定 `NAND(a,b)==target`，選 `nand_pair`。若要求任意兩顆 gate 的 output function 相同，選 `equivalent_pairs`。

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

scope：

```text
whole
net_fanin <net>
net_fanout <net>
gate_fanin <gate>
gate_fanout <gate>
```

## 4. Options

| Option | 預設 | 用途 |
|---|---:|---|
| `--find-any` | 是 | 找到第一組 SAT-proven pair 即停止 |
| `--all` | 否 | 嘗試完整列舉與分類 |
| `--max-results N` | 256 | 最多展開 N 組 pair records |
| `--patterns N` | 256 | simulation prefilter patterns |
| `--time-limit seconds` | 30 | simulation、enumeration、SAT 共用時間 |
| `--allow-same` | 否 | NAND mode 允許 `(a,a)` |
| `--include-boundary-signals` | 否 | NAND mode 允許 PI/PO/boundary candidates |
| `--gate-type type` | ANY | equivalent-pair mode 限制兩個成員的 gate type |

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
```

只有 `status: ok` 且 `complete: true` 時，found/no-match 才是完整證明。FindAny 的
`all_candidates_examined` 可為 false，因為找到 witness 後存在性問題已回答。FindAll
不完整時先保留全部已證明 matches；正式競賽答案依 `LLM_NOTES.md` 的 Competition Answer
Policy 作 best-effort 補答，但不得把未經 SAT 證明的 pair 標成 confirmed match。

`nand_pair` 主要讀：

```text
matches[].net_a
matches[].net_b
matches[].proof_method = SAT_UNSAT_MITER
matches[].solver_status = UNSAT
```

`equivalent_pairs` 主要讀：

```text
scope / scope_name / gate_type_filter
candidate_gate_count
equivalence_class_count
equivalent_pair_count
matches[].gate_a / gate_b / net_a / net_b
equivalence_classes
proof_method = SAT_EQUIVALENCE_CLASS
solver_status = UNSAT
```

`equivalent_pair_count` 是 class 可展開的總 pair 數。`truncated:true` 時，`match_count` 只表示實際列出的前 N 組。

要求完整 pair records 時可採兩階段呼叫：

```text
1. 先執行 --all，讀 candidate_gate_count 或 candidate_signal_count。
2. 計算 unordered pair 上界：
   N*(N-1)/2；若 nand_pair 使用 --allow-same，則為 N*(N+1)/2。
3. 以該上界重送 --max-results。
4. 只有 truncated:false、timed_out:false、complete:true 時才把 records 當成完整列表。
```

`equivalent_pairs` 的 `equivalence_classes` 可用來理解等價群組，但 LLM 不應在
`truncated:true` 時自行假設 envelope 已完整；應提高 `--max-results` 重試。

## 6. Prompt Examples

```text
Prompt: Find two internal signals whose NAND is equivalent to n25.
Command: func_search nand_pair n25 --time-limit 30
Read: found, matches[0], proven_equivalent
```

```text
Prompt: List all functionally equivalent gate pairs in the design.
Command: func_search equivalent_pairs whole --all --time-limit 30
Read: complete, equivalence_class_count, equivalent_pair_count, equivalence_classes
```

```text
Prompt: Find equivalent AND gate pairs in the fanin cone of n10.
Command: func_search equivalent_pairs net_fanin n10 --all --gate-type AND
Read: scope, gate_type_filter, equivalence_classes
```

## 7. 限制

- simulation 只負責候選過濾；公開 matches 均需 SAT UNSAT proof。
- DFF 不可作為 equivalent-pair candidate，DFF.Q 只作為 function boundary。
- timeout、solver unknown、unsupported 與 result limit 都可能使 FindAll 不完整。
- equivalent output function 不等於 observability redundancy；本 tool 不修改或刪除 gate。
- 實際 merge 使用 `edit_apply merge_functionally_equivalent_gates <scope>`；該 command 會
  強制執行 whole-design SAT 與 rollback。
