# EditApply Prompt Coverage

這份文件只整理 `testcase/` 與 `NewTestCase/` 中屬於 edit / transformation / cleanup 類的 prompt，對照目前 public `runEditApply()` 能否處理。

目前兩個資料夾的 40 個 `prompt.txt` 內容 hash 相同，因此以下 coverage 同時適用於 `testcase` 與 `NewTestCase`。

---

## 1. 結論

目前 public `EditApply` 已能覆蓋大部分基礎修改類 prompt：

```text
Rename:
  RenameGate / RenameNet

Cleanup / simplification:
  SafeCleanupFixpoint
  TrimDeadLogic / RemoveDanglingLogic / RemoveUnusedNets
  CollapseDoubleInverter
  MergeStructurallyEquivalentGates
  SimplifyConstants / SimplifySameInput

Buffer insertion:
  InsertBuffersForFanout
  InsertBuffersForSpecificNet
  InsertBuffersOnEachLoad

Technology mapping:
  ConvertToBasis
  ReplaceGateType
```

目前 public flow 已由 tools CLI expose rename、cleanup、simplification、buffer insertion、technology mapping 與 depth optimization，並有 original/current whole-design `equiv_query`。剩餘主要缺口是：

```text
1. depth optimization / restructure with cost function 已由
   `opt_apply critical_path_depth` 覆蓋。固定 basis conversion 仍由
   `edit_apply convert_basis` 負責。

2. 任意 functional-equivalent pair 已可由 `func_search equivalent_pairs` 搜尋，並由 `edit_apply merge_functionally_equivalent_gates` 完成 SAT-class proof、cycle-safe merge、結構驗證、詳細 report 與失敗 rollback；正式流程不執行 final whole-design SAT。
   structural duplicate 仍可使用成本較低的 `merge_structurally_equivalent_gates`；observability-aware redundancy removal 是不同問題，尚未完整。
```

---

## 2. 已由 Public EditApply 覆蓋

### 2.1 Rename

| Prompt 類型 | 出現 testcase | 對應 command | 目前狀態 |
|---|---|---|---|
| `Rename gate g0 to renamed_gate.` | test24 | `RenameGate` | API + tools CLI covered |
| `Change the identifier of gate g0 to renamed_gate...` | test25 | `RenameGate` | API + tools CLI covered |
| `Change the identifier of wire n74 to renamed_wire...` | test25 | `RenameNet` | API + tools CLI covered |
| `Try to rename internal signal n1289 to renamed_sig...` | test31 | `RenameNet` | API + tools CLI covered |
| `Rename wire n1214 to renamed_wire.` | test32 | `RenameNet` | API + tools CLI covered |
| `Update the name of signal n7431 to renamed_wire...` | test35 | `RenameNet` | API + tools CLI covered |
| `Update the name of signal n440 to renamed_wire...` | test38 | `RenameNet` | API + tools CLI covered |

後續 follow-up：

```text
List all gates that now connect to the renamed signal renamed_sig.
```

應接 `DirectConnectivityQuery` / `ConeQuery`，不是 edit command。

### 2.2 Cleanup / Dead Logic

| Prompt 類型 | 出現 testcase | 對應 command | 目前狀態 |
|---|---|---|---|
| `Trim unused wires and gates.` | test23 | `SafeCleanupFixpoint` | API + tools CLI covered |
| `Remove all dangling gates...` | test24, test26 | `RemoveDanglingLogic` | API + tools CLI covered |
| `Eliminate unused logic gates...` | test25 | `SafeCleanupFixpoint` | API + tools CLI covered |
| `Sweep out dangling gates.` | test28 | `RemoveDanglingLogic` | API + tools CLI covered |
| `Prune the netlist of unused gates.` | test29, test37 | `SafeCleanupFixpoint` | API + tools CLI covered |
| `Remove floating nodes that do not affect outputs.` | test30 | `SafeCleanupFixpoint` | API + tools CLI covered |

可由 report 回答的 follow-up：

```text
How many dangling gates were removed?
  -> -report.diff.activeGateCountDelta

How many redundant gates were removed?
  -> report.diff.activeGateCountDelta / changedGateNames
```

廣義 cleanup prompt 若沒有指定單一操作，建議先 route 到 `SafeCleanupFixpoint`，再用 report 的 `diff`、`depthChange`、`warnings` 回答 follow-up。

注意：`Check if there are floating inputs or unconnected output ports` 是 query 類，應由 `BasicQuery::StructuralIssues` 或類似 report 回答，不應走 edit。

### 2.3 Back-to-Back Inverter Collapse

| Prompt 類型 | 出現 testcase | 對應 command | 目前狀態 |
|---|---|---|---|
| `Find all back-to-back inverter pairs and collapse them...` | test26, test27, test28, test29, test30, test31, test35, test38, test39, test40 | `CollapseDoubleInverter` | API + tools CLI covered |

可由 report 回答：

```text
是否有 collapse:
  report.changed

移除/變更估計:
  report.diff.activeGateCountDelta
```

### 2.4 Constant Propagation / Local Simplification

| Prompt 類型 | 出現 testcase | 對應 command | 目前狀態 |
|---|---|---|---|
| `Try to replace all 2-input NAND gates that have one input tied to constant 1 with inverters.` | test32, test40 | `SimplifyConstants` | API + tools CLI covered：`NAND 1 --inputs 2` |
| `Simplify the reported NAND gates by propagating their constant inputs.` | test32, test38 | `SimplifyConstants` | API + tools CLI covered：`NAND any` |
| `Simplify the reported AND gates by propagating their constant 0 input.` | test36 | `SimplifyConstants` | API + tools CLI covered：`AND 0` |
| `Simplify the reported OR gates by propagating their constant 1 input.` | test39 | `SimplifyConstants` | API + tools CLI covered：`OR 1` |
| `Simplify the reported NOR gates by propagating their constant inputs.` | test39 | `SimplifyConstants` | API + tools CLI covered：`NOR any` |

可由 report 回答：

```text
How many NAND/AND/OR/NOR gates were eliminated?
  -> report.constantSimplification.eliminatedTargetGateCount

How many NOT gates are currently in the design?
  -> BasicQuery::CountByGateType(NOT) after edit
```

### 2.5 Structural / Functional Duplicate Merge

| Prompt 類型 | 出現 testcase | 對應 command | 目前狀態 |
|---|---|---|---|
| `Try to merge any pairs of gates ... structural duplicates.` | test33 | `MergeStructurallyEquivalentGates` | API + tools CLI covered |
| `Find and merge all gate pairs ... functionally equivalent.` | test29, test30 | `MergeFunctionallyEquivalentGates` | API + tools CLI covered；class-only SAT search、cycle-safe merge、CertifiedRewrite、failure rollback、detailed report；無 final whole-design SAT |
| `Are there any redundant gates ... Remove them if found.` | test38 | `MergeStructurallyEquivalentGates` | API + tools CLI covered；official circuit 實測為 14 個 structural duplicates，whole-design SAT 通過 |

目前建議：

```text
structural duplicates:
  use MergeStructurallyEquivalentGates

arbitrary functional-equivalent merge:
  use MergeFunctionallyEquivalentGates
  CLI: edit_apply merge_functionally_equivalent_gates whole
```

### 2.6 Buffer Insertion

| Prompt 類型 | 出現 testcase | 對應 command | 目前狀態 |
|---|---|---|---|
| `Insert buffers wherever needed so that no gate drives more than 4 loads.` | test21 | `InsertBuffersForFanout(maxFanout=4)` | API + tools CLI covered；constant literal 排除於 bufferable constraint universe |
| `Insert buffers wherever needed so that no signal drives more than 16 loads.` | test72 | `InsertBuffersForFanout(maxFanout=16)` | API + tools CLI covered；constant literal 排除於 bufferable constraint universe |
| `Try to insert buffers on the reset signal n1 ... at most 4 loads per driver.` | test38 | `InsertBuffersForSpecificNet(netName=n1,maxFanout=4)` | API + tools CLI covered |
| `Insert a BUF gate on signal n2 so that each load ... dedicated buffer.` | test31, test39 | `InsertBuffersOnEachLoad(netName=n2)` | API + tools CLI covered |

可由 report 回答：

```text
How many BUF gates were added?
  -> report.diff.gateTypeCountDelta[GateType::BUF]
  -> report.changedGateNames.size()

What is the maximum fanout of n1 now?
  -> DirectConnectivityQuery::FanoutLoadReport or GlobalFanoutReport after edit
```

### 2.7 Technology Mapping / Basis Conversion

| Prompt 類型 | 出現 testcase | 對應 command | 目前狀態 |
|---|---|---|---|
| `Convert the logic cone of n10 to use only NOR and NOT gates...` | test26 | `ConvertToBasis(scope=NET_FANIN, scopeName=n10, allowedTypes={NOR,NOT})` | API + tools CLI covered |
| `Decompose all XOR gates in the fanin cone of n15 into AND, OR, and NOT gates...` | test27 | `ReplaceGateType(scope=NET_FANIN, scopeName=n15, targetGateType=XOR, allowedTypes={AND,OR,NOT})` | API + tools CLI covered |
| `Reconstruct the entire netlist using only AND and NOT gates...` | test28, test29, test30, test34 | `ConvertToBasis(scope=WHOLE_NETLIST, allowedTypes={AND,NOT})` | API + tools CLI covered |
| `Convert every XNOR gate ... NOR-only circuit.` | test33 | `ReplaceGateType(scope=WHOLE_NETLIST, targetGateType=XNOR, allowedTypes={NOR})` | API + tools CLI covered |
| `Try to replace all XNOR gates ... NOR-only implementations.` | test35 | `ReplaceGateType(scope=WHOLE_NETLIST, targetGateType=XNOR, allowedTypes={NOR})` | API + tools CLI covered |
| `Try to replace all XOR gates ... NAND-only implementations.` | test35 | `ReplaceGateType(scope=WHOLE_NETLIST, targetGateType=XOR, allowedTypes={NAND})` | API + tools CLI covered |
| `Convert every XOR gate ... 4-NAND circuit.` | test39 | `ReplaceGateType(scope=WHOLE_NETLIST, targetGateType=XOR, allowedTypes={NAND})` | API + tools CLI covered |
| `Remap the entire design to use only NAND and NOT gates.` | test40 | `ConvertToBasis(scope=WHOLE_NETLIST, allowedTypes={NAND,NOT})` | API + tools CLI covered |
| `Try to restructure the logic cone of output n8 using only NAND and NOT gates...` | test33, test37 | `ConvertToBasis(scope=NET_FANIN, scopeName=n8, allowedTypes={NAND,NOT})` | API + tools CLI covered if interpreted as basis conversion |

可由 report 回答：

```text
How many NOR gates were added by replacing XNOR gates?
  -> report.mappingDelta->addedCountByType[NOR]

How many NAND gates were added by replacing XOR gates?
  -> report.mappingDelta->addedCountByType[NAND]

How many NAND gates are now in the design after XOR-to-NAND conversion?
  -> report.mappingDelta->finalGateCountByType[NAND]
  -> or BasicQuery::CountByGateType(NAND) after edit

How many AND gates are now in the reconstructed netlist?
  -> report.mappingDelta->finalGateCountByType[AND]
  -> or BasicQuery::CountByGateType(AND) after edit
```

---

## 3. tools.cpp Public Exposure

Rename、cleanup、simplification、structural merge、buffer insertion、technology mapping 與 cached edit report 均已由 public `edit_apply` / `report_query` expose。正式 LLM-facing grammar 與 report 判讀見：

```text
TOOLS_SPEC/EDIT_APPLY_TOOL.md
TOOLS_SPEC/REPORT_QUERY_TOOL.md
```

---

## 4. 已完成驗證與未來 API

### 4.1 Original / Current Whole-Design Equivalence（已完成）

Prompt 類型：

```text
Prove that the transformed design is equivalent to the pre-transformation netlist.
Verify functional equivalence between the current design and the original loaded netlist.
Confirm that the design is still functionally equivalent to the original.
Check whether the current netlist is functionally equivalent to the netlist as last loaded from disk.
```

出現 testcase：

```text
test31, test32, test33, test34, test35, test36, test37, test38, test40
```

目前狀態：

```text
Covered by session original snapshot and tools `equiv_query original/previous_edit`.
EditApply wrappers 仍先回 local rewrite certificate；題目明確要求整體證明時再執行 whole-design SAT query。
```

公開用法見 `TOOLS_SPEC/EQUIVALENCE_QUERY_TOOL.md`。

### 4.2 Depth Optimization / Restructuring With Cost Function

Prompt 類型：

```text
Reduce the critical path depth through restructuring.
Optimize the logic to minimize maximum path depth.
Minimize the maximum logic depth of the design while preserving a required basis.
Optimize the depth of the cone of n8/n14 while ensuring the cone remains NAND/NOT.
```

出現 testcase：

```text
test22, test23, test24, test25, test26, test27, test28, test29, test30, test33, test40
```

目前 public flow：

```text
Covered by OptApply::CriticalPathDepth:
1. global/scoped depth objective
2. whole/local allowed/banned basis
3. DFF.Q fanin boundary no-op
4. targetDepth / no-improvement original retention
5. structure + basis validation + CertifiedRewrite（無 final whole-design SAT）
6. NetlistEditReport.depthChange / depthOptimization
```

`tools.cpp` 已 expose `opt_query/opt_apply critical_path_depth`；LLM-facing
command、參數與輸出判讀見 `TOOLS_SPEC/OPTIMIZATION_TOOL.md`，C++ 串接見
`API_SPEC/OPT_APPLY_USAGE.md`。

### 4.3 Boolean Equation / Advanced Analysis Follow-ups

這些不是 edit_apply，但常出現在修改流程後：

```text
Derive the Boolean equation...
What Boolean function does output n25 compute?
Check symmetry...
Find articulation points / cut points...
Report D input enable/hold structures...
```

這些不應塞進 EditApply。Boolean/function 問題由 `FunctionQuery`、cut/articulation 由 public `PathQuery`、DFF enable/hold 由 `SequentialPatternQuery` 負責。

---

## 5. 剩餘缺口

```text
1. general observability-aware redundancy removal
   official test38 已確認可由 structural duplicate merge 完整處理；但未知 hidden case 若要求「局部功能不同、只因 outputs 不可觀測而可刪除」的任意 redundancy，仍需 observability proof。

2. Objective-driven depth optimization 已由 `opt_apply critical_path_depth`
   負責並完成 tools/LLM command 串接。固定 basis conversion 仍不等於
   best-depth search，必須 route 到不同 high-level API。
```
