# Function Analysis API 整理

這份文件整理 `Netlist` 的 Boolean / function analysis API，也就是回答「訊號功能是否等價、是否可能為某值、是否恆為常數、Boolean expression 是什麼」這類問題。

使用範例與 prompt 對應請看：

```text
API_SPEC/FUNCTION_ANALYSIS_USAGE.md
```

---

## 1. 核心定位

Function Analysis 負責回答「邏輯功能」問題，而不是只看連線拓樸。

典型問題：

```text
Is output n16 always 0 regardless of all inputs?
Is net y functionally equivalent to net z?
Can net n3 ever become 1?
Is output y a constant function?
Classify whether n16 is always 0, always 1, or non-constant.
Derive the Boolean equation for output n16.
What Boolean function does output n25 compute?
Express output n12 in terms of primary inputs.
```

這一層不做：

```text
單純物件資訊查詢 -> BasicQuery
未知 signal pair / Boolean candidate 搜尋 -> FunctionSearchQuery
直接相連查詢 -> DirectConnectivityQuery
transitive reachable / fanin / fanout -> ConeQuery
指定 startpoint 到 endpoint 的 path search -> PathQuery
timing depth / critical path -> DepthAnalysis
optimization rewrite -> EditApply / Optimization flow
whole-design original/current equivalence -> WholeDesignEquivalence
```

`FunctionQuery` 的目標名稱必須已知。例如 `Are n1 and n2 equivalent?` 使用本 API；`Does any pair (a,b) satisfy NAND(a,b)==n25?` 應改用 `FunctionSearchQuery`，請看 `API_SPEC/FUNCTION_SEARCH_USAGE.md`。

---

## 2. 底層方法

Function Analysis 目前包含兩類方法：

```text
1. SAT-based function property checking
2. Symbolic fanin traversal for Boolean expression extraction
```

SAT-based query 的基本流程：

```text
1. 找到目標 net 或兩個比較目標。
2. 從目標 net 往 fanin 方向展開 combinational cone。
3. 將 cone 內 primitive gates 轉成 CNF。
4. 將 constant net 加入固定值 clause。
5. 根據 query 類型加入額外限制。
6. 呼叫 CaDiCaL SAT solver。
7. 根據 SAT / UNSAT 判斷功能性質。
```

Boolean expression query 的基本流程：

```text
1. 從指定 net 往 fanin 方向遞迴展開。
2. PI / constant / DFF.Q 視為 leaf。
3. combinational gate 轉成 AND(a, b)、NOT(x)、XOR(a, b) 等字串。
4. 需要避免過長輸出時，可用 depth-limited expression。
```

重要概念：

```text
SAT 表示存在一組 primary input / pseudo primary input assignment 讓條件成立。
UNSAT 表示不存在任何 assignment 讓條件成立。
Boolean expression 是結構展開結果，不代表已經做代數化簡。
```

---

## 3. DFF 處理規則

Function Analysis 遵守目前全專案的 sequential boundary 規則：

```text
DFF output Q 視為 pseudo primary input。
分析 DFF.Q fanin 時，不穿越 DFF 回到 D / CK / RN / SN。
```

例子：

```text
dff ff1(.D(n1), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q));
or g1(y, q, a);
```

分析 `y` 時：

```text
Boolean expression 會得到 OR(q, a)。
q 被視為 leaf / pseudo PI。
不會繼續穿越 ff1 回到 n1。
```

這表示 Function Analysis 目前回答的是「組合邏輯視角」下的功能性質，不是跨 clock cycle 的 sequential equivalence。

---

## 4. 低階 Function Helper

目前提供以下低階 API：

```cpp
bool areNetsEquivalent(const std::string& netA, const std::string& netB) const;
bool canNetBeValue(const std::string& netName, int value) const;
bool isNetConstantFunction(const std::string& netName, int constValue) const;
bool isNetAlwaysZero(const std::string& netName) const;
bool isNetAlwaysOne(const std::string& netName) const;

std::string getBooleanExpression(const std::string& netName) const;
std::string getBooleanExpressionOfNet(int netId) const;
std::string getSimplifiedBooleanExpression(const std::string& netName,
                                           int maxDepth = 10) const;
std::vector<std::string> getPrimaryInputsOfNet(const std::string& netName) const;
```

用途：

| API | 用途 |
|---|---|
| `areNetsEquivalent(a, b)` | 檢查兩個 net / bus 是否功能等價 |
| `canNetBeValue(net, value)` | 檢查 scalar net 是否存在某組輸入可等於 `value` |
| `isNetConstantFunction(net, constValue)` | 檢查 scalar net 是否恆等於 `constValue` |
| `isNetAlwaysZero(net)` | 檢查 scalar net 是否永遠為 0 |
| `isNetAlwaysOne(net)` | 檢查 scalar net 是否永遠為 1 |
| `getBooleanExpression(net)` | 展開完整 Boolean expression |
| `getSimplifiedBooleanExpression(net, maxDepth)` | 產生 depth-limited Boolean expression |
| `getPrimaryInputsOfNet(net)` | 回報 fanin cone 的 PI / DFF.Q pseudo-PI leaves |

LLM / tools 對外應優先使用 `runFunctionQuery()`，不要直接呼叫散裝 helper。

---

## 5. SAT Property API

### 5.1 canNetBeValue

```cpp
bool canNetBeValue(const std::string& netName, int value) const;
```

判斷某條 scalar net 是否存在一組輸入 assignment 可以讓它等於 `value`。

回傳語意：

| 回傳 | 意思 |
|---|---|
| `true` | 存在某組輸入使 net 等於 `value` |
| `false` | 不存在、無法證明、參數不合法、或目前不支援 |

### 5.2 Constant Function

```cpp
bool isNetConstantFunction(const std::string& netName, int constValue) const;
bool isNetAlwaysZero(const std::string& netName) const;
bool isNetAlwaysOne(const std::string& netName) const;
```

核心想法是反例檢查：

```text
要證明 net always 0：檢查 canNetBeValue(net, 1) 是否為 false。
要證明 net always 1：檢查 canNetBeValue(net, 0) 是否為 false。
```

### 5.3 Equivalence

```cpp
bool areNetsEquivalent(const std::string& netA, const std::string& netB) const;
```

目前是既有 `checkEquivalence()` 的語意化 wrapper。

底層做法：

```text
1. 展開 netA / netB，支援 bus。
2. 若 bit width 不同，直接回傳 false。
3. 建立兩邊 fanin cone 的 combined SAT model。
4. 建立 miter：比較每一對 bit 是否不同。
5. 若「存在任一 bit 不同」為 UNSAT，代表兩者等價。
6. 若 SAT，代表找到反例，兩者不等價。
```

#### 5.3.1 Conditional Equivalence

`ConditionalEquivalence` 重用同一份 combined-cone miter，另外把一條 scalar `conditionNetName` 固定為 `conditionValue`。例如：

```text
netNameA = D
netNameB = Q
conditionNetName = EN
conditionValue = 0
```

UNSAT 表示在 `EN=0` 的可行 assignments 中不存在 `D != Q`，可供 DFF hold pattern 驗證。SAT 表示存在違反條件等價的反例。TIMEOUT/UNKNOWN 不得解讀成不等價。

### 5.4 Exact Functional Dependence

`FunctionalDependence` 回答 target Boolean function 是否真的依賴某個 PI / DFF.Q pseudo-PI。定義為：是否存在一組其餘 inputs 的相同 assignment，使 target 在該 input 分別設為 0 與 1 時產生不同輸出。

實作使用雙份 fanin-cone SAT miter：

```text
1. 複製 target combinational cone 兩份。
2. 除 selected input 外，其餘 PI / pseudo-PI leaves 兩份相等。
3. selected input 在 copy 0 固定為 0，copy 1 固定為 1。
4. 要求兩份 target output 不同。
5. SAT 表示 functionally dependent；UNSAT 表示 functionally independent。
```

`PrimaryInputsOfNet` 只是 structural support candidate list。例如 `x XOR x` 結構上含 `x`，但 exact dependence 為 false。

### 5.5 Symmetry Analysis

`Symmetry` 檢查交換兩個獨立輸入後，target Boolean function 是否保持不變：

```text
f(..., A=0, B=1, ...) == f(..., A=1, B=0, ...)
```

實作使用雙份 fanin-cone SAT miter：其餘 PI / DFF.Q leaves 在兩份 cone 中保持相同，第一份固定 `(A,B)=(0,1)`，第二份固定 `(A,B)=(1,0)`，並要求至少一個 target bit 不同。SAT 代表找到非對稱反例；UNSAT 才能證明 symmetric。target 可為 scalar 或 bus，兩個交換輸入必須是不同的 scalar PI 或 DFF.Q pseudo-PI。

若兩個指定輸入都不在 target structural support，交換不可能影響 target，可直接回 `SYMMETRIC` 與 `solverStatus=NOT_NEEDED`。只有一個輸入在 support 時仍必須執行 SAT，不能直接判定 symmetric。

---

## 6. Boolean Expression API

### 6.1 完整展開

```cpp
std::string getBooleanExpression(const std::string& netName) const;
```

用途：

```text
回答 Derive the Boolean equation for output n16.
回答 What Boolean function does output n25 compute?
```

輸出格式：

```text
AND(a, b)
OR(AND(a, b), NOT(c))
NAND(x, y)
XOR(a, b)
XNOR(a, b)
```

注意：

```text
這是 structural expression，不做 algebraic minimization。
大型 cone 可能產生非常長的字串。
```

### 6.2 Depth-limited 展開

```cpp
std::string getSimplifiedBooleanExpression(const std::string& netName,
                                           int maxDepth = 10) const;
```

用途：

```text
當完整 expression 太長時，限制展開深度。
超過 maxDepth 的節點直接以 net name 表示。
```

例子：

```text
完整：NOT(OR(AND(a, b), c))
maxDepth = 1：NOT(n_deep2)
```

### 6.3 Support PI / pseudo-PI

```cpp
std::vector<std::string> getPrimaryInputsOfNet(const std::string& netName) const;
```

用途：

```text
回答 output n12 depends on which primary inputs?
輔助 Boolean expression 回答「in terms of primary inputs」。
```

注意：

```text
DFF.Q 會被列為 pseudo PI leaf。
undriven non-PI leaf 也會保守列入。
```

---

## 7. 高階 FunctionQuery

統一高階入口：

```cpp
FunctionReport runFunctionQuery(const FunctionQuery& query) const;
```

支援的 query type：

| Query type | 用途 |
|---|---|
| `Equivalence` | 檢查兩個 net / bus 是否功能等價 |
| `ConditionalEquivalence` | 固定一條 scalar condition net 後檢查兩個 net / bus 是否等價 |
| `CanBeValue` | 檢查 scalar net 是否可能為指定 0/1 |
| `ConstantFunction` | 檢查 scalar net 是否恆等於指定 0/1 |
| `AlwaysZero` | 檢查 scalar net 是否永遠為 0 |
| `AlwaysOne` | 檢查 scalar net 是否永遠為 1 |
| `TruthStatus` | 分類 scalar net 是 always 0、always 1，或 non-constant |
| `FunctionalDependence` | exact SAT/cofactor 檢查 target 是否依賴某 PI / pseudo-PI |
| `Symmetry` | exact SAT/cofactor 檢查交換兩個 PI / pseudo-PI 後 target 是否不變 |
| `BooleanExpression` | 展開完整 Boolean expression |
| `SimplifiedBooleanExpression` | 展開 depth-limited Boolean expression |
| `PrimaryInputsOfNet` | 回報 fanin cone 的 PI / pseudo-PI leaves |

核心資料結構：

```cpp
struct FunctionQuery {
    FunctionQueryType type;
    std::string netNameA;
    std::string netNameB;
    std::string conditionNetName;
    std::string symmetryInputNameA;
    std::string symmetryInputNameB;
    int conditionValue = -1;
    int constValue = -1;
    int maxExpressionDepth = 10;
};

struct FunctionReport {
    bool ok = false;
    bool exists = false;
    std::string message;

    std::string netNameA;
    std::string netNameB;
    std::string conditionNetName;
    int netIdA = -1;
    int netIdB = -1;
    int constValue = -1;
    int conditionNetId = -1;
    int conditionValue = -1;

    bool equivalent = false;
    bool canBeZero = false;
    bool canBeOne = false;
    bool isConstant = false;
    bool inputInStructuralSupport = false;
    bool dependsOnInput = false;
    bool symmetric = false;
    bool symmetryInputAInStructuralSupport = false;
    bool symmetryInputBInStructuralSupport = false;
    bool counterexampleFound = false;
    std::vector<std::string> mismatchedTargetBitNames;
    std::map<std::string, int> counterexampleAssignments;
    std::map<std::string, int> outputValuesBeforeSwap;
    std::map<std::string, int> outputValuesAfterSwap;
    std::string status;
    bool solverRan = false;
    bool solverTimedOut = false;
    bool solverUnknown = false;
    bool unsupported = false;
    std::string solverStatus;

    std::string expression;
    size_t expressionLength = 0;
    int maxExpressionDepth = -1;
    bool expressionDepthLimited = false;
    std::vector<std::string> supportPrimaryInputs;
};
```

---

## 8. FunctionReport status

`status` 是方便 LLM 或 debug 使用的分類文字。

常見值：

| Status | 意思 |
|---|---|
| `EQUIVALENT` | 兩個 function 等價 |
| `NOT_EQUIVALENT` | 兩個 function 不等價 |
| `SATISFIABLE_VALUE` | 指定值可達 |
| `UNSATISFIABLE_VALUE` | 指定值不可達 |
| `ALWAYS_ZERO` | net 恆為 0 |
| `ALWAYS_ONE` | net 恆為 1 |
| `NON_CONSTANT` | net 可為 0 也可為 1 |
| `FUNCTIONALLY_DEPENDENT` | target Boolean function 依賴 selected input |
| `FUNCTIONALLY_INDEPENDENT` | target Boolean function 不依賴 selected input |
| `SYMMETRIC` | 交換兩個指定 inputs 後所有 target bits 保持不變 |
| `NOT_SYMMETRIC` | SAT 找到交換後至少一個 target bit 改變的反例 |
| `CONDITIONALLY_EQUIVALENT` | condition 固定後兩個 function 等價 |
| `NOT_CONDITIONALLY_EQUIVALENT` | condition 固定後仍存在差異 assignment |
| `BOOLEAN_EXPRESSION` | 已產生完整 Boolean expression |
| `SIMPLIFIED_BOOLEAN_EXPRESSION` | 已產生 depth-limited Boolean expression |
| `PRIMARY_INPUT_SUPPORT` | 已回報 fanin support leaves |
| `SCALAR_NET_REQUIRED` | 此 query 需要 existing scalar net |
| `NET_NOT_FOUND` | 指定 net 不存在 |
| `INVALID_ARGUMENT` | query 欄位不完整或參數不合法 |
| `SOLVER_TIMEOUT` | SAT solver 在 time limit 內未完成 |
| `SOLVER_UNKNOWN` | SAT solver 回 UNKNOWN，不能當成 false |
| `UNSUPPORTED` | 目前無法建模或不支援此 function cone |

SAT 類 report 欄位：

| 欄位 | 意思 |
|---|---|
| `solverRan` | 是否真的呼叫 SAT solver |
| `solverTimedOut` | 是否因 time limit 中止 |
| `solverUnknown` | 是否收到 UNKNOWN |
| `unsupported` | 是否因不支援而無法建模 |
| `solverStatus` | `SAT`、`UNSAT`、`NOT_NEEDED`、`TIMEOUT`、`UNKNOWN`、`UNSUPPORTED` |

---

## 9. 目前限制

目前完成的是 function analysis 的高階 query 第一版。

限制：

```text
1. canNetBeValue / isNetConstantFunction / TruthStatus 只支援 scalar net。
2. Equivalence 支援 bus，但 ConstantFunction 類還沒支援整個 bus 常數分類。
3. Symmetry 會回傳非對稱 counterexample；其他 SAT query 目前不一定回傳 witness assignment。
4. Boolean expression 是 structural expansion，不做 algebraic simplification。
5. Boolean expression 對大型 cone 可能很長，建議使用 SimplifiedBooleanExpression。
6. 目前不是 sequential equivalence checking，不跨 DFF cycle。
7. SAT solver timeout / UNKNOWN 會回報 `ok=false` 與 `solverTimedOut` / `solverUnknown`，不再被混成普通 false。
8. FunctionalDependence 目前要求 scalar target 與 scalar PI / DFF.Q pseudo-PI input，不把 internal driven net 當成可獨立切換的 input。
9. Symmetry target 支援 scalar/bus；交換輸入只接受兩個不同的 scalar PI / DFF.Q pseudo-PI，不把 internal driven net 當成獨立變數。
```

---

## 10. 目前完成狀態

實作位置：

```text
include/core/NetlistQueries.h
include/core/Netlist.h
src/analysis/FunctionAnalysis.cpp
src/analysis/Booleanexpression.cpp
```

已完成：

```text
areNetsEquivalent()
canNetBeValue()
isNetConstantFunction()
isNetAlwaysZero()
isNetAlwaysOne()
getBooleanExpression()
getSimplifiedBooleanExpression()
getPrimaryInputsOfNet()
FunctionalDependence dual-cone SAT miter
Symmetry dual-cone SAT miter、bus target 與 counterexample report
FunctionQuery / FunctionReport / runFunctionQuery()
```

測試狀態：

```text
mini test/test4/test4.cpp 已覆蓋 FunctionQuery 的 BooleanExpression、
SimplifiedBooleanExpression、PrimaryInputsOfNet、DFF.Q boundary、SAT-based
AlwaysZero / AlwaysOne / TruthStatus / CanBeValue / Equivalence、invalid query。
目前 test4 結果：Summary: 15 passed, 0 failed.
CLI integration regression test9-test17：150 passed, 0 failed。
```

未來可補：

```text
bus constant status
SAT counterexample model extraction
expression algebraic simplification
expression size budget / streaming mode
bounded sequential analysis
```
