# Function Analysis API 整理

這份文件整理 `Netlist` 的 Boolean / function analysis API，也就是回答「訊號功能是否等價、是否可能為某值、是否恆為常數」這類問題。

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
```

這一層不做：

```text
單純物件資訊查詢 -> BasicQuery
直接相連查詢 -> DirectConnectivityQuery
transitive reachable / fanin / fanout -> ConeQuery
指定 startpoint 到 endpoint 的 path search -> PathQuery
timing depth / critical path -> DepthAnalysis
Boolean expression derivation
optimization rewrite
```

---

## 2. 底層方法

目前 Function Analysis 使用 SAT-based 方法。

基本流程：

```text
1. 找到目標 net 或兩個比較目標。
2. 從目標 net 往 fanin 方向展開 combinational cone。
3. 將 cone 內 primitive gates 轉成 CNF。
4. 將 constant net 加入固定值 clause。
5. 根據 query 類型加入額外限制。
6. 呼叫 CaDiCaL SAT solver。
7. 根據 SAT / UNSAT 判斷功能性質。
```

重要概念：

```text
SAT 表示存在一組 primary input / pseudo primary input assignment 讓條件成立。
UNSAT 表示不存在任何 assignment 讓條件成立。
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
buf g1(y, q);
```

分析 `y` 時：

```text
y 的 fanin 會包含 q。
q 被視為一個自由輸入。
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
```

用途：

| API | 用途 |
|---|---|
| `areNetsEquivalent(a, b)` | 檢查兩個 net / bus 是否功能等價 |
| `canNetBeValue(net, value)` | 檢查 scalar net 是否存在某組輸入可等於 `value` |
| `isNetConstantFunction(net, constValue)` | 檢查 scalar net 是否恆等於 `constValue` |
| `isNetAlwaysZero(net)` | 檢查 scalar net 是否永遠為 0 |
| `isNetAlwaysOne(net)` | 檢查 scalar net 是否永遠為 1 |

---

## 5. canNetBeValue

```cpp
bool canNetBeValue(const std::string& netName, int value) const;
```

用途：

```text
判斷某條 scalar net 是否存在一組輸入 assignment 可以讓它等於 value。
```

內部流程：

```text
1. 檢查 value 是否為 0 或 1。
2. 用 expandNetToBits(netName) 解析目標 net。
3. 第一版只支援 scalar net；若展開後不是 1 bit，回傳 false。
4. 從 target net 往 fanin BFS。
5. 遇到 combinational driver gate，就加入 gatesToEncode，並繼續追它的 input nets。
6. 遇到 DFF driver gate，停止展開，因為 DFF.Q 視為 pseudo PI。
7. 建立 CaDiCaL solver。
8. 將 visited constant net 加入固定值 clause。
9. 將 gatesToEncode 內的 primitive gates 轉成 CNF。
10. 加入 target net 等於 value 的 clause。
11. SAT 表示可能為該值；UNSAT / UNKNOWN 回傳 false。
```

回傳語意：

| 回傳 | 意思 |
|---|---|
| `true` | 存在某組輸入使 net 等於 `value` |
| `false` | 不存在、無法證明、參數不合法、或目前不支援 |

---

## 6. Constant Function 判斷

```cpp
bool isNetConstantFunction(const std::string& netName, int constValue) const;
bool isNetAlwaysZero(const std::string& netName) const;
bool isNetAlwaysOne(const std::string& netName) const;
```

核心想法是反例檢查：

```text
要證明 net always 0：
    檢查 canNetBeValue(net, 1) 是否為 false。

要證明 net always 1：
    檢查 canNetBeValue(net, 0) 是否為 false。
```

對照：

| API | 等價邏輯 |
|---|---|
| `isNetConstantFunction(net, 0)` | `!canNetBeValue(net, 1)` |
| `isNetConstantFunction(net, 1)` | `!canNetBeValue(net, 0)` |
| `isNetAlwaysZero(net)` | `isNetConstantFunction(net, 0)` |
| `isNetAlwaysOne(net)` | `isNetConstantFunction(net, 1)` |

注意：

```text
函式會先確認 net 是 existing scalar net。
找不到 net 或傳入 bus 時，不會誤判成 constant。
```

---

## 7. Equivalence 判斷

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

---

## 8. 高階 FunctionQuery

目前已提供統一高階入口：

```cpp
FunctionReport runFunctionQuery(const FunctionQuery& query) const;
```

支援的 query type：

| Query type | 用途 |
|---|---|
| `Equivalence` | 檢查兩個 net / bus 是否功能等價 |
| `CanBeValue` | 檢查 scalar net 是否可能為指定 0/1 |
| `ConstantFunction` | 檢查 scalar net 是否恆等於指定 0/1 |
| `AlwaysZero` | 檢查 scalar net 是否永遠為 0 |
| `AlwaysOne` | 檢查 scalar net 是否永遠為 1 |
| `TruthStatus` | 分類 scalar net 是 always 0、always 1，或 non-constant |

核心資料結構：

```cpp
struct FunctionQuery {
    FunctionQueryType type;
    std::string netNameA;
    std::string netNameB;
    int constValue = -1;
};

struct FunctionReport {
    bool ok = false;
    bool exists = false;
    std::string message;

    std::string netNameA;
    std::string netNameB;
    int netIdA = -1;
    int netIdB = -1;
    int constValue = -1;

    bool equivalent = false;
    bool canBeZero = false;
    bool canBeOne = false;
    bool isConstant = false;
    std::string status;
};
```

---

## 9. FunctionReport status

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
| `SCALAR_NET_REQUIRED` | 此 query 需要 existing scalar net |
| `INVALID_ARGUMENT` | query 欄位不完整或參數不合法 |
| `UNSAT_OR_UNSUPPORTED` | 無法證明任一值可達，可能是 unsupported 或建模問題 |

---

## 10. 目前限制

目前完成的是第一版 Function Analysis。

限制：

```text
1. canNetBeValue / isNetConstantFunction / TruthStatus 只支援 scalar net。
2. Equivalence 支援 bus，但 ConstantFunction 類還沒支援整個 bus 常數分類。
3. 目前不回傳 SAT counterexample assignment。
4. 目前不產生 Boolean expression。
5. 目前不是 sequential equivalence checking，不跨 DFF cycle。
6. SAT solver timeout / UNKNOWN 會保守回傳 false。
```

---

## 11. 目前完成狀態

已完成：

```text
areNetsEquivalent()
canNetBeValue()
isNetConstantFunction()
isNetAlwaysZero()
isNetAlwaysOne()
FunctionQuery / FunctionReport / runFunctionQuery()
mini test 覆蓋所有 FunctionQueryType
```

未來可補：

```text
bus constant status
batch classify all primary outputs
Boolean equation derivation
SAT counterexample model extraction
bounded sequential analysis
```
