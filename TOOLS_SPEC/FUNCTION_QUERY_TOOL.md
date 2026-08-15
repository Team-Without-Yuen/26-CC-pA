# Function Query Tool

## 1. 責任

`func_query` 負責題目已指定 target signals 的 Boolean property，包括 equivalence、conditional equivalence、constant、truth status、functional dependence、symmetry、expression 與 support。SAT 類結果必須先確認 solver 完成。

完整性規則：`boolean_expression` 不建立可能指數膨脹的單一平面字串，而是自動將
完整 named-net DAG equations 寫入唯一 artifact。每個 cone net 只出現一次，不設
gate、depth 或輸出字元上限；只受 request deadline 與 I/O 錯誤限制。LLM 不提供
output path，也不得省略 artifact 內的中間 equations。

## 2. 選擇條件

已知要分析的 net/bus 名稱，且 prompt 出現 `functionally equivalent`、`when condition`、`always 0/1`、`can be`、`depends on`、`symmetric`、`Boolean expression` 或 `support inputs` 時使用本 tool。

候選名稱未知、要求從全設計找一組 signals 時使用 `func_search`。比較 current/original 兩份 design 使用 `equiv_query`。

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

`boolean_expression` 不使用上述 yes/no 欄位。只有 envelope `complete:true` 且
`expression artifact complete: yes` 時，`output_file` 才包含完整答案。artifact footer
也必須是 `Complete: yes`；timeout 或 I/O failure 時不可把部分檔案說成完整。

Boolean expression 的 leaf 必須依輸出分類判讀：

- `real primary input count`：真正 top-level PI。
- `DFF.Q boundary count`：目前 state 的 sequential boundary，不是真正 PI。
- `undriven boundary count`：floating/undriven leaf，也不是真正 PI。
- `primary-input-only combinational expression available: no`：不能在不跨 DFF cycle、也不
  假設 floating value 的前提下，寫成只含 top-level PI 的組合式。

若 prompt 要求 `using only primary input names`，但此欄位為 `no`，LLM 必須說明完整的
current-cycle combinational expression 以 DFF.Q 或 undriven signal 為 boundary；不得把
pseudo-PI 說成 primary input，也不得沿 DFF.Q 回追 D pin。

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
  都有定義，語意完整，但不是將所有中間 nets 重複代入成單一指數級字串。
- `simplified_expression` 只在 prompt 明確要求 manageable、depth-limited 或指定深度時使用；
  不可為了縮短答案而自行取代完整 `boolean_expression`。
- `conditional_equivalence` 的 condition 必須是存在的 scalar net，value 只能為 0 或 1。
- 本 tool 只比較同一 current design 中的 named signals，不取代 whole-design equivalence。
- DFF.Q 是 pseudo-primary-input boundary。FunctionQuery 不進行 time-frame expansion；要求
  top-level-PI-only expression 時，不能把 Q 對應 D pin 的前一週期邏輯直接代入。
