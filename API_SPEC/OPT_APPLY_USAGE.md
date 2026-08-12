# OptApply 使用說明

這份文件只負責說明 `Netlist::runOptQuery()`、`Netlist::runOptApply()` 與
`OptPassKind::CriticalPathDepth` 的使用方式。

設計背景與內部 contract 請看：

```text
API_SPEC/OPT_APPLY_API.md
```

---

## 1. 基本概念與入口

最小 C++ 呼叫：

```cpp
OptApplyRequest request;
request.passKind = OptPassKind::CriticalPathDepth;

NetlistEditReport report = netlist.runOptApply(request);
```

標準流程：

```text
1. 判斷 cost 是 global maximum depth 或指定 fanin cone depth
2. 設定 scope 與可選 gate-basis constraint
3. 呼叫 runOptApply()
4. 先讀 success / changed / rolledBack
5. 再讀 depthChange、validation、depthOptimization
```

目前 C++ API 與 `tools.cpp` 的公開 `opt_apply critical_path_depth` 均可用。

## 2. Mode / Command 總表

| Mode | 必要輸入 | 主要輸出 | 語意 |
|---|---|---|---|
| `CriticalPathDepth + GlobalMaximum` | `passKind` | `depthChange` | 最小化全設計 maximum logic depth |
| `CriticalPathDepth + ScopedFaninCone` | fanin `scope/scopeName` | `depthChange` | 最小化指定 fanin cone root depth |
| global basis-constrained depth | whole scope + allowed/banned | constraint summary | 全設計維持指定 basis 並嘗試降 depth |
| local basis-constrained depth | fanin scope + allowed/banned | resolved scope + constraint summary | 指定 cone 維持 basis 並嘗試最佳化 |
| target depth | `targetDepth >= 0` | `meetsTarget` | 未達 target 不提交候選 |

## 3. 輸入欄位

| 欄位 | 型別 | 預設 | 使用規則 |
|---|---|---:|---|
| `passKind` | `OptPassKind` | `Unknown` | depth optimization 必須設 `CriticalPathDepth` |
| `scope` | `TargetScope` | `WHOLE_NETLIST` | 局部 cone 通常使用 `NET_FANIN` |
| `scopeName` | string | empty | 非 whole scope 必填 |
| `depthObjective` | `OptDepthObjective` | `GlobalMaximum` | scoped objective 只支援 fanin scope |
| `allowedTypes` | vector | empty | 白名單；empty 表示不限制 |
| `bannedTypes` | vector | empty | 黑名單優先，且不得與白名單重疊 |
| `targetDepth` | int | `-1` | `-1` best effort；`0+` 為 hard acceptance target |
| `timeLimitSeconds` | double | `290.0` | 必須為有限且大於 0；前置、optimizer、mapping 與 final SAT 共用同一 deadline |
| `requireDepthImprovement` | bool | true | baseline 合規而無改善時保留 original |
| `verbose` | bool | false | 輸出 optimizer debug log |

安全欄位：

```text
validateEquivalence=false
  CriticalPathDepth 仍會驗證等價；graph identity 使用 StructuralIdentity，
  其餘候選做 mandatory whole-design SAT。

rollbackOnFailure=false
  CriticalPathDepth 仍會保留 original，並在 warnings 說明此值被忽略。

candidateIds
  目前 CriticalPathDepth 是 pass-level search，candidateIds 尚未使用。
```

### 3.1 Legacy cleanup pass

`CleanupBufferChain`、`CollapseDoubleInverter` 與
`LocalSimplificationFixpoint` 只保留預設 whole-design 相容呼叫。若需要指定 cone、
候選 ID、gate basis、target depth、time limit 或額外 equivalence/rollback 語意，請改用
對應的 `EditApply` command；legacy OptApply 會在修改前回失敗，message 會列出所有不支援
欄位。`verbose=true` 可執行，但 warnings 會說明目前沒有 verbose log。

## 4. 回傳欄位與狀態判讀

最先讀：

```cpp
report.success
report.changed
report.rolledBack
report.message
```

接著讀：

```cpp
report.depthChange->beforeDepth
report.depthChange->afterDepth
report.depthChange->targetDepth
report.depthChange->improved
report.depthChange->meetsTarget

report.validation.structureValid
report.validation.problemAConstraintsRegressed
report.validation.equivalenceChecked
report.validation.functionallyEquivalent
report.validation.equivalenceMethod

report.depthOptimization->resolvedRootNetName
report.depthOptimization->resolvedThroughDffDataPin
report.depthOptimization->baselineConstraintsSatisfied
report.depthOptimization->finalConstraintsSatisfied
report.depthOptimization->candidateAccepted
report.depthOptimization->wholeDesignTimedOut
report.depthOptimization->comparedOutputCount
report.depthOptimization->comparedDffDCount
```

目前已知例外：whole-design SAT 若回 UNKNOWN/inconclusive，contest scoring policy
可能仍接受候選。此時 warning 會包含 `This has not been proven by SAT`；在 report
redesign 完成前，不得只依 `functionallyEquivalent=true` 宣稱已有 SAT proof。

若 optimizer 執行後 graph 完全不變，不會啟動 whole-design SAT。此時
`equivalenceMethod=StructuralIdentity`、`wholeDesignEquivalenceChecked=false`，且
`candidateGenerated=false`、`candidateAccepted=false`；這是已由結構同一性證明的
no-op，不是 SAT UNKNOWN。

判讀規則：

```text
success=true, changed=true:
  新候選已提交。

success=true, changed=false:
  找不到可接受的改善，或命中可證明的 lower bound；current design 仍是 original。

success=false, rolledBack=false:
  request/scope 在產生候選前即無效。

success=false, rolledBack=true:
  候選已產生，但未達 target、basis 違規、timeout 或不等價。
```

不要只讀 `coreStatus` 判斷是否成功提交；最終以 top-level report 與
`candidateAccepted` 為準。

## 5. 各 Mode 使用範例

### 5.1 全設計 maximum depth

```cpp
OptApplyRequest request;
request.passKind = OptPassKind::CriticalPathDepth;
request.scope = TargetScope::WHOLE_NETLIST;
request.depthObjective = OptDepthObjective::GlobalMaximum;
request.requireDepthImprovement = true;
request.timeLimitSeconds = 290.0;

NetlistEditReport report = netlist.runOptApply(request);
```

主要讀取：

```text
success / changed
depthChange.beforeDepth / afterDepth / improved
validation.functionallyEquivalent
```

### 5.2 全設計維持 AND/NOT

```cpp
OptApplyRequest request;
request.passKind = OptPassKind::CriticalPathDepth;
request.scope = TargetScope::WHOLE_NETLIST;
request.depthObjective = OptDepthObjective::GlobalMaximum;
request.allowedTypes = {GateType::AND, GateType::NOT};

NetlistEditReport report = netlist.runOptApply(request);
```

主要讀取：

```text
depthOptimization.finalConstraintsSatisfied
depthChange
validation.functionallyEquivalent
```

### 5.3 n10 cone 維持 NOR/NOT，cost 是 global depth

```cpp
OptApplyRequest request;
request.passKind = OptPassKind::CriticalPathDepth;
request.scope = TargetScope::NET_FANIN;
request.scopeName = "n10";
request.depthObjective = OptDepthObjective::GlobalMaximum;
request.allowedTypes = {GateType::NOR, GateType::NOT};

NetlistEditReport report = netlist.runOptApply(request);
```

若 `n10` 是 DFF.Q：

```text
depthOptimization.resolvedThroughDffDataPin == false
depthOptimization.resolvedRootNetName == "n10"
depthChange.beforeDepth == 0
depthChange.afterDepth == 0
```

`NET_FANIN <DFF.Q>` 是 empty combinational cone；basis compliance vacuously
satisfied，depth objective 會回 original / already optimal。若要分析 DFF
instance 的 D-input data cone，應明確使用 `GATE_FANIN <DFF instance>`。

### 5.4 最小化 n8 fanin cone depth

```cpp
OptApplyRequest request;
request.passKind = OptPassKind::CriticalPathDepth;
request.scope = TargetScope::NET_FANIN;
request.scopeName = "n8";
request.depthObjective = OptDepthObjective::ScopedFaninCone;
request.allowedTypes = {GateType::NAND, GateType::NOT};

NetlistEditReport report = netlist.runOptApply(request);
```

`depthChange.endpointName` 保留 `"n8"`；before/after depth 以 resolved fanin
root 計算。

### 5.5 要求 depth <= 5

```cpp
OptApplyRequest request;
request.passKind = OptPassKind::CriticalPathDepth;
request.targetDepth = 5;

NetlistEditReport report = netlist.runOptApply(request);
```

若候選 depth 大於 5：

```text
success=false
rolledBack=true（若曾產生候選）
depthChange.meetsTarget=false
current netlist 維持 original
```

### 5.6 無改善時報告 original

保留：

```cpp
request.requireDepthImprovement = true;
```

結果：

```text
success=true
changed=false
message = No improving candidate was accepted...
equivalenceMethod = StructuralIdentity
```

若 scoped objective 命中保守 lower-bound proof，`coreStatus` 為
`NO_IMPROVEMENT`，`coreMessage` 會說明 depth 0 或 NAND/NOT depth 2 的證明；
此時不會執行 heuristic optimizer。

## 6. Prompt 對應

| Prompt 語意 | Request | 主要讀取 |
|---|---|---|
| `Reduce the critical path depth through restructuring.` | whole + `GlobalMaximum` | `depthChange` |
| `Optimize the logic to minimize maximum path depth.` | whole + `GlobalMaximum` | `depthChange` |
| `...cone of n10 maintains only NOR and NOT gates.` | `NET_FANIN n10` + allowed NOR/NOT + global objective | scope/constraint/depth |
| `...cone of n15 contains only AND, OR, and NOT gates.` | `NET_FANIN n15` + allowed AND/OR/NOT | constraint/depth |
| `...netlist remains AND and NOT only.` | whole + allowed AND/NOT | final constraint/depth |
| `Optimize the depth of the cone of n8...` | `NET_FANIN n8` + `ScopedFaninCone` | cone `depthChange` |
| `Report original if already optimal.` | `requireDepthImprovement=true` | `success=true, changed=false` |
| `Make the critical path depth <= 5.` | `targetDepth=5` | `meetsTarget` |

## 7. 何時不要使用

| 問題 | 改用 |
|---|---|
| 只問目前 depth、critical path、哪些 outputs depth > N | `DepthQuery` |
| 明確指定 basis conversion，不要求搜尋較小 cost | `EditApply::ConvertToBasis` |
| 明確指定 gate type replacement | `EditApply::ReplaceGateType` |
| cleanup、constant propagation、buffer insertion | `EditApply` |
| 單獨比較 current/original | `WholeDesignEquivalence` |

不要直接對外呼叫 `DepthOptimizer::executeCriticalPathOptimization()`；它只產生
unchecked candidate，沒有 transaction commit contract。

## 8. 實作與測試狀態

目前 C++ 已支援：

```text
global maximum depth
scoped fanin cone depth
whole/local allowed/banned basis
DFF.Q boundary no-op
targetDepth acceptance
no-improvement original retention
structure / Problem A validation
mandatory whole-design SAT
rollback / detailed NetlistEditReport
```

Regression：

```text
mini test/test30: 10 passed
mini test/test31: 22 passed
mini test/test31/double_inverter_worklist_regression: 3 passed
mini test/test32: 27 passed（tools + lower-bound + timeout regression）
testcase/test22: depth 41 -> 20
testcase/test26: depth 58 -> 30, NOR/NOT cone compliance PASS
NewTestCase/test40: official sequence + n14 proven-optimal no-change PASS
```

尚未完成：

```text
mockturtle core mid-pass cancellation
global optimum proof
strict cone-isolated ECO rewrite
non-DFF.Q large scoped cone completion within bounded runtime
```
