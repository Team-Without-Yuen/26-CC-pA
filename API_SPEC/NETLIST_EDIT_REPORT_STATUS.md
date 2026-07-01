# Netlist Edit Report Status

這份文件整理目前已完成的 `NetlistEditReport` / `WithReport()` 架構，以及它和後續 OPT flow 的關係。

目前設計原則是：

```text
底層 primitive 保持 bool / int / legacy report，給內部演算法快速使用。
高階 LLM / CLI / OPT flow 一律走 NetlistEditReport，取得 before/after、validation、rollback 與 changed object 資訊。
```

---

## 1. 為什麼需要 NetlistEditReport

原本很多會修改 netlist 的 API 只回傳 `bool`、`int`、`BufferInsertionReport` 或 `TechMapReport`。這對內部 pass 夠用，但對 LLM 或 CLI 不夠，因為題目可能追問：

```text
改了哪些 gate / net？
before / after 差異是多少？
修改後 graph 是否合法？
是否符合 Problem A 的 PO / DFF constraint？
失敗時有沒有 rollback？
technology mapping 或 fanout insertion 的細節是什麼？
```

因此目前新增共同回傳格式 `NetlistEditReport`，作為所有 mutation / transformation / optimization apply 的共同外層 report。

---

## 2. NetlistEditReport 主要欄位

位置：

```cpp
include/core/NetlistEditReport.h
```

核心欄位：

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

std::vector<int> changedGateIds;
std::vector<int> changedNetIds;
std::vector<std::string> changedGateNames;
std::vector<std::string> changedNetNames;
std::vector<std::string> warnings;
```

目前 validation 會檢查：

```text
validateStructure()
validateProblemAConstraints()
```

equivalence 欄位已預留，但目前還沒有整合 whole-design equivalence check。

---

## 3. WithReport() 的執行方式

`WithReport()` 不是單純把 `bool` 包起來，而是多做一層可解釋流程：

```text
1. clone before snapshot
2. 執行原本底層 primitive / pass
3. 收集 changed gate / net 或轉換原本專屬 report
4. 建立 before / after stats 與 diff
5. 執行 structure / Problem A validation
6. validation 失敗則 rollback
7. 回傳 NetlistEditReport
```

單次操作適合直接使用 `WithReport()`。大型 pass 則不應每一步都呼叫 `WithReport()`，未來應改成：

```text
snapshot once
run many primitive changes
record EditTrace
validate once
return one NetlistEditReport
```

---

## 4. 已完成的 Report Building Blocks

已新增：

```cpp
NetlistStats collectNetlistStats() const;
static NetlistDiff diffStats(const NetlistStats& before, const NetlistStats& after);
static EditValidationResult validateEditResult(const Netlist& before, const Netlist& after);
static NetlistEditReport buildEditReport(...);
static void certifyEquivalence(NetlistEditReport& report, EquivalenceCheckMethod method, ...);
```

目前 `buildEditReport()` 會填入：

```text
beforeStats
afterStats
diff
validation
success
changed
operationName
operationKind
```

`EditValidationResult` 目前也新增 equivalence certificate 欄位：

```cpp
enum class EquivalenceCheckMethod {
    NotChecked,
    StructuralIdentity,
    LocalRewriteRule,
    WholeDesignSat
};
```

`validateEditResult()` 本身仍不做 whole-design equivalence，因此預設是 `NotChecked`。
只有特定 `WithReport()` wrapper 知道自己的修改規則時，才會呼叫 `certifyEquivalence()` 將結果標成等價。

---

## 5. 已完成的 WithReport API

### 5.1 Graph Mutation Primitives

```cpp
renameGateWithReport()
renameNetWithReport()
disconnectGateInputWithReport()
connectGateInputWithReport()
removeGateWithReport()
replaceAllLoadsOfNetWithReport()
```

用途：提供 LLM / CLI 對單一 graph edit 的安全入口。

其中 `renameGateWithReport()` / `renameNetWithReport()` 會標成 `StructuralIdentity`。
其他低階 mutation 預設不標等價，因為它們可能真的改變電路功能。

---

### 5.2 Buffer Insertion Transformations

```cpp
insertBuffersForFanoutWithReport()
insertBuffersForSpecificNetWithReport()
insertBuffersForDffControlWithReport()
insertBuffersOnEachLoadWithReport()
insertBufferAtDriverWithReport()
insertBufferBeforeGateWithReport()
insertBuffersByGateTypeWithReport()
```

這些 wrapper 會把原本 `BufferInsertionReport` 轉成：

```text
NetlistEditReport.changedGateIds / changedGateNames
NetlistEditReport.changedNetIds / changedNetNames
NetlistEditReport.fanoutChange
```

這類 wrapper 會標成 `LocalRewriteRule`，理由是插入 BUF 只改變連接結構，不改變被驅動 load 的 Boolean function。

---

### 5.3 Cleanup / Simplification Passes

```cpp
trimDeadLogicWithReport()
collapseBackToBackInvertersWithReport()
mergeEquivalentGatesWithReport()
cleanupAllRemovableBuffersWithReport()
removeDanglingLogicWithReport()
mergeStructurallyEquivalentGatesWithReport()
simplifyAllGatesWithConstantsWithReport()
simplifyAllSameInputGatesWithReport()
runLocalSimplificationFixpointWithReport()
```

用途：作為後續 `opt_apply` 第一版最適合接的 pass。

目前 optimization / cleanup 類 wrapper 會填入：

```text
NetlistEditReport.depthChange
```

目前下列類型會填入 equivalence certificate：

```text
StructuralIdentity:
trimDeadLogicWithReport()
removeDanglingLogicWithReport()
mergeEquivalentGatesWithReport()
mergeStructurallyEquivalentGatesWithReport()
removeNetIfUnusedWithReport()
removeUnusedNetsWithReport()

LocalRewriteRule:
cleanupAllRemovableBuffersWithReport()
collapseBackToBackInvertersWithReport()
simplifyAllGatesWithConstantsWithReport()
simplifyAllSameInputGatesWithReport()
runLocalSimplificationFixpointWithReport()
```

第一版 depth comparison 使用全設計 global critical path：

```text
beforeDepth
afterDepth
improved
meetsTarget
```

目前 targetDepth 尚未由 opt request 傳入，因此 `meetsTarget` 只有在呼叫 helper 時提供 target 才會判斷。

---

### 5.4 Gate Replacement Primitives

```cpp
replaceGateWithNetWithReport()
replaceGateWithConstantWithReport()
replaceGateWithNotOfNetWithReport()
```

用途：支援 constant propagation、same-input simplification、local rewrite 的可解釋套用。

---

### 5.5 Driver / Output Rewrite Primitives

```cpp
replaceDriverOfNetWithReport()
rewireGateOutputToExistingNetWithReport()
replaceNetFunctionWithNetKeepingNameWithReport()
```

用途：處理 PO-safe rewrite、driver replacement、保留 output net 名稱的 function replacement。

---

### 5.6 Net Merge / Net Cleanup Primitives

```cpp
mergeNetIntoNetWithReport()
bypassNetKeepingPortSemanticsWithReport()
redirectAllLoadsWithReport()
removeNetIfUnusedWithReport()
removeUnusedNetsWithReport()
```

目前也修正了 `mergeNetIntoNet()` 的 PO 語意轉移問題：

```text
若 fromNet 是 PO，primaryOutputs 轉到 toNet 後，fromNet 必須取消 isPO。
否則 fromNet 會變成沒有 driver 的 PO，造成 validation 失敗。
```

---

### 5.7 Technology Mapping

```cpp
mapTechnologyWithReport()
mapTechnologyForConeWithReport()
convertToAndNotWithReport()
convertToOrNotWithReport()
convertToNandWithReport()
convertToNorWithReport()
convertToXagWithReport()
convertToAnfWithReport()
convertToXorOrWithReport()
convertToXnorAndWithReport()
convertToXnorOrWithReport()
customMapTechnologyWithReport()
```

這些 wrapper 會把 `TechMapReport` 轉成：

```text
NetlistEditReport.mappingDelta
NetlistEditReport.changedGateNames
NetlistEditReport.validation
```

---

## 6. 已修正的底層問題

report 化過程中，validation 抓到幾個實際問題，已修正：

```text
1. removed net 需要 tombstone flag，避免重複移除與 addNet name collision。
2. stricter validateStructure() 檢查 driverGateId / outputNetId 雙向一致。
3. trimDeadLogic / removeDanglingLogic 改用 markGateRemoved()，避免 stale driver。
4. insertBufferAtDriver / insertBufferBeforeGate 避免 vector reallocation 後繼續使用失效 reference。
5. mergeNetIntoNet() 轉移 PO 語意後取消舊 net 的 isPO。
```

---

## 7. 測試狀態

目前 `mini test/tester.cpp` 已補上 smoke tests，涵蓋：

```text
report helper
primitive mutation wrappers
buffer insertion wrappers
cleanup / simplification wrappers
gate replacement wrappers
driver / output rewrite wrappers
net merge / cleanup wrappers
technology mapping wrapper failure rollback
```

目前驗證結果：

```text
Summary: 106 passed, 0 failed.
```

---

## 8. 目前限制

目前仍有幾個限制：

```text
1. whole-design equivalence 尚未接入；目前只有 rename / buffer insertion / cleanup / simplification 等可由局部規則證明的 wrapper 會標記 equivalence certificate。
2. depthChange 已接 global critical depth comparison，但尚未支援 opt request targetDepth / endpoint-specific depth comparison。
3. 大型 optimization pass 尚未導入 EditTrace，因此大量細節仍應由 pass 層額外記錄。
4. CLI dispatcher 尚未建立，main.cpp 目前仍是簡單讀檔 / 輸出入口。
5. opt_report / opt_rollback 尚未實作。
6. OptimizationResult 仍是 legacy / transitional format，後續應逐步由 NetlistEditReport 或 OptQueryReport 取代。
```

---

## 9. OPT Flow Skeleton

完成 `WithReport()` API 層後，已開始建立 OPT 第一版流程。新增型別位置：

```cpp
include/core/OptimizationFlow.h
src/optimization/OptimizationFlow.cpp
```

目前提供：

```cpp
OptQueryReport runOptQuery(const OptQueryRequest& request) const;
NetlistEditReport runOptApply(const OptApplyRequest& request);
```

設計方向：

```text
opt_query:
  只分析 candidate，不修改 netlist。

opt_apply:
  第一版 dispatch 到既有 WithReport pass，回傳 NetlistEditReport。

opt_report:
  尚未實作，後續應保存最近一次 apply 的 NetlistEditReport。

opt_rollback:
  尚未實作，後續應保存 apply 前 snapshot stack。
```

第一版目前支援：

```text
cleanup_buffer_chain
collapse_double_inverter
local_simplification_fixpoint
```

第一版限制：

```text
1. opt_apply 目前是 pass-level apply，不是 candidate-specific apply。
2. candidateIds 已保留在 OptApplyRequest，但目前只記 warning。
3. scopeName 目前只記錄，不強制限制 scope。
4. whole-design equivalence 尚未接入。
5. 尚未有 opt_report / opt_rollback session state。
```

目前測試已涵蓋：

```text
runOptQuery(CleanupBufferChain)
runOptApply(CleanupBufferChain)
runOptQuery(CollapseDoubleInverter)
runOptApply(CollapseDoubleInverter)
runOptQuery(LocalSimplificationFixpoint)
runOptApply(LocalSimplificationFixpoint)
```

---

## 10. Edit Apply Flow

完成 `WithReport()` API 層後，新增一層高階 edit apply API，作為 LLM / CLI 的統一修改入口。

新增型別位置：

```cpp
include/core/EditFlow.h
src/transformation/EditFlow.cpp
```

目前提供：

```cpp
NetlistEditReport runEditApply(const EditApplyRequest& request);
```

設計方向：

```text
LLM / CLI command
  -> EditApplyRequest
  -> runEditApply()
  -> existing WithReport()
  -> NetlistEditReport
```

第一版支援三大類 command：

```text
1. primitive / mutation:
   rename、connect/disconnect、remove、replace、rewire、merge、bypass。

2. cleanup / simplification:
   cleanup buffer、collapse inverter、local simplification、dead/dangling/unused cleanup、structural merge。

3. buffer insertion:
   global fanout、specific net、DFF control、per-load、driver-side、before-gate、by-gate-type。
```

`runEditApply()` 會把底層 wrapper 的 `operationName` 統一改成：

```text
edit_apply:<command_name>
```

例如：

```text
edit_apply:rename_gate
edit_apply:cleanup_buffers
edit_apply:insert_buffers_for_specific_net
```

詳細用法整理在：

```text
API_SPEC/EDIT_APPLY_API_USAGE.md
```

目前測試已涵蓋：

```text
runEditApply rename_gate
runEditApply cleanup_buffers
runEditApply insert_buffers_for_specific_net
runEditApply replace_gate_with_net remains unchecked
runEditApply unsupported command
```

目前驗證結果：

```text
Summary: 111 passed, 0 failed.
```
