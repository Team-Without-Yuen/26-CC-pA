# Sequential Pattern Query API 整理

這份文件說明 `SequentialPatternQuery` 的責任、functional pattern engine、report contract 與效能限制。呼叫範例請看 `API_SPEC/SEQUENTIAL_PATTERN_USAGE.md`。

---

## 1. 功能範圍

`SequentialPatternQuery` 從固定 DFF 的 D-input 組合邏輯推導 register-control semantics。目前公開模式為 `DffEnableHold`，可回答哪些 DFF 具有 enable/hold、控制訊號與有效電位、載入資料與 Q feedback 等問題。

它不取代其他 API：DFF 清單與 clock/reset 直接連線仍由 Structure Query 負責；一般 cone/path/depth 與指定訊號的 Boolean 問題仍分別由 Cone、Path、Depth、Function Query 負責。

## 2. 公開入口

```cpp
SequentialPatternReportSet runSequentialPatternQuery(
    const SequentialPatternQuery& query) const;
```

```cpp
struct SequentialPatternQuery {
    SequentialPatternQueryType type = SequentialPatternQueryType::DffEnableHold;
    std::string dffName; // 空字串表示所有 active DFF
    bool includeAndGatedCandidates = true;
    bool verifyCanonicalMatchesWithSat = false;

    bool enableFunctionalFallback = false;
    size_t maxFunctionalCandidates = 64;
    size_t maxFunctionalMatchesPerDff = 8;
    bool findAllFunctionalMatches = true;
    bool resolveFunctionalDataNets = true;
    size_t maxFunctionalDataCandidatesPerMatch = 16;
    bool enableFunctionalSimulationFilter = true;
    size_t functionalSimulationPatternCount = 256;
    double functionalPerDffTimeLimitSeconds = 0.25;
    double functionalTimeLimitSeconds = 5.0;
};
```

`functionalTimeLimitSeconds` 是整次 query 共用的 wall-clock 預算；`functionalPerDffTimeLimitSeconds` 則避免單一大型 DFF 耗盡全部時間。候選數、每顆 DFF 的 match/data 候選數與時間都必須是正值。

## 3. 兩階段辨識

### 3.1 Canonical fast path

第一階段以結構方式辨識下列 canonical feedback MUX：

```text
OR(AND(S, A), AND(!S, B))
NAND(NAND(S, A), NAND(!S, B))
AND(OR(S, A), OR(!S, B))
NOR(NOR(S, A), NOR(!S, B))
```

其中一個 branch 必須是目前 DFF 的 Q feedback。辨識器會正規化 BUF、`NOT(x)`、`NAND(x,x)`、`NOR(x,x)` 與成對反相 wrapper。這條路徑不需要逐一搜尋 control，適合大型 testcase；`verifyCanonicalMatchesWithSat` 可再用 conditional equivalence 驗證 hold/load cofactor。

### 3.2 Functional cofactor fallback

第二階段由內部 `FunctionalPatternEngine` 處理非 canonical、但功能等價的實作。所有 DFF 共用一次 bit-parallel simulation；simulation 只在同時觀察到 control=0/1，且兩側都有 `D != Q` counterexample 時排除候選。這兩個實際反例已分別否定兩側成為 hold cofactor，因此排除是安全的；patterns 不足時只會保守地不排除。simulation 不會建立 positive match，未被排除的候選最後仍必須由 SAT 證明。

control candidates 先使用固定且可重現的結構排序：PI 優先，其次是無 driver 的 leaf；同一類別中優先檢查到 D-input gate distance 較短的 net，最後以 net ID 排序。simulation 可用時再做 stable rerank：只在單一 control level 觀察到 `D != Q` 的候選優先，兩側仍可能 hold 的其次、simulation 不可判斷者再次。已在兩側觀察到 mismatch 的安全負例會保留在 candidate/report 統計中，但在套用 `maxFunctionalCandidates` 前從 bounded SAT search 移除，不占 candidate quota，也不增加 `functionalCandidatesExamined`。

因此 `maxFunctionalCandidates` 限制的是「尚未被 simulation 安全排除、仍需要 reachability/SAT 判定」的候選數。若全部未排除候選都已完整檢查，即使結構候選總數大於 limit，也不會因已排除候選觸發 `candidateLimitReached`，並可回傳 `complete=true`。若所有結構候選都被安全排除，搜尋可完整回傳 `NO_MATCH`，且不需執行 SAT。

MUX-hold matcher 對 D-input cone 中的候選 control `C` 證明：

```text
C=0 與 C=1 都可由實際輸入到達
恰有一個 cofactor 滿足 D == Q
另一個 cofactor 視為 active/load function
```

若 active cofactor 與 cone 中某條具名 net 等價，會回傳 `dataNetName`；找不到具名 net 時，hold decomposition 仍可成立，但 `dataFunctionResolved=false`。control 的兩個值都必須可達，可避免不可達條件造成 vacuous SAT proof。

若 simulation 已實際看過 control 的兩個值，這些 patterns 本身就是 reachability witnesses；若其中一個 cofactor 已出現 `D != Q`，該側也已有不等價反例。此時只需對剩下可能的 hold cofactor 執行一次 SAT proof。simulation 不可用或訊號值單一時，會保守退回原本的 reachability 加 two-cofactor SAT 流程。

SAT proof 採 hybrid session。每顆 DFF 的第一個 proof 使用既有 one-shot cone query，保留首候選命中時的低延遲；只有搜尋需要第二個 proof 時，才按需建立該 D-input cone 的 reusable CNF。後續 hold 與 named-data proof 只更換 SAT assumptions，避免反覆收集相同 cone 與重建 solver。若 cone 含目前 CNF encoder 不支援的 gate，session 會自動退回既有 one-shot query，不會把 unsupported 誤判成不等價。

數量題可使用 `findAllFunctionalMatches=false` 與 `resolveFunctionalDataNets=false`：每顆 DFF 找到第一個 proof 後就能確定「存在」，不必列出 nested controls 或搜尋 named data。細節題才開啟 FindAll/data resolution，並由 `maxFunctionalDataCandidatesPerMatch` 限制每個 match 的成本。

fallback 只在沒有 confirmed canonical match 時執行，預設關閉。未來增加 clock-enable、reset mux 或其他 sequential control pattern 時，應新增 `FunctionalPatternMatcher`，不要把所有 pattern 寫進 `SequentialPatternAnalysis.cpp` 的單一分支。

## 4. Pattern report

`DffInputPatternReport` 每顆 DFF 一筆，`patterns` 可包含多筆已證明的 nested/alternative control interpretation。

| 欄位 | 語意 |
| --- | --- |
| `enableNetName` | control/select net |
| `dataNetName` | active cofactor 對應的具名 data net；未解析時為空 |
| `feedbackNetName` | 該 DFF 的 Q feedback |
| `activeLevel`, `holdLevel` | 載入與保持時的 control value |
| `detectionMethod` | `StructuralCanonical`、`StructuralCanonicalWithSat` 或 `FunctionalCofactorSat` |
| `dataFunctionResolved` | active function 是否找到精確具名 net |
| `dataSearchAttempted` | 是否要求搜尋 named data net |
| `dataSearchComplete`, `dataSearchTimedOut` | data role 搜尋是否完整／逾時 |
| `dataCandidateCount`, `dataCandidatesExamined` | named-data 候選總數與實際檢查數 |
| `structuralMatch` | 是否由 canonical 結構辨識 |
| `holdFunctionallyProven` | hold cofactor 是否已由 SAT 證明 |
| `loadFunctionallyProven` | active cofactor 是否已證明等於回傳的 data net |
| `confirmed` | 是否為可計入 enable/hold 的已證明 match |
| `semanticsPending` | 是否仍等待題意定義；目前用於 AND-only candidate |

`matchedDffCount` 依 DFF instance 去重；同一 DFF 有多筆 pattern 仍只計數一次。`functionalMatchCount` 則計算 functional fallback 找到的 pattern 筆數，兩者不可混用。

## 5. 完整性與成本 report

functional search 的 candidate 成本分成：structural、simulation-safe Reject、searchable、examined、unexamined 與 inconclusive。safe Reject 已有兩側 `D != Q` counterexamples，不需 SAT；searchable 才是仍需 reachability/SAT 的候選。

每顆 DFF 會回傳：

```text
functionalFallbackAttempted
functionalFallbackComplete
functionalFallbackTimedOut
functionalCandidateLimitReached
functionalCandidateCount
functionalSearchableCandidateCount
functionalCandidatesExamined
functionalUnexaminedCandidateCount
functionalInconclusiveCandidateCount
functionalSimulationCandidateCount
functionalSimulationRejectedCandidateCount
functionalSatCheckCount
```

其中：

```text
functionalCandidateCount
  = 結構收集到的 control candidate 總數，包含 simulation 安全排除者

functionalSimulationCandidateCount
  = 具有可用 control=0/1 simulation signatures 的候選數

functionalSimulationRejectedCandidateCount
  = control=0/1 都被觀察到，且兩側都有 D != Q counterexample 的安全負例數

functionalSearchableCandidateCount
  = structural candidates 扣除 simulation-safe Reject

functionalCandidatesExamined
  = 未被安全排除、實際進入 bounded reachability/SAT search loop 的候選數

functionalUnexaminedCandidateCount
  = guarded max(searchable - examined, 0)

functionalInconclusiveCandidateCount
  = 已進入 loop，但因 timeout/unknown/unsupported 未完成的 control candidates；同一 control 最多一次
```

必須維持：`candidate = safe Reject + searchable`、`examined <= searchable`、`unexamined = searchable - examined`、`inconclusive <= examined`。整體 report 的同名欄位是所有 per-DFF 欄位的算術加總。

若 query-wide time budget 在某顆 DFF 尚未呼叫 engine 前就用完，該 DFF 沒有 candidate plan，因此不虛構 unexamined/inconclusive 數量；此時仍以 top-level `timedOut/complete` 判斷整體不完整，不能只看 aggregate candidate counters。

`maxFunctionalCandidates` 只限制未被安全排除的候選；`functionalCandidateLimitReached` 只表示仍有這類候選尚未檢查。整體 report 另提供共用 simulation 的 `functionalSimulationPatternCount` 與 `functionalSimulationSeconds`。

整體 report 會聚合 `complete`、`timedOut`、candidate/match/SAT 數量與 `elapsedSeconds`。FindAll 只有在 `unexamined=0`、`inconclusive=0`，且沒有 match limit/timeout 時完整。FindAny 一旦找到 SAT-proven match，existence 已完整決定；即使較早 candidate 為 unknown/unsupported，仍可 `complete=true`，同時保留 nonzero inconclusive counter 供診斷。已回傳的 `confirmed` match 仍有 proof；但未找到 match 不能在 partial 狀態下解讀成「確定不存在」。named-data 是否完整仍另外讀 pattern 的 `dataSearchComplete`。

## 6. AND-only 語意

直接 `D = EN & DATA` 沒有 Q feedback，目前只回傳：

```text
kind = AndGatedDataCandidate
confirmed = false
semanticsPending = true
```

它會計入 `candidateDffCount`，不會計入 `matchedDffCount`。

## 7. 實作位置

```text
include/core/FunctionalPatternEngine.h
src/analysis/FunctionalPatternEngine.cpp
src/analysis/SequentialPatternAnalysis.cpp
src/analysis/FunctionAnalysis.cpp
```

`FunctionalPatternEngine` 是內部可擴充 engine；對外 authoritative facade 仍是 `runSequentialPatternQuery()`。

## 8. 驗證狀態

```text
mini test/test19：canonical pattern 15/15
mini test/test20：edit/mapping/equivalence flow 13/13
mini test/test21：canonical 相容、functional proof、partial/timeout/error、參數與 envelope 18/18
mini test/test28：functional fallback、simulation on/off、simulation-aware ranking、safe-Reject quota、cost invariant、FindAny-after-inconclusive、internal selector、負例、FindAny/FindAll、data bound、limit/timeout 19/19
```

NewTestCase/test40 的既有正式數字仍是 canonical/default 模式結果。目前只做過短時間、允許 partial 的 functional benchmark，尚未完成全候選搜尋，因此不能把額外 matches 當成最終完整計數。

受控 benchmark（O0 test build、原始 test40、FindAny、不解析 named data）：canonical 約 0.020 秒。加入 simulation prefilter、simulation-aware stable ranking、safe-Reject quota 與 hybrid SAT session 後，12 顆 unmatched 抽樣連續執行皆為 12/12 complete；每顆都在第一個候選、一次 SAT proof 找到 match，總 wall time 約 0.25–0.30 秒。

成本掃描顯示 quota 會直接影響全設計 FindAny 的涵蓋率。`maxFunctionalCandidates=64` 在 2 秒內通常只開始約 43 顆 fallback DFF，找到 27 筆額外 proven matches；改為 quota 4 或 8 時可找到約 90–92 筆。quota 4 在 10 秒找到 193 筆；20 秒 budget 的一次執行實際於約 13.4 秒走完全部 1002 顆 fallback DFF，找到 263 筆，unique matched DFF 從 1583 增至 1846。該次沒有 timeout 或 inconclusive candidate，但仍有 760 顆 DFF 留下未檢查的 searchable candidates，因此結果仍為 `PARTIAL`。這些數字都是已證明下界，不是完整候選搜尋後的最終計數。
