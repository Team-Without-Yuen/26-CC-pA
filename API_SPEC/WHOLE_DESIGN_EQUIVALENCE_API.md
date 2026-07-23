# Whole Design Equivalence API 整理

這份文件說明 `checkWholeDesignEquivalence()` 的用途與回傳格式。它用來回答「目前設計是否仍與 original design 功能等價」這類 prompt。

使用方式與 prompt 範例請看：

```text
API_SPEC/WHOLE_DESIGN_EQUIVALENCE_USAGE.md
```

---

## 1. 基本概念

`WholeDesignEquivalence` 是跨兩個 netlist 的整體功能檢查，不是單一 net 的 function query，也不是 edit pass 本身的 local certificate。

基本形式：

```text
current.checkWholeDesignEquivalence(original, totalTimeBudgetSeconds) -> WholeDesignEquivalenceReport
```

使用時機：

```text
1. prompt 明確要求 verify / confirm / prove current design equivalent to original
2. edit / optimization 已完成，使用者追問前一步是否保持整體功能
3. 需要指出是哪個 primary output 或 DFF.D next-state function 不等價
```

不建議每次 `runEditApply()` 都自動呼叫，因為 whole-design SAT 比 local rewrite certificate 更重。一般 edit command 仍先使用 `NetlistEditReport.validation` 中的 local / structural certificate；只有題目明確要求整體驗證時才呼叫此 API。

---

## 2. API

```cpp
Netlist::WholeDesignEquivalenceReport report =
    current.checkWholeDesignEquivalence(original);
```

可選擇指定整體時間 budget：

```cpp
Netlist::WholeDesignEquivalenceReport report =
    current.checkWholeDesignEquivalence(original, 240.0);
```

預設 `totalTimeBudgetSeconds = 240.0`。官方非 basic request 時限為 300 秒，因此這裡保留約 60 秒給 parsing、routing、report formatting 與其他工具流程。

語意：

```text
original: 修改前或題目給定的 baseline design
current : 修改後或目前正在回答的 design
```

目前會比對：

```text
1. 同名 primary inputs
2. 同名 primary outputs
3. 同名 DFF instances
4. 每個同名 primary output 的 Boolean function
5. 每個同名 DFF 的 D-input next-state function
```

兩側的 PI 依 net name 共用 SAT variable；DFF.Q 則依 DFF instance name 共用 SAT variable。因此即使 edit 改變了 Q net name，只要 DFF instance interface 不變，仍可正確對齊 sequential boundary。這是 combinational-boundary equivalence，不是跨 cycle sequential equivalence。

---

## 3. Report 欄位

| 欄位 | 意思 |
|---|---|
| `ok` | 檢查是否成功完成；interface mismatch、unsupported logic 或 SAT timeout 時可能為 false |
| `equivalent` | whole design 是否功能等價 |
| `message` | 簡短狀態說明 |
| `comparedOutputCount` | 實際做 SAT 比較的 PO 數量 |
| `comparedDffDCount` | 實際做 SAT 比較的 DFF.D endpoint 數量 |
| `matchedOutputNames` | 已證明等價的 PO |
| `mismatchedOutputNames` | 發現不等價的 PO |
| `matchedDffDNames` | 已證明等價的 DFF.D，名稱格式為 `instance.D` |
| `mismatchedDffDNames` | 發現不等價的 DFF.D |
| `missingInputNames` | original 有、current 沒有的 PI |
| `extraInputNames` | current 多出的 PI |
| `missingOutputNames` | original 有、current 沒有的 PO |
| `extraOutputNames` | current 多出的 PO |
| `missingDffNames` | original 有、current 沒有的 DFF instance |
| `extraDffNames` | current 多出的 DFF instance |
| `unsupportedReasons` | 不支援或 SAT 未完成的原因 |
| `warnings` | 限制或假設提醒 |
| `method` | 目前應為 `WholeDesignSat` |
| `timeBudgetExceeded` | 是否因整體時間 budget 不足而停止剩餘 PO 比較 |
| `skippedOutputCount` | 因時間不足未比較的 PO 數量 |
| `skippedDffDCount` | 未完成比較的 DFF.D 數量 |
| `timeBudgetSeconds` | 本次呼叫使用的整體時間 budget |
| `skippedOutputNames` | 因時間不足未比較的 PO 名稱 |
| `skippedDffDNames` | 未完成比較的 DFF.D 名稱 |

時間語意：

```text
1. original/current 各只編碼一次，所有 PO 與 DFF.D 共用同一個 SAT context。
2. 先用單一 global endpoint miter 檢查是否存在任何 mismatch；UNSAT 可一次證明全部 endpoints。
3. 只有 global miter 為 SAT 時，才逐 endpoint 求解以定位 mismatch。
4. 每次 SAT solve 最多 30 秒，且所有 solves 共用 `totalTimeBudgetSeconds`。
5. budget 或 solver 無法完成的 endpoints 會進入 skipped list；此時 `report.ok = false`，不可宣稱已證明等價。
```

---

## 4. 範例

```cpp
Netlist original;
Netlist current;

// original/current 由 VerilogReader 或前後版本流程建立
Netlist::WholeDesignEquivalenceReport report =
    current.checkWholeDesignEquivalence(original);

if (report.ok && report.equivalent) {
    // Current design is equivalent to original.
}

if (!report.mismatchedOutputNames.empty()) {
    // These POs differ from original.
}

if (!report.mismatchedDffDNames.empty()) {
    // These DFF next-state functions differ from original.
}

if (report.timeBudgetExceeded) {
    // Some outputs were not checked because the whole-design budget was exhausted.
}
```

常見回答方式：

```text
The current design is functionally equivalent to the original design at all PO and DFF.D combinational boundaries.
Compared outputs: y, z. Compared DFF.D endpoints: ff0.D, ff1.D.
Method: WholeDesignSat.
```

若有 mismatch：

```text
The current design is not equivalent to the original design.
Mismatched outputs: z. Mismatched DFF.D endpoints: ff1.D.
```

若介面不同：

```text
Whole-design equivalence was not attempted because the interface differs.
Missing outputs: z.
Extra outputs: extra.
```

若時間不足：

```text
Whole-design equivalence was not fully completed within the time budget.
Compared outputs: 18. Compared DFF.D endpoints: 42.
Skipped endpoints: out19, ff43.D.
```

---

## 5. 限制

第一版限制：

```text
1. 以同名 PI / PO 與同名 DFF instance 對齊，不做 rename inference。
2. 比較 PO 與 DFF.D 的 combinational function。
3. DFF.Q 視為 sequential boundary leaf，不分析 DFF initial state。
4. 不做 sequential equivalence / multi-cycle equivalence。
5. 若 PI/PO/DFF 介面不同，會回報 missing / extra，不強行比較。
6. 大型設計可能因整體時間 budget 跳過部分 endpoints；此時不視為已證明等價。
7. AND/OR/NAND/NOR 與 XOR/XNOR 的任意 fanin combinational gate 均可編碼；多輸入 XOR/XNOR 使用 auxiliary parity chain。
```

DFF 設計的解讀：

```text
若設計包含 DFF，report.warnings 會提醒：
DFF outputs are treated as sequential boundary leaves; DFF.D next-state functions are compared, but initial state is not analyzed.
```

---

## 6. 與其他 API 的差異

| 問題 | 應使用 |
|---|---|
| 兩條 internal net 是否等價 | `FunctionQuery::Equivalence` |
| 某條 net 是否 always 0 / always 1 | `FunctionQuery` |
| 一個 edit command 是否由 local rewrite 保證等價 | `runEditApply()` 的 `NetlistEditReport.validation` |
| 修改後整個 design 是否等價於 original | `checkWholeDesignEquivalence()` |

---

## 7. Prompt Routing

適合 routing 到此 API 的 prompt：

```text
Verify the current design is equivalent to the original.
Confirm the previous edit preserved whole-design functionality.
Check whether any primary output or register next-state function changed after optimization.
Which outputs or DFF.D endpoints differ from the original design?
```

不適合 routing 到此 API 的 prompt：

```text
Are n1 and n2 equivalent?
  -> FunctionQuery::Equivalence

Did CleanupBuffers preserve function?
  -> 先讀 NetlistEditReport.validation；若題目要求 whole-design，再呼叫本 API。

Count gates / list fanout / find cone.
  -> BasicQuery / DirectConnectivityQuery / ConeQuery
```
