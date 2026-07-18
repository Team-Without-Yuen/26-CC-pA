# Edit Apply API 整理

這份文件是內部用的 Edit Apply 詳細規格，整理 `NetlistEditReport`、`WithReport()` wrapper、`runEditApply()`、以及不對外暴露的低階 edit primitive。

外部使用手冊請看：

```text
API_SPEC/EDIT_APPLY_USAGE.md
```

---

## 1. 設計原則

Edit Apply 分成兩層：

```text
Public layer:
  LLM / CLI / prompt router
    -> EditApplyRequest
    -> runEditApply()
    -> NetlistEditReport

Internal layer:
  transformation / optimization pass
    -> low-level WithReport primitive
    -> local proof / SAT / whole-design equivalence check
    -> NetlistEditReport
```

public `runEditApply()` 只暴露能由 wrapper 解釋 functional equivalence 的安全修改，例如 rename、cleanup、buffer insertion、constant simplification、technology mapping / basis conversion。低階 rewiring / replacement primitive 保留給內部 pass 使用，不直接給 prompt routing 呼叫。

整理原則：

```text
1. Public command 只放 prompt 可能直接要求、且 wrapper 能說明等價性的操作。
2. 低階 graph rewiring 即使有 WithReport，也不直接暴露給 LLM / CLI。
3. Technology mapping 會修改 netlist，因此歸入 edit_apply，而不是 query。
4. Optimization apply 之後可重用 NetlistEditReport，但不混入目前的 low-level primitive。
```

---

## 2. NetlistEditReport

位置：

```cpp
include/core/NetlistEditReport.h
```

主要欄位：

```cpp
bool success;
bool changed;
bool rolledBack;

NetlistEditOperationKind operationKind;
std::string operationName;
std::string message;

NetlistStats beforeStats;
NetlistStats afterStats;
NetlistDiff diff;

EditValidationResult validation;

std::optional<DepthChange> depthChange;
std::optional<FanoutChange> fanoutChange;
std::optional<MappingDelta> mappingDelta;
std::optional<ConstantSimplificationSummary> constantSimplification;

std::vector<int> changedGateIds;
std::vector<int> changedNetIds;
std::vector<std::string> changedGateNames;
std::vector<std::string> changedNetNames;
std::vector<std::string> warnings;
```

equivalence method：

```cpp
enum class EquivalenceCheckMethod {
    NotChecked,
    StructuralIdentity,
    LocalRewriteRule,
    WholeDesignSat
};
```

目前 `validateEditResult()` 檢查 structure 與 Problem A constraints，但本身不做 whole-design equivalence。Problem A constraint 採 baseline-aware 語意：`problemAConstraintsValid` 表示 after design 絕對合法，`problemAConstraintsRegressed` 表示 edit 是否新增 baseline 原本沒有的違規；只有新增違規才使 edit validation 失敗。已知自身 rewrite rule 的 wrapper 會另呼叫 `certifyEquivalence()` 補上 certificate。

---

## 3. Report Building Blocks

已完成的共用 helper：

```cpp
NetlistStats collectNetlistStats() const;
static NetlistDiff diffStats(const NetlistStats& before, const NetlistStats& after);
static EditValidationResult validateEditResult(const Netlist& before, const Netlist& after);
static NetlistEditReport buildEditReport(...);
static void certifyEquivalence(NetlistEditReport& report, EquivalenceCheckMethod method, ...);
```

一般 `WithReport()` 流程：

```text
1. clone before snapshot
2. 執行底層 primitive / pass
3. 收集 changed gate / net 或轉換原本專屬 report
4. 建立 before / after stats 與 diff
5. 執行 structure / Problem A validation
6. validation 失敗則 rollback
7. 若此 wrapper 可證明等價，補 equivalence certificate
8. 回傳 NetlistEditReport
```

大型 pass 不應每個小步驟都做一次完整 snapshot / validation。後續較好的方向是：

```text
snapshot once
run many primitive changes
record EditTrace
validate once
return one NetlistEditReport
```

---

## 4. Public runEditApply()

位置：

```cpp
include/core/EditFlow.h
src/transformation/EditFlow.cpp
```

入口：

```cpp
NetlistEditReport runEditApply(const EditApplyRequest& request);
```

流程：

```text
EditApplyRequest
  -> request validation
  -> block internal-only primitive commands
  -> dispatch to safe WithReport wrapper
  -> normalize operationName as edit_apply:<command_name>
  -> return NetlistEditReport
```

失敗 report 的共通語意：

```text
success = false
changed = false
rolledBack = false
message = 具體錯誤原因
```

目前 validation 會檢查：

```text
rename:
  oldName / newName 不可空、舊物件需存在、新名稱不可衝突

name-based command:
  gateName / netName 不可空，且物件需存在

id-based command:
  ID 必須在有效範圍內，且 gate/net 不可已 removed

fanout command:
  maxFanout 若有設定，必須 >= 2；未設定時使用預設 4

DFF control buffer:
  processClock / processReset 至少一個為 true

by-gate-type buffer:
  gateType != UNKNOWN，且至少選擇 input/output 其中一種
```

---

## 5. Public Edit Commands

`runEditApply()` 目前對外支援：

```text
Rename:
  RenameGate
  RenameNet

Cleanup / Simplification:
  CleanupBuffers
  CollapseDoubleInverter
  LocalSimplificationFixpoint
  SafeCleanupFixpoint
  TrimDeadLogic
  RemoveDanglingLogic
  RemoveUnusedNets
  MergeStructurallyEquivalentGates
  SimplifyConstants
  SimplifySameInput
  RemoveNetIfUnused

Buffer insertion:
  InsertBuffersForFanout
  InsertBuffersForSpecificNet
  InsertBuffersForDffControl
  InsertBuffersOnEachLoad
  InsertBufferAtDriver
  InsertBufferBeforeGate
  InsertBuffersByGateType

Technology mapping:
  ConvertToBasis
  ReplaceGateType
```

`EditCommandKind::MergeEquivalentGates` 與 `mergeEquivalentGates*()` 只為內部舊程式相容性保留；實作仍是 structural identity merge，不是 SAT-based functional equivalence。它不得出現在 public CLI/tool schema。對外 structural merge 統一使用 `MergeStructurallyEquivalentGates`；未來真正功能等價合併應使用新名稱 `MergeFunctionallyEquivalentGates`。

equivalence certificate 原則：

```text
StructuralIdentity:
  rename、dead/dangling/unused removal、structural merge、remove unused net

LocalRewriteRule:
  buffer insertion、buffer cleanup、double inverter collapse、
  constant propagation、same-input simplification、local/safe cleanup fixpoint、
  technology mapping / basis conversion
```

成功的 public `runEditApply()` 不應回傳 `NotChecked`。若使用者要求 equivalence，但該 command 沒有 certificate，report 應給 warning 或失敗，不應宣稱等價。

dispatch 對應：

```text
Rename*                    -> rename*WithReport()
Cleanup / Simplification   -> cleanup / simplification WithReport wrappers, including SafeCleanupFixpoint
Buffer insertion           -> insertBuffer*WithReport()
Technology mapping         -> TechMapper::*WithReport()
Internal primitive         -> validation 階段直接擋下
```

---

## 6. Internal Low-Level Primitives

以下 primitive 保留給內部 transformation / optimization pass，不對 public `runEditApply()` 暴露。

Graph mutation：

```cpp
disconnectGateInputWithReport()
connectGateInputWithReport()
removeGateWithReport()
replaceAllLoadsOfNetWithReport()
```

Gate replacement：

```cpp
replaceGateWithNetWithReport()
replaceGateWithConstantWithReport()
replaceGateWithNotOfNetWithReport()
```

Driver / output rewrite：

```cpp
replaceDriverOfNetWithReport()
rewireGateOutputToExistingNetWithReport()
replaceNetFunctionWithNetKeepingNameWithReport()
```

Net rewrite / merge：

```cpp
mergeNetIntoNetWithReport()
bypassNetKeepingPortSemanticsWithReport()
redirectAllLoadsWithReport()
```

這些操作可能改變 circuit functionality。即使 `success = true` 且 graph validation 通過，也只代表結構合法，不代表功能等價。

若透過 `runEditApply()` 呼叫 internal-only command，會回傳 failed report：

```text
success = false
changed = false
message = "Command is an internal low-level primitive and is not exposed through EditApply because functional equivalence is not guaranteed."
```

內部使用規則：

```text
1. 不要從 prompt routing / CLI 直接 dispatch 到這些 primitive。
2. 只有 verified transformation / optimization pass 可以呼叫。
3. 呼叫前應確認 rewrite precondition，例如 constant input、same input、double inverter、equivalent driver。
4. 呼叫後若要宣稱 functionality preserved，必須補上 local certificate 或 whole-design equivalence check。
5. 若無法證明等價，只能保留 NotChecked，不能回覆使用者「等價已保證」。
```

---

## 7. Technology Mapping 關係

Technology mapping 已收斂為 `runEditApply()` 的 public 子功能，而不是獨立平行的 `TechnologyApply` 入口。原因是它會直接修改 netlist，語意上屬於 edit / transformation 類 command；對 LLM 來說可以維持「查詢走 query、修改走 edit_apply」的單一入口。

```cpp
EditCommandKind::ConvertToBasis
EditCommandKind::ReplaceGateType
```

public request 欄位：

```cpp
TargetScope scope;
std::string scopeName;
std::vector<GateType> allowedTypes;
std::vector<GateType> bannedTypes;
GateType targetGateType;
bool verbose;
```

dispatch：

```text
ConvertToBasis
  -> TechMapper::convertToBasisWithReport()
  -> allowedTypes / bannedTypes 決定最終 basis

ReplaceGateType
  -> TechMapper::replaceGateTypeWithReport()
  -> targetGateType = 要移除的 gate
  -> allowedTypes = RHS 允許生成的 gate basis
  -> 若 scope 內沒有 targetGateType，回 success/no change
```

report：

```text
operationKind = TechnologyMapping
operationName = edit_apply:convert_basis 或 edit_apply:replace_type
mappingDelta.removedCountByType
mappingDelta.addedCountByType
mappingDelta.finalGateCountByType
validation.equivalenceMethod = LocalRewriteRule
```

目前 `convertToBasis()` 已修正兩個容易誤判的情況：

```text
1. target gate type 在 scope 內本來就不存在時會跳過，不再因 no-op mapping 誤報失敗。
2. cone scope 轉換後只檢查該 scope 內是否還有違規 gate，不會被 cone 外同類 gate 影響。
```

---

## 8. 已修正問題

report 化與 validation 過程中已修正：

```text
1. removed net 需要 tombstone flag，避免重複移除與 addNet name collision。
2. stricter validateStructure() 檢查 driverGateId / outputNetId 雙向一致。
3. trimDeadLogic / removeDanglingLogic 改用 markGateRemoved()，避免 stale driver。
4. insertBufferAtDriver / insertBufferBeforeGate 避免 vector reallocation 後繼續使用失效 reference。
5. mergeNetIntoNet() 轉移 PO 語意後取消舊 net 的 isPO。
6. removeDanglingLogic() 忽略 removed gates，避免 cleanup fixpoint 卡住。
7. constant propagation 支援 gate type、constant value 與 input count 篩選，不再只能全域執行。
8. constant folding 會檢查所有 inputs，且直接驅動 PO 時以 BUF/NOT 保留原 PO net。
9. constant candidate matching 使用 `Net.constVal`，不依賴 constant net 名稱。
```

---

## 9. 目前限制

```text
1. public edit wrapper 目前只標局部 certificate；whole-design SAT 已由 tools 的獨立 `equiv_query original/previous_edit` 提供，尚未自動嵌入每次 edit。
2. Technology mapping 已作為 `edit_apply` 子功能公開；其 edit report 使用 rule-based local certificate，需要時再另外執行 whole-design `equiv_query`。
3. depthChange 已接 global critical depth comparison，但尚未支援 opt request targetDepth / endpoint-specific depth comparison。
4. 大型 optimization pass 尚未導入 EditTrace。
5. tools.cpp 已接完整 public edit_apply command family 與 `report_query last_edit`；internal low-level rewiring 仍不對外開放。
6. opt_report / opt_rollback 尚未實作。
```

---

## 10. 測試狀態

目前 `mini test/tester.cpp` 已涵蓋：

```text
report helper
primitive mutation wrappers
buffer insertion wrappers
cleanup / simplification wrappers
gate replacement wrappers
driver / output rewrite wrappers
net merge / cleanup wrappers
technology mapping wrapper failure rollback
runEditApply rename_gate
runEditApply cleanup_buffers
runEditApply insert_buffers_for_specific_net
runEditApply blocks unchecked low-level primitive
runEditApply validateEquivalence has certificate for public edit
runEditApply missing required rename argument
runEditApply missing fanout net
runEditApply invalid fanout limit
runEditApply unsupported command
```

目前驗證結果：

```text
Summary: 115 passed, 0 failed.
```
