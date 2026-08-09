# OptApply API 問題與修改紀錄

## 1. 盤查範圍

| 項目 | 內容 |
|---|---|
| 高階入口 | `Netlist::runOptQuery()`、`Netlist::runOptApply()` |
| 主要程式 | `src/optimization/OptimizationFlow.cpp`、`src/optimization/DepthOptimizer.cpp` |
| 公開型別 | `include/core/OptimizationFlow.h`、`include/core/OptimizationTypes.h` |
| API 文件 | `API_SPEC/OPT_APPLY_API.md`、`API_SPEC/OPT_APPLY_USAGE.md` |
| 既有測試 | `mini test/test30`、`mini test/test31`、`mini test/test32` |
| 外部盤查 | `C:/Users/user/Downloads/OPT_APPLY_REVIEW.md` |

本文件只記錄本專案 facade、transaction、report、scope 與 optimizer orchestration。
mockturtle 第三方 pass 內部演算法不在盤查範圍；whole-design SAT/AIG 內部效能留待後期
Boolean 重構，但 OptApply 如何解讀 proof 結果仍屬本 API 的責任。

## 2. 狀態總表

| ID | 優先級 | 狀態 | 問題摘要 | 建議處理 |
|---|---:|---|---|---|
| OPTAPPLY-001 | P0 | Deferred Boolean / Report Redesign | SAT UNKNOWN/inconclusive 候選被提交並標成 functionally equivalent | AIG/SAT 重構時重新決定 acceptance 並拆分 report 狀態 |
| OPTAPPLY-002 | P1 | Fixed / Verified | `mini test/test30` 使用不存在的 `TechMapReport::changed`，目前無法編譯 | 改用現有 report delta 欄位；10/10 PASS |
| OPTAPPLY-003 | P2 | Fixed / Verified | 文件記錄 test30/test31 數字過期 | API/Usage 已同步為 10/10、22/22 |
| OPTAPPLY-004 | P1 | Fixed / Verified | double-inverter worklist 的長鏈效能與安全語意 | focused regression 3/3 PASS；200k chain 約 0.022s |
| OPTAPPLY-005 | P0 | Known Optimizer Blocker | mockturtle primitive 無 cooperative cancellation，可能超過 request time limit | 保留 optimizer-core 待辦，不在 facade 偽造 timeout 保證 |
| OPTAPPLY-006 | P2 | Deferred Boolean / Report Redesign | inconclusive proof 的 report 欄位無法可靠區分 attempted、proved、accepted-by-assumption | 與 OPTAPPLY-001 一併重整 |
| OPTAPPLY-007 | P2 | Deferred Boolean | whole-design SAT 效能可能成為 candidate acceptance bottleneck | 待 AIG/SAT 重構 |
| OPTAPPLY-008 | P1 | Fixed / Verified | transaction timer 在 scope resolution 後才開始，且進 optimizer core 前不檢查剩餘時間 | timer 前移、pre-core guard；test31 PASS |
| OPTAPPLY-009 | P1 | Fixed / Verified | NaN time limit、非法 depth objective / gate type enum 可通過 validation | finite/exact-enum validation；test31 PASS |
| OPTAPPLY-010 | P2 | Fixed / Verified | 無 graph change 且 `requireDepthImprovement=false` 時仍可能執行 whole-design SAT | 已加入 StructuralIdentity no-op fast path；test31 PASS |
| OPTAPPLY-011 | P2 | Fixed / Verified | 非 CriticalPathDepth pass 靜默忽略 scope、target、basis 與 time limit | 非預設語意欄位在 mutation 前拒絕；verbose 加 warning |

## 3. 已確認基準

### 3.1 附件修改已存在

目前原始碼已包含：

```text
DepthOptimizer::eliminateDoubleInverters() worklist
runOptApply() 的 SAT inconclusive accept-by-assumption 政策
```

這兩者位於目前提交基準，不是本輪尚未套用的 patch。

### 3.2 Regression 現況

```text
mini test/test30: Summary: 10 passed, 0 failed.
mini test/test31: Summary: 22 passed, 0 failed.
```

test30 原失敗原因：

```text
test30.cpp:106: TechMapReport has no member named changed
```

測試已改用目前 `TechMapReport` 的 delta collections 判斷 no-op，重新編譯與執行通過。
test32 是 tools-facing regression，依目前分工應在 core/API 修正完成後再同步執行。

## 4. 問題詳情

### OPTAPPLY-001：inconclusive equivalence 被當成已等價並提交

狀態：Deferred Boolean / Report Redesign

涉及位置：

```text
src/optimization/OptimizationFlow.cpp
  Netlist::runOptApply()
  CriticalPathDepth acceptance branch
```

目前程式以 `!provenNotEquivalent` 作為接受條件。只要 whole-design checker 沒有提供
明確 mismatch，即使 `equivalence.ok == false`、SAT UNKNOWN、timeout 或 unsupported，
仍可能：

```text
restoreFrom(working)
report.success = true
candidateAccepted = true
wholeDesignEquivalent = true
validation.functionallyEquivalent = true
equivalenceMethod = WholeDesignSat
```

但同一份 report warning 又寫明 `This has not been proven by SAT.`。這造成結構化欄位
與 warning 自相矛盾；LLM 若依 API 文件只讀 `functionallyEquivalent`，會把未證明結果
回答成已證明等價。

此外，mockturtle pass 只涵蓋 network 內部 rewrite 的理論保真；本專案還有
`Netlist -> mockturtle -> Netlist` 轉換、scope/basis enforcement、name/interface
重建等自有程式。whole-design proof 的目的正是涵蓋整條 glue pipeline，不能用「第三方
pass 按設計應保持功能」取代。

後期建議 contract：

```text
equivalence.ok && equivalence.equivalent
  -> commit candidate，functionallyEquivalent=true。

equivalence.ok && !equivalence.equivalent
  -> rollback，回報 proven mismatch。

!equivalence.ok（UNKNOWN / timeout / unsupported / incomplete）
  -> rollback，functionallyEquivalent=false，保留 checker message/warnings。
```

目前決議先不修改 SAT、候選接受政策或公開欄位。AIG/SAT 重構時應重新決定 UNKNOWN
是否允許以 trusted rewrite 狀態提交；若仍允許，report 必須明確拆分「已證明等價」與
「接受但未證明」，不得再讓 `functionallyEquivalent=true` 同時搭配
`This has not been proven by SAT`。

### OPTAPPLY-002：test30 regression 已過期

狀態：Fixed / Verified

`TechMapReport` 目前沒有 `changed` 欄位。test30 的 DFF.Q basis no-op 已改用現有
contract 判斷：status success、modified gate list 為空、added/removed count 為空，
並再次確認 resolved cone 為空及 basis constraint 成立。

驗證：`Summary: 10 passed, 0 failed.`

### OPTAPPLY-003：文件測試數字過期

狀態：Fixed / Verified

目前 test30 實際為 10/10、test31 為 22/22；API/Usage 已同步更新。

### OPTAPPLY-004：double-inverter worklist 缺少 focused regression

狀態：Fixed / Verified

worklist 實作已取代 repeated full scan，並保留 PO guard。仍需 focused test 覆蓋：

```text
反向 gate-ID 長 NOT chain
多個獨立 NOT pairs
PO driver 不被移除
pin-level load adjacency 維持一致
大型 chain runtime
```

已新增 `mini test/test31/double_inverter_worklist_regression.cpp`，不經 mockturtle，
直接隔離驗證 private helper：

```text
even reverse-ID chain function/structure: PASS
primary-output driver protection: PASS
200,000-gate reverse-ID NOT chain: removed 199,999, about 0.022 seconds, PASS
Summary: 3 passed, 0 failed.
```

### OPTAPPLY-005：optimizer core 不可中途取消

狀態：Known Optimizer Blocker

request time limit 只能在 mockturtle primitive 前後檢查，無法中斷正在執行的
balancing/cut rewriting/resubstitution。這可能讓外層 process timeout 先發生。

本項需要 cone-isolated synthesis、可中斷的子程序，或替換 optimizer core；不應在
facade 只改 report 文字假裝已符合 hard wall-clock limit。

### OPTAPPLY-006：proof/report 語意

狀態：Deferred Boolean / Report Redesign

現有欄位足以表達「checker 曾執行」與「是否已證明等價」，前提是：

```text
equivalenceChecked = checker 確實被呼叫
functionallyEquivalent = 只有 conclusive proof 才為 true
wholeDesignEquivalent = 只有 conclusive proof 才為 true
wholeDesignTimedOut = checker/time budget 的真實結果
candidateAccepted = 只有所有 hard validation 通過才為 true
```

不需要為了目前 bug 立刻擴充 public schema；先修正填值即可。

### OPTAPPLY-007：whole-design proof 效能

狀態：Deferred Boolean

大型 candidate 可能因 whole-design SAT 無法在剩餘時間內完成而被安全拒絕。這是得分與
proof capability 的問題，但不能靠把 UNKNOWN 改成 true 解決。後續應由 AIG functional
index、incremental/local proof 或較可靠的 whole-design equivalence engine 改善。

### OPTAPPLY-008：transaction time budget 沒有涵蓋完整流程

狀態：Fixed / Verified

`startedAt` 目前在 `resolveRewriteScope()` 完成後才建立，因此 scope traversal 不計入
`timeLimitSeconds`。後續 clone、constraint scan 與 lower-bound analysis 雖然計時，
但呼叫 `executeCriticalPathOptimization()` 前沒有檢查剩餘時間；即使前置流程已用完
預算，仍會啟動目前不可 cooperative-cancel 的 mockturtle core。

建議：

```text
CriticalPathDepth case 一開始就啟動 transaction timer。
scope/clone/constraint/lower-bound 後、進 optimizer core 前檢查剩餘時間。
已超時則保留 original，candidateGenerated=false，不啟動 core。
```

已將 timer 移到 CriticalPathDepth case 起點，並在 cone/constraint 前置作業完成後、
建立 `DepthOptimizer` 前加入 budget guard。這不能解決 mockturtle primitive 執行中
超時，但能避免「明知預算已用完仍開始 core」。

驗證：極小時間預算會在 core 前回 `TIMEOUT`，`candidateGenerated=false`，current
netlist 的 depth、gate count 與 structure 均維持不變。

### OPTAPPLY-009：request validation 未拒絕非有限時間與非法 enum

狀態：Fixed / Verified

`timeLimitSeconds <= 0.0` 無法拒絕 NaN；NaN 會讓後續所有 `<= 0` timeout 判斷失效。
`depthObjective` 只特判 `ScopedFaninCone`，其他非法 enum value 會被當成 GlobalMaximum。
`isCombinationalGateType()` 只排除 DFF/UNKNOWN，非法 cast 的 GateType 也會被視為合法。

已使用 `std::isfinite()` 驗證時間，並以 switch/白名單驗證 depth objective 與
Problem A combinational GateType。

驗證：NaN time limit、`static_cast<OptDepthObjective>(999)` 與
`static_cast<GateType>(999)` 都在 mutation 前安全失敗。

### OPTAPPLY-010：真正 no-op 仍可能執行 whole-design SAT

狀態：Fixed / Verified

若 core 沒有改變 graph、constraints/target 已滿足，但 caller 設定
`requireDepthImprovement=false`，目前流程不會走「無改善保留 original」分支，仍可能
對 identical design 執行 whole-design SAT。大型設計可能浪費剩餘預算，甚至得到
UNKNOWN，儘管 structural identity 已足以證明等價。

已在 candidate 的 structure、constraint 與 target 驗證完成後、whole-design SAT 前加入
fast path。當實際 graph comparison 顯示 `changed=false` 時，直接保留 original 並回：

```text
equivalenceMethod = StructuralIdentity
candidateGenerated = false
candidateAccepted = false
wholeDesignEquivalenceChecked = false
wholeDesignEquivalent = true
```

涉及檔案：

```text
src/optimization/OptimizationFlow.cpp
mini test/test31/test31.cpp
API_SPEC/OPT_APPLY_API.md
API_SPEC/OPT_APPLY_USAGE.md
```

驗證：使用具精確 converter 命名與常數 net 佈局的單層 AND fixture，確認 optimizer core
執行後 graph identity 不會進 SAT；test31 對應 regression PASS。

### OPTAPPLY-011：非 critical pass 的 request 欄位被靜默忽略

狀態：Fixed / Verified

CleanupBufferChain、CollapseDoubleInverter、LocalSimplificationFixpoint 直接呼叫全域
WithReport wrapper；`scope`、`scopeName`、`targetDepth`、allowed/banned basis 與
`timeLimitSeconds` 不會生效。只有 candidateIds/equivalence/rollback 的部分情況會加
warning，容易讓 C++ caller 誤以為 scoped cleanup 已執行。

這三種操作實際 owner 是 EditApply，因此保留無額外參數的 whole-design legacy 呼叫，但
新增共用 contract validation。以下欄位只要不是預設值，就會在 mutation 前回
`success=false / changed=false / rolledBack=false`，message 同時列出所有違規欄位：

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

`verbose=true` 不會改變 transformation 語意，因此仍允許執行，但 report 會加入目前未
支援 verbose log 的 warning。

涉及檔案：

```text
src/optimization/OptimizationFlow.cpp
mini test/test31/test31.cpp
API_SPEC/OPT_APPLY_API.md
API_SPEC/OPT_APPLY_USAGE.md
```

驗證：unsupported semantic fields 會完整列名且不修改 current design；預設 legacy pass
仍可執行；`verbose=true` 會產生 warning。test31 22/22 PASS。

## 5. 建議修正順序

1. OPTAPPLY-001/006 已標記 Deferred Boolean / Report Redesign，本階段不修改。
2. test30 過期欄位已修正，10/10 regression 通過。
3. double-inverter focused regression 已完成，3/3 通過。
4. test30/test31 core regression 已完成，目前為 10/10、22/22。
5. OPT_APPLY API/Usage 的真實測試數字與 deferred 限制已同步。
6. OPTAPPLY-008~011 的非 Boolean facade 問題已處理完成。
7. tools.cpp/test32 留到 core/API 穩定後統一同步。
