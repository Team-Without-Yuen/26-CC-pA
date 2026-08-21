# NewTestCase Current API Coverage Summary

這份文件整理目前高階 API 對 `NewTestCase/test01` 到 `test40` prompt 的覆蓋狀態。重點不是重複列出每題原文，而是判斷哪些題目已能由現有 API 回答，哪些需要補 wrapper / 文件，哪些仍缺核心演算法。

## 1. 已可直接處理的範圍

`test01` 的 load/write 屬於 I/O flow，不是 analysis API 本身。`test02` 到 `test20` 主要是 query 類問題，目前可由既有高階 query API 組合處理：

| Prompt 類型 | 對應 API |
| --- | --- |
| gate total / gate type breakdown | `BasicQuery` / gate type count |
| one/many/excluded gate-type list with input/output pin signals | `BasicQuery::GatesByType` include/exclude sets with `includeConnectionDetails`; CLI `gates_by_type --gate-types <type...> --exclude-gate-types <type...> --with-pins` |
| constant-input gate list with constant pin and input/output signals | `BasicQuery::GatesWithConstantInput` with `includeConnectionDetails`; CLI `const_input_gates [type|all] [0|1|any] --with-pins` |
| exact net driver/load gate pins and roles | `DirectConnectivityQuery` with `includePinDetails`; CLI `net_driver <net> --with-pins` / `net_loads <net> --with-pins` |
| primary input / primary output count and bit width | `BasicQuery` |
| constant-input gates / floating signals / structural issues | `BasicQuery` |
| immediate fanout, direct loads, DFF clock/reset/data loads | `DirectConnectivityQuery` / `FanoutLoadReport` |
| transitive fanin / fanout cone | `ConeQuery` |
| fanin cone gate count / cone gate type breakdown / shared fanin gates | `ConeQuery` |
| cone 內單一或多 gate-type gate list、pin/net details | `ConeQuery::gateTypeFilters` + `includeGateDetails`；CLI `--gate-types` / `--with-pins` 已完成 |
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
| DFF enable/hold structure 與 unique DFF count | `sequential_query enable_hold` | canonical safe fast path 與預設 Q-cofactor functional proof 已完成；AND-only data gating 不計 match |

逐 prompt 的 command、必要 report 欄位、完整性條件與缺口已整理在：

```text
API_SPEC/NEW_TESTCASE_TOOLS_COMMAND_COVERAGE.md
```

Batch 5 已直接 expose `gate_on_critical`、`every_through`、`deepest_output`、`largest_output`、`direct_pi_po`、`shared_fanin` 與 `po_exceeding`。目前仍需多次 command 的主要題型是「列出所有 PI/PO 並附 bit width」及「指定 gate type 的 constant propagation」。

## 4. Optimization tools 與 hidden-case hardening 狀態

以下 prompt 的 C++ API 與 tools CLI 狀態不同，需分開判讀：

| Prompt 片段 | 出現位置 | 缺口 |
| --- | --- | --- |
| reduce / optimize / minimize critical path depth through restructuring | `test22` 到 `test30`, `test33`, `test40` | public command/report/rollback/SAT 已串接；大型 mockturtle primitive 尚不可 cooperative cancel |
| optimize cone depth while maintaining NAND/NOT, NOR/NOT, AND/OR/NOT | `test25` 到 `test28`, `test33`, `test40` | DFF.Q target 已改為 boundary depth 0；非 DFF.Q 大型 cone 仍需要 optimizer core hardening |
| all gate pairs functionally equivalent | `test29`, `test30`, `test35` | 已支援 SAT class search、cycle-safe functional merge、whole-design SAT 與 rollback；test29/test30 已實測 |
| redundant gates removable without changing functionality | `test38` | 已確認是 structural duplicates；`MergeStructurallyEquivalentGates` 實測移除 14 gates，whole-design SAT 通過 |

## 5. 下一步建議

優先順序建議如下：

| Priority | 要補的內容 | 原因 |
| --- | --- | --- |
| Completed | 串接 depth optimization tools command | parser/help/envelope/report cache 與 mini test32 已完成 |
| P1 | large scoped-cone optimizer cancellation/resynthesis | 針對 hidden 中非 DFF.Q 的大型 cone；目前 core 先進全域 XAG，`--time-limit` 無法中止單次 primitive |
| P2 | general observability-aware redundancy hardening | official test38 已由 structural merge 完整處理；此項只針對可能的 hidden 任意 ODC redundancy |

目前結論：已知 NewTestCase 的 query、mapping、edit 與 depth optimization prompt 均已有 tools CLI routing。whole-design equivalence 已涵蓋 PO 與 DFF.D，functional duplicate search/merge、test38 structural redundancy、canonical DFF enable/hold 與 symmetry 已完成驗證。depth optimization 的 transaction、DFF.Q boundary no-op 與 test40 official flow 已通過；hidden 若出現非 DFF.Q 的大型 scoped cone，仍有 optimizer-core runtime blocker，不可宣稱所有 optimization testcase 均完整覆蓋。
