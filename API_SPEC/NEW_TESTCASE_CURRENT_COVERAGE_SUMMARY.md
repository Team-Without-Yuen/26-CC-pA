# NewTestCase Current API Coverage Summary

這份文件整理目前高階 API 對 `NewTestCase/test01` 到 `test40` prompt 的覆蓋狀態。重點不是重複列出每題原文，而是判斷哪些題目已能由現有 API 回答，哪些需要補 wrapper / 文件，哪些仍缺核心演算法。

## 1. 已可直接處理的範圍

`test01` 的 load/write 屬於 I/O flow，不是 analysis API 本身。`test02` 到 `test20` 主要是 query 類問題，目前可由既有高階 query API 組合處理：

| Prompt 類型 | 對應 API |
| --- | --- |
| gate total / gate type breakdown | `BasicQuery` / gate type count |
| primary input / primary output count and bit width | `BasicQuery` |
| constant-input gates / floating signals / structural issues | `BasicQuery` |
| immediate fanout, direct loads, DFF clock/reset/data loads | `DirectConnectivityQuery` / `FanoutLoadReport` |
| transitive fanin / fanout cone | `ConeQuery` |
| fanin cone gate count / cone gate type breakdown / shared fanin gates | `ConeQuery` |
| path existence, path avoiding node, PI-to-PO path list / depth-0 direct connections | `PathQuery` |
| cone depth, global critical path, endpoints deeper than threshold | `DepthQuery` |
| register-to-register path / DFF D-pin depth | path/depth API with DFF boundary rules |
| signal equivalence / always-zero / always-one / Boolean expression / support PI | `FunctionQuery` |
| DFF D-input canonical enable/hold pattern、enable polarity、feedback/data extraction | `SequentialPatternQuery` |

`test21` 的 fanout buffer insertion 也已能處理，對應 `EditApply::InsertBuffersForFanout`，可用 `report.fanoutChange` 回答 before / after max fanout、是否滿足限制、以及有哪些 net 仍違反 constraint。

## 2. Edit / Cleanup / Mapping 類目前覆蓋狀態

`test23` 到 `test40` 中大量出現 cleanup、rename、buffer insertion、constant propagation、technology mapping 類 prompt。這一批目前大多已有 public `EditApply` command，並統一回傳 `NetlistEditReport`。

| Prompt 片段 | 目前處理方式 | 狀態 |
| --- | --- | --- |
| rename gate / wire / signal | `RenameGate`, `RenameNet` | 已支援 |
| collapse back-to-back inverters | `CollapseDoubleInverter` | 已支援 |
| trim / prune / sweep unused or dangling logic | `SafeCleanupFixpoint`, `RemoveDanglingLogic`, `TrimDeadLogic`, `RemoveUnusedNets` | 已支援 |
| redundant structural duplicate gates | `MergeStructurallyEquivalentGates` | 支援 structural duplicates；legacy `MergeEquivalentGates` 不對外 |
| constant-0 / constant-1 propagation | `SimplifyConstants`，可依 gate type、constant value、input count 篩選 | 已支援 |
| same-input simplification | `SimplifySameInput` | 已支援 |
| insert buffers for max fanout | `InsertBuffersForFanout`, `InsertBuffersForSpecificNet` | 已支援 |
| dedicated buffer for each load of one net | `InsertBuffersOnEachLoad` | 已支援 |
| DFF clock/reset control fanout buffering | `InsertBuffersForDffControl` | 已支援 |
| whole-design AND/NOT, NAND/NOT, NOR/NOT conversion | `ConvertToBasis` | 已支援 |
| cone-restricted basis conversion | `ConvertToBasis` with `scope = NET_FANIN` | 已支援 |
| replace XOR/XNOR/OR/NAND gate type with basis logic | `ReplaceGateType` | 已支援 |
| prove current design equals original loaded design | `checkWholeDesignEquivalence(original)` | 已支援 PO + DFF.D batched SAT；尚未自動接入每個 edit |

這些 edit command 的細節回答主要從 `NetlistEditReport` 取得：

| 細問內容 | Report 欄位 |
| --- | --- |
| 總共改了多少 gate / net | `diff.activeGateCountDelta`, `diff.activeNetCountDelta` |
| 某 gate type 增減多少 | `diff.gateTypeCountDelta` |
| mapping 新增/移除多少指定 gate type | `mappingDelta.addedCountByType`, `mappingDelta.removedCountByType` |
| buffer insertion 後 max fanout | `fanoutChange.afterMaxFanout` |
| 是否滿足 fanout constraint | `fanoutChange.meetsConstraint` |
| 是否可宣稱功能等價 | `validation.equivalenceChecked`, `validation.functionallyEquivalent`, `validation.equivalenceMethod` |
| 有沒有 rollback / warning | `rolledBack`, `warnings` |

因此像 `How many OR gates were eliminated by constant-1 propagation?`、`How many BUF gates were added?`、`How many NOR gates were added by replacing XNOR gates?` 這類問題，原則上可由 edit report 的 delta 回答。

## 3. test31 到 test40 Tools Routing Audit

Boolean expression / support PI 目前已整理進正式 `FunctionQuery`：

| Prompt 類型 | 對應 API | 狀態 |
| --- | --- | --- |
| derive Boolean equation for output n16 / n12 | `FunctionQuery::BooleanExpression` | 已 public 化 |
| large cone expression with depth limit | `FunctionQuery::SimplifiedBooleanExpression` | 已 public 化 |
| output depends on which PI | `FunctionQuery::PrimaryInputsOfNet` | 已 public 化 |
| output 是否 functionally depend on 特定 PI | `FunctionQuery::FunctionalDependence` | exact dual-cone SAT/cofactor query 已 public 化 |
| function 對兩個 inputs 是否 symmetric | `FunctionQuery::Symmetry` | exact swapped-cofactor SAT、bus target、反例 report 已 public 化 |
| signal equivalence / constant / truth status | `FunctionQuery` SAT 類 mode | 已 public 化，且 report 可區分 timeout / UNKNOWN |
| DFF enable/hold structure 與 unique DFF count | `sequential_query enable_hold` | canonical API/CLI 已完成；arbitrary-equivalent/AND-only 官方語意待確認 |

逐 prompt 的 command、必要 report 欄位、完整性條件與缺口已整理在：

```text
API_SPEC/NEW_TESTCASE_TOOLS_COMMAND_COVERAGE.md
```

Batch 5 已直接 expose `gate_on_critical`、`every_through`、`deepest_output`、`largest_output`、`direct_pi_po`、`shared_fanin` 與 `po_exceeding`。目前仍需多次 command 的主要題型是「列出所有 PI/PO 並附 bit width」及「指定 gate type 的 constant propagation」。

## 4. 目前仍缺核心演算法的範圍

以下 prompt 目前不能算完整覆蓋，因為缺正式演算法或 public high-level flow：

| Prompt 片段 | 出現位置 | 缺口 |
| --- | --- | --- |
| reduce / optimize / minimize critical path depth through restructuring | `test22` 到 `test30`, `test33`, `test40` | 真正 depth optimization 尚未形成穩定 public API、report、rollback、basis constraint flow |
| optimize cone depth while maintaining NAND/NOT, NOR/NOT, AND/OR/NOT | `test25` 到 `test28`, `test33`, `test40` | 需要 constrained depth optimization flow |
| all gate pairs functionally equivalent | `test29`, `test30`, `test35` | 目前只安全支援 structural duplicate merge，尚未支援任意 SAT-based functional merge |
| arbitrary redundant gates removable without changing functionality | `test38` | 需要 SAT / observability-aware redundancy removal |

## 5. 下一步建議

優先順序建議如下：

| Priority | 要補的內容 | 原因 |
| --- | --- | --- |
| P1 | 補 SAT-based functional duplicate / redundancy detection | 可提升 `functionally equivalent gate pair` 與 redundant gate 題型 |
| P2 | 整理 depth optimization public flow | 最難，但會影響最大分數的 optimization testcase |

目前結論：一般 query、mapping 與大部分 edit prompt 已能由 tools CLI 處理；whole-design equivalence 已涵蓋 PO 與 DFF.D，canonical DFF enable/hold API/CLI 已通過 test40，symmetry API/CLI 也已通過 test36/test37 與 mini test22 驗證。下一步進入 functional pair/redundancy search 與 constrained depth optimization；AND-only/general pattern 語意仍依官方定義保守處理。
