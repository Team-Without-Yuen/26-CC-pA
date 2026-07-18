# Sequential Pattern Query 使用說明

這份文件只負責說明 `SequentialPatternQuery` 與 `sequential_query` 的使用方式。

設計背景、pattern 定義與 report contract 請看：

```text
API_SPEC/SEQUENTIAL_PATTERN_API.md
```

---

## 1. Include 與入口

```cpp
#include "include/core/Netlist.h"

Netlist::SequentialPatternReportSet report =
    netlist.runSequentialPatternQuery(query);
```

## 2. 分析所有 DFF

```cpp
Netlist::SequentialPatternQuery query;
query.type = Netlist::SequentialPatternQueryType::DffEnableHold;
query.dffName.clear();
query.includeAndGatedCandidates = true;
query.verifyCanonicalMatchesWithSat = false;

const auto report = netlist.runSequentialPatternQuery(query);
```

主要讀取欄位：

```cpp
report.analyzedDffCount
report.matchedDffCount
report.candidateDffCount
report.reports
```

`matchedDffCount` 只統計具有 confirmed canonical feedback-MUX pattern 的 unique DFF。`candidateDffCount` 還會包含只有 AND-gated candidate、但官方語意仍未確定的 DFF。

## 3. 分析指定 DFF

```cpp
Netlist::SequentialPatternQuery query;
query.type = Netlist::SequentialPatternQueryType::DffEnableHold;
query.dffName = "ff1";

const auto report = netlist.runSequentialPatternQuery(query);
```

指定名稱不存在或不是 DFF 時，會回傳：

```text
ok = false
status = DFF_NOT_FOUND
```

## 4. 讀取 confirmed pattern

```cpp
for (const auto& dff : report.reports) {
    for (const auto& pattern : dff.patterns) {
        if (!pattern.confirmed) {
            continue;
        }

        std::cout << dff.dffName << " enable=" << pattern.enableNetName
                  << " data=" << pattern.dataNetName
                  << " branch=" << pattern.dataBranchNetName
                  << " feedback=" << pattern.feedbackNetName
                  << " activeLevel=" << pattern.activeLevel << "\n";
    }
}
```

如果 pattern 等價於：

```text
D = EN ? DATA : Q
```

report 會回傳：

```text
kind = MuxHold
enableNetName = EN
dataNetName = DATA
feedbackNetName = Q
activeLevel = 1
confirmed = true
```

## 5. Active-high 與 Active-low

Active-high enable：

```text
D = EN ? DATA : Q
activeLevel = 1
```

Active-low enable：

```text
D = EN_N ? Q : DATA
activeLevel = 0
```

LLM 回答時應根據 `activeLevel` 說明在哪一個 enable value 下載入新資料，以及在哪一個 value 下保持 Q。

## 6. 選擇性 SAT 驗證

```cpp
query.verifyCanonicalMatchesWithSat = true;
```

開啟後，每一個 canonical match 都會透過 `FunctionQuery::ConditionalEquivalence` 檢查 inactive 與 active cofactors。

```text
inactive：D == Q
active：D == data branch
```

SAT 驗證不是 batch query 的預設行為，因為對數千顆 DFF 分別啟動 solver 可能增加大量執行時間。Canonical structural match 本身已具有精確的代數語意，SAT 欄位主要提供額外證據與 timeout 狀態。

若前一步已執行 technology mapping，API 會把 `NOT(x)`、`NAND(x,x)` 與 `NOR(x,x)` 統一視為反相 literal，也會跨越成對反相 wrapper。呼叫方式不需要改變。

主要讀取欄位：

| 欄位 | 語意 |
| --- | --- |
| `holdFunctionallyProven` | inactive cofactor 已證明等於 Q |
| `loadFunctionallyProven` | active cofactor 已證明等於 data branch |
| `solverRan` | 是否真的啟動 SAT solver |
| `solverTimedOut` | 是否超過 SAT time limit |
| `solverUnknown` | solver 是否回傳 UNKNOWN |
| `solverStatus` | `PROVEN`、`FAILED`、`TIMEOUT`、`UNKNOWN` 或 `UNSUPPORTED` |

## 7. AND-gated Candidate

在官方定義尚未確認前，以下結構：

```text
D = EN & DATA
```

只會回傳：

```text
kind = AndGatedDataCandidate
confirmed = false
semanticsPending = true
```

這種 DFF 會計入 `candidateDffCount`，但不會計入 `matchedDffCount`。

## 8. Prompt 對應

| Prompt | 主要讀取欄位 |
| --- | --- |
| Report D-input enable/hold structures. | `reports[].patterns` |
| How many flip-flops have enable/hold structures? | `matchedDffCount` |
| Which signal enables ff1? | `enableNetName`, `activeLevel` |
| Which net provides hold feedback? | `feedbackNetName` |
| Which DFFs contain AND-gated candidates? | `kind`, `semanticsPending` |

## 9. tools CLI 使用

數量題：

```text
sequential_query enable_hold all --summary-only
```

讀取 `matched_dff_count` 作為 confirmed unique DFF 數量；`candidate_dff_count` 包含 AND-only semantics-pending candidates，兩者不可混用。

詳細列舉採固定分頁：

```text
sequential_query enable_hold all
sequential_query enable_hold all --offset 50 --limit 50
sequential_query enable_hold ff1 --verify-sat
```

all-DFF detail 預設最多 50 筆。`records_truncated:true` 時必須依 `next_record_offset` 取得下一頁；尚未讀完時 envelope 為 `partial`。`--verify-sat` 只接受指定 DFF，避免在大型 testcase 對數千顆 DFF 個別啟動 solver。

## 10. 不屬於這個 API 的問題

| 問題 | 應使用 API |
| --- | --- |
| 列出所有 DFF | `BasicQuery` |
| 列出 clock n0 驅動的 DFF | `DirectConnectivityQuery` |
| 查 D-input fanin cone | `ConeQuery` |
| 查 Q-to-D path | `PathQuery` |
| 查 DFF.D depth | `DepthQuery` |
| 產生 D-input Boolean expression | `FunctionQuery` |
| 插入或修改 MUX | `EditApply` |
| 最佳化 register logic | `OptimizationFlow` |

這個 API 是唯讀分析，不會修改 netlist，也不負責 MUX insertion、technology mapping 或 optimization。
