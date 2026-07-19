# Function Search API 整理

這份文件整理 `FunctionSearchQuery` 的設計、責任與內部 contract。

使用方式與 prompt 範例請看：

```text
API_SPEC/FUNCTION_SEARCH_USAGE.md
```

---

## 1. 核心定位

`FunctionSearchQuery` 處理「候選名稱未知，必須掃描設計才能找到答案」的 Boolean search。目前公開支援：

```text
NandEquivalentInputPairs
EquivalentGatePairs
```

前者搜尋使 `NAND(a,b) == target` 的既有 signal pair；後者搜尋 output function 完全相同的 active combinational gate pairs，並整理成等價類。

本 API 是 read-only analysis，不會 merge gate、rewire 或修改 netlist。修改必須交給 `EditApply`，並依題意執行修改後等價驗證。

---

## 2. 資料模型與邊界

搜尋以 current netlist 的 scalar nets、active combinational gates、PI 與 DFF.Q pseudo-PI 為 function boundary。

`EquivalentGatePairs` 比較的是兩顆 gate 的 output function：

```text
gateA.output XOR gateB.output
```

若 miter 在所有 PI / DFF.Q assignments 下皆 UNSAT，兩者才會進入同一個等價類。simulation signature 只做候選分桶，不構成功能等價證明。

scope 可限制在 whole design 或既有 cone：

```text
WholeDesign
NetFanin / NetFanout
GateFanin / GateFanout
```

scope 的 gate 集合沿用 `ConeQuery` 語意；fanin 包含驅動 root net 的 combinational gate，fanout 從 root net 往下游搜尋。

---

## 3. Query 型別

```cpp
enum class FunctionSearchQueryType {
    NandEquivalentInputPairs,
    EquivalentGatePairs
};

enum class FunctionSearchMode {
    FindAny,
    FindAll
};

enum class FunctionSearchScope {
    WholeDesign,
    NetFanin,
    NetFanout,
    GateFanin,
    GateFanout
};
```

共同限制：

| 欄位 | 預設值 | 語意 |
|---|---:|---|
| `mode` | `FindAny` | 找第一組 witness，或完整分類 |
| `maxResults` | `256` | `FindAll` 最多展開的 pair 數 |
| `simulationPatternCount` | `256` | deterministic bit-parallel patterns，合法範圍 1..4096 |
| `timeLimitSeconds` | `30.0` | simulation、分類與 SAT 共用總時間 |
| `expandEquivalentPairs` | `true` | false 時只回 equivalence classes，不展開平方級 pair records；functional merge 內部使用 |

`NandEquivalentInputPairs` 使用：

| 欄位 | 語意 |
|---|---|
| `targetNetName` | existing scalar target |
| `internalSignalsOnly` | 是否只找 internal signals |
| `allowSameSignalPair` | 是否允許 `(a,a)` |

`EquivalentGatePairs` 使用：

| 欄位 | 語意 |
|---|---|
| `scope` | 搜尋範圍 |
| `scopeName` | 非 whole scope 的 net/gate 名稱 |
| `gateTypeFilter` | `UNKNOWN` 代表全部 combinational types；也可限制單一 type |

---

## 4. Report Contract

共同完整性欄位：

| 欄位 | 語意 |
|---|---|
| `ok` | request 合法，且目前答案可安全使用 |
| `found` | 至少找到一組 SAT-proven match |
| `complete` | FindAny 已回答 exists，或 FindAll 已完成 |
| `allCandidatesExamined` | 所有候選均已完成分類 |
| `timedOut` | 已超過共用時間上限 |
| `truncated` | pair 展開達到 `maxResults` |
| `unsupported` | 部分 function cone 無法編碼或分析 |
| `status`, `message` | machine-readable 狀態與說明 |

`EquivalentGatePairs` 主要統計：

```text
candidateGateCount
candidateSignalCount
simulationEligibleSignalCount
simulationBucketCount
candidatePairsConsidered
candidatePairsRejectedBySimulation
satChecks
satUnknownCount
unsupportedSignalCount
equivalenceClassCount
equivalentPairCount
simulationPatternCount
elapsedSeconds
```

`equivalenceClasses` 保存 SAT-proven class 的 gate/net IDs 與 names。`equivalentPairCount` 是所有已證明 class 可展開的 pair 總數；`matches` 是實際回傳的 pair records，可能受 `maxResults` 限制。

每筆 equivalent-gate match 包含：

```text
gateIdA / gateNameA / netIdA / netNameA
gateIdB / gateNameB / netIdB / netNameB
provenEquivalent = true
proofMethod = SAT_EQUIVALENCE_CLASS
solverStatus = UNSAT
```

---

## 5. 高階入口與執行流程

```cpp
FunctionSearchReport runFunctionSearchQuery(
    const FunctionSearchQuery& query) const;
```

`EquivalentGatePairs` 執行流程：

```text
解析 scope 與 gate-type filter
  -> 收集 active combinational gate outputs
  -> deterministic bit-parallel simulation
  -> 依完整 signature 分桶
  -> 每桶以 representative SAT comparison 建立等價類
  -> FindAny 回第一組 witness；FindAll 展開 class pairs
```

不同 simulation bucket 的 outputs 已有 concrete simulation assignment 證明不同，因此不需 SAT。相同 bucket 仍逐步做 SAT，避免把 simulation collision 當成 proof。

---

## 6. 與其他 API 的責任界線

| 問題 | API |
|---|---|
| 已知 `n1`、`n2`，判斷是否等價 | `FunctionQuery::Equivalence` |
| 已知 condition，判斷兩 net 是否條件等價 | `FunctionQuery::ConditionalEquivalence` |
| 搜尋 `NAND(a,b)==target` 的未知 pair | `FunctionSearchQuery::NandEquivalentInputPairs` |
| 搜尋任意 output function 相同的 gate pairs | `FunctionSearchQuery::EquivalentGatePairs` |
| 找結構完全相同的 duplicate gates | `EditApply::MergeStructurallyEquivalentGates` |
| 實際合併、rewire、清理 gate | `EditApply` |
| 修改前後 whole-design equivalence | `WholeDesignEquivalence` |

gate output 等價不等於該 gate 在 observability 上 redundant；本 API 不做任意 redundant-gate removal。

---

## 7. 限制與安全規則

- 目前 simulation/SAT 支援 AND、OR、NAND、NOR、NOT、BUF、XOR、XNOR combinational logic。
- DFF 不作為候選 gate，但 DFF.Q 可作為 function boundary。
- gate type filter 不接受 DFF 或 UNKNOWN 字串；C++ 的 `UNKNOWN` 值代表不過濾。
- `NO_MATCH` 只有在 `ok && complete && allCandidatesExamined` 時才是完整否定答案。
- TIMEOUT、SOLVER_UNKNOWN、unsupported 或 result truncation 都不得解讀為「不存在」。
- `FindAll` 的 worst case 仍可能需要大量 SAT checks；scope、gate filter 與 time limit 是必要的規模控制。
- 搜尋結果只證明 output functions 相同，不會自動套用 merge。

---

## 8. 實作與測試狀態

| 項目 | 路徑 |
|---|---|
| Query / Report 型別 | `include/core/NetlistQueries.h` |
| Public aliases / entry | `include/core/Netlist.h` |
| simulation、SAT、分類 | `src/analysis/FunctionAnalysis.cpp` |
| public CLI | `tools.cpp` |
| NAND API regression | `mini test/test23` |
| NAND CLI regression | `mini test/test24` |
| equivalent-gate API/CLI regression | `mini test/test26` |

`test26` 覆蓋跨 gate type 等價、多個 equivalence classes、FindAny/FindAll、scope、gate filter、result limit、no-match 與 invalid input。

NewTestCase whole-design 實測（256 patterns、20 秒上限）：

| Case | Candidate gates | SAT checks | Proven pairs | Elapsed |
|---|---:|---:|---:|---:|
| test29 | 2324 | 3444 | 7 | 約 6.5 秒 |
| test30 | 768 | 269 | 1 | 約 0.16 秒 |

兩者皆回 `MATCHES_FOUND`、`complete=true`，且沒有 solver unknown 或 unsupported signals。對應的 `EditApply::MergeFunctionallyEquivalentGates` 已實測完成 cycle-safe merge：test29/test30 分別移除 7/1 顆 gate，並通過 mandatory whole-design SAT。
