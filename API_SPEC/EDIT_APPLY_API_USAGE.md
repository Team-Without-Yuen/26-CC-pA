# Edit Apply API 使用說明

這份文件只負責說明 `EditApplyRequest` / `runEditApply()` 怎麼使用。  
`NetlistEditReport` 的欄位設計與目前完成狀態請看：

```text
API_SPEC/NETLIST_EDIT_REPORT_STATUS.md
```

---

## 1. 基本概念

`runEditApply()` 是 LLM / CLI 面向的統一修改入口。

基本形式：

```text
request.kind + optional fields -> runEditApply() -> NetlistEditReport
```

使用流程：

```text
1. 建立 Netlist::EditApplyRequest request
2. 設定 request.kind
3. 視 command 設定 gateName / netName / oldName / newName / maxFanout 等欄位
4. 視需求設定 request.validateEquivalence / request.rollbackOnFailure
5. 呼叫 netlist.runEditApply(request)
6. 從 report.success / report.changed / report.validation / report.diff / report.*Change 讀結果
```

注意：

```text
EditApply API 只處理會修改 netlist 的 command。
純查詢問題應使用 BasicQuery / DirectConnectivityQuery / ConeQuery / PathQuery / DepthQuery / FunctionQuery。
runEditApply() 內部會 dispatch 到既有 WithReport() wrapper，回傳統一 NetlistEditReport。
```

基本範例：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::CleanupBuffers;
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
if (report.success) {
    bool changed = report.changed;
    bool equivalent = report.validation.functionallyEquivalent;
}
```

---

## 2. EditCommandKind 總表

| Command kind | 問題類型 | 需要設定 | 主要讀取欄位 |
|---|---|---|---|
| `RenameGate` | rename gate | `oldName`, `newName` | `success`, `validation.equivalenceMethod` |
| `RenameNet` | rename wire/net | `oldName`, `newName` | `success`, `validation.equivalenceMethod` |
| `DisconnectGateInput` | 斷開 gate input | `gateName`, `netName` | `success`, `validation` |
| `ConnectGateInput` | 連接 gate input | `gateName`, `netName`, optional `pinIndex` | `success`, `validation` |
| `RemoveGate` | 移除單一 gate | `gateId` 或 `gateName` | `success`, `changedGateNames` |
| `ReplaceAllLoadsOfNet` | 把 old net 的 loads 改接到 new net | `oldNetId`, `newNetId` | `success`, `changedGateNames` |
| `CleanupBuffers` | 移除可 bypass 的 BUF | 無 | `diff`, `depthChange`, `validation.equivalenceMethod` |
| `CollapseDoubleInverter` | collapse NOT -> NOT | 無 | `diff`, `depthChange`, `validation.equivalenceMethod` |
| `LocalSimplificationFixpoint` | 執行 local simplification 到 fixpoint | 無 | `diff`, `depthChange`, `validation.equivalenceMethod` |
| `TrimDeadLogic` | 移除 dead logic | 無 | `diff`, `validation.equivalenceMethod` |
| `RemoveDanglingLogic` | 移除 dangling gates | 無 | `diff`, `validation.equivalenceMethod` |
| `RemoveUnusedNets` | 移除 unused internal nets | 無 | `changedNetNames`, `diff` |
| `MergeEquivalentGates` | 合併結構等價 gates | 無 | `diff`, `validation.equivalenceMethod` |
| `MergeStructurallyEquivalentGates` | structural hashing merge | 無 | `diff`, `validation.equivalenceMethod` |
| `SimplifyConstants` | constant propagation | 無 | `diff`, `validation.equivalenceMethod` |
| `SimplifySameInput` | same-input simplification | 無 | `diff`, `validation.equivalenceMethod` |
| `InsertBuffersForFanout` | 全設計 fanout buffer insertion | `maxFanout` | `fanoutChange`, `changedGateNames` |
| `InsertBuffersForSpecificNet` | 指定 net fanout buffer insertion | `netName`, `maxFanout` | `fanoutChange`, `changedGateNames` |
| `InsertBuffersForDffControl` | clock/reset fanout buffer insertion | `maxFanout`, `processClock`, `processReset` | `changedGateNames`, `validation` |
| `InsertBuffersOnEachLoad` | 每個 load 前插 dedicated BUF | `netName` | `changedGateNames`, `diff` |
| `InsertBufferAtDriver` | driver side 插 BUF | `netName` | `changedGateNames`, `changedNetNames` |
| `InsertBufferBeforeGate` | 指定 gate 前插 BUF | `netName`, `targetGateName` | `changedGateNames`, `changedNetNames` |
| `InsertBuffersByGateType` | 依 gate type 插 BUF | `gateType`, `bufferInputs`, `bufferOutputs` | `changedGateNames`, `diff` |
| `ReplaceGateWithNet` | 用 net 取代 gate output | `gateId`/`gateName`, `sourceNetId`/`netName` | `success`, `validation.equivalenceChecked` |
| `ReplaceGateWithConstant` | 用 constant net 取代 gate | `gateId`/`gateName`, `sourceNetId` | `success`, `validation.equivalenceChecked` |
| `ReplaceGateWithNotOfNet` | 用 NOT(source net) 取代 gate | `gateId`/`gateName`, `sourceNetId`/`netName` | `success`, `validation.equivalenceChecked` |
| `ReplaceDriverOfNet` | 換 target net driver | `targetNetId`, `newDriverGateId` | `success`, `validation` |
| `RewireGateOutputToExistingNet` | gate output 改接既有 net | `gateId`/`gateName`, `newOutputNetId` | `success`, `validation` |
| `ReplaceNetFunctionKeepingName` | 保留 net 名稱但替換 function | `targetNetId`, `sourceNetId` | `success`, `validation` |
| `MergeNetIntoNet` | merge net | `oldNetId`, `newNetId` | `success`, `changedNetNames` |
| `BypassNetKeepingPortSemantics` | bypass net 並保留 port 語意 | `netId`, `replacementNetId` | `success`, `validation` |
| `RedirectAllLoads` | 改接所有 loads | `oldNetId`, `newNetId`, optional `allowDuplicateLoads` | `success`, `changedGateNames` |
| `RemoveNetIfUnused` | 移除單一 unused net | `netId` 或 `netName` | `success`, `validation.equivalenceMethod` |

---

## 3. EditApplyRequest 輸入欄位

| 欄位 | 型別 | 預設值 | 用途 |
|---|---|---|---|
| `kind` | `EditCommandKind` | `Unknown` | 決定要執行哪一種 edit command |
| `gateName` | `std::string` | `""` | gate name 型 command 使用 |
| `netName` | `std::string` | `""` | net name 型 command 使用 |
| `oldName` | `std::string` | `""` | rename 類 command 的舊名稱 |
| `newName` | `std::string` | `""` | rename 類 command 的新名稱 |
| `targetGateName` | `std::string` | `""` | `InsertBufferBeforeGate` 使用 |
| `gateId` | `int` | `-1` | gate ID 型 command 使用；若同時有 `gateName`，優先用 ID |
| `netId` | `int` | `-1` | net ID 型 command 使用；若同時有 `netName`，優先用 ID |
| `oldNetId` | `int` | `-1` | old/from net ID |
| `newNetId` | `int` | `-1` | new/to net ID |
| `sourceNetId` | `int` | `-1` | source net ID |
| `targetNetId` | `int` | `-1` | target net ID |
| `replacementNetId` | `int` | `-1` | replacement net ID |
| `newDriverGateId` | `int` | `-1` | new driver gate ID |
| `newOutputNetId` | `int` | `-1` | new output net ID |
| `maxFanout` | `int` | `-1` | fanout buffer insertion 使用；未設定時使用 4 |
| `pinIndex` | `int` | `-1` | 指定 gate input pin；`-1` 表示自動選擇 |
| `gateType` | `GateType` | `UNKNOWN` | `InsertBuffersByGateType` 使用 |
| `processClock` | `bool` | `false` | `InsertBuffersForDffControl` 是否處理 clock |
| `processReset` | `bool` | `false` | `InsertBuffersForDffControl` 是否處理 reset |
| `bufferInputs` | `bool` | `true` | `InsertBuffersByGateType` 是否 buffer inputs |
| `bufferOutputs` | `bool` | `true` | `InsertBuffersByGateType` 是否 buffer outputs |
| `allowDuplicateLoads` | `bool` | `false` | `RedirectAllLoads` 是否允許 duplicate loads |
| `validateEquivalence` | `bool` | `false` | 要求等價性資訊；若 command 沒 certificate，report 會加 warning |
| `rollbackOnFailure` | `bool` | `true` | 保留欄位；目前 WithReport wrapper validation 失敗會 rollback |

---

## 4. NetlistEditReport 回傳欄位

`runEditApply()` 一律回傳 `NetlistEditReport`。不同 command 會填不同欄位。

### 4.1 共用狀態

| 欄位 | 意思 |
|---|---|
| `success` | edit 是否成功且通過 validation |
| `changed` | netlist 是否有實際變更 |
| `rolledBack` | 失敗時是否已 rollback |
| `operationName` | 統一為 `edit_apply:<command_name>` |
| `message` | debug / LLM response 用的簡短訊息 |
| `warnings` | 附加警告，例如要求 equivalence 但 command 沒 certificate |

### 4.2 差異與驗證

| 欄位 | 意思 |
|---|---|
| `beforeStats` | edit 前統計 |
| `afterStats` | edit 後統計 |
| `diff` | gate/net/type count 差異 |
| `validation.structureValid` | graph 結構是否合法 |
| `validation.problemAConstraintsValid` | Problem A 約束是否合法 |
| `validation.equivalenceChecked` | 是否有等價性證明 |
| `validation.functionallyEquivalent` | 若有檢查，是否等價 |
| `validation.equivalenceMethod` | `NotChecked` / `StructuralIdentity` / `LocalRewriteRule` / `WholeDesignSat` |

### 4.3 特定類型欄位

| 欄位 | 使用情境 |
|---|---|
| `depthChange` | cleanup / simplification 類 wrapper |
| `fanoutChange` | fanout buffer insertion 類 command |
| `mappingDelta` | technology mapping 類 wrapper；目前不由 `runEditApply()` dispatch |
| `changedGateIds`, `changedGateNames` | 新增、移除、修改的 gates |
| `changedNetIds`, `changedNetNames` | 新增、移除、修改的 nets |

---

## 5. RenameNet

用途：

```text
重新命名 wire/net，功能不變。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::RenameNet;
request.oldName = "n1214";
request.newName = "renamed_wire";
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否成功 | `report.success` |
| 是否改名 | `report.changed` |
| 等價性理由 | `report.validation.equivalenceMethod`，應為 `StructuralIdentity` |

---

## 6. InsertBuffersForSpecificNet

用途：

```text
對指定 net 插入 buffer，讓 fanout 不超過限制。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::InsertBuffersForSpecificNet;
request.netName = "n1";
request.maxFanout = 4;
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 原本最大 fanout | `report.fanoutChange->beforeMaxFanout` |
| 修改後最大 fanout | `report.fanoutChange->afterMaxFanout` |
| 是否滿足 fanout limit | `report.fanoutChange->meetsConstraint` |
| 新增 BUF 名稱 | `report.changedGateNames` |
| 等價性理由 | `report.validation.equivalenceMethod`，應為 `LocalRewriteRule` |

---

## 7. InsertBuffersOnEachLoad

用途：

```text
對指定 signal 的每個 load 插 dedicated buffer。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::InsertBuffersOnEachLoad;
request.netName = "n2";
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 新增幾個 BUF | `report.changedGateNames.size()` 或 `report.diff.gateTypeCountDelta[GateType::BUF]` |
| 新增 BUF 名稱 | `report.changedGateNames` |
| 等價性理由 | `report.validation.equivalenceMethod` |

---

## 8. CleanupBuffers

用途：

```text
移除可安全 bypass 的 BUF chain。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::CleanupBuffers;
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否有移除 | `report.changed` |
| gate count 變化 | `report.diff.activeGateCountDelta` |
| critical depth 是否改善 | `report.depthChange->improved` |
| 等價性理由 | `report.validation.equivalenceMethod`，應為 `LocalRewriteRule` |

---

## 9. CollapseDoubleInverter

用途：

```text
找出 NOT -> NOT 並 collapse 成 wire。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::CollapseDoubleInverter;
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否有 collapse | `report.changed` |
| gate count 變化 | `report.diff.activeGateCountDelta` |
| depth 變化 | `report.depthChange` |
| 等價性理由 | `report.validation.equivalenceMethod`，應為 `LocalRewriteRule` |

---

## 10. RemoveDanglingLogic

用途：

```text
移除不會貢獻到任何 PO / DFF input endpoint 的 dangling gates。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::RemoveDanglingLogic;
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否有移除 dangling gates | `report.changed` |
| 移除數量估計 | `-report.diff.activeGateCountDelta` |
| 等價性理由 | `report.validation.equivalenceMethod`，應為 `StructuralIdentity` |

---

## 11. SimplifyConstants

用途：

```text
對 constant input gate 做 constant propagation。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::SimplifyConstants;
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否有化簡 | `report.changed` |
| gate type 變化 | `report.diff.gateTypeCountDelta` |
| 等價性理由 | `report.validation.equivalenceMethod`，應為 `LocalRewriteRule` |

---

## 12. ReplaceGateWithNet

用途：

```text
低階 primitive，把 gate 的 loads 改接到指定 source net。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::ReplaceGateWithNet;
request.gateName = "g_const";
request.netName = "a";

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

注意：

```text
這類 primitive mutation 可能改變功能。
目前只做 structure / Problem A validation，不自動宣稱 functional equivalence。
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否修改成功 | `report.success` |
| 是否 rollback | `report.rolledBack` |
| 是否有等價性證明 | `report.validation.equivalenceChecked`，通常為 `false` |

---

## 13. Prompt 對應表

| Prompt | 建議 EditCommandKind | 需要設定 | 主要讀取 |
|---|---|---|---|
| Rename wire n1214 to renamed_wire. Ensure functional equivalence is preserved. | `RenameNet` | `oldName = "n1214"`, `newName = "renamed_wire"` | `success`, `validation.equivalenceMethod` |
| Update the name of signal n440 to renamed_wire throughout the netlist. | `RenameNet` | `oldName = "n440"`, `newName = "renamed_wire"` | `success`, `changed` |
| Try to insert buffers on reset signal n1 to reduce fanout to at most 4. | `InsertBuffersForSpecificNet` | `netName = "n1"`, `maxFanout = 4` | `fanoutChange`, `changedGateNames` |
| Insert buffers wherever needed so that no signal drives more than 16 loads. | `InsertBuffersForFanout` | `maxFanout = 16` | `fanoutChange`, `changedGateNames` |
| Insert a BUF gate on signal n2 so that each load is driven through a dedicated buffer. | `InsertBuffersOnEachLoad` | `netName = "n2"` | `changedGateNames`, `diff` |
| Find all back-to-back inverter pairs and collapse them into a wire. | `CollapseDoubleInverter` | 無 | `changed`, `diff`, `validation.equivalenceMethod` |
| Check if there are dangling gates. Remove them if found. | `RemoveDanglingLogic` | 無 | `changed`, `diff.activeGateCountDelta` |
| Prune the netlist of unused gates. | `TrimDeadLogic` 或 `RemoveDanglingLogic` | 無 | `changed`, `diff` |
| Sweep out dangling gates. | `RemoveDanglingLogic` | 無 | `changed`, `diff` |
| Simplify reported NAND gates by propagating constant inputs. | `SimplifyConstants` | 無 | `diff`, `validation.equivalenceMethod` |
| Simplify OR gates with constant 1 input. | `SimplifyConstants` | 無 | `diff`, `validation.equivalenceMethod` |
| Try to merge structural duplicates. | `MergeStructurallyEquivalentGates` | 無 | `changed`, `diff` |
| Remove unused internal nets. | `RemoveUnusedNets` | 無 | `changedNetNames`, `diff` |

---

## 14. 何時不要用 EditApply

如果問題只是查詢，不要使用 `runEditApply()`。

| 問題 | 應改用 |
|---|---|
| Please count all gates by gate type. | BasicQuery |
| What type of gate is g0? | BasicQuery / GateInfo |
| Report every gate connected to the output of g0. | DirectConnectivityQuery |
| What is the fanout of primary input n0? | DirectConnectivityQuery / FanoutLoadReport |
| Determine all gates reachable from n2. | ConeQuery |
| Compute transitive fanout cone of input n13. | ConeQuery |
| Compute maximum logic depth from input n13 to output n25[0]. | PathQuery / DepthAnalysis |
| What is the global critical path? | DepthAnalysis |
| Check functional equivalence between internal signals n1287 and n2404. | FunctionQuery |
| Verify current design equivalent to original loaded netlist. | Future whole-design equivalence API |

---

## 15. 等價性欄位解讀

| equivalenceMethod | 意思 | 常見 command |
|---|---|---|
| `NotChecked` | 目前沒有等價性證明 | low-level primitive rewrite |
| `StructuralIdentity` | 名稱改變、unused/dangling/dead logic removal、structural merge | `RenameNet`, `RemoveDanglingLogic`, `TrimDeadLogic` |
| `LocalRewriteRule` | 可由局部 Boolean identity 證明 | `CleanupBuffers`, `CollapseDoubleInverter`, `SimplifyConstants`, buffer insertion |
| `WholeDesignSat` | 保留給未來 whole-design SAT equivalence | 尚未接入 |

---

## 16. 測試狀態

目前 `mini test/tester.cpp` 已涵蓋：

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
