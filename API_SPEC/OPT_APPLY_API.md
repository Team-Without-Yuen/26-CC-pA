# OptApply API 整理

這份文件整理 `Netlist::runOptQuery()`、`Netlist::runOptApply()` 與
`OptPassKind::CriticalPathDepth` 的設計、責任與內部 contract。

使用方式與 prompt 範例請看：

```text
API_SPEC/OPT_APPLY_USAGE.md
```

---

## 1. 核心定位

`OptApply` 負責「使用者指定 objective / cost function，由系統搜尋較佳
netlist」的修改流程。目前正式完成的 cost-driven mode 是 critical-path
depth optimization。

它與 `EditApply` 的差別：

| Prompt 語意 | Owner |
|---|---|
| 指定 rename、cleanup、basis conversion、gate replacement | `EditApply` |
| minimize / reduce / best depth / target depth | `OptApply::CriticalPathDepth` |
| 只查目前 depth 或 critical path | `DepthQuery` |

`DepthOptimizer::executeCriticalPathOptimization()` 是內部候選產生器，不是安全
提交入口。外部應呼叫 `runOptApply()`。

## 2. 資料模型與邊界

depth 使用 Problem A 定義：每顆 combinational gate 都算一層，`NOT`、`BUF`
與其他 gate 相同；DFF 是 sequential boundary。

一般 read-only cone query 遇到 DFF 不穿越。rewrite/optimization fanin scope
若指定的 net 是 DFF.Q，`resolveRewriteScope()` 會改取該 DFF 的 D-pin data
cone。例如 `scopeName="n10"` 且 n10 由 DFF 驅動時，限制套用到 DFF.D
前方的組合邏輯，不把 DFF 本身當成 NOR/NOT 等 basis constraint 的一部分。

`runOptApply()` 採 transaction：

```text
original snapshot
  -> working copy 產生候選
  -> structure / Problem A constraint
  -> gate-basis constraint
  -> depth objective / targetDepth
  -> whole-design SAT (PO + DFF.D)
  -> 全部通過才 commit working copy
```

候選失敗、timeout、不等價或未達要求時，原 Netlist 不會被修改。

## 3. Query / Request 型別

### 3.1 OptPassKind

```cpp
enum class OptPassKind {
    Unknown,
    CleanupBufferChain,
    CollapseDoubleInverter,
    LocalSimplificationFixpoint,
    CriticalPathDepth
};
```

前三個 pass 是既有 skeleton；公開 optimization 的主要入口是
`CriticalPathDepth`。cleanup 類 prompt 仍優先走 `EditApply`。

### 3.2 OptDepthObjective

| 值 | cost 判定 |
|---|---|
| `GlobalMaximum` | 全設計 PI/DFF.Q 到 PO/DFF.D 的 maximum logic depth |
| `ScopedFaninCone` | 指定 fanin scope 的 root depth；支援 `NET_FANIN` / `GATE_FANIN` |

### 3.3 OptApplyRequest

| 欄位 | 預設 | 語意 |
|---|---:|---|
| `passKind` | `Unknown` | depth 題設為 `CriticalPathDepth` |
| `scope` | `WHOLE_NETLIST` | 全域或局部 rewrite/basis 範圍 |
| `scopeName` | `""` | 非 whole scope 的 net/gate 名稱 |
| `depthObjective` | `GlobalMaximum` | cost 使用全域 depth 或指定 cone depth |
| `allowedTypes` | empty | 非空時為 combinational gate 白名單 |
| `bannedTypes` | empty | combinational gate 黑名單；不得與白名單重疊 |
| `targetDepth` | `-1` | `-1` 表示 best effort；非負值表示候選必須達成 |
| `timeLimitSeconds` | `240.0` | transaction 預算；mockturtle 單次 primitive 尚不可搶占 |
| `requireDepthImprovement` | `true` | baseline 已合規時，無改善就保留 original |
| `verbose` | `false` | 內部 optimizer log |
| `validateEquivalence` | `false` | CriticalPathDepth 仍固定執行 whole-design SAT |
| `rollbackOnFailure` | `true` | CriticalPathDepth 固定保留 original；false 會被忽略 |

`candidateIds` 對 CriticalPathDepth 尚未生效；目前是 pass-level search。

## 4. Report Contract

統一回傳 `NetlistEditReport`，主要欄位：

| 欄位 | 語意 |
|---|---|
| `success` | 請求是否安全完成；包含「無改善而保留 original」 |
| `changed` | current design 是否真的提交新候選 |
| `rolledBack` | 是否曾產生候選但最後保留 original |
| `depthChange` | objective 的 before/after/target/improved/meetsTarget |
| `beforeStats/afterStats/diff` | 候選或提交結果的結構統計 |
| `validation` | structure、Problem A constraint、whole-design equivalence |
| `depthOptimization` | scope、constraint、core status、SAT endpoint 與接受狀態 |
| `warnings` | 強制安全語意、DFF boundary、SAT 或 unsupported 診斷 |

`DepthOptimizationSummary` 會記錄：

```text
objectiveMetric
scope / requestedScopeName / resolvedRootNetName
resolvedThroughDffDataPin
allowedTypes / bannedTypes
baselineConstraintsSatisfied / finalConstraintsSatisfied
coreStatus / coreMessage
candidateGenerated / candidateAccepted
wholeDesignEquivalenceChecked / wholeDesignEquivalent / wholeDesignTimedOut
comparedOutputCount / comparedDffDCount
timeBudgetSeconds / elapsedSeconds
```

狀態組合：

| success | changed | rolledBack | 解讀 |
|---:|---:|---:|---|
| true | true | false | 候選通過全部驗證並提交 |
| true | false | false/true | 無可量測改善，original 被保留 |
| false | false | false | request/scope 在 mutation 前即無效 |
| false | true/false | true | 候選未達 target、違規、timeout 或不等價，未提交 |

## 5. 高階入口與執行流程

```cpp
OptQueryReport Netlist::runOptQuery(const OptQueryRequest& request) const;
NetlistEditReport Netlist::runOptApply(const OptApplyRequest& request);
```

CriticalPathDepth 內部流程：

1. 驗證 scope、gate type constraint、depth objective、target 與 time limit。
2. 解析 rewrite scope；DFF.Q fanin 轉成 D-pin data cone。
3. 若 scoped objective 命中保守 lower-bound proof，直接以
   `StructuralIdentity` 回 original。
4. 否則 clone original，在 working copy 呼叫 `DepthOptimizer`。
5. XAG/AIG 候選一律轉回 Netlist，以正式 Problem A depth 選 best candidate。
6. 必要時執行 whole/local basis enforcement、double-inverter cleanup 與 inverter absorption。
7. 驗證結構、basis、depth improvement/target。
8. 對所有同名 PO 與 DFF.D 執行 whole-design SAT。
9. 驗證通過才 `restoreFrom(working)`；否則保留 original。

## 6. 與其他 API 的責任界線

| 問題 | API |
|---|---|
| 現在的 global/cone depth 是多少 | `DepthQuery` |
| 將 cone 固定轉為 NAND/NOT，不要求找 best depth | `EditApply::ConvertToBasis` |
| 將 XOR 固定替換為指定 basis | `EditApply::ReplaceGateType` |
| minimize maximum depth 並維持 basis | `OptApply::CriticalPathDepth` |
| current 是否等價於 original/previous snapshot | `WholeDesignEquivalence` |
| OptApply 內部候選是否可提交 | OptApply 固定執行 whole-design SAT |

## 7. 限制與安全規則

1. optimizer 是 heuristic search，不證明數學上的 globally optimal depth。
2. `targetDepth >= 0` 是 acceptance requirement；未達成即不提交。
3. baseline 已符合 hard gate constraint 且 depth 未改善時，回 original。
4. baseline 不符合 hard gate constraint 時，合規候選可優先於 depth improvement，
   report 會加入 warning。
5. whole-design checker 比較 PO 與 DFF.D；DFF.Q 視為 boundary leaf，initial state
   尚未納入。
6. time budget 會限制後續 SAT 並在 core 結束後檢查；mockturtle core 尚無可中途
   cancel 的 callback。
7. scoped optimization 目前仍可能先做 global AIG/XAG restructuring，再重套局部
   basis；不是只允許 cone 內拓樸改動的 ECO isolation mode。
8. CriticalPathDepth 不接受 unsafe no-rollback 或跳過 equivalence。
9. scoped lower-bound proof 目前只涵蓋 depth 0，以及 NAND/NOT basis 下
   `NOT(NAND(a,b))` 且 a/b 為不同 independent boundary signals 的 depth 2。

## 8. 實作與測試狀態

實作：

```text
include/core/OptimizationFlow.h
include/core/OptimizationTypes.h
include/core/NetlistEditReport.h
include/core/EditFlow.h
src/optimization/OptimizationFlow.cpp
src/optimization/DepthOptimizer.cpp
src/transformation/EditFlow.cpp
```

測試：

```text
mini test/test30
  DFF.Q -> D-pin rewrite scope、局部 NOR/NOT conversion

mini test/test31
  global/scoped depth improvement
  mandatory whole-design SAT
  targetDepth rollback
  invalid scope
  DFF.Q local basis constraint
  no-basis scoped depth objective

mini test/test32
  27 passed：tools parser/report/cache/timeout
  scoped depth-0 lower bound
  NAND/NOT two-input AND depth-2 lower bound

testcase/test22
  global depth 41 -> 20

testcase/test26
  n10 -> D-pin n1113、global depth 58 -> 30、cone NOR/NOT compliance PASS

NewTestCase/test40
  完整前置 mapping/cleanup 後 n14 = NOT(NAND(n514,n412))
  NAND/NOT depth 2 lower bound，回 original，bounded smoke 約 3.6 秒

NewTestCase/test33
  n8 解析到大型 D-pin cone；mockturtle core 在 120 秒 outer timeout 內未完成
  目前列為 optimizer-core cancellation/scoped-resynthesis blocker
```

`tools.cpp` / LLM-facing `opt_apply` 已完成。大型 scoped cone 的 cooperative
timeout 與真正 cone-isolated resynthesis 仍待 optimizer core 補強。
