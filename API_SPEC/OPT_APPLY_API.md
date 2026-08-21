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

一般 read-only cone query 遇到 DFF 不穿越。rewrite/optimization 的
`NET_FANIN` scope 也沿用相同語意：若指定的 net 是 DFF.Q/register output，
該 net 視為 sequential boundary，fanin cone 是空的 combinational cone。
例如 `scopeName="n10"` 且 n10 由 DFF 驅動時，scoped basis/depth request
應回 no-change / already optimal，而不是改取同一顆 DFF 的 D-pin data cone。

只有在 scope 明確指定 `GATE_FANIN <DFF instance>` 時，`resolveRewriteScope()`
才可解析到該 DFF 的 D-pin data cone，因為這時 target 是 sequential cell
本身，而不是它的 Q signal。

`runOptApply()` 採 transaction：

```text
original snapshot
  -> working copy 產生候選
  -> structure / Problem A constraint
  -> gate-basis constraint
  -> depth objective / targetDepth
  -> graph 不變時 StructuralIdentity；否則嘗試 whole-design SAT (PO + DFF.D)
  -> SAT 證明不等價時拒絕；SAT 證明等價或現行 policy 接受 inconclusive 時才 commit
```

候選失敗、未達要求或已證明不等價時，原 Netlist 不會被修改。目前 SAT
UNKNOWN/inconclusive 的候選可能依 contest scoring policy 被接受；此例外尚未有獨立
report status，已列為 Deferred Boolean / Report Redesign。

因此，現行契約中的「執行/檢查 whole-design SAT」只表示 checker 已被呼叫，不等於
SAT 已完成等價證明。`functionallyEquivalent=true`、`wholeDesignEquivalent=true` 與
`EquivalenceMethod::WholeDesignSat` 也可能出現在 trusted-but-unproven 的接受結果；此時
warnings 會包含 `This has not been proven by SAT`。呼叫端必須保留這個區別。

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

前三個 pass 是 legacy whole-design compatibility entry；公開 optimization 的主要入口是
`CriticalPathDepth`，cleanup 類 prompt 應優先走 `EditApply`。legacy pass 只接受
`OptApplyRequest` 的預設 whole-design 語意；若傳入 scoped、candidate selection、depth、
basis、time、equivalence 或 rollback 條件，會在 mutation 前明確失敗，不會靜默忽略。
`verbose=true` 不影響語意，因此 pass 仍執行，但 report 會加入 unsupported warning。

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
| `timeLimitSeconds` | `290.0` | 必須為 finite positive；scope、optimizer、mapping 與 final SAT 共用；mockturtle 單次 primitive 尚不可搶占 |
| `requireDepthImprovement` | `true` | baseline 已合規時，無改善就保留 original |
| `verbose` | `false` | 內部 optimizer log |
| `validateEquivalence` | `false` | CriticalPathDepth 仍固定執行等價驗證流程；graph identity 以 `StructuralIdentity`，其餘候選嘗試 whole-design SAT；inconclusive 的接受例外見第 7 節 |
| `rollbackOnFailure` | `true` | CriticalPathDepth 固定保留 original；false 會被忽略 |

`candidateIds` 對 CriticalPathDepth 尚未生效；目前是 pass-level search。

對 `CleanupBufferChain`、`CollapseDoubleInverter`、
`LocalSimplificationFixpoint`，下列非預設欄位目前不支援，若出現即回
`success=false / changed=false / rolledBack=false`：

```text
candidateIds
scope / scopeName
depthObjective
allowedTypes / bannedTypes
targetDepth
timeLimitSeconds
requireDepthImprovement
validateEquivalence
rollbackOnFailure
```

## 4. Report Contract

統一回傳 `NetlistEditReport`，主要欄位：

| 欄位 | 語意 |
|---|---|
| `success` | 請求是否安全完成；包含「無改善而保留 original」 |
| `changed` | current design 是否真的提交新候選 |
| `rolledBack` | 是否曾產生候選但最後保留 original |
| `depthChange` | objective 的 before/after/target/improved/meetsTarget |
| `beforeStats/afterStats/diff` | 候選或提交結果的結構統計 |
| `validation` | structure、Problem A constraint、等價方法與現行 acceptance 結果；不保證單靠 bool 欄位即可判定 SAT proof |
| `depthOptimization` | scope、constraint、core status、SAT attempt endpoint 與接受狀態 |
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
| true | true | false | 候選依現行 acceptance policy 提交；可能是 SAT-proven，也可能是 trusted-but-unproven |
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
2. 解析 rewrite scope；`NET_FANIN <DFF.Q>` 停在 sequential boundary。
3. 若 scoped objective 命中保守 lower-bound proof，直接以
   `StructuralIdentity` 回 original。
4. 否則 clone original，在 working copy 呼叫 `DepthOptimizer`。
5. XAG/AIG 候選一律轉回 Netlist，以正式 Problem A depth 選 best candidate。
6. 必要時執行 whole/local basis enforcement、double-inverter cleanup 與 inverter absorption。
7. 驗證結構、basis、depth improvement/target。
8. 若 optimizer 未造成 graph change，以 `StructuralIdentity` 回 original，不啟動 SAT。
9. 其餘候選對所有同名 PO 與 DFF.D 嘗試 whole-design SAT。
10. SAT 證明 mismatch 時拒絕；SAT 證明等價時接受；UNKNOWN/inconclusive 且未找到
    mismatch 時，現行 scoring-oriented policy 可能信任 mockturtle rewrite 並接受。
11. 接受時才 `restoreFrom(working)`；否則保留 original。

## 6. 與其他 API 的責任界線

| 問題 | API |
|---|---|
| 現在的 global/cone depth 是多少 | `DepthQuery` |
| 將 cone 固定轉為 NAND/NOT，不要求找 best depth | `EditApply::ConvertToBasis` |
| 將 XOR 固定替換為指定 basis | `EditApply::ReplaceGateType` |
| minimize maximum depth 並維持 basis | `OptApply::CriticalPathDepth` |
| current 是否等價於 original/previous snapshot | `WholeDesignEquivalence` |
| OptApply 內部候選是否可提交 | graph identity 使用 `StructuralIdentity`；其餘候選嘗試 whole-design SAT，再依 proof/mismatch/inconclusive policy 決定 |

## 7. 限制與安全規則

1. optimizer 是 heuristic search，不證明數學上的 globally optimal depth。
2. `targetDepth >= 0` 是 acceptance requirement；未達成即不提交。
3. baseline 已符合 hard gate constraint 且 depth 未改善時，回 original。
4. baseline 不符合 hard gate constraint 時，合規候選可優先於 depth improvement，
   report 會加入 warning。
5. 非 whole scope 若解析後沒有任何 combinational gate，視為 empty rewrite
   scope；不進 optimizer、不修改設計，以 `StructuralIdentity` 回 original。
   若指定 `targetDepth` 且 original 未達標，則回 failure/no-change。
6. whole-design checker 比較 PO 與 DFF.D；DFF.Q 視為 boundary leaf，initial state
   尚未納入。
7. time budget 會限制後續 SAT 並在 core 結束後檢查；mockturtle core 尚無可中途
   cancel 的 callback。transaction timer 從 CriticalPathDepth 分支起點開始，進 core
   前若預算已耗盡則不啟動 candidate。
8. scoped optimization 目前仍可能先做 global AIG/XAG restructuring，再重套局部
   basis；不是只允許 cone 內拓樸改動的 ECO isolation mode。
9. CriticalPathDepth 不接受 unsafe no-rollback，也不會跳過 changed candidate 的
   whole-design SAT attempt；但 attempt 為 inconclusive 時仍存在第 11 點的接受例外。
10. scoped lower-bound proof 目前只涵蓋 depth 0，以及 NAND/NOT basis 下
   `NOT(NAND(a,b))` 且 a/b 為不同 independent boundary signals 的 depth 2。
11. whole-design SAT 若為 UNKNOWN/inconclusive，目前可能信任 mockturtle rewrite 而
    接受候選；`functionallyEquivalent` 在此情境不能視為 SAT proof。後續 AIG/SAT 與
    report redesign 必須拆分 proved-equivalent 與 trusted-but-unproven。
12. optimizer 執行後若 graph 完全不變，回 `changed=false`、
    `candidateGenerated=false`、`candidateAccepted=false`、
    `wholeDesignEquivalenceChecked=false`，並以 `StructuralIdentity` 證明等價；不消耗
    whole-design SAT 預算。

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
  DFF.Q boundary no-op、局部 NOR/NOT conversion
  10 passed

mini test/test31
  global/scoped depth improvement
  mandatory whole-design SAT attempt（不保證每次得到 proof）
  graph-identity no-op SAT fast path
  targetDepth rollback
  invalid scope
  DFF.Q local basis constraint
  no-basis scoped depth objective
  finite/enum request validation、pre-core timeout guard
  legacy pass unsupported-field rejection、verbose warning
  22 passed

mini test/test31/double_inverter_worklist_regression.cpp
  reverse gate-ID chain correctness、PO driver protection、structure validation
  200,000-gate reverse-ID NOT chain 約 0.022 秒
  3 passed

mini test/test32
  27 passed：tools parser/report/cache/timeout
  scoped depth-0 lower bound
  NAND/NOT two-input AND depth-2 lower bound

testcase/test22
  global depth 41 -> 20

testcase/test26
  n10 為 DFF.Q boundary；scoped cone depth 0，回 original/already optimal

NewTestCase/test40
  完整前置 mapping/cleanup 後 n14 = NOT(NAND(n514,n412))
  NAND/NOT depth 2 lower bound，回 original，bounded smoke 約 3.6 秒

NewTestCase/test33
  若 n8 為 DFF.Q，應視為 boundary depth 0；非 DFF.Q 大型 cone 仍可能受
  mockturtle core runtime 影響
  目前列為 optimizer-core cancellation/scoped-resynthesis blocker
```

`tools.cpp` / LLM-facing `opt_apply` 已完成。大型 scoped cone 的 cooperative
timeout 與真正 cone-isolated resynthesis 仍待 optimizer core 補強。
