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

public `runEditApply()` 只暴露能由 wrapper 證明 functional equivalence 的安全修改，例如 rename、cleanup、buffer insertion、constant simplification、functional duplicate merge、technology mapping / basis conversion。低階 rewiring / replacement primitive 保留給內部 pass 使用，不直接給 prompt routing 呼叫。

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
std::optional<FunctionalMergeSummary> functionalMerge;

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
    CertifiedRewrite,
    WholeDesignSat
};
```

目前 `validateEditResult()` 檢查 structure 與 Problem A constraints，但本身不做 whole-design equivalence。Problem A constraint 採 baseline-aware 語意：`problemAConstraintsValid` 表示 after design 絕對合法，`problemAConstraintsRegressed` 表示 edit 是否新增 baseline 原本沒有的違規；只有新增違規才使 edit validation 失敗。已知自身 rewrite rule 的 wrapper 會另呼叫 `certifyEquivalence()` 補上 certificate。

`functionalMerge` 會保留 scope/filter、search status/completeness、apply timeout、candidate/class/pair/SAT 統計，以及每筆 representative/removed gate/net record。legacy whole-design verification 欄位維持 false。這使 follow-up 能直接回答實際合併數量與保留/移除對象，不必重新分析 after design。

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

functional merge:
  scope target 必須存在、gateType 必須為 combinational 或 UNKNOWN、
  simulationPatternCount 必須在 1..4096、timeLimitSeconds 必須為正數
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
  MergeFunctionallyEquivalentGates
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

`EditCommandKind::MergeEquivalentGates` 與 `mergeEquivalentGates*()` 只為內部舊程式相容性保留；實作仍是 structural identity merge，不是 SAT-based functional equivalence。它不得出現在 public CLI/tool schema。對外 structural merge 使用 `MergeStructurallyEquivalentGates`；跨結構、SAT-proven functional merge 使用 `MergeFunctionallyEquivalentGates`。

equivalence certificate 原則：

```text
StructuralIdentity:
  rename、dead/dangling/unused removal、structural merge、remove unused net

LocalRewriteRule:
  buffer insertion、buffer cleanup、double inverter collapse、
  constant propagation、same-input simplification、local/safe cleanup fixpoint、
  technology mapping / basis conversion

CertifiedRewrite:
  MergeFunctionallyEquivalentGates；先以 FunctionSearch 建立 SAT 等價類，
  再執行 cycle-safe rewiring 與結構驗證，不執行 final whole-design SAT
```

成功的 public `runEditApply()` 不應回傳 `NotChecked`。若使用者要求 equivalence，但該 command 沒有 certificate，report 應給 warning 或失敗，不應宣稱等價。

dispatch 對應：

```text
Rename*                    -> rename*WithReport()
Cleanup / Simplification   -> cleanup / simplification WithReport wrappers, including SafeCleanupFixpoint
Functional duplicate merge -> FunctionSearch class-only SAT -> cycle-safe merge -> structure validation -> CertifiedRewrite
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

新版 optimizer 另外使用下列內部 helper：

```text
convertToBasisOnGateSet()
absorbInverters()
absorbInvertersOnGateSet()
```

這些 helper 用於指定 gate-set 的 basis enforcement、NOT(AND/OR/XOR) absorption
與 strict containment，不新增 `EditCommandKind`，也不直接開放給 tools/LLM。
對外 gate universe 仍是：

```text
AND / OR / NAND / NOR / NOT / BUF / XOR / XNOR
```

本次強化是 scope safety、depth-aware rule selection 與 optimizer integration，
不是新增 MUX 或其他 gate primitive。

---

## 8. 已修正問題

report 化與 validation 過程中已修正：

```text
1. removed net 需要 tombstone flag，避免重複移除與 addNet name collision。
2. stricter validateStructure() 檢查 driverGateId / outputNetId 雙向一致。
3. trimDeadLogic 刪除 gate 時同步清除 fanin loads 與 output driver；只有 DFF endpoint、沒有 PO 的設計也可安全反向標記。
4. insertBufferAtDriver / insertBufferBeforeGate 避免 vector reallocation 後繼續使用失效 reference。
5. mergeNetIntoNet() 轉移 PO 語意後取消舊 net 的 isPO。
6. removeDanglingLogic() 忽略 removed gates，避免 cleanup fixpoint 卡住。
7. constant propagation 支援 gate type、constant value 與 input count 篩選，不再只能全域執行。
8. constant folding 會檢查所有 inputs，且直接驅動 PO 時以 BUF/NOT 保留原 PO net。
9. constant candidate matching 使用 `Net.constVal`，不依賴 constant net 名稱。
10. CollapseDoubleInverter 不再隱含呼叫 buffer cleanup，避免後續 edit 拆除已建立的 fanout tree。
11. InsertBuffersForSpecificNet 的 fanout report 改為 selected net + generated buffer tree scope，不再被全域其他 high-fanout nets 誤判為 constraint failure。
12. gate-set basis conversion 使用 strict containment，mapping rule 不得跨出指定 scope。
13. inverter absorption 沒有可套用 pattern 時回 success/no change，不再誤用 simulation failure。
14. fanout buffer insertion 先將 pin-level `loadGateIds` 去重成候選 gate，再展開實際 matching pins；同一 gate 多個 inputs 接同一 net 時不再重複計數或產生 stale edge。
15. RenameNet 拒絕重新命名 constant net，也拒絕把一般 net 改成 `1'b0` / `1'b1`，避免破壞 Verilog literal 與錯誤宣告 StructuralIdentity。
16. bulk constant simplification 會延後維護 `loadGateIds`，完成後依 active gate input pins 線性重建；高 fanout circuit 不再因逐 gate vector erase 形成平方級耗時。
17. constant lookup 僅使用 canonical `1'b0` / `1'b1` name index；`addNet()` 去重與 constant rename 防護共同保證 O(1) lookup。
18. `replaceAllLoadsOfNet()` 保留 pin-level load multiplicity，同一 gate 有多個 input pins 接同一 net 時不會被 gate-level unique 壓縮。
19. double-inverter finder、單對 bypass 與 bulk worklist 共用 safe-collapse predicate；會先驗證 active/tombstone、ID range、NOT arity、driver/load/input 雙向一致與 PO guard，拒絕 stale candidate 後才 mutation。
20. structural merge 使用 canonical-key + affected-load worklist；canonical key 命中時會重新核對 current key，避免 cascade rewiring 使用 stale hash，且不再隱含執行 dangling/dead cleanup。
21. structural merge 保留不同具名 PO 的各自 driver；PO 與 internal duplicate 衝突時選 PO driver 為 canonical，避免破壞 output net 名稱。
22. `validateStructure()` 以 expected/actual pin-level adjacency 一次比對，完整檢查 multiplicity，不再對每個 input pin 重掃 high-fanout load list。
23. internal `redirectAllLoads()` 復用 pin-level `replaceAllLoadsOfNet()`；legacy `allowDuplicateLoads` 參數保留相容性，但不得再把多個實際 pins 壓成單一 load ID，report changed gate 清單則依 gate 去重。
```

---

## 9. 目前限制

```text
1. public edit wrapper 使用 structural/local/certified rewrite certificate；`MergeFunctionallyEquivalentGates` 保留候選 gate 的 SAT proof，但不執行 final whole-design SAT。只有 prompt 明確要求獨立 baseline comparison 時才呼叫 `equiv_query original/previous_edit`。
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
runEditApply functional equivalent merge / class-only search / cycle-safe representative
runEditApply functional merge CertifiedRewrite / no-change / scoped merge / timeout-before-or-during-mutation
runEditApply missing required rename argument
runEditApply missing fanout net
runEditApply invalid fanout limit
runEditApply unsupported command
```

目前驗證結果：

```text
Summary: 142 passed, 0 failed.
```

`mini test/test2` 的 Edit Apply regression 為 39 passed、0 failed；另有
`fanout_tied_pin_regression.cpp` 覆蓋同一 gate 兩個 input pins 接同一 source 的案例，
確認 fanout=2、只插入兩顆 dedicated BUF、沒有 rollback 且 structure validation 通過；
同一 suite 亦覆蓋 canonical constant direct dedup、repeated technology mapping，
constant rename 的 low-level/high-level rejection、Writer literal preservation，及
replace-all-loads pin-level multiplicity。

`constant_simplification_scalability.cpp` 以 1,000,000 個 constant-input gates
實測 public `runEditApply(SimplifyConstants)`：0.96 秒完成、化簡數量完整、無 rollback，
且 mutation 後 structure validation 通過；依賴的 depth traversal 另通過 test5 11/11。

`double_inverter_scalability.cpp` 以 1,000,000 個串接 NOT 實測 public
`runEditApply(CollapseDoubleInverter)`：0.741 秒 collapse 500,000 pairs、無 rollback，
且 structure validation 通過；test2 另覆蓋 PO/stale/tombstone/invalid-ID safe rejection。

`structural_merge_scalability.cpp` 的 public `MergeStructurallyEquivalentGates` 實測：
1,000,000 個同組 gates 於 0.615 秒 merge 999,999 gates；500,000 層 cascade
（共 1,000,000 gates）於 1.141 秒 merge 500,000 gates。兩者皆無 rollback 且
structure validation 通過；test2 另覆蓋 dangling scope、named PO、tied pins 與
invalid-load rejection。DFF initial-state 合併語意仍列為 Deferred Boolean。

非 Boolean public Edit Apply 共 22 個 commands，均已有正向 regression；internal
low-level primitive、legacy structural alias 與代表性 invalid request 另有拒絕測試。
`rollbackOnFailure=false` 僅保留 request 相容性，public wrapper 在 validation failure
時仍固定 rollback，不提供保留無效設計的對外模式。

另外 `mini test/test26` 的 functional search/merge regression 覆蓋直接 timeout、apply timeout、cycle-safe rewrite 與 `CertifiedRewrite` report。官方原始 test29/test30 單獨執行 functional merge 時分別合併 7/1 顆；`mini test/test45` 依官方 prompt 重播 AND/NOT conversion、cleanup、double-inverter collapse 後再 merge，並以開發期獨立 CEC 與 write/readback CEC 抽查結果。正式 edit flow 不執行 final whole-design SAT。
