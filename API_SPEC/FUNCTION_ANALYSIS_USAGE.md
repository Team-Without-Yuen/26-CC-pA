# Function Analysis 使用說明

這份文件只負責說明 `FunctionQuery` 怎麼使用。  
設計背景與分類整理請看：

```text
API_SPEC/FUNCTION_ANALYSIS_API.md
```

---

## 1. 基本概念

`FunctionQuery` 用來回答「Boolean function」問題。

基本形式：

```text
query.type + target/comparison/condition/symmetry inputs/constant/depth fields
    -> runFunctionQuery()
    -> FunctionReport
```

使用流程：

```text
1. 建立 Netlist::FunctionQuery query。
2. 設定 query.type。
3. 視 query.type 設定 target、comparison、condition、symmetry inputs、constant 或 expression depth 欄位。
4. 呼叫 netlist.runFunctionQuery(query)。
5. 從 report.ok / report.exists / report.status / report.* 欄位讀結果。
```

注意：

```text
FunctionQuery 是功能分析，不是連線分析。
FunctionQuery 驗證已指定的 target；未知 pair/candidate 搜尋使用 FunctionSearchQuery。
SAT 類 query 目前使用 production legacy proof backend；AIG 只保留為內部 differential
與已證明適合的 batch backend，不由 caller 選擇。Boolean expression tools mode 以
named-net DAG streaming 寫入完整 artifact，不建立指數膨脹的平面字串。
DFF.Q 視為 pseudo primary input，不穿越 DFF 回到 D pin。
```

基本範例：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::AlwaysZero;
query.netNameA = "n16";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
if (report.ok && report.exists) {
    // n16 is always 0
}
```

---

## 2. FunctionQueryType 總表

| Query type | 問題類型 | 需要設定 | 主要讀取欄位 |
|---|---|---|---|
| `Equivalence` | 兩個 net / bus 是否功能等價 | `netNameA`, `netNameB` | `equivalent`, `exists`, `status` |
| `ConditionalEquivalence` | 固定 condition 後兩個 function 是否等價 | `netNameA`, `netNameB`, `conditionNetName`, `conditionValue` | `equivalent`, `solverStatus`, `status` |
| `CanBeValue` | scalar net 是否可能為指定值 | `netNameA`, `constValue` | `exists`, `canBeZero`, `canBeOne`, `status` |
| `ConstantFunction` | scalar net 是否恆等於指定值 | `netNameA`, `constValue` | `exists`, `isConstant`, `constValue`, `status` |
| `AlwaysZero` | scalar net 是否永遠為 0 | `netNameA` | `exists`, `isConstant`, `status` |
| `AlwaysOne` | scalar net 是否永遠為 1 | `netNameA` | `exists`, `isConstant`, `status` |
| `TruthStatus` | 分類 scalar net 的 truth status | `netNameA` | `canBeZero`, `canBeOne`, `isConstant`, `constValue`, `status` |
| `FunctionalDependence` | target function 是否 exact 依賴 selected input | `netNameA`, `netNameB` | `dependsOnInput`, `inputInStructuralSupport`, `status` |
| `Symmetry` | 交換兩個 inputs 後 target function 是否不變 | `netNameA`, `symmetryInputNameA`, `symmetryInputNameB` | `symmetric`, support flags、counterexample、`status` |
| `BooleanExpression` | 完整 Boolean DAG equations | `netNameA`；tools 內部自動設定 artifact path | artifact 完整性、equation/boundary counts、output path、support 分類 |
| `SimplifiedBooleanExpression` | 展開 depth-limited Boolean expression | `netNameA`, `maxExpressionDepth` | `expression`, `expressionDepthLimited`, `maxExpressionDepth` |
| `PrimaryInputsOfNet` | 回報並分類 fanin cone leaves | `netNameA` | `supportPrimaryInputs` 與三個分類欄位 |

---

## 3. FunctionQuery 輸入欄位

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `FunctionQueryType` | `TruthStatus` | 決定要執行哪一種 function query |
| `netNameA` | `std::string` | `""` | 主要分析目標；大部分 query 都需要 |
| `netNameB` | `std::string` | `""` | `Equivalence` / `ConditionalEquivalence` 的第二個 net / bus，或 `FunctionalDependence` 的 selected input |
| `conditionNetName` | `std::string` | `""` | `ConditionalEquivalence` 使用的 scalar condition net |
| `symmetryInputNameA` | `std::string` | `""` | `Symmetry` 使用的第一個 scalar PI / DFF.Q pseudo-PI |
| `symmetryInputNameB` | `std::string` | `""` | `Symmetry` 使用的第二個 scalar PI / DFF.Q pseudo-PI |
| `conditionValue` | `int` | `-1` | `ConditionalEquivalence` 使用；只能是 0 或 1 |
| `constValue` | `int` | `-1` | `CanBeValue` / `ConstantFunction` 使用；只能是 0 或 1 |
| `maxExpressionDepth` | `int` | `10` | `SimplifiedBooleanExpression` 使用；必須 >= 0 |
| `writeExpressionToFile` | `bool` | `false` | 內部/C++ artifact mode；public tool 固定設為 true |
| `expressionOutputFilePath` | `std::string` | `""` | 內部輸出路徑；public tool 自動產生，不由 LLM 指定 |
| `timeLimitSeconds` | `double` | `290.0` | Boolean backend wall-clock 預算；constant/truth family 的兩個 value proof 各使用最多一半預算 |

---

## 4. FunctionReport 回傳欄位

### 4.1 共用狀態

| 欄位 | 意思 |
|---|---|
| `ok` | query 是否成功執行 |
| `exists` | yes/no 問題的主要答案；expression 類成功時也會是 true |
| `message` | debug / LLM response 用的簡短訊息 |
| `status` | 更明確的分類字串 |
| `solverRan` | 是否實際呼叫 solver；若 AIG structural sharing/constant shortcut 已直接證明，可為 false |
| `solverTimedOut` | SAT solver 是否因 time limit 回 UNKNOWN |
| `solverUnknown` | SAT solver 是否回 UNKNOWN；不能解讀成 false |
| `unsupported` | 目標 cone / gate type / 參數是否不支援 |
| `solverStatus` | `SAT`、`UNSAT`、`TIMEOUT`、`UNKNOWN`、`UNSUPPORTED` |

### 4.2 目標資訊

| 欄位 | 意思 |
|---|---|
| `netNameA` | 查詢目標 A |
| `netNameB` | 查詢目標 B |
| `conditionNetName` | conditional query 的 condition net |
| `netIdA` | scalar net A 的 ID；bus 或不存在時可為 -1 |
| `netIdB` | scalar net B 的 ID；bus 或不存在時可為 -1 |
| `constValue` | query 指定或推導出的常數值；unknown 時為 -1 |
| `conditionNetId`, `conditionValue` | conditional query 實際使用的 condition |

### 4.3 SAT 功能結果

| 欄位 | 意思 |
|---|---|
| `equivalent` | `Equivalence` 的結果 |
| `canBeZero` | 目標 scalar net 是否可能為 0 |
| `canBeOne` | 目標 scalar net 是否可能為 1 |
| `isConstant` | 目標 scalar net 是否為 constant function |
| `inputInStructuralSupport` | selected input 是否出現在 target fanin support |
| `dependsOnInput` | 雙份 cofactor SAT miter 證明的 exact functional dependence |

### 4.4 Boolean Expression 結果

| 欄位 | 意思 |
|---|---|
| `expression` | 完整或 depth-limited Boolean expression |
| `expressionLength` | `expression.size()` |
| `maxExpressionDepth` | depth-limited query 使用的展開深度；完整展開時為 -1 |
| `expressionDepthLimited` | 完整 mode 表示實際觸發 safety limit；simplified mode 表示採用 depth-limited expansion |
| `wroteExpressionToFile` | 是否建立 artifact |
| `expressionArtifactComplete` | artifact 是否包含完整 target DAG equations |
| `expressionArtifactTimedOut` | 是否因 request deadline 中止 |
| `expressionEquationCount` | 寫出的 combinational equations 數 |
| `expressionBoundaryCount` | PI/constant/DFF.Q/undriven boundary 數 |
| `expressionArtifactFormat` | 目前為 `NAMED_DAG_EQUATIONS_V1` |
| `expressionOutputFilePath` | 完整 artifact 路徑 |
| `supportPrimaryInputs` | real PI、DFF.Q pseudo-PI、undriven leaf 的排序聯集 |
| `supportRealPrimaryInputs` | 真正宣告的 top-level PI |
| `supportDffPseudoInputs` | 作為 sequential boundary 的 DFF.Q leaves |
| `supportUndrivenLeaves` | 沒有 driver 且不是 PI 的 floating/undriven leaves |

---

## 5. Equivalence

用途：

```text
判斷兩個 net / bus 是否在所有輸入情況下功能等價。
```

寫法：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::Equivalence;
query.netNameA = "y";
query.netNameB = "z";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否成功 | `report.ok` |
| 是否等價 | `report.equivalent` 或 `report.exists` |
| 狀態文字 | `report.status` |

注意：

```text
Equivalence 支援 bus，但兩邊 bit width 必須一致。
```

### 5.1 ConditionalEquivalence

固定一條 scalar condition net 後，檢查兩個 net / bus 是否等價。這個 mode 主要供其他高階分析重用，例如證明 DFF hold cofactor：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::ConditionalEquivalence;
query.netNameA = "d";
query.netNameB = "q";
query.conditionNetName = "en";
query.conditionValue = 0;

const auto report = netlist.runFunctionQuery(query);
```

`report.equivalent=true` 表示在 `en=0` 的所有可行 assignments 中，`d == q`。若 solver timeout 或回 UNKNOWN，`report.ok=false`，不可把結果當成不等價。

---

## 6. CanBeValue

用途：

```text
判斷 scalar net 是否存在某組輸入可以等於指定值。
```

寫法：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::CanBeValue;
query.netNameA = "n16";
query.constValue = 1;

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| n16 是否可能為 1 | `report.exists` |
| n16 是否可能為 0 | `report.canBeZero` |
| n16 是否可能為 1 | `report.canBeOne` |
| 狀態文字 | `report.status` |

---

## 7. ConstantFunction

用途：

```text
判斷 scalar net 是否在所有輸入情況下都等於指定常數。
```

寫法：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::ConstantFunction;
query.netNameA = "n16";
query.constValue = 0;

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否恆等於指定常數 | `report.exists` |
| 是否為 constant function | `report.isConstant` |
| 常數值 | `report.constValue` |
| 狀態文字 | `report.status` |

---

## 8. AlwaysZero / AlwaysOne

用途：

```text
回答最常見的 yes/no constant 問題。
```

AlwaysZero：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::AlwaysZero;
query.netNameA = "n16";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

AlwaysOne：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::AlwaysOne;
query.netNameA = "n16";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否成立 | `report.exists` |
| 是否 constant | `report.isConstant` |
| 狀態文字 | `report.status` |

prompt 對應：

| Prompt | Query type |
|---|---|
| Is output n16 always 0 regardless of all inputs? | `AlwaysZero` |
| Is n3 always 1? | `AlwaysOne` |
| Is this signal tied to logic 0 functionally? | `AlwaysZero` |
| Is this signal tied to logic 1 functionally? | `AlwaysOne` |

---

## 9. TruthStatus

用途：

```text
一次分類 scalar net 是 always 0、always 1，還是 non-constant。
```

寫法：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::TruthStatus;
query.netNameA = "n16";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否可能為 0 | `report.canBeZero` |
| 是否可能為 1 | `report.canBeOne` |
| 是否 constant | `report.isConstant` |
| 若 constant，常數值是多少 | `report.constValue` |
| 分類 | `report.status` |

分類表：

| `canBeZero` | `canBeOne` | `status` | 意思 |
|---|---|---|---|
| true | false | `ALWAYS_ZERO` | 恆為 0 |
| false | true | `ALWAYS_ONE` | 恆為 1 |
| true | true | `NON_CONSTANT` | 0/1 都可能 |
| false | false | `SOLVER_TIMEOUT` / `SOLVER_UNKNOWN` / `UNSUPPORTED` | 未能安全完成 SAT 分類 |

SAT 類 query 的安全解讀：

```text
1. report.ok=false 且 status=SOLVER_TIMEOUT / SOLVER_UNKNOWN 時，不能把 exists=false 解讀成「性質不成立」。
2. report.solverTimedOut=true 代表需要回覆「未能在時間限制內證明」，而不是回覆 no。
3. report.unsupported=true 代表目前 API 無法安全回答，應回報 unsupported 或改用其他 query。
4. 對 CanBeValue，solverStatus 代表指定 constValue 那次 SAT 查詢的結果。
```

---

## 10. BooleanExpression

用途：

```text
回答完整 Boolean equation / Boolean function。
```

寫法：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::BooleanExpression;
query.netNameA = "n16";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否成功 | `report.ok` |
| expression 字串 | `report.expression` |
| expression 長度 | `report.expressionLength` |
| 依賴哪些 PI / pseudo-PI | `report.supportPrimaryInputs` |
| 只看真正 PI | `report.supportRealPrimaryInputs` |
| 跨時序 boundary 狀態 | `report.supportDffPseudoInputs` |
| 是否含 undriven leaf | `report.supportUndrivenLeaves` |
| 狀態文字 | `report.status` |

prompt 對應：

| Prompt | Query type |
|---|---|
| Derive the Boolean equation for output n16. | `BooleanExpression` |
| What Boolean function does output n25 compute? | `BooleanExpression` |
| Express output n12 in terms of primary inputs. | `BooleanExpression` + `supportPrimaryInputs` |

注意：

```text
Public tool 的 BooleanExpression 使用 exact structural named-DAG equations，不做一般
algebraic minimization；每個 net 只輸出一次，因此不需 depth/size cap。C++ caller 若不
啟用 artifact mode，仍可使用 legacy inline expression 與 `expressionDepthLimited`。
```

---

## 11. SimplifiedBooleanExpression

用途：

```text
當完整 expression 太長時，用 maxExpressionDepth 限制展開深度。
超過限制的節點會直接用 net name 表示。
```

寫法：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::SimplifiedBooleanExpression;
query.netNameA = "n16";
query.maxExpressionDepth = 8;

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| depth-limited expression | `report.expression` |
| 使用的 depth limit | `report.maxExpressionDepth` |
| 是否為 depth-limited | `report.expressionDepthLimited` |
| expression 長度 | `report.expressionLength` |

---

## 12. PrimaryInputsOfNet

用途：

```text
只想知道某個 output / internal net 的 Boolean support leaves。
```

寫法：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::PrimaryInputsOfNet;
query.netNameA = "n16";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| support leaves | `report.supportPrimaryInputs` |
| 數量 | `report.supportPrimaryInputs.size()` |
| 真正 primary inputs | `report.supportRealPrimaryInputs` |
| DFF.Q pseudo inputs | `report.supportDffPseudoInputs` |
| undriven/floating leaves | `report.supportUndrivenLeaves` |

注意：

```text
DFF.Q 會被視為 pseudo PI leaf。
undriven non-PI net 會列入 `supportUndrivenLeaves`，不會誤當成真正 PI。
PrimaryInputsOfNet 只回報 structural support，不能單獨證明 exact functional dependence。
```

---

## 13. FunctionalDependence

用途：

```text
回答「Does target output depend on this input?」的 exact yes/no。
```

寫法：

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::FunctionalDependence;
query.netNameA = "n8";
query.netNameB = "n1";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| exact yes/no | `report.dependsOnInput` 或 `report.exists` |
| 是否先出現在 structural support | `report.inputInStructuralSupport` |
| SAT 是否有完整結論 | `report.ok`, `report.solverStatus`, `report.solverTimedOut`, `report.unsupported` |

SAT 表示存在一組其餘 inputs 的 assignment，使 selected input 從 0 變 1 時 target 跟著改變。UNSAT 才能回答 functionally independent。若 selected input 不在 structural support，可不啟動 solver 直接得到 no。

---

## 14. Symmetry

用途：檢查交換兩個獨立 inputs 後，target scalar/bus function 是否完全不變。

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::Symmetry;
query.netNameA = "n11";
query.symmetryInputNameA = "n3";
query.symmetryInputNameB = "n9[0]";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

主要讀取：

| 想知道 | 讀取欄位 |
|---|---|
| exact yes/no | `report.symmetric` 或 `report.exists` |
| 兩個 inputs 是否在 structural support | `symmetryInputAInStructuralSupport`, `symmetryInputBInStructuralSupport` |
| 是否找到反例 | `counterexampleFound`, `counterexampleAssignments` |
| 哪些 target bits 改變 | `mismatchedTargetBitNames`, `outputValuesBeforeSwap`, `outputValuesAfterSwap` |
| 分析是否完整 | `ok`, `solverStatus`, `solverTimedOut`, `solverUnknown`, `unsupported` |

`UNSAT` 表示已證明 symmetric；`SAT` 表示 not symmetric 並回傳反例。`NOT_NEEDED` 表示兩個 inputs 都不在 target support，無須啟動 solver 即可證明 symmetric。target 可為 bus，但兩個 input 參數都必須解析成單一 PI 或 DFF.Q bit。

CLI：

```text
func_query symmetry n11 n3 n9[0]
```

---

## 15. Prompt 對應表

| Prompt | 建議 FunctionQueryType | 需要設定 | 主要讀取 |
|---|---|---|---|
| Is output n16 always 0 regardless of all inputs? | `AlwaysZero` | `netNameA = "n16"` | `exists`, `status` |
| Is n16 always 1? | `AlwaysOne` | `netNameA = "n16"` | `exists`, `status` |
| Is n16 a constant 0 function? | `ConstantFunction` | `netNameA = "n16"`, `constValue = 0` | `exists`, `isConstant` |
| Can n16 ever become 1? | `CanBeValue` | `netNameA = "n16"`, `constValue = 1` | `exists` |
| Can n16 ever become 0? | `CanBeValue` | `netNameA = "n16"`, `constValue = 0` | `exists` |
| Are y and z equivalent? | `Equivalence` | `netNameA = "y"`, `netNameB = "z"` | `equivalent` |
| When en is 0, is d equivalent to q? | `ConditionalEquivalence` | `netNameA = "d"`, `netNameB = "q"`, `conditionNetName = "en"`, `conditionValue = 0` | `equivalent`, `solverStatus` |
| Does output n8 depend on input n1? | `FunctionalDependence` | `netNameA = "n8"`, `netNameB = "n1"` | `dependsOnInput`, `status` |
| Is n11 symmetric with respect to n3 and n9[0]? | `Symmetry` | `netNameA = "n11"`, `symmetryInputNameA = "n3"`, `symmetryInputNameB = "n9[0]"` | `symmetric`, `solverStatus` |
| Classify the truth status of n16. | `TruthStatus` | `netNameA = "n16"` | `status`, `canBeZero`, `canBeOne` |
| Is bus a equivalent to bus b? | `Equivalence` | `netNameA = "a"`, `netNameB = "b"` | `equivalent` |
| Derive the Boolean equation for output n16. | `BooleanExpression` | `netNameA = "n16"` | `expression` |
| What Boolean function does output n25 compute? | `BooleanExpression` | `netNameA = "n25"` | `expression` |
| Express n12 in terms of primary inputs. | `BooleanExpression` | `netNameA = "n12"` | `expression`, `supportPrimaryInputs` |
| Which primary inputs are in output n8's structural support? | `PrimaryInputsOfNet` | `netNameA = "n8"` | `supportPrimaryInputs` |

---

## 16. 何時不要用 FunctionQuery

| 問題 | 應改用 |
|---|---|
| How many gates are in the design? | BasicQuery |
| Which gate directly drives n1? | DirectConnectivityQuery |
| Report every gate connected to output of g0. | DirectConnectivityQuery |
| Determine all gates reachable from n2. | ConeQuery |
| Is there a path from a to y? | PathQuery |
| Find all paths from a to y avoiding g3. | PathQuery |
| What is the maximum depth to output y? | DepthAnalysis |
| Check current design equivalent to original loaded netlist. | WholeDesignEquivalence |

---

## 17. 低階 Helper 對照

| FunctionQueryType | 底層 helper |
|---|---|
| `Equivalence` | legacy combined-cone SAT miter，支援 scalar/bus |
| `ConditionalEquivalence` | shared equivalence CNF + scalar condition unit clause |
| `CanBeValue` | legacy fanin-cone SAT，分別檢查 target=0/1 |
| `ConstantFunction` | legacy fanin-cone SAT 對 const0/const1 的兩次 proof |
| `AlwaysZero` | legacy fanin-cone constant proof |
| `AlwaysOne` | legacy fanin-cone constant proof |
| `TruthStatus` | legacy fanin-cone SAT 對 const0/const1 的兩次 proof |
| `FunctionalDependence` | `runFunctionQuery()` 內部 dual-cone SAT miter |
| `Symmetry` | `runFunctionQuery()` 內部 swapped-cofactor dual-cone SAT miter |
| `BooleanExpression` | `getBooleanExpression()` |
| `SimplifiedBooleanExpression` | `getSimplifiedBooleanExpression()` |
| `PrimaryInputsOfNet` | `getPrimaryInputsOfNet()` |

---

## 18. 常見使用範例

### 18.1 回答 yes/no

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::AlwaysZero;
query.netNameA = "n16";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);

std::string answer = (report.ok && report.exists) ? "yes" : "no";
```

### 18.2 查 non-constant

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::TruthStatus;
query.netNameA = "y";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
if (report.status == "NON_CONSTANT") {
    // y can be both 0 and 1
}
```

### 18.3 查等價

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::Equivalence;
query.netNameA = "y";
query.netNameB = "z";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
bool equivalent = report.ok && report.equivalent;
```

### 18.4 產生 Boolean equation

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::BooleanExpression;
query.netNameA = "n16";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
if (report.ok) {
    std::string equation = report.expression;
}
```

### 18.5 大 cone 使用 depth-limited expression

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::SimplifiedBooleanExpression;
query.netNameA = "n16";
query.maxExpressionDepth = 6;

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
```

---

## 19. 目前限制

使用時要注意：

```text
1. AlwaysZero / AlwaysOne / CanBeValue / TruthStatus 目前只支援 existing scalar net。
2. Equivalence 支援 bus，但兩邊 bit width 必須一致。
3. Symmetry 會回傳非對稱 counterexample；其他 SAT query 目前不一定回傳 witness assignment。
4. Boolean expression 只做固定 local identity，不做一般 algebraic minimization。
5. Public Boolean expression artifact 無 gate/depth/字元上限；legacy inline helper 仍有
   safety limit。SimplifiedBooleanExpression 只用於 prompt 明確要求 depth-limited 輸出。
6. DFF.Q 是 pseudo PI；這不是 sequential equivalence。
7. 若 SAT solver timeout 或 UNKNOWN，`report.ok=false`，並透過 `solverTimedOut` / `solverUnknown` / `solverStatus` 明確回報，不應解讀成普通 false。
8. FunctionalDependence 只接受 scalar target 與 scalar PI / DFF.Q pseudo-PI selected input，目前不回傳 SAT witness assignment。
9. Symmetry target 支援 scalar/bus；交換輸入必須是兩個不同的 scalar PI / DFF.Q pseudo-PI。internal driven net 與 whole bus input 會明確回 unsupported/error。
```

---

## 20. 目前實作與測試狀態

```text
實作檔案：src/analysis/FunctionAnalysis.cpp
expression 檔案：src/analysis/Booleanexpression.cpp
型別檔案：include/core/NetlistQueries.h
tester：mini test/test4/test4.cpp
artifact/scalability tester：mini test/test46/test46.cpp
目前 test4：Summary: 17 passed, 0 failed.
目前 test46：Summary: 8 passed, 0 failed.
CLI integration regression test9-test17：150 passed, 0 failed。
symmetry integration test22：14 passed, 0 failed。
NewTestCase test36/test37：兩題皆回 SYMMETRIC；指定 inputs 均不在 target support，solverStatus=NOT_NEEDED。
test39：test33/36/37 正式 prompts、四個 SAT-active dependence/symmetry 案例與兩個 timeout 參數邊界，共 9/9 通過。
```
