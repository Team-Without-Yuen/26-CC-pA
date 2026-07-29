# Function Query Tool

## 1. 責任

`func_query` 負責題目已指定 target signals 的 Boolean property，包括 equivalence、conditional equivalence、constant、truth status、functional dependence、symmetry、expression 與 support。SAT 類結果必須先確認 solver 完成。

完整性規則：不自行限制 expression、support 或 target records；時間限制依題目指定。大型
Boolean expression 目前沒有 file output，屬已知輸出風險。詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

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
| `boolean_expression` | `<net>` | 完整展開 expression | `expression`, `expression length` |
| `simplified_expression` | `<net> <max_depth>` | depth-limited expression | expression 與 depth-limit fields |
| `support_pi` | `<net>` | fanin PI/DFF.Q pseudo-PI support | `Support primary inputs` |

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

## 7. 限制

- `support_pi` 是 structural support，不等於 exact functional dependence；yes/no dependence 使用 `depends_on`。
- 完整 `boolean_expression` 在大型 cone 可能非常長；優先使用 `simplified_expression`。
- `conditional_equivalence` 的 condition 必須是存在的 scalar net，value 只能為 0 或 1。
- 本 tool 只比較同一 current design 中的 named signals，不取代 whole-design equivalence。
