# Whole Design Equivalence 使用說明

這份文件只負責說明 `checkWholeDesignEquivalence()` 與公開 `equiv_query` command 的使用方式。

設計背景、SAT 流程與完整 report contract 請看：

```text
API_SPEC/WHOLE_DESIGN_EQUIVALENCE_API.md
```

---

## 1. 基本概念與入口

Whole-design equivalence 比較 current design 與一個 baseline，在所有同名 PO 與 DFF.D next-state boundary 上確認功能是否相同。

```cpp
Netlist::WholeDesignEquivalenceReport report =
    current.checkWholeDesignEquivalence(original, 240.0);
```

公開 CLI：

```text
equiv_query original [time_budget_seconds]
equiv_query previous_edit [time_budget_seconds]
```

`original` 是最近一次成功 `read` 後保存的原始設計；`previous_edit` 是最近一次 edit 前的 snapshot。

## 2. Mode 總表

| Mode | Baseline | 主要用途 |
|---|---|---|
| `original` | 初次成功載入的 design | 驗證目前所有修改後的設計是否仍等價於題目原始設計 |
| `previous_edit` | 最近一次 edit 前的 design | 驗證上一個修改步驟是否保持功能 |

## 3. 輸入欄位

| 輸入 | 型別/預設 | 語意 |
|---|---|---|
| baseline mode | `original` 或 `previous_edit` | 決定比較對象 |
| `time_budget_seconds` | 正數，預設 `240` | 所有 SAT solve 共用的整體時間 budget |

必要條件：

```text
original mode 需要先成功 read design。
previous_edit mode 需要至少有一次成功進入 edit transaction。
time budget 必須大於 0。
```

## 4. 回傳欄位與狀態判讀

| 想知道 | 主要欄位 |
|---|---|
| 是否完成檢查 | `report.ok` |
| 是否已證明等價 | `report.ok && report.equivalent` |
| 哪些 PO 不等價 | `mismatchedOutputNames` |
| 哪些 DFF.D 不等價 | `mismatchedDffDNames` |
| interface 是否不同 | `missing*Names` / `extra*Names` |
| 是否超時或未完整 | `timeBudgetExceeded`、`skippedOutputNames`、`skippedDffDNames` |
| 實際比較數量 | `comparedOutputCount`、`comparedDffDCount` |

判讀規則：

```text
ok=true, equivalent=true   -> 已完整證明等價。
ok=true, equivalent=false  -> 已找到明確 mismatch。
ok=false                   -> interface mismatch、unsupported、timeout 或結果不完整；不得解讀成等價。
```

## 5. 使用範例

### 5.1 與 original 比較

```text
read design.v
edit_apply cleanup_buffers
equiv_query original 240
```

### 5.2 驗證上一個 edit

```text
edit_apply collapse_double_inverter
equiv_query previous_edit 60
```

### 5.3 C++ 判讀

```cpp
const auto report = current.checkWholeDesignEquivalence(original, 240.0);
if (report.ok && report.equivalent) {
    // All compared PO and DFF.D boundaries are equivalent.
} else if (report.ok) {
    // Read mismatchedOutputNames and mismatchedDffDNames.
} else {
    // Read interface, timeout, skipped and unsupported fields.
}
```

## 6. Prompt 對應

| Prompt 語意 | Command | 主要讀取 |
|---|---|---|
| Verify the current design is equivalent to the original. | `equiv_query original` | `ok`, `equivalent` |
| Did the previous edit preserve functionality? | `equiv_query previous_edit` | `ok`, `equivalent` |
| Which outputs changed after optimization? | `equiv_query original` | mismatch name lists |

## 7. 何時不要使用

| 問題 | 應使用 |
|---|---|
| 同一張 netlist 中兩條 internal nets 是否等價 | `FunctionQuery::Equivalence` |
| edit 本身是否有 local rewrite certificate | `NetlistEditReport.validation` |
| 只想取得上一個 edit 的 before/after 數量 | `report_query last_edit` |

Whole-design SAT 的成本高於 structural/local certificate。題目沒有要求 whole-design verification 時，不需要在每個 edit 後自動執行。

## 8. 實作與測試狀態

```text
Header：include/core/NetlistEditReport.h、include/core/Netlist.h
Source：src/analysis/WholeDesignEquivalence.cpp
C++ regression：mini test/test3、test7、test20
CLI regression：mini test/test12、test14、test16
```

目前比較 PO 與 DFF.D combinational boundary；DFF initial state 與 multi-cycle sequential equivalence 不在此 API 範圍。
