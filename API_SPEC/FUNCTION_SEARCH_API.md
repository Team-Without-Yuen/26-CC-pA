# Function Search API 整理

這份文件說明 `FunctionSearchQuery` 的內部設計、證明規則與限制。對外使用範例請看：

```text
API_SPEC/FUNCTION_SEARCH_USAGE.md
```

---

## 1. API 定位

`FunctionQuery` 用來驗證名稱已知的 net；`FunctionSearchQuery` 用來從大量 signals 中自行找出符合 Boolean 條件的候選者。

第一版直接對應 NewTestCase/test35：

```text
Does there exist any pair of internal signals (a, b) already in the netlist
such that NAND(a, b) is equivalent to n25?
```

`FunctionSearchQuery` 是 read-only analysis，不會合併 gate、rewire 或修改 netlist。搜尋結果若要交給修改流程，必須另外呼叫 `EditApply`。

---

## 2. 公開型別

```cpp
enum class FunctionSearchQueryType {
    NandEquivalentInputPairs
};

enum class FunctionSearchMode {
    FindAny,
    FindAll
};

FunctionSearchReport runFunctionSearchQuery(
    const FunctionSearchQuery& query) const;
```

目前只實作 `NandEquivalentInputPairs`。functional-equivalent gate pair、constant signal discovery 與 observability-aware redundancy search 尚未納入此 API。

---

## 3. Request 欄位

| 欄位 | 預設值 | 語意 |
|---|---:|---|
| `type` | `NandEquivalentInputPairs` | 搜尋的 Boolean pattern |
| `mode` | `FindAny` | 找任意一組或完整列舉 |
| `targetNetName` | `""` | 必須是存在的 scalar net |
| `internalSignalsOnly` | `true` | 只使用 active、非 PI/PO/constant、且有 driver 的 signals |
| `allowSameSignalPair` | `false` | 是否允許 `(a,a)` |
| `maxResults` | `256` | `FindAll` 的結果上限；必須大於 0 |
| `simulationPatternCount` | `256` | bit-parallel simulation patterns；合法範圍 1..4096 |
| `timeLimitSeconds` | `30.0` | simulation、candidate enumeration 與 SAT 共用的總時間上限 |

---

## 4. 搜尋與證明流程

```text
收集合法 internal signals
  -> deterministic bit-parallel simulation
  -> 淘汰不可能滿足 NAND(a,b)==target 的 signals/pairs
  -> 對剩餘 pair 建立 NAND miter
  -> CaDiCaL SAT verification
  -> 只有 miter UNSAT 的 pair 才能放入 matches
```

simulation 只是一個必要條件 filter。signature 相符不能證明功能等價；signature 不符則代表已找到一組具體 assignment 使兩者不同，可以安全淘汰。

最終 proof 建立：

```text
diff = NAND(a,b) XOR target
assert diff = 1
```

結果為 UNSAT，才代表所有 PI 與 DFF.Q pseudo-PI assignments 下都不存在差異。

---

## 5. Report 語意

| 欄位 | 語意 |
|---|---|
| `ok` | request 合法且問題已有確定答案；partial/timeout 時為 false |
| `found` | 至少存在一組 SAT-proven match |
| `complete` | `FindAny` 已確定 exists，或 `FindAll` 已完整搜尋 |
| `allCandidatesExamined` | 所有合法候選都已處理；FindAny 提前成功時可為 false |
| `timedOut` | 總時間或 SAT proof 超時 |
| `truncated` | `FindAll` 達到 `maxResults`，結果不完整 |
| `unsupported` | target/candidate cone 含無法分析的 logic |
| `matches` | 只包含 SAT UNSAT miter 證明過的 pair |

搜尋統計：

```text
candidateSignalCount
simulationEligibleSignalCount
candidatePairsConsidered
candidatePairsRejectedBySimulation
satChecks
satUnknownCount
unsupportedSignalCount
simulationPatternCount
elapsedSeconds
```

每筆 `FunctionSearchMatch` 會回傳 net ID/name、`provenEquivalent`、`proofMethod` 與 `solverStatus`。目前有效 match 應為：

```text
provenEquivalent = true
proofMethod = SAT_UNSAT_MITER
solverStatus = UNSAT
```

---

## 6. 狀態分類

| `status` | 意思 |
|---|---|
| `MATCH_FOUND` | FindAny 找到一組，存在性答案已確定 |
| `MATCHES_FOUND` | FindAll 完整列舉且至少一組 |
| `NO_MATCH` | 完整搜尋後確定不存在 |
| `RESULT_LIMIT_REACHED` | 已證明還有超過 maxResults 的 match，只回 partial matches |
| `TIMEOUT` | 搜尋或 SAT 超過總時間 |
| `SOLVER_UNKNOWN` | 至少一組 SAT proof 無法判定 |
| `UNSUPPORTED_OR_PARTIAL` | 部分 signal/cone 無法分析 |
| `TARGET_NOT_FOUND` | target 名稱不存在 |
| `SCALAR_TARGET_REQUIRED` | target 展開後不是一個 scalar bit |
| `INVALID_ARGUMENT` | pattern/result/time limit 不合法 |

`NO_MATCH` 只能在 `ok=true && complete=true` 時使用。TIMEOUT、UNKNOWN 或 unsupported 不得回答「不存在」。

---

## 7. 實測結果

mini test23 C++ 專用電路覆蓋 found、no-match、FindAll、result limit、invalid target 與 timeout。mini test24 另外覆蓋 `func_search` parser、統一 envelope、SAT proof 輸出與 help/schema exposure。

NewTestCase/test35 的 `n25` 實測：

```text
candidate signals: 34639
simulation-eligible signals: 11783
SAT checks before first match: 1485
elapsed: approximately 6.5 seconds through public `func_search`
match: (n26080, n6359)
proof: UNSAT
```

因此可回答存在一組 internal signals，使 `NAND(n26080, n6359)` 與 `n25` 功能等價。

---

## 8. 後續擴充

後續可以沿用同一份 request/report 骨架新增：

```text
FindFunctionallyEquivalentGatePairs
FindConstantSignals
FindRedundancyCandidates
```

其中 redundancy removal 仍需 observability proof 與 `EditApply`，不能因兩個局部 functions 相同就直接刪除 gate。
