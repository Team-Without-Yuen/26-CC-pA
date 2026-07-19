# Tools Specification

這份文件是 `tools.cpp` 的 LLM-facing 唯一入口，負責協助判斷「題目應交給哪個高階 API」。選定 API 後，再閱讀本資料夾對應的 `*_TOOL.md`，取得實際 command、mode、輸入參數、輸出欄位與使用限制。

## 使用流程

```text
1. 依本文件判斷 prompt 的主要問題類型。
2. 選擇一個或多個 public tool families。
3. 開啟對應的 *_TOOL.md，不自行猜測 mode 或參數。
4. 先判讀 envelope 的 status 與 complete，再讀 data。
5. 只有 complete=true 的結果可以直接當成完整答案。
```

共同回傳格式：

```text
TOOL_RESULT_BEGIN
ok: true
status: ok
command: structure_query
mode: summary
message: ...
design_revision: 0
complete: true
data:
...
TOOL_RESULT_END
```

`partial`、`timeout`、`unsupported`、`error` 或 `complete:false` 不得解讀為 `no`、`false`、`0` 或「不存在」。`ok` 只表示 request 是否成功執行，不一定是題目本身的 yes/no 答案。

---

## 1. 判斷順序

LLM 應先辨識題目的主要動作：

```text
count / list / inspect direct connection       -> StructureQuery
find transitive fanin/fanout scope             -> ConeQuery
find connectivity between two endpoints        -> PathQuery
measure logic level or critical depth           -> DepthQuery
prove a property of known signals               -> FunctionQuery
search unknown signals satisfying a function    -> FunctionSearchQuery
recognize DFF enable/hold semantics              -> SequentialPatternQuery
perform a specified transformation              -> EditApply
inspect the previous transformation result      -> EditReport
compare current design with a saved design       -> WholeDesignEquivalence
minimize / optimize / find best cost             -> Optimization flow
```

一個 prompt 可以同時需要多個 API。應依題目中的每個要求分開 routing，不要強迫由單一 summary query 回答全部內容。

---

## 2. StructureQuery

公開 tool：`structure_query`

負責目前 netlist 的基本結構與單層直接連線：

- gate、net、PI、PO、DFF 的 active 數量與名稱列表。
- gate type 的數量與 gate 列表。
- 單一 gate、net、port 的基本資訊。
- floating net、undriven net、unused gate 等 structural issue。
- net 的直接 driver/load、gate 的 input/output、immediate fanin/fanout。
- fanout load 分類，包括由指定 clock/reset net 直接驅動的 DFF。

典型 prompt：

```text
How many primary inputs and primary outputs does this design have?
List all NOR gates.
How many floating signals were found?
What gate drives n10?
List all flip-flops driven by clock n0.
Which primary input has the highest direct fanout?
```

責任邊界：只處理物件本身與一層 connectivity。題目若要求 transitive cone，改用 `ConeQuery`；若要求 A 到 B 的完整路徑，改用 `PathQuery`；若問 signal 是否功能常數，不是 structural constant connection，改用 `FunctionQuery`。

詳細用法：[`STRUCTURE_QUERY_TOOL.md`](STRUCTURE_QUERY_TOOL.md)

---

## 3. ConeQuery

公開 tool：`cone_query`

負責從指定 gate 或 net 出發的多層 transitive fanin/fanout 範圍：

- 找出 cone 內所有 gates、nets、PIs、POs 與 DFF boundary。
- 統計 cone 內 gate 數量或各 gate type 數量。
- 比較或尋找 shared fanin gates。
- 查詢 largest output cone 等 cone 層級摘要。

典型 prompt：

```text
Find the transitive fanin cone of n10.
Which gates can affect output y?
How many NOR gates are in the cone of n15?
Which gates are shared by the fanin cones of n10 and n12?
Which output has the largest fanin cone?
```

責任邊界：`ConeQuery` 回答「範圍中有哪些物件」，不負責證明某一條 A-to-B path，也不計算全域 critical depth。指定 endpoints 的路徑使用 `PathQuery`；logic depth 使用 `DepthQuery`。

詳細用法：[`CONE_QUERY_TOOL.md`](CONE_QUERY_TOOL.md)

---

## 4. PathQuery

公開 tool：`path_query`

負責明確 startpoint 與 endpoint 之間的路徑關係：

- path existence、任一條 path、所有 paths 與 path count。
- shortest/longest path，以及 through/avoid constraint。
- PI-to-PO、PI-to-DFF.D、DFF.Q-to-PO 與 DFF.Q-to-DFF.D path。
- register-to-register path。
- every-path condition、mandatory nodes、separator 與 PI-to-PO cut。

典型 prompt：

```text
Is there a path from n1 to n20?
List all paths from input a to output y that avoid n8.
Find the longest path from ff1.Q to ff2.D.
Which nodes occur on every path from a to y?
Is n10 a separator between the primary inputs and outputs?
```

責任邊界：題目必須關心兩個 endpoints 之間如何連通。只問某個 signal 的完整 fanin/fanout 範圍使用 `ConeQuery`；只問全設計 endpoint depth 或 critical path 使用 `DepthQuery`。separator/mandatory-node 題目對外仍由 `PathQuery` 處理，不直接選內部 `GraphQuery`。

詳細用法：[`PATH_QUERY_TOOL.md`](PATH_QUERY_TOOL.md)

---

## 5. DepthQuery

公開 tool：`depth_query`

負責量測目前設計的 combinational logic depth：

- 指定 net、PO 或 DFF.D endpoint 的 depth。
- 所有 PO/DFF.D 的 depth report。
- 超過指定 depth threshold 的 endpoints 與數量。
- global maximum logic depth、critical endpoint 與代表性 critical path。

典型 prompt：

```text
How many outputs have a logic depth greater than 4?
What is the logic depth of n15?
Which output has the greatest depth?
Report the critical path and maximum logic depth.
```

責任邊界：`DepthQuery` 只量測與回報 current design，不會修改電路。若 prompt 使用 `reduce`、`minimize`、`optimize`、`best depth` 或 cost function，屬於 optimization，不可因為能查到 critical path 就宣稱可以完成最佳化。

詳細用法：[`DEPTH_QUERY_TOOL.md`](DEPTH_QUERY_TOOL.md)

---

## 6. FunctionQuery

公開 tool：`func_query`

負責分析題目已明確指定的 signal/net Boolean 性質：

- 兩條已知 internal nets 是否 functionally equivalent。
- 固定一條 scalar condition net 為 0/1 後，兩條已知 nets 是否 equivalent。
- signal 是否 constant 0、constant 1，或能否取某個值。
- output 是否 functionally dependent on 指定 input。
- 兩個 inputs 對指定 function 是否 symmetric。
- Boolean expression、simplified expression 與 functional support。

典型 prompt：

```text
Are n10 and n12 functionally equivalent?
When en is 0, is d equivalent to q?
Is output n8 always zero?
Does y functionally depend on input a?
Are inputs a and b symmetric with respect to y?
Derive the Boolean expression for n15.
```

責任邊界：候選 signal 名稱必須已知。若題目要求從全設計「找出任意一組」符合條件的 signals，使用 `FunctionSearchQuery`。若比較的是修改前後兩份 design，不是兩條 internal nets，使用 `WholeDesignEquivalence`。

詳細用法：[`FUNCTION_QUERY_TOOL.md`](FUNCTION_QUERY_TOOL.md)

---

## 7. FunctionSearchQuery

公開 tool：`func_search`

負責候選未知、必須掃描設計才能找到答案的 Boolean search。它與 `FunctionQuery` 的差異不在 SAT 與否，而在題目是否已提供待分析的 signal 名稱。

典型 prompt：

```text
Find two existing signals whose NAND is functionally equivalent to n20.
Does any signal pair in the design satisfy the requested Boolean relation?
List all functionally equivalent gate pairs in the design.
Find equivalent AND gates in the fanin cone of n10.
```

目前公開支援 `NAND(a,b) == target` signal-pair search，以及 whole design / cone scope 內的 arbitrary equivalent combinational gate-pair search。後者可依 gate type 過濾並回傳 SAT-proven equivalence classes，但不代表 gate 已可直接刪除；任意 observability-aware redundancy candidate 或其他尚未列於 Usage 的 Boolean search，不可假設已支援。

詳細用法：[`FUNCTION_SEARCH_TOOL.md`](FUNCTION_SEARCH_TOOL.md)

---

## 8. SequentialPatternQuery

公開 tool：`sequential_query`

負責從 DFF 的 D-input combinational logic 推導 register-control semantics：

- enable condition。
- hold condition。
- feedback MUX 或相關 canonical sequential pattern。
- 指定 DFF 或全設計 DFF 的 pattern summary。

典型 prompt：

```text
Which flip-flops have an enable condition?
Find the hold condition of register ff1.
Identify DFFs implemented with a feedback MUX pattern.
```

責任邊界：一般 `List all DFFs`，或 `List all flip-flops driven by clock n0`，只是物件與直接 clock connectivity，應使用 `StructureQuery`。只有題目要求 enable、hold、feedback 或 register-control pattern 時才使用 `SequentialPatternQuery`。

詳細用法：[`SEQUENTIAL_QUERY_TOOL.md`](SEQUENTIAL_QUERY_TOOL.md)

---

## 9. EditApply 與 EditReport

公開 tools：`edit_apply`、`report_query`

`EditApply` 負責執行題目已指定的 transformation：

- gate/net rename。
- dangling logic、buffer、double inverter、constant propagation 等 cleanup/simplification。
- structural duplicate 與 SAT-proven functional duplicate gate merge。
- buffer insertion 與 fanout-related specified edit。
- gate replacement、basis conversion 與 technology mapping。
- 修改限制檢查與統一 edit report 建立。

`EditReport` 負責回答上一筆修改的成果：

- 是否真的改變 design。
- 新增、移除或變更了多少 gates/nets。
- before/after 統計、gate-type delta、限制與驗證狀態。

典型 prompt：

```text
Remove dangling logic from the design.
Collapse all back-to-back inverters.
Remove redundant gates from test38 without changing functionality.
Convert the cone of n10 to NOR and NOT gates.
Find and merge all functionally equivalent gate pairs.
How many gates were removed in the previous step?
What changed after the last transformation?
```

責任邊界：`EditApply` 執行指定操作，不負責搜尋最佳 cost/depth。一般 edit 若題目要求 whole-design 功能不變，修改後還要使用 `WholeDesignEquivalence`；`merge_functionally_equivalent_gates` 已在 transaction 內強制執行 whole-design SAT。任何情況都不能只看到 edit changed 就宣稱等價。

test38 的 redundant-gate step 已確認 routing 到 `merge_structurally_equivalent_gates`，不是 general observability search。未知 hidden prompt 若明確需要 observability-only redundancy，仍不可由 structural merge 的結果外推。

詳細用法：[`EDIT_APPLY_TOOL.md`](EDIT_APPLY_TOOL.md)、[`REPORT_QUERY_TOOL.md`](REPORT_QUERY_TOOL.md)

---

## 10. WholeDesignEquivalence

公開 tool：`equiv_query`

負責比較 current design 與 CLI session 保存的 design baseline：

- current design 對 original design。
- current design 對上一筆 edit 前的 design。
- 以所有 PO 與 DFF.D combinational boundary 判斷 whole-design equivalence。

典型 prompt：

```text
Is the current design functionally equivalent to the original?
Did the previous transformation preserve functionality?
Make sure nothing changes functionally.
```

責任邊界：同一份設計中兩條已知 nets 的 equivalence 使用 `FunctionQuery`。`WholeDesignEquivalence` 也不是修改命令；它只能驗證 edit 前後或 original/current 的功能結果。

詳細用法：[`EQUIVALENCE_QUERY_TOOL.md`](EQUIVALENCE_QUERY_TOOL.md)

---

## 11. 常見組合題型

| 題目要求 | API 組合 |
|---|---|
| 統計某個 cone 內各 gate type | `ConeQuery`，不是全設計 `StructureQuery` |
| 找 critical path 並列出其節點 | `DepthQuery` 取得 global critical result；需要額外 endpoint/path constraint 時再用 `PathQuery` |
| 修改後回報移除數量 | `EditApply` → `EditReport` |
| 修改後確認功能不變 | `EditApply` → `WholeDesignEquivalence` |
| 修改、回報成果並確認功能不變 | `EditApply` → `EditReport` → `WholeDesignEquivalence` |
| 找出未知候選，再對候選做詳細功能分析 | `FunctionSearchQuery` → `FunctionQuery` |
| 列出某 clock 的 DFF，再分析這些 DFF 的 enable/hold | `StructureQuery` → `SequentialPatternQuery` |

選定高階 API 後，LLM 必須進入表中連結的 `*_TOOL.md`，再決定實際 command mode、輸入參數與應讀取的 report 欄位。`API_SPEC/*_USAGE.md` 保留給 `tools.cpp` 或 C++ API 串接者，不是 LLM 的直接 tool contract。

---

## 12. Public Tool 文件索引

| Public command | 詳細文件 |
|---|---|
| `read`, `write`, `help`, `quit` | [`SESSION_AND_IO_TOOL.md`](SESSION_AND_IO_TOOL.md) |
| `structure_query` | [`STRUCTURE_QUERY_TOOL.md`](STRUCTURE_QUERY_TOOL.md) |
| `cone_query` | [`CONE_QUERY_TOOL.md`](CONE_QUERY_TOOL.md) |
| `path_query` | [`PATH_QUERY_TOOL.md`](PATH_QUERY_TOOL.md) |
| `depth_query` | [`DEPTH_QUERY_TOOL.md`](DEPTH_QUERY_TOOL.md) |
| `func_query` | [`FUNCTION_QUERY_TOOL.md`](FUNCTION_QUERY_TOOL.md) |
| `func_search` | [`FUNCTION_SEARCH_TOOL.md`](FUNCTION_SEARCH_TOOL.md) |
| `sequential_query` | [`SEQUENTIAL_QUERY_TOOL.md`](SEQUENTIAL_QUERY_TOOL.md) |
| `edit_apply` | [`EDIT_APPLY_TOOL.md`](EDIT_APPLY_TOOL.md) |
| `report_query` | [`REPORT_QUERY_TOOL.md`](REPORT_QUERY_TOOL.md) |
| `equiv_query` | [`EQUIVALENCE_QUERY_TOOL.md`](EQUIVALENCE_QUERY_TOOL.md) |

`basic_query`、`conn_query`、`reg_path_query` 與 `graph_query` 只為 legacy 相容保留，不是 LLM 應選擇的 public command。新增或修改 tool 文件時遵循 [`TOOL_DOCUMENT_TEMPLATE.md`](TOOL_DOCUMENT_TEMPLATE.md)。
