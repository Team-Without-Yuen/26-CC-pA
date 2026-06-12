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
query.type + netNameA/netNameB/constValue -> runFunctionQuery() -> FunctionReport
```

使用流程：

```text
1. 建立 Netlist::FunctionQuery query
2. 設定 query.type
3. 視 query.type 設定 query.netNameA / query.netNameB / query.constValue
4. 呼叫 netlist.runFunctionQuery(query)
5. 從 report.ok / report.exists / report.status / report.* 欄位讀結果
```

注意：

```text
FunctionQuery 是功能分析，不是連線分析。
它會用 SAT model 判斷某個功能性質是否成立。
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
| `CanBeValue` | scalar net 是否可能為指定值 | `netNameA`, `constValue` | `exists`, `canBeZero`, `canBeOne`, `status` |
| `ConstantFunction` | scalar net 是否恆等於指定值 | `netNameA`, `constValue` | `exists`, `isConstant`, `constValue`, `status` |
| `AlwaysZero` | scalar net 是否永遠為 0 | `netNameA` | `exists`, `isConstant`, `status` |
| `AlwaysOne` | scalar net 是否永遠為 1 | `netNameA` | `exists`, `isConstant`, `status` |
| `TruthStatus` | 分類 scalar net 的 truth status | `netNameA` | `canBeZero`, `canBeOne`, `isConstant`, `constValue`, `status` |

---

## 3. FunctionQuery 輸入欄位

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `type` | `FunctionQueryType` | `TruthStatus` | 決定要執行哪一種 function query |
| `netNameA` | `std::string` | `""` | 主要分析目標；大部分 query 都需要 |
| `netNameB` | `std::string` | `""` | `Equivalence` 使用的第二個 net / bus |
| `constValue` | `int` | `-1` | `CanBeValue` / `ConstantFunction` 使用；只能是 0 或 1 |

---

## 4. FunctionReport 回傳欄位

### 4.1 共用狀態

| 欄位 | 意思 |
|---|---|
| `ok` | query 是否成功執行 |
| `exists` | yes/no 問題的主要答案 |
| `message` | debug / LLM response 用的簡短訊息 |
| `status` | 更明確的分類字串 |

### 4.2 目標資訊

| 欄位 | 意思 |
|---|---|
| `netNameA` | 查詢目標 A |
| `netNameB` | 查詢目標 B |
| `netIdA` | scalar net A 的 ID；bus 或不存在時可為 -1 |
| `netIdB` | scalar net B 的 ID；bus 或不存在時可為 -1 |
| `constValue` | query 指定或推導出的常數值；unknown 時為 -1 |

### 4.3 功能結果

| 欄位 | 意思 |
|---|---|
| `equivalent` | `Equivalence` 的結果 |
| `canBeZero` | 目標 scalar net 是否可能為 0 |
| `canBeOne` | 目標 scalar net 是否可能為 1 |
| `isConstant` | 目標 scalar net 是否為 constant function |

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

解讀：

| 結果 | 意思 |
|---|---|
| `exists = true` | 目標值可達 |
| `exists = false` | 目標值不可達，或目前無法證明 |

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
| false | false | `UNSAT_OR_UNSUPPORTED` | 無法證明任一值可達 |

---

## 10. Prompt 對應表

| Prompt | 建議 FunctionQueryType | 需要設定 | 主要讀取 |
|---|---|---|---|
| Is output n16 always 0 regardless of all inputs? | `AlwaysZero` | `netNameA = "n16"` | `exists`, `status` |
| Is n16 always 1? | `AlwaysOne` | `netNameA = "n16"` | `exists`, `status` |
| Is n16 a constant 0 function? | `ConstantFunction` | `netNameA = "n16"`, `constValue = 0` | `exists`, `isConstant` |
| Can n16 ever become 1? | `CanBeValue` | `netNameA = "n16"`, `constValue = 1` | `exists` |
| Can n16 ever become 0? | `CanBeValue` | `netNameA = "n16"`, `constValue = 0` | `exists` |
| Are y and z equivalent? | `Equivalence` | `netNameA = "y"`, `netNameB = "z"` | `equivalent` |
| Classify the truth status of n16. | `TruthStatus` | `netNameA = "n16"` | `status`, `canBeZero`, `canBeOne` |
| Is bus a equivalent to bus b? | `Equivalence` | `netNameA = "a"`, `netNameB = "b"` | `equivalent` |

---

## 11. 何時不要用 FunctionQuery

| 問題 | 應改用 |
|---|---|
| How many gates are in the design? | BasicQuery |
| Which gate directly drives n1? | DirectConnectivityQuery |
| Report every gate connected to output of g0. | DirectConnectivityQuery |
| Determine all gates reachable from n2. | ConeQuery |
| Is there a path from a to y? | PathQuery |
| Find all paths from a to y avoiding g3. | PathQuery |
| What is the maximum depth to output y? | DepthAnalysis |
| Derive the Boolean equation for n16. | Future Boolean expression derivation |

---

## 12. 低階 Helper 對照

| FunctionQueryType | 底層 helper |
|---|---|
| `Equivalence` | `areNetsEquivalent()` / `checkEquivalence()` |
| `CanBeValue` | `canNetBeValue()` |
| `ConstantFunction` | `isNetConstantFunction()` |
| `AlwaysZero` | `isNetAlwaysZero()` |
| `AlwaysOne` | `isNetAlwaysOne()` |
| `TruthStatus` | `canNetBeValue(net, 0)` + `canNetBeValue(net, 1)` |

---

## 13. 常見使用範例

### 13.1 回答 yes/no

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::AlwaysZero;
query.netNameA = "n16";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);

std::string answer = (report.ok && report.exists) ? "yes" : "no";
```

### 13.2 查 non-constant

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::TruthStatus;
query.netNameA = "y";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
if (report.status == "NON_CONSTANT") {
    // y can be both 0 and 1
}
```

### 13.3 查等價

```cpp
Netlist::FunctionQuery query;
query.type = Netlist::FunctionQueryType::Equivalence;
query.netNameA = "y";
query.netNameB = "z";

Netlist::FunctionReport report = netlist.runFunctionQuery(query);
bool equivalent = report.ok && report.equivalent;
```

---

## 14. 目前限制

使用時要注意：

```text
1. AlwaysZero / AlwaysOne / CanBeValue / TruthStatus 目前只支援 existing scalar net。
2. Equivalence 支援 bus，但兩邊 bit width 必須一致。
3. 目前不回傳反例 input assignment。
4. 目前不輸出 Boolean equation。
5. DFF.Q 是 pseudo PI；這不是 sequential equivalence。
6. 若 SAT solver timeout 或 UNKNOWN，結果會保守地偏向 false / unsupported。
```

---

## 15. 目前實作與測試狀態

```text
實作檔案：src/analysis/FunctionAnalysis.cpp
型別檔案：include/core/NetlistQueries.h
tester：mini test/tester.cpp
目前 regression：Summary: 45 passed, 0 failed.
```
