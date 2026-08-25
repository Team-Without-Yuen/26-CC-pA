# Edit Apply 使用說明

這份文件只負責說明 `EditApplyRequest` / `runEditApply()` 怎麼使用。  
內部 API 設計、`NetlistEditReport` 欄位與 low-level primitive 邊界請看：

```text
API_SPEC/EDIT_APPLY_API.md
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
5. runEditApply() 先做 request validation
6. validation 通過後才執行對應 edit command
7. 從 report.success / report.changed / report.validation / report.diff / report.*Change 讀結果
```

注意：

```text
EditApply API 只處理會修改 netlist 的 command。
純查詢問題應使用 BasicQuery / DirectConnectivityQuery / ConeQuery / PathQuery / DepthQuery / FunctionQuery。
runEditApply() 會回傳統一 NetlistEditReport。
缺少必要參數、物件不存在或 fanout limit 不合法時，會直接回傳 failed NetlistEditReport，不修改 netlist。
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

## 2. Public Command 分類

外部 LLM / CLI 只應使用這些 public command。低階 rewiring、未經證明的 arbitrary merge、driver replacement 不在這份 usage 文件中開放；SAT-proven functional duplicate merge 只能走專用高階 command。

| 類別 | Command kind | 常見問題 |
|---|---|---|
| Rename | `RenameGate`, `RenameNet` | 改 gate / net 名稱且保持功能不變 |
| Cleanup / Simplification | `CleanupBuffers`, `CollapseDoubleInverter`, `LocalSimplificationFixpoint`, `SafeCleanupFixpoint`, `TrimDeadLogic`, `RemoveDanglingLogic`, `RemoveUnusedNets`, `MergeStructurallyEquivalentGates`, `MergeFunctionallyEquivalentGates`, `SimplifyConstants`, `SimplifySameInput` | 移除結構/功能重複、dangling、dead logic、unused nets、constant propagation、local simplification |
| Buffer Insertion | `InsertBuffersForFanout`, `InsertBuffersForSpecificNet`, `InsertBuffersForDffControl`, `InsertBuffersOnEachLoad`, `InsertBufferAtDriver`, `InsertBufferBeforeGate`, `InsertBuffersByGateType` | 降 fanout、指定 signal/gate 插 BUF |
| Technology Mapping | `ConvertToBasis`, `ReplaceGateType` | basis conversion、XOR/XNOR/OR 等 gate type replacement |
| Safe Single-Net Cleanup | `RemoveNetIfUnused` | 移除已確認 unused 的 net |

### 2.1 Public Command 總表

| Command kind | 問題類型 | 需要設定 | 主要讀取欄位 |
|---|---|---|---|
| `RenameGate` | rename gate | `oldName`, `newName` | `success`, `validation.equivalenceMethod` |
| `RenameNet` | rename wire/net | `oldName`, `newName` | `success`, `validation.equivalenceMethod` |
| `CleanupBuffers` | 移除可 bypass 的 BUF | 無 | `diff`, `depthChange`, `validation.equivalenceMethod` |
| `CollapseDoubleInverter` | collapse NOT -> NOT | 無 | `diff`, `depthChange`, `validation.equivalenceMethod` |
| `LocalSimplificationFixpoint` | 執行 local simplification 到 fixpoint | 無 | `diff`, `depthChange`, `validation.equivalenceMethod` |
| `SafeCleanupFixpoint` | 執行安全 cleanup / simplification 到 fixpoint | 無 | `diff`, `depthChange`, `validation.equivalenceMethod`, `warnings` |
| `TrimDeadLogic` | 移除 dead logic | 無 | `diff`, `validation.equivalenceMethod` |
| `RemoveDanglingLogic` | 移除 dangling gates | 無 | `diff`, `validation.equivalenceMethod` |
| `RemoveUnusedNets` | 移除 unused internal nets | 無 | `changedNetNames`, `diff` |
| `MergeStructurallyEquivalentGates` | structural hashing merge | 無 | `diff`, `validation.equivalenceMethod` |
| `MergeFunctionallyEquivalentGates` | SAT-based functional duplicate merge | `scope`，可選 `scopeName`, `gateType`, `simulationPatternCount`, `timeLimitSeconds` | `functionalMerge`, `diff`, `validation` |
| `SimplifyConstants` | typed constant propagation | 可選 `gateType`, `constValue`, `inputCount` | `constantSimplification`, `diff`, `validation.equivalenceMethod` |
| `SimplifySameInput` | same-input simplification | 無 | `diff`, `validation.equivalenceMethod` |
| `InsertBuffersForFanout` | 全設計 fanout buffer insertion | `maxFanout` | `fanoutChange`, `changedGateNames` |
| `InsertBuffersForSpecificNet` | 指定 net fanout buffer insertion | `netName`, `maxFanout` | `fanoutChange`, `changedGateNames` |
| `InsertBuffersForDffControl` | clock/reset fanout buffer insertion | `maxFanout`, `processClock`, `processReset` | `changedGateNames`, `validation` |
| `InsertBuffersOnEachLoad` | 每個 load 前插 dedicated BUF | `netName` | `changedGateNames`, `diff` |
| `InsertBufferAtDriver` | driver side 插 BUF | `netName` | `changedGateNames`, `changedNetNames` |
| `InsertBufferBeforeGate` | 指定 gate 前插 BUF | `netName`, `targetGateName` | `changedGateNames`, `changedNetNames` |
| `InsertBuffersByGateType` | 依 gate type 插 BUF | `gateType`, `bufferInputs`, `bufferOutputs` | `changedGateNames`, `diff` |
| `ConvertToBasis` | 全設計或指定 cone 轉成指定 gate basis | `scope`, `scopeName`, `allowedTypes` 或 `bannedTypes` | `mappingDelta`, `diff`, `validation.equivalenceMethod` |
| `ReplaceGateType` | 全設計或指定 cone 中替換某種 gate type | `scope`, `scopeName`, `targetGateType`, `allowedTypes` | `mappingDelta`, `diff`, `validation.equivalenceMethod` |
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
| `scope` | `TargetScope` | `WHOLE_NETLIST` | technology mapping 的作用範圍 |
| `scopeName` | `std::string` | `""` | scope 不是 whole 時指定 net/gate 名稱 |
| `netId` | `int` | `-1` | `RemoveNetIfUnused` 可用；若同時有 `netName`，優先用 ID |
| `maxFanout` | `int` | `-1` | fanout buffer insertion 使用；未設定時使用 4 |
| `constValue` | `int` | `-1` | `SimplifyConstants`：`-1` 不限、`0/1` 指定 constant input |
| `inputCount` | `int` | `-1` | `SimplifyConstants`：`-1` 不限、正整數指定 gate fanin 數 |
| `gateType` | `GateType` | `UNKNOWN` | gate-type buffer、`SimplifyConstants` 或 functional merge filter；`UNKNOWN` 表示不限 |
| `simulationPatternCount` | `size_t` | `256` | functional merge simulation prefilter，合法範圍 1..4096 |
| `timeLimitSeconds` | `double` | `290.0` | functional merge 或 technology mapping 的巢狀流程共用同一總 deadline |
| `targetGateType` | `GateType` | `UNKNOWN` | `ReplaceGateType` 要移除/替換的 gate 類型 |
| `allowedTypes` | `std::vector<GateType>` | `{}` | `ConvertToBasis` / `ReplaceGateType` 允許生成的 gate basis |
| `bannedTypes` | `std::vector<GateType>` | `{}` | `ConvertToBasis` 額外禁止的 gate 類型 |
| `processClock` | `bool` | `false` | `InsertBuffersForDffControl` 是否處理 clock |
| `processReset` | `bool` | `false` | `InsertBuffersForDffControl` 是否處理 reset |
| `bufferInputs` | `bool` | `true` | `InsertBuffersByGateType` 是否 buffer inputs |
| `bufferOutputs` | `bool` | `true` | `InsertBuffersByGateType` 是否 buffer outputs |
| `verbose` | `bool` | `false` | technology mapping debug log |
| `validateEquivalence` | `bool` | `false` | 要求等價性資訊；若 command 沒 certificate，report 會加 warning |
| `rollbackOnFailure` | `bool` | `true` | 相容性保留欄位；public wrapper 在 validation failure 時固定 rollback，即使設為 `false` 也只回 warning，不保留無效設計 |

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
| `validation.problemAConstraintsBaselineValid` | edit 前是否已滿足全部 Problem A structural constraints |
| `validation.problemAConstraintsValid` | edit 後是否絕對滿足全部 Problem A structural constraints |
| `validation.problemAConstraintsRegressed` | edit 是否新增 baseline 原本沒有的違規；為 true 時 edit 失敗並 rollback |
| `validation.newProblemAConstraintViolations` | 此次 edit 新增的違規清單 |
| `validation.equivalenceChecked` | 是否有等價性證明 |
| `validation.functionallyEquivalent` | 若有檢查，是否等價 |
| `validation.equivalenceMethod` | `NotChecked` / `StructuralIdentity` / `LocalRewriteRule` / `CertifiedRewrite` / `WholeDesignSat` |

### 4.3 特定類型欄位

| 欄位 | 使用情境 |
|---|---|
| `depthChange` | cleanup / simplification 類 wrapper |
| `fanoutChange` | fanout buffer insertion 類 command |
| `mappingDelta` | technology mapping 類結果，包含 removed / added / final gate count |
| `constantSimplification` | constant propagation 的 filter、candidate、simplified、skipped 與 target gate eliminated count |
| `functionalMerge` | functional search/class 統計、apply timeout、merged/skipped 數量與逐筆 representative/removed records；legacy whole-design 欄位維持 false |
| `changedGateIds`, `changedGateNames` | 新增、移除、修改的 gates |
| `changedNetIds`, `changedNetNames` | 新增、移除、修改的 nets |

---

## 5. Request Validation

`runEditApply()` 會先檢查 request 是否足夠明確。若 validation 失敗，不會執行修改，也不會改變 netlist。

失敗 report 的共通語意：

```text
success = false
changed = false
rolledBack = false
operationName = edit_apply:<command_name>
message = 具體錯誤原因
validation.messages 會包含同一個錯誤訊息
```

目前已檢查：

| 類型 | 檢查內容 |
|---|---|
| rename | `oldName` / `newName` 不可空、舊物件需存在、新名稱不可衝突 |
| name-based command | `gateName` / `netName` 不可空，且物件需存在 |
| id-based command | ID 必須在有效範圍內，且 gate/net 不可已 removed |
| fanout command | `maxFanout` 若有設定，必須 >= 2；未設定時使用預設 4 |
| rename net | constant net 不可改名；一般 net 也不可改成保留 literal `1'b0` / `1'b1` |
| DFF control buffer | `processClock` / `processReset` 至少一個為 true |
| by-gate-type buffer | `gateType != UNKNOWN`，且至少選擇 input/output 其中一種 |
| constant simplification | `gateType` 必須為 combinational type 或 `UNKNOWN`；`constValue` 只能是 `-1/0/1`；`inputCount` 只能是 `-1` 或正整數 |
| technology mapping | scope target 存在、gate type 合法、`allowedTypes` / `bannedTypes` 足夠明確、`timeLimitSeconds` 為 finite positive |

範例：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::InsertBuffersForFanout;
request.maxFanout = 1;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

預期：

```text
report.success = false
report.changed = false
report.message = "Invalid maxFanout: must be at least 2."
```

---

## 6. RenameNet

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

## 7. InsertBuffersForSpecificNet

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

`InsertBuffersForSpecificNet` 的 `beforeMaxFanout` / `afterMaxFanout` 只統計指定 net（bus 時含各 bit）與本次建立的 buffer tree，不受其他未指定 high-fanout net 影響。全設計 fanout constraint 必須改用 `InsertBuffersForFanout`。

---

## 8. InsertBuffersOnEachLoad

用途：

```text
對指定 signal 的每個 load 插 dedicated buffer。
```

這裡的 load 依 Problem A fanout 規則以 sink pin 為單位。同一顆 gate 若有兩個
input pins 都接到指定 signal，會建立兩顆 dedicated BUF，而不是把該 gate 只算一次。

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

## 9. CleanupBuffers

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

## 10. CollapseDoubleInverter

用途：

```text
找出 NOT -> NOT 並 collapse 成 wire。
此 command 只處理 inverter pair，不會隱含執行 BUF cleanup；先前建立的 fanout buffer tree 會保留。
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

若第二顆 NOT 直接驅動 PO，目前會保守跳過；一般 internal NOT -> NOT chain 可直接 collapse。

candidate 必須同時滿足兩顆 NOT 的單一輸入、三條 active net、driver/load/input
雙向一致與 pin-level fanout 一致。遇到 tombstone、stale edge 或非法 ID 時會安全跳過，
不會在 validation 前先進行部分 mutation。100 萬個串接 NOT 已以 public
`runEditApply()` 實測，約 0.741 秒 collapse 500,000 pairs。

---

## 11. SafeCleanupFixpoint

用途：

```text
對整個 netlist 反覆執行安全 cleanup，直到沒有更多可套用的改變。
適合題目用語較廣的 prompt，例如 trim/prune unused logic、remove redundant gates、remove floating nodes。
```

目前會組合：

```text
LocalSimplificationFixpoint
MergeStructurallyEquivalentGates
TrimDeadLogic
RemoveDanglingLogic
RemoveUnusedNets
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::SafeCleanupFixpoint;
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否有任何 cleanup | `report.changed` |
| gate / net count 變化 | `report.diff.activeGateCountDelta`, `report.diff.activeNetCountDelta` |
| depth 是否改善 | `report.depthChange` |
| 若已經沒有可清理項目 | `report.warnings` |
| 等價性理由 | `report.validation.equivalenceMethod`，應為 `LocalRewriteRule` |

---

### 11.1 MergeStructurallyEquivalentGates

用途：合併 gate type 與 input net 結構完全相同的 duplicates。

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::MergeStructurallyEquivalentGates;
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取 `report.changed` 判斷是否找到 duplicates，並以
`-report.diff.activeGateCountDelta` 取得本次實際移除 gate 數量。此 command 不會
隱含移除 unrelated dangling/dead logic；需要廣義 cleanup 時使用
`SafeCleanupFixpoint`。不同具名 PO 的 duplicate drivers 會保守保留；PO driver 與
internal duplicate 同組時使用 PO 作 canonical。

100 萬個同組 gates 實測約 0.615 秒；50 萬層、共 100 萬 gates 的 cascade 實測約
1.141 秒。DFF initial-state / sequential functional merge 屬後期 Boolean/AIG 項目，
目前不可將本節視為已提供 sequential proof。

---

## 12. RemoveDanglingLogic

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

## 13. SimplifyConstants

用途：

```text
對 constant input gate 做 constant propagation。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::SimplifyConstants;
request.gateType = GateType::NAND;
request.constValue = 1;
request.inputCount = 2;
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否有化簡 | `report.changed` |
| 符合篩選的 gates | `report.constantSimplification->candidateGateNames` |
| 實際 rewrite 數 | `report.constantSimplification->simplifiedCount` |
| 指定 gate type 消失數 | `report.constantSimplification->eliminatedTargetGateCount` |
| 無法 rewrite 數與名稱 | `skippedCount`, `skippedGateNames` |
| gate type 變化 | `report.diff.gateTypeCountDelta` |
| 等價性理由 | `report.validation.equivalenceMethod`，應為 `LocalRewriteRule` |

`simplifiedCount` 不一定等於 gate eliminated count。例如 `NAND(a,b,1)` 會移除 constant input、保留 NAND gate，因此 simplified 為 1，但 eliminated NAND 為 0。

bulk 執行時會在內部延後 adjacency 維護，全部 rewrite 完成後再依 active input pins
一次重建 pin-level `loadGateIds`；呼叫方式與 report schema 不變。100 萬個符合條件的
gate 已以 public `runEditApply()` 實測通過，完整回傳 `simplifiedCount`，不會因內部
固定數量上限截斷。

---

## 14. Technology Mapping

### 14.1 ConvertToBasis

用途：

```text
將 whole netlist 或指定 fanin/fanout cone 轉成指定 gate basis，例如 AND/NOT、NOR/NOT、NAND/NOT。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::ConvertToBasis;
request.scope = TargetScope::WHOLE_NETLIST;
request.allowedTypes = {GateType::AND, GateType::NOT};
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

指定 cone 的寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::ConvertToBasis;
request.scope = TargetScope::NET_FANIN;
request.scopeName = "n10";
request.allowedTypes = {GateType::NOR, GateType::NOT};
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否有轉換 | `report.changed` |
| 移除了哪些 gate | `report.mappingDelta->removedCountByType` |
| 新增了哪些 gate | `report.mappingDelta->addedCountByType` |
| 轉換後各 gate 數量 | `report.mappingDelta->finalGateCountByType` |
| 等價性理由 | `report.validation.equivalenceMethod`，應為 `LocalRewriteRule` |

### 14.2 ReplaceGateType

用途：

```text
將 whole netlist 或指定 cone 中的某一種 gate type 替換成指定 basis。
例如 XOR -> NAND、XNOR -> NOR、OR -> NAND/NOT。
```

寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::ReplaceGateType;
request.scope = TargetScope::WHOLE_NETLIST;
request.targetGateType = GateType::XOR;
request.allowedTypes = {GateType::NAND};
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

指定 cone 的寫法：

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::ReplaceGateType;
request.scope = TargetScope::NET_FANIN;
request.scopeName = "n11[0]";
request.targetGateType = GateType::OR;
request.allowedTypes = {GateType::NAND, GateType::NOT};
request.validateEquivalence = true;

Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

讀取：

| 想知道 | 讀取欄位 |
|---|---|
| 是否有替換 | `report.changed` |
| 被替換的 gate 數量 | `report.mappingDelta->removedCountByType[targetGateType]` |
| 新增 gate 數量 | `report.mappingDelta->addedCountByType` |
| 指定 gate type 最後剩多少 | `report.mappingDelta->finalGateCountByType[targetGateType]` |
| 等價性理由 | `report.validation.equivalenceMethod`，應為 `LocalRewriteRule` |

`ReplaceGateType` 若在指定 scope 內找不到目標 gate type，會回傳 `success = true`、`changed = false`，並在 `mappingDelta.finalGateCountByType` 保留 target / allowed gate 的目前數量。

目前 public mapping 支援的 gate type 為：

```text
AND / OR / NAND / NOR / NOT / BUF / XOR / XNOR
```

optimizer 內部新增的 gate-set conversion 與 inverter absorption 不會改變
`ConvertToBasis` / `ReplaceGateType` 的 request 格式，也不是額外的 public command。

---

## 15. MergeFunctionallyEquivalentGates

用途：搜尋指定 scope 內 output function 相同的 combinational gates，將每個 SAT-proven equivalence class 合併成一個 cycle-safe representative，並以結構驗證與 `CertifiedRewrite` 提交。正式流程不執行 final whole-design SAT。

```cpp
Netlist::EditApplyRequest request;
request.kind = Netlist::EditCommandKind::MergeFunctionallyEquivalentGates;
request.scope = TargetScope::WHOLE_NETLIST;
request.gateType = GateType::UNKNOWN;
request.simulationPatternCount = 256;
request.timeLimitSeconds = 290.0;
request.validateEquivalence = true;
request.rollbackOnFailure = true;

const Netlist::NetlistEditReport report = netlist.runEditApply(request);
```

CLI：

```text
edit_apply merge_functionally_equivalent_gates whole
edit_apply merge_functionally_equivalent_gates net_fanin n10 --gate-type AND
```

必要判讀：

| 想知道 | 讀取欄位 |
|---|---|
| 搜尋是否完整 | `functionalMerge.searchComplete`, `searchStatus` |
| 找到多少等價類/pairs | `equivalenceClassCount`, `equivalentPairCount` |
| 實際移除多少 gates | `mergedGateCount` 或 `-diff.activeGateCountDelta` |
| 每顆保留/移除對象 | `functionalMerge.records` |
| 是否有安全問題而跳過 | `skippedGateCount`, `skippedGateNames` |
| rewrite certificate | `validation.equivalenceMethod == CertifiedRewrite` 且 `functionallyEquivalent=true` |

搜尋不完整、apply timeout、任何 class 無法 cycle-safe 合併或 structure validation 失敗時，整批 edit 不會留下部分修改。搜尋 timeout 發生在 mutation 前；apply timeout 或修改後結構驗證失敗則 `rolledBack=true`。`wholeDesignEquivalenceChecked/Equivalent/TimedOut` 為 legacy compatibility fields，本流程維持 false。

---

## 16. Prompt 對應表

| Prompt | 建議 EditCommandKind | 需要設定 | 主要讀取 |
|---|---|---|---|
| Rename wire n1214 to renamed_wire. Ensure functional equivalence is preserved. | `RenameNet` | `oldName = "n1214"`, `newName = "renamed_wire"` | `success`, `validation.equivalenceMethod` |
| Update the name of signal n440 to renamed_wire throughout the netlist. | `RenameNet` | `oldName = "n440"`, `newName = "renamed_wire"` | `success`, `changed` |
| Try to insert buffers on reset signal n1 to reduce fanout to at most 4. | `InsertBuffersForSpecificNet` | `netName = "n1"`, `maxFanout = 4` | `fanoutChange`, `changedGateNames` |
| Insert buffers wherever needed so that no signal drives more than 16 loads. | `InsertBuffersForFanout` | `maxFanout = 16` | `fanoutChange`, `changedGateNames` |
| Insert a BUF gate on signal n2 so that each load is driven through a dedicated buffer. | `InsertBuffersOnEachLoad` | `netName = "n2"` | `changedGateNames`, `diff` |
| Find all back-to-back inverter pairs and collapse them into a wire. | `CollapseDoubleInverter` | 無 | `changed`, `diff`, `validation.equivalenceMethod` |
| Check if there are dangling gates. Remove them if found. | `SafeCleanupFixpoint` 或 `RemoveDanglingLogic` | 無 | `changed`, `diff.activeGateCountDelta` |
| Trim unused wires and gates. | `SafeCleanupFixpoint` | 無 | `changed`, `diff`, `warnings` |
| Prune the netlist of unused gates. | `SafeCleanupFixpoint` | 無 | `changed`, `diff` |
| Sweep out dangling gates. | `RemoveDanglingLogic` 或 `SafeCleanupFixpoint` | 無 | `changed`, `diff` |
| Remove floating nodes that do not affect outputs. | `SafeCleanupFixpoint` | 無 | `changed`, `diff` |
| Are there any redundant gates? Remove them if found. | `SafeCleanupFixpoint` | 無 | `changed`, `diff`, `validation.equivalenceMethod` |
| Simplify reported NAND gates by propagating constant inputs. | `SimplifyConstants` | `gateType = NAND`, `constValue = -1` | `constantSimplification`, `diff` |
| Simplify OR gates with constant 1 input. | `SimplifyConstants` | `gateType = OR`, `constValue = 1` | `eliminatedTargetGateCount`, `validation.equivalenceMethod` |
| Replace all 2-input NAND gates tied to 1 with inverters. | `SimplifyConstants` | `gateType = NAND`, `constValue = 1`, `inputCount = 2` | `simplifiedGateNames`, `eliminatedTargetGateCount` |
| Try to merge structural duplicates. | `MergeStructurallyEquivalentGates` | 無 | `changed`, `diff` |
| Find and merge all gate pairs that are functionally equivalent. | `MergeFunctionallyEquivalentGates` | `scope = WHOLE_NETLIST` | `functionalMerge.mergedGateCount`, `records`, `validation` |
| Remove unused internal nets. | `RemoveUnusedNets` | 無 | `changedNetNames`, `diff` |
| Reconstruct the netlist using only AND and NOT gates. | `ConvertToBasis` | `scope = WHOLE_NETLIST`, `allowedTypes = {AND, NOT}` | `mappingDelta`, `validation.equivalenceMethod` |
| Ensure the cone of n10 maintains only NOR and NOT gates. | `ConvertToBasis` | `scope = NET_FANIN`, `scopeName = "n10"`, `allowedTypes = {NOR, NOT}` | `mappingDelta`, `diff` |
| Replace each XOR gate using NAND gates. | `ReplaceGateType` | `targetGateType = XOR`, `allowedTypes = {NAND}` | `mappingDelta.removedCountByType`, `mappingDelta.addedCountByType` |
| Replace OR gates in the cone of n11[0] using NAND and NOT gates. | `ReplaceGateType` | `scope = NET_FANIN`, `scopeName = "n11[0]"`, `targetGateType = OR`, `allowedTypes = {NAND, NOT}` | `mappingDelta`, `validation` |

---

## 17. 何時不要用 EditApply

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
| Verify current design equivalent to original loaded netlist. | `checkWholeDesignEquivalence()` / tools `equiv_query original` |
| Replace a gate driver, reconnect a gate input, or arbitrarily merge two nets. | 不屬於 public EditApply；需由內部 verified pass 或 expert API 處理 |

---

## 18. 等價性欄位解讀

| equivalenceMethod | 意思 | 常見 command |
|---|---|---|
| `NotChecked` | 目前沒有等價性證明 | 不應出現在成功的 public `runEditApply()` edit |
| `StructuralIdentity` | 名稱改變、unused/dangling/dead logic removal、structural merge | `RenameNet`, `RemoveDanglingLogic`, `TrimDeadLogic` |
| `LocalRewriteRule` | 可由局部 Boolean identity 或安全 cleanup 組合證明 | `CleanupBuffers`, `CollapseDoubleInverter`, `SafeCleanupFixpoint`, `SimplifyConstants`, buffer insertion, `ConvertToBasis`, `ReplaceGateType` |
| `CertifiedRewrite` | 由已 qualification 的 function-preserving transformation pipeline 認證；functional merge 的候選 gate 有 SAT proof，但此值不是 whole-design SAT proof | `MergeFunctionallyEquivalentGates`、`opt_apply critical_path_depth` 的 accepted changed candidate |
| `WholeDesignSat` | current 與 original/previous-edit 的 whole-design SAT equivalence | tools `equiv_query` |

---

## 19. 測試狀態

目前 `mini test/tester.cpp` 已涵蓋：

```text
runEditApply rename_gate
runEditApply cleanup_buffers
runEditApply insert_buffers_for_specific_net
runEditApply validateEquivalence has certificate for public edit
runEditApply missing required rename argument
runEditApply missing fanout net
runEditApply invalid fanout limit
runEditApply convert_basis whole allowed AND NOT
runEditApply convert_basis net_fanin n10 allowed NOR NOT
runEditApply replace_type XOR using NAND
runEditApply functional merge whole/cone scope, class-only search, cycle-safe representative
runEditApply functional merge CertifiedRewrite, no-change, detailed records, timeout-before-or-during-mutation
runEditApply unsupported command
```

目前驗證結果：

```text
Summary: 142 passed, 0 failed.
```

`mini test/test2` 另有 39 個 Edit Apply assertions，包含 tied-input pin-level fanout
insertion；專用 regression 確認兩個實際 sink pins 只建立兩顆 BUF，且 mutation 後
structure validation 通過；另覆蓋 canonical constant dedup、repeated mapping 與
constant rename rejection/Writer literal preservation，以及 replace-all-loads 保留
同一 gate 多個 input pins 的 load multiplicity。

`mini test/test2/constant_simplification_scalability.cpp` 另以 1,000,000 gates 驗證
public `SimplifyConstants`：約 0.96 秒完成，完整化簡 1,000,000 gates、無 rollback，
且 structure validation 通過。

`mini test/test2/double_inverter_scalability.cpp` 另以 1,000,000 個串接 NOT 驗證
public `CollapseDoubleInverter`：約 0.741 秒 collapse 500,000 pairs、無 rollback，
且 structure validation 通過；test2 同時覆蓋 PO/stale/tombstone/invalid-ID 拒絕。

`mini test/test2/structural_merge_scalability.cpp` 另覆蓋兩種 100 萬 gate worst case：
同組 duplicates 約 0.615 秒，50 萬層 cascade 約 1.141 秒；兩者皆完整 merge、
無 rollback 且 structure validation 通過。

非 Boolean public Edit Apply 共 22 個 commands，均有正向 regression；完整 public
command suite 為 test2 39/39，通用 wrapper/report/validation suite 為 test1 142/142。
Boolean functional merge、DFF initial-state 與 sequential proof 維持 Deferred。

`mini test/test26` 的 C++ API 與 CLI assertions 涵蓋 functional search/merge、直接 timeout、apply rollback 與 `CertifiedRewrite` report。NewTestCase test29/test30 的原始設計單獨 merge 為 7/1 顆；`mini test/test45` 重播官方前序 edit，並以開發期 independent CEC 及 write/readback CEC 抽查結果。正式 edit flow 不執行 final whole-design SAT。
