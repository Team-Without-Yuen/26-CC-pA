# Sequential Pattern Query 使用說明

這份文件主要說明 C++ 高階 API 的呼叫與 report 讀法。設計與證明語意請看 `API_SPEC/SEQUENTIAL_PATTERN_API.md`；公開 CLI grammar、成本控制與輸出欄位請看 `TOOLS_SPEC/SEQUENTIAL_QUERY_TOOL.md`。functional fallback 已由 `tools.cpp` 以 opt-in 方式公開，未指定 `--functional-fallback` 時仍維持 canonical/default 行為。

---

## 1. 預設快速查詢

```cpp
#include "include/core/Netlist.h"

Netlist::SequentialPatternQuery query;
query.type = Netlist::SequentialPatternQueryType::DffEnableHold;
query.dffName.clear();
query.includeAndGatedCandidates = true;

const auto report = netlist.runSequentialPatternQuery(query);
```

預設 `enableFunctionalFallback=false`，只跑 canonical fast path。大型 testcase 的 summary 應先使用這個模式。

## 2. 指定 DFF

```cpp
Netlist::SequentialPatternQuery query;
query.dffName = "ff1";

const auto report = netlist.runSequentialPatternQuery(query);
```

不存在或不是 DFF 時：

```text
ok = false
status = DFF_NOT_FOUND
```

## 3. 開啟任意功能等價 MUX 搜尋

```cpp
Netlist::SequentialPatternQuery query;
query.dffName = "ff1";
query.enableFunctionalFallback = true;
query.maxFunctionalCandidates = 64;
query.maxFunctionalMatchesPerDff = 8;
query.findAllFunctionalMatches = true;
query.resolveFunctionalDataNets = true;
query.maxFunctionalDataCandidatesPerMatch = 16;
query.enableFunctionalSimulationFilter = true;
query.functionalSimulationPatternCount = 256;
query.functionalPerDffTimeLimitSeconds = 0.25;
query.functionalTimeLimitSeconds = 5.0;

const auto report = netlist.runSequentialPatternQuery(query);
```

這個細節模式可辨識例如：

```text
delta = Q XOR DATA
masked = EN AND delta
D = Q XOR masked
```

雖然外觀不是兩層 AND/OR MUX，但功能為 `D = EN ? DATA : Q`。此模式成本較高，建議先指定 DFF，或設定嚴格候選數與總時間預算。

數量題建議使用：

```cpp
query.findAllFunctionalMatches = false;
query.resolveFunctionalDataNets = false;
```

FindAny 在第一個 proven control 後即可完整回答該 DFF 是否存在 enable/hold；略過 named-data resolution 可避免為了回答 count 額外執行大量 SAT。

simulation filter 預設開啟，只負責用實際 counterexample 排除候選，最後的 confirmed match 仍必須來自 SAT。除錯或 A/B 驗證時可設定：

```cpp
query.enableFunctionalSimulationFilter = false;
```

engine 先建立 PI / leaf 與 D-input distance 的固定結構順序，再以 simulation evidence 做 stable rerank。只在單側看過 `D != Q` 的 control 會優先；同一 simulation 類別仍保留原結構順序。control=0/1 都被觀察到、且兩側都有 mismatch counterexample 的安全負例會直接從 bounded SAT search 排除，不占 `maxFunctionalCandidates`，也不增加 `functionalCandidatesExamined`。simulation 仍不會建立 positive match，confirmed control 最後必須有 SAT proof。

例如共有 100 個結構候選，其中 96 個由 simulation 安全排除，且 `maxFunctionalCandidates=4`：engine 會完整檢查剩餘 4 個候選，`functionalCandidateLimitReached=false`，若沒有 timeout/unknown/unsupported，則可回傳 `complete=true`。若剩餘未排除候選有 5 個，才會因 limit 留下未檢查候選並回傳 partial。

全設計 FindAny 或計數型探索可先使用 quota 4 到 8，讓有限時間優先覆蓋更多 DFF；這只適合取得 SAT-proven matches 的下界。若問題要求證明不存在或完整列出所有 controls，必須提高 quota、延長時間或使用後續的分階段搜尋，並以 `complete`、`functionalUnexaminedCandidateCount` 與 `functionalInconclusiveCandidateCount` 判斷是否真的完整。

SAT engine 會自動在 one-shot proof 與 reusable per-DFF session 間切換，不需要額外 query 參數。這是內部效能策略；呼叫端仍只依 `confirmed`、`complete`、`timedOut` 與 limit 欄位判讀答案。

## 4. 讀取 confirmed pattern

```cpp
for (const auto& dff : report.reports) {
    for (const auto& pattern : dff.patterns) {
        if (!pattern.confirmed) {
            continue;
        }

        std::cout << dff.dffName
                  << " control=" << pattern.enableNetName
                  << " active=" << pattern.activeLevel
                  << " hold=" << pattern.holdLevel
                  << " data=" << pattern.dataNetName
                  << " data_resolved=" << pattern.dataFunctionResolved
                  << "\n";
    }
}
```

`activeLevel=1, holdLevel=0` 表示 active-high；`activeLevel=0, holdLevel=1` 表示 active-low。functional match 的 `dataNetName` 可能為空，此時只能回答 hold/control 已證明，不可虛構 data net。

若需要回答 data net，還必須確認：

```cpp
pattern.dataSearchAttempted
pattern.dataSearchComplete
pattern.dataSearchTimedOut
pattern.dataFunctionResolved
pattern.dataCandidatesExamined
```

`dataFunctionResolved=false` 且 `dataSearchComplete=true` 表示完整搜尋後沒有精確具名 net；`dataSearchComplete=false` 只表示尚未完整解析，不能回答不存在。

## 5. 判斷結果是否完整

```cpp
if (!report.complete) {
    // 已回傳的 confirmed matches 可使用，absence 不可當成確定不存在。
}

for (const auto& dff : report.reports) {
    if (dff.functionalCandidateLimitReached ||
        dff.functionalFallbackTimedOut ||
        !dff.functionalFallbackComplete) {
        // 說明這顆 DFF 的 functional search 不完整。
    }
}
```

常用統計：

```cpp
report.matchedDffCount
report.candidateDffCount
report.functionalMatchCount
report.functionalCandidateCount
report.functionalSearchableCandidateCount
report.functionalCandidatesExamined
report.functionalUnexaminedCandidateCount
report.functionalInconclusiveCandidateCount
report.functionalSimulationPatternCount
report.functionalSimulationCandidateCount
report.functionalSimulationRejectedCandidateCount
report.functionalSimulationSeconds
report.functionalSatCheckCount
report.elapsedSeconds
```

`functionalCandidateCount` 是 structural 總數；`functionalSimulationRejectedCandidateCount` 是不占 quota 的 safe Reject；`functionalSearchableCandidateCount` 是兩者相減；`functionalCandidatesExamined` 是實際進入 loop 的 searchable candidates；`functionalUnexaminedCandidateCount` 是尚未進入者；`functionalInconclusiveCandidateCount` 是進入後因 timeout/unknown/unsupported 未完成者。

完整性判讀：

```text
- 全部未排除候選都完成判定：complete=true，可確定沒有其他 supported match。
- FindAny 找到第一個 SAT-proven match：complete=true，可確定 existence=true；較早的 inconclusive candidate 仍保留在 counter，但不推翻存在性 proof。
- FindAll，或尚未找到 proof 的 FindAny：timeout、unsupported、solver unknown、match limit，或仍有未檢查的 searchable candidate 時為 PARTIAL。
- 只有安全 Reject 未進入 SAT loop：不會造成 candidateLimitReached，也不會單獨造成 PARTIAL。
```

若整體時間在某顆 DFF 進入 engine 前已用完，該 DFF 的 candidate counters 仍為 0，但 top-level `timedOut=true`、`complete=false`；因此回答完整性一律先讀 top-level envelope。

數量題應讀 `matchedDffCount`，因為它依 DFF 去重。`candidateDffCount` 只代表可能的 enable/hold pattern，不包含 data-gating-only diagnostic。`functionalMatchCount` 可能包含同一 DFF 的多個 nested controls。

## 6. Canonical match 額外 SAT 驗證

```cpp
query.verifyCanonicalMatchesWithSat = true;
```

這會驗證 inactive `D==Q` 與 active `D==data branch`。它和 functional fallback 不同：前者驗證已找到的 canonical 結構，後者會搜尋未知 control。大量 DFF 不應無限制同時開啟兩者。

## 7. AND-only data gating diagnostic

```text
D = EN & DATA
```

官方已確認這不算 enable/hold，因為沒有同一顆 DFF 的 Q feedback。工具會回傳 `DataGatingWithoutHoldFeedback`、`confirmed=false`、`semanticsPending=false` 與 `status=DATA_GATING_WITHOUT_HOLD_FEEDBACK`。回答時只能說這是 data-gating non-match diagnostic，不能列為 enable/hold candidate，也不能併入 `matchedDffCount` 或 `candidateDffCount`。

真正的 match 應以 Boolean function 判斷，功能上需能表示成：

```text
D = EN ? DATA : Q
```

依官方 Q69，`EN` 和 `DATA` 不一定要是實體 net，也可以是 Q-free Boolean function。因此遇到非 canonical 結構時，應使用 functional fallback，而不是只靠固定 MUX gate pattern 或 gate 名稱。

## 8. Prompt 對應

| Prompt 語意 | 主要欄位 |
| --- | --- |
| 哪些 DFF 有 enable/hold？ | `reports[].matched`, `patterns[]` |
| 有幾顆 DFF 有 enable/hold？ | `matchedDffCount` |
| ff1 的 enable 與有效電位？ | `enableNetName`, `activeLevel`, `holdLevel` |
| active 時載入哪條 net？ | `dataFunctionResolved`, `dataNetName` |
| 是否為非 canonical 功能辨識？ | `detectionMethod=FunctionalCofactorSat` |
| enable/hold 搜尋是否完整？ | `complete`, `timedOut` 與 per-DFF fallback 欄位 |
| named data 是否解析完整？ | `dataSearchAttempted`, `dataSearchComplete`, `dataFunctionResolved` |

## 9. 不屬於此 API

一般 DFF/clock/reset 直接連線用 Structure Query；fanin cone 用 Cone Query；Q-to-D path 用 Path Query；DFF.D depth 用 Depth Query；插入或修改邏輯用 EditApply；成本導向最佳化用 Optimization Flow。
