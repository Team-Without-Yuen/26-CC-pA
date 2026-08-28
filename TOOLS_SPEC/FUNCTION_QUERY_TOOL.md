# Function Query Tool

## 1. 責任

`func_query` 負責題目已指定 target signals 的 Boolean property，包括 equivalence、conditional equivalence、constant、truth status、functional dependence、symmetry、expression 與 support。SAT 類結果必須先確認 solver 完成。

完整性規則：`boolean_expression` 不建立可能指數膨脹的單一平面字串，而是自動將
完整 named-net DAG equations 寫入唯一 artifact。每個 cone net 只出現一次，不設
gate、depth 或輸出字元上限；只受 request deadline 與 I/O 錯誤限制。LLM 不提供
output path，也不得省略 artifact 內的中間 equations。

`support_pi` 的 support lists，以及 `symmetry` 的 support/counterexample/detail lists，若保守
估算達到 3072 tokens，或 list records 達到 256 筆，會在正式 4096-token response 上限前自動
寫入完整 `QUERY_LIST_ARTIFACT_V1`。
response 保留 target、proof status、主要 bool 與各類 count，再提供 `output_file`；門檻只改變
呈現位置，不會限制分析筆數。`boolean_expression` 已有專用 equation artifact，因此明確跳過
generic list artifact，單次 response 只會有一個 `output_file`。

## 2. 選擇條件

當 prompt 已提供要判定的 target net/bus，以及 property 所需的其他 operand identities 時，使用
本 tool 對這組已知 signals 做 Boolean analysis。mode 由 property 決定：同一 current design
內的 equivalence、條件式 equivalence、可滿足值、constant/truth status、functional dependence、
input symmetry、Boolean equation 或 support boundaries。

責任邊界：

- operand identities 未知，答案必須掃描 candidate domain 找 witness/list/count 時，使用
  `func_search`。
- 比較的是兩個 named signals，而不是兩份 design，使用本工具的 `equivalence`。
- 比較 current design 與 original/previous design 的 whole-design state，使用 `equiv_query`。
- structural fanin leaves 或 cone size 不需要 Boolean proof，使用 Structure/Cone；exact functional
  dependence 才使用本工具。

不能只因 prompt 出現 `equivalent`、`constant` 或 `input` 就選本工具；必須先確認 proof object
是已知 signal property，而非 candidate search、design baseline comparison 或結構屬性。

## 3. Command Grammar

```text
func_query <mode> [args]
```

## 4. Modes

| Mode | 參數 | 用途 | 主要 data |
|---|---|---|---|
| `equivalence` | `<net_a> <net_b>` | 兩個已知 nets/buses 是否等價 | `equivalent`, solver fields |
| `conditional_equivalence` | `<net_a> <net_b> <condition_net> <0\|1>` | 固定 scalar condition 後是否等價 | `condition net`, `condition value`, `equivalent` |
| `can_be_value` | `<net> <0\|1>` | 是否存在輸入使 net 取指定值 | `answer`, `can be 0/1` |
| `constant` | `<net> <0\|1>` | 是否恆等於指定值 | `answer`, `is constant` |
| `always_zero` | `<net>` | 是否永遠為 0 | `answer`, `status` |
| `always_one` | `<net>` | 是否永遠為 1 | `answer`, `status` |
| `truth_status` | `<net>` | 分類 always 0、always 1、non-constant | `status`, `can be 0/1`, `is constant` |
| `depends_on` | `<target_net> <input_net>` | target 是否功能依賴 input | `depends on input`, structural-support field |
| `symmetry` | `<target_net_or_bus> <input_a> <input_b>` | 交換兩 inputs 是否不改變 target | `symmetric`, counterexample fields |
| `boolean_expression` | `<net>` | 完整 named-net Boolean DAG equations | artifact format/completeness、equation/boundary counts、`output_file` |
| `simplified_expression` | `<net> <max_depth>` | depth-limited expression | expression 與 depth-limit fields |
| `support_pi` | `<net>` | fanin real PI、DFF.Q 與 undriven support leaves | `Support leaves` 與三類 boundary counts |

`equivalence_when` 是 `conditional_equivalence` 的 parser alias，但 LLM 應使用正式名稱 `conditional_equivalence`。

所有 mode 採 exact arity。名稱缺失、option-like 名稱或 trailing token 都回
`status:error, complete:false`，不得忽略後繼 token。`condition_value` 與 `const_value` 僅接受
strict integer `0/1`；`simplified_expression` 的 `max_depth` 僅接受 `0..INT_MAX`。文字、小數、
負數與 overflow 必須在 CLI parser 層拒絕。recognized-mode argument error 不得誤報為
`Unknown func_query mode`。

## 5. SAT 輸出判讀

主要共同欄位包括：

```text
status
solver ran
solver status
solver timed out
solver unknown
unsupported
answer
equivalent
```

只有 envelope `complete:true` 時，`answer`、`equivalent`、`depends on input` 或 `symmetric`
才是已證明的 yes/no。`timeout`、`partial`、`unsupported` 不能直接解讀為 `no`；競賽正式
答案仍應依 `LLM_NOTES.md` 的 Competition Answer Policy 選擇最可能的單一 yes/no，
但不得將該推定描述成 SAT proof。

`answer` 是 mode-specific 主要布林答案；回答 equivalence 時優先讀 `equivalent`，dependence 讀 `depends on input`，symmetry 讀 `symmetric`，不要混用其他預設布林欄位。

`boolean_expression` 不使用上述 yes/no 欄位。LLM 只有在 envelope `complete:true` 且
`expression artifact complete: yes` 時，才可把 `output_file` 描述為完整答案；LLM 不開檔
檢查 footer。Producer 仍必須寫入 `Complete: yes` footer，供 evaluator 與檔案完整性檢查使用。

Boolean expression 的 leaf 必須依輸出分類判讀：

- `real primary input count`：此 target function support 中出現的真正 top-level PI 數量，不是整個
  design 的 PI inventory；此值為 0 不代表 design 沒有 primary inputs。
- `DFF.Q boundary count`：目前 state 的 sequential boundary，不是真正 PI。
- `DFF.Q current-state variable mappings`：`state_qN = <DFF>.Q (net <net>)`，用來把
  artifact 方程式中的狀態變數映射回 named netlist；`state_qN` 只代表當前週期狀態。
- `undriven boundary count`：floating/undriven leaf，也不是真正 PI。
- `primary-input-only combinational expression available: no`：不能在不跨 DFF cycle、也不
  假設 floating value 的前提下，寫成只含 top-level PI 的組合式。

`a`、`b` 等若在 prompt 中是 existential/unknown operands，代表要從現有 signals 中搜尋的角色，
不是可以直接傳給 `func_query equivalence` 的 literal net names；這類問題屬 `func_search`。

若 prompt 要求 `using only primary input names`，但此欄位為 `no`，LLM 必須說明完整的
current-cycle combinational expression 以 DFF.Q 或 undriven signal 為 boundary；不得把
pseudo-PI 說成 primary input，也不得沿 DFF.Q 回追 D pin。

`support_pi` / `symmetry` 若出現：

```text
list artifact format: QUERY_LIST_ARTIFACT_V1
list artifact complete: yes
wrote list to file: yes
output_file: <path>
```

表示完整 support 或 counterexample records 交付於該檔案；response 中的 count 與主要判定仍可
直接使用。LLM 依 envelope 的 `complete` 與 `list artifact complete` 判讀，不開檔檢查 footer。
小結果不觸發 artifact，仍直接完整列於 response。

## 6. Prompt Examples

```text
Prompt: Are n10 and n12 functionally equivalent?
Command: func_query equivalence n10 n12
Read: equivalent；先確認 complete=true
```

```text
Prompt: When en is 0, is d equivalent to q?
Command: func_query conditional_equivalence d q en 0
Read: condition net/value, equivalent
```

```text
Prompt: Is output y always zero?
Command: func_query always_zero y
Read: answer/status
```

```text
Prompt: Does y functionally depend on input a?
Command: func_query depends_on y a
Read: depends on input
```

```text
Prompt: Are a and b symmetric with respect to y?
Command: func_query symmetry y a b
Read: symmetric；若為 false 可讀 counterexample
```

```text
Prompt: Derive a manageable Boolean expression for n20.
Command: func_query simplified_expression n20 8
Read: expression, expression depth limited
```

```text
Prompt: Derive the Boolean equation for output n16 in terms of its primary inputs.
Command: func_query boolean_expression n16
Read: equation count、boundary count、output_file；完整 equations 位於 artifact
```

```text
Prompt: Write the logic expression for n30 using only the primary input names.
Command: func_query boolean_expression n30
Read: 若 DFF.Q boundary count > 0 且 primary-input-only combinational expression available=no，
      回答 n30 是 sequential state boundary，無法在 current combinational frame 中只用 top-level PI 表示
```

## 7. 限制

- `support_pi` 是 structural support，不等於 exact functional dependence；yes/no dependence 使用 `depends_on`。
- `boolean_expression` artifact 使用具名中間 nets 表示 DAG；每個 boundary 與 equation
  都有定義。`NAMED_DAG_EQUATIONS_V2` 會將 DFF.Q leaf 寫成 `state_qN`，並在
  `Current-state variables` 區段列出 DFF instance、Q pin 與原 net；它不是跨週期展開。
- `simplified_expression` 只在 prompt 明確要求 manageable、depth-limited 或指定深度時使用；
  不可為了縮短答案而自行取代完整 `boolean_expression`。
- `conditional_equivalence` 的 condition 必須是存在的 scalar net，value 只能為 0 或 1。
- 本 tool 只比較同一 current design 中的 named signals，不取代 whole-design equivalence。
- DFF.Q 是 pseudo-primary-input boundary。FunctionQuery 不進行 time-frame expansion；要求
  top-level-PI-only expression 時，不能把 Q 對應 D pin 的前一週期邏輯直接代入。
