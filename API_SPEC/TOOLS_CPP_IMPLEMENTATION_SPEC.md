# tools.cpp Detailed Implementation Specification

本文件是後續重寫與補齊 `tools.cpp` 的實作依據。內容以目前原始碼、`API_SPEC` 文件、`NewTestCase/test01` 到 `test40` prompt，以及已完成的 mini tests 為準。

本文件的目的不是重新發明分析或修改演算法，而是確保已經存在的高階 API 都能被 LLM 穩定呼叫、結果不被誤讀，並把仍缺少高階 API 的題型在進入 `tools.cpp` 前先辨識出來。

---

## 1. 核心目標

`tools.cpp` 應成為 LLM、人工 debug 與 prompt coverage test 共用的唯一外部入口：

```text
Natural-language prompt
  -> LLM tool selection / argument extraction
  -> tools.cpp normalized request
  -> existing high-level Netlist API
  -> normalized structured response
  -> LLM final answer
```

外部入口只允許呼叫已定義的公開 facade；facade 再 dispatch 到內部高階 API：

```text
StructureQuery facade -> BasicQuery / DirectConnectivityQuery
ConeQuery
PathQuery -> path/register/separator/mandatory/cut
DepthQuery
FunctionQuery
EditApply
WholeDesignEquivalence
future specialized query / optimization APIs
```

`tools.cpp` 不應直接呼叫低階 rewiring primitive，也不應自行實作 graph traversal、SAT、cone intersection、gate rewrite 或 optimization algorithm。

若某個 prompt 需要大量結果的交集、過濾或計數，應先補進對應的高階 API/report，再由 `tools.cpp` expose。不可把這類 domain logic 藏在 CLI printer 中。

---

## 2. 目前實作盤點

目前 `tools.cpp` 已有以下 command family：

```text
read
write
structure_query
cone_query
path_query
depth_query
func_query
func_search
edit_apply
```

但目前 command parser、API dispatch 與文字輸出全部集中在同一檔案，且 tool coverage 已落後於公開 header。

### 2.1 Public API 串接狀態

| 高階 API 功能 | 原始碼狀態 | tools.cpp 狀態 |
|---|---|---|
| `ConeQuery::LargestOutputCone` | 已實作 | Batch 2 已串接並測試 |
| `DepthQuery::GateOnCriticalPath` | 已實作 | Batch 2 已串接，固定輸出 yes/no |
| `DepthQuery::DeepestOutputCone` | 已實作 | Batch 2 已串接並測試 |
| `FunctionQuery::BooleanExpression` | 已實作 | Batch 2 已串接並測試 |
| `FunctionQuery::SimplifiedBooleanExpression` | 已實作 | Batch 2 已串接並測試 |
| `FunctionQuery::PrimaryInputsOfNet` | 已實作 | Batch 2 已串接並測試 |
| Function SAT timeout / UNKNOWN 欄位 | 已實作 | Batch 2 已輸出完整 solver 狀態 |
| Path enumerate time/path limit/count-only | 已實作 | Batch 2 已串接並測試 |
| Register path time/path limit/count-only | 已實作 | Batch 2 已串接並測試 |
| `checkWholeDesignEquivalence()` | 已實作 | Batch 4 已串接並測試 |
| Rename / cleanup / simplify / buffer EditApply | 已 public | Batch 3 已串接並測試 |

### 2.2 輸出誤判風險與處理狀態

| 問題 | 風險 | 處理方式 | 狀態 |
|---|---|---|---|
| `DirectlyConnected=false` 欄位缺失 | LLM 可能把 false 當未知 | 固定輸出 `connected: yes/no` | 已修正並加入 test9 regression |
| Function error 時 solver 欄位遺失 | timeout / UNKNOWN / unsupported 被誤讀 | 輸出 solver 狀態與 complete=false | Batch 2 已修正 |
| `PathQueryResult` 無 `ok/message` | 無路徑和 endpoint 名稱錯誤可能都像 `exists=false` | 高階 report 補 validation 狀態 | Batch 5 已修正 |
| enumerate 截斷原因未印 | 部分 paths 被當成完整列表 | 輸出 completeness 與 stop reason | Batch 2 已修正 |
| edit follow-up 缺少穩定資料 | 追問前次修改數量不穩定 | cache 完整 report 並提供 `report_query last_edit` | Batch 3 已修正 |
| read 未保存 original snapshot | 無法比較 current/original | atomic read 後建立 original/current | Batch 1 已修正 |
| coverage 文件過期 | 可能重複實作已完成 API | 以 header、implementation、tests 為準並同步 usage | 持續維護 |

---

## 3. tools.cpp 的責任邊界

`tools.cpp` 只負責：

```text
1. 維護目前 design session。
2. 解析外部 request。
3. 檢查 command-level 必要參數與 enum token。
4. 將 request 轉成既有高階 query/apply struct。
5. 呼叫一個明確的高階 API。
6. 將 report 完整序列化。
7. 保存 last edit/report/snapshot 等 session 狀態。
```

`tools.cpp` 不負責：

```text
1. 自行走訪 netlist graph。
2. 自行計算 cone intersection 或 gate type count。
3. 自行執行 SAT encoding。
4. 直接呼叫 ConnectGateInput、ReplaceDriverOfNet 等 internal mutation。
5. 把 optimization skeleton 宣稱成真正 depth optimization。
6. 在 report 不完整時猜測 yes/no 答案。
```

---

## 4. Session State

建議新增 `ToolSession`，避免所有狀態散落在 `main()`：

```cpp
struct ToolSession {
    Netlist current;
    std::optional<Netlist> original;
    std::optional<Netlist> lastEditBaseline;
    std::optional<NetlistEditReport> lastEditReport;

    std::string loadedFilePath;
    size_t designRevision = 0;
    bool designLoaded = false;
};
```

### 4.1 Read 語意

`read` 必須先讀入暫存 netlist，成功後才替換 session：

```text
read temporary design
  -> fail: current/original 保持不變
  -> success:
       current = loaded design
       original = exact loaded snapshot
       lastEditBaseline = empty
       lastEditReport = empty
       designRevision = 0
```

這可避免 read 失敗後破壞目前設計，也避免重複 read 時資料被 append 到舊 netlist。

### 4.2 Edit 語意

每次合法 public edit 執行前：

```text
lastEditBaseline = current.cloneForRollback()
report = current.runEditApply(request)
lastEditReport = report
```

revision 規則：

```text
report.success && report.changed  -> designRevision += 1
report.success && !report.changed -> revision 不變
report.rolledBack                 -> revision 不變
request validation failed         -> revision 不變
```

### 4.3 Equivalence baseline

Whole-design equivalence 必須區分兩種 prompt：

| Prompt 語意 | baseline |
|---|---|
| current vs original / last loaded from disk | `original` |
| transformed vs pre-transformation / previous edit | `lastEditBaseline` |

不可把這兩種 baseline 混成同一個 `original`。

---

## 5. Request 與 Response 架構

### 5.1 雙模式介面

最後版本建議保留兩種介面，但共用同一個 dispatcher：

```text
Interactive mode:
  給人工 debug，保留目前 command line 語法與 eda> prompt。

Machine mode:
  給 LLM tool adapter / automated test。
  不印 eda>，每個 request 僅產生一個 structured response。
```

兩種模式都必須先轉成同一個 normalized request，再呼叫：

```cpp
ToolResponse dispatchToolRequest(ToolSession& session,
                                 const ToolRequest& request);
```

### 5.2 Machine request envelope

Machine mode 每行接收一個 request object：

```json
{
  "request_id": "req-001",
  "command": "func_query",
  "args": {
    "mode": "always_zero",
    "net_name": "n16"
  }
}
```

共用欄位：

| 欄位 | 必要 | 語意 |
|---|---:|---|
| `request_id` | 建議 | 讓 adapter 對齊 request/response |
| `command` | 是 | 高階 command family |
| `args.mode` | query/apply 類是 | 對應高階 API enum |
| 其他 `args` | 依 mode | 使用各 command 章節定義的具名欄位 |

不可在 machine request 中使用大量無法判斷歸屬的 positional token。互動 CLI 可以保留 positional grammar，但兩者都要先轉成具名的 normalized request。

範例：

```json
{"command":"structure_query","args":{"mode":"summary"}}
{"command":"depth_query","args":{"mode":"gate_on_critical","gate_name":"g0"}}
{"command":"path_query","args":{"mode":"enumerate","startpoints":["pi:n2"],"endpoints":["po:n12"],"max_enumerated_paths":100000,"time_limit_seconds":55,"count_only":false}}
{"command":"edit_apply","args":{"mode":"insert_buffers_for_net","net_name":"n1","max_fanout":4}}
{"command":"equiv_query","args":{"baseline":"original","time_budget_seconds":240}}
```

### 5.3 LLM tool schema 分法

LLM-facing schema 採「一個高階 API family 對應一個 tool」：

```text
read_design
write_design
structure_query
cone_query
path_query
depth_query
function_query
function_search
edit_apply
equivalence_query
last_edit_report
```

不要使用下列兩種極端：

```text
1. 一個 eda_tool 塞進所有 query/edit 的 optional fields。
   問題：欄位彼此衝突，LLM 很難知道 mode 真正需要哪些參數。

2. 每條 testcase prompt 建一個獨立 tool。
   問題：schema 重複、難維護，也無法泛化到不同名稱或門檻。
```

每個 family 的 `mode` 只出現一次，response 回傳完整且穩定的 report。若 prompt 只需要 report 的一個欄位，LLM直接讀該欄位，不新增另一個重複的 summary mode。

### 5.4 共用 response envelope

每次工具回傳至少包含：

```json
{
  "ok": true,
  "status": "ok",
  "command": "depth_query",
  "mode": "global_critical",
  "message": "Global critical path",
  "design_revision": 3,
  "complete": true,
  "data": {},
  "warnings": [],
  "error": null
}
```

`status` 固定使用：

```text
ok          request 完成且結果完整
no_change   edit 成功，但 design 沒有變更
partial     有有效部分結果，但 enumerate / whole-design check 未完成
timeout     SAT 或 enumeration 超時，不能當成 false
unsupported request 合法，但目前演算法不支援
error       參數、名稱、I/O 或執行失敗
```

重要語意：

```text
timeout / partial / unsupported 絕對不能序列化成單純的 yes=false。
yes/no 類回答只有在 complete=true 時才可供 LLM 直接下結論。
```

### 5.5 錯誤格式

```json
{
  "ok": false,
  "status": "error",
  "complete": false,
  "error": {
    "code": "NET_NOT_FOUND",
    "field": "net_name",
    "value": "n999",
    "message": "Net not found: n999"
  }
}
```

建議錯誤碼：

```text
NO_DESIGN_LOADED
INVALID_COMMAND
INVALID_MODE
MISSING_ARGUMENT
INVALID_ARGUMENT
GATE_NOT_FOUND
NET_NOT_FOUND
PORT_NOT_FOUND
DFF_NOT_FOUND
READ_FAILED
WRITE_FAILED
QUERY_FAILED
EDIT_FAILED
NO_ORIGINAL_SNAPSHOT
NO_LAST_EDIT_BASELINE
NO_LAST_EDIT_REPORT
SOLVER_TIMEOUT
ENUMERATION_TRUNCATED
UNSUPPORTED
```

---

## 6. Command Family 總表

| Command | 對應高階 API | 是否修改 current | 第一輪實作優先度 |
|---|---|---:|---:|
| `read` | `VerilogReader` + session snapshot | 是，載入新 design | P0 |
| `write` | `VerilogWriter` | 否 | P0 |
| `structure_query` | mode dispatch 到 `runBasicQuery()` / `runDirectConnectivityQuery()` | 否 | P0 |
| `cone_query` | `runConeQuery()` | 否 | P0/P1 |
| `path_query` | `runPathQuery()` | 否 | P0 |
| `depth_query` | `runDepthQuery()` | 否 | P0/P1 |
| `func_query` | `runFunctionQuery()` | 否 | P0/P2 |
| `func_search` | `runFunctionSearchQuery()` | 否 | P1 completed |
| `edit_apply` | `runEditApply()` | 是 | P0 |
| `equiv_query` | `checkWholeDesignEquivalence()` | 否 | P0 |
| `report_query` | session cached report | 否 | P0 |
| `sequential_query` | `runSequentialPatternQuery()` | 否 | P1 completed |
| `opt_apply` | future real depth optimization | 是 | P3 |

---

## 7. I/O Commands

### 7.1 read

```text
read <verilog_file>
```

成功 response 必須包含：

```text
loaded_file
design_revision = 0
gate_count
net_count
primary_input_count
primary_output_count
original_snapshot_available = true
```

### 7.2 write

```text
write <verilog_file>
```

成功 response 必須包含：

```text
output_file
design_revision
written = true
```

沒有 design 時必須回 `NO_DESIGN_LOADED`。

---

## 8. structure_query：Basic modes

下列公開 mode 由 `structure_query` dispatch 到 `BasicQueryType`：

| mode | 必要參數 | 主要輸出 | NewTestCase 用途 |
|---|---|---|---|
| `summary` | 無 | design counts、gate type breakdown | 每個 testcase 開頭 gate 統計 |
| `list_gates` | 無 | gate names/count | 列出所有 gates |
| `list_nets` | 無 | net names/count | 列出所有 signals |
| `list_pi` | 無 | PI names/count + width/range summaries | PI list 與 bit widths |
| `list_po` | 無 | PO names/count + width/range summaries | PO list 與 bit widths |
| `list_dffs` | 無 | DFF names/count | sequential inventory |
| `list_comb` | 無 | combinational gate names/count | combinational inventory |
| `gate_info` | `gate_name` | type、pins、connections | `What type of gate is g0?` |
| `net_info` | `net_name` | PI/PO/constant/internal | signal information |
| `port_info` | `port_name` | width、bus、bit nets | `list PI/PO with bit widths` |
| `count_by_type` | optional `gate_type` | count / all type counts | `How many NOT gates...` |
| `gates_by_type` | `gate_type` | names/count | `List all XOR gates` |
| `const_input_gates` | optional `gate_type`, optional `const_value` | matching gates | constant propagation 前置查詢 |
| `structural_issues` | 無 | undriven/no-load/floating/unconnected | floating signals |

規格要求：

```text
1. machine schema 中 gate_type 與 const_value 必須是獨立欄位，避免 positional ambiguity。
2. count 為 0 時仍必須輸出欄位。
3. GateInfo 必須輸出 pin connections，不能只輸出 formattedInfo 字串。
4. `list_pi/list_po` 必須一次回傳全部 port width/range；`PortInfo` 仍保留單一 port 的 bus bit names。
```

---

## 9. structure_query：Direct Connectivity modes

| mode | 必要參數 | 主要輸出 | Prompt 用途 |
|---|---|---|---|
| `net_driver` | `net_name` | direct driver gates | net driver |
| `net_loads` | `net_name` | direct load gates | direct loads |
| `fanout_load` | `net_name` | QA fanout count + pin categories | `fanout of n0` |
| `global_fanout` | optional `fanout_limit` | max fanout / violations | global constraint |
| `pi_fanout` | optional `fanout_limit` | highest fanout PI | test36/test38 |
| `gate_inputs` | `gate_name` | input nets | pin connections |
| `gate_output` | `gate_name` | output net | gate output |
| `gate_fanin` | `gate_name` | immediate predecessor gates | one-level fanin |
| `gate_fanout` | `gate_name` | immediate successor gates | `connected to output of g0` |
| `is_connected` | `gate_name`, `net_name` | connected yes/no | direct connection |

`fanout_load` 必須完整輸出：

```text
total_load_count
combinational_gate_load_names
dff_data_load_names
dff_clock_load_names
dff_reset_set_load_names
dff_other_load_names
primary_output_load_count
drives_primary_output
```

這可直接回答：

```text
List all flip-flops driven by clock n0.
Which primary input has the highest fanout?
What is the fanout of primary input n0?
```

---

## 10. cone_query

### 10.1 已有核心 API，可直接 expose

| mode | API type | 參數 |
|---|---|---|
| `net_fanin` | `NetTransitiveFanin` | `net_name` |
| `net_fanout` | `NetTransitiveFanout` | `net_name` |
| `gate_fanin` | `GateTransitiveFanin` | `gate_name` |
| `gate_fanout` | `GateTransitiveFanout` | `gate_name` |
| `largest_output` | `LargestOutputCone` | 無 |

輸出至少包含：

```text
source_name
gate_count
net_count
gate_names
net_names
root_net_names
checked_output_count
longest_depth / shortest_depth（有要求時）
```

### 10.2 Batch 5 已補 ConeReport 並 expose

| 新需求 | 建議 API | 原因 |
|---|---|---|
| cone 內各 gate type 數量 | `ConeReport.gateTypeCounts` | 不應由 LLM 對 gate list 自行統計 |
| 兩個 fanin cone shared gates | `ConeQueryType::SharedFaninGates` + second net field | 不應由 tools.cpp 自行做集合交集 |

對應 prompt：

```text
Report the number of each gate type in the cone of n8/n14.
Report all gates shared between the fanin cones of n16 and n17.
Which output has the largest fanin cone?
```

---

## 11. path_query

### 11.1 Modes

```text
exists
find_any
enumerate
min_depth
max_depth
every_through
every_avoids
direct_pi_po
```

### 11.2 Endpoint token

```text
net:<name>
pi:<port>
po:<port>
all_pi or pi:*
all_po or po:*
dff_q:<ff>
dff_d:<ff>
all_dff_q or dff_q:*
all_dff_d or dff_d:*
dff_clk:<ff>[:pin]
dff_reset:<ff>[:pin]
gate_out:<gate>
gate_in:<gate>:<index_or_pin>
```

底層 `resolvePathEndpoint()` 以空 PI/PO name 代表全部；CLI 已提供 `all_pi`、`pi:*`、`all_po` 與 `po:*` token。

### 11.3 Enumeration safety options

```text
max_printed_paths
max_enumerated_paths
time_limit_seconds
count_only
output_file
```

預設值沿用 `PathQuery`：

```text
maxPrintedPaths = 20
maxEnumeratedPaths = 100000
enumerationTimeLimitSeconds = 55.0
countOnly = false
```

enumerate response 必須包含：

```text
path_count
complete_enumeration
timed_out
path_limit_reached
count_only
stop_reason
wrote_paths_to_file
output_file
displayed_paths
```

若 `complete_enumeration=false`，response envelope 必須為 `partial` 或 `timeout`，不可回答成「列出的就是所有 paths」。

### 11.4 PathQueryResult 錯誤語意（Batch 5 已完成）

目前 result 已可靠區分：

```text
合法查詢但沒有 path
startpoint 名稱不存在
endpoint 名稱不存在
combinationalOnly=false 尚未支援
```

已新增：

```cpp
bool ok;
std::string message;
std::vector<std::string> unresolvedStartpoints;
std::vector<std::string> unresolvedEndpoints;
std::vector<std::string> unresolvedRequiredNodes;
std::vector<std::string> unresolvedAvoidedNodes;
bool unsupported;
```

### 11.5 Direct PI-to-PO length-0 paths

題目會問：

```text
Find all paths of length 0 (direct wire connections from PI to PO).
```

已由 `PathQueryMode::DirectPiPoConnections` 直接掃描 PI/PO net intersection，不做大量 path enumeration，並 expose 為：

```text
path_query direct_pi_po
```

---

## 12. Register path preset（整併至 path_query）

公開格式使用 `path_query <mode> all_dff_q all_dff_d`；指定單一 register 時使用 `dff_q:<ff>` 與 `dff_d:<ff>`。底層 `RegisterPathQuery` / `runRegisterPathQuery()` 只作為 legacy C++ wrapper，不再建立獨立 tool schema。

Modes：

```text
exists
find_any
enumerate
min_depth
max_depth
```

輸入：

```text
start_dff_names: empty means all DFF.Q
end_dff_names: empty means all DFF.D
required_nodes
avoided_nodes
max_printed_paths
max_enumerated_paths
time_limit_seconds
count_only
output_file
```

所有 enumerate completeness 欄位必須原樣從 `report.pathResult` 輸出。

可回答：

```text
List all register-to-register paths through combinational logic.
What is the maximum combinational depth on any register-to-register path?
```

---

## 13. depth_query

### 13.1 已實作並 expose

| mode | API type | 必要參數 | 主要結果 |
|---|---|---|---|
| `net` | `SpecificNet` | `net_name` | endpoint depth/path |
| `all_po` | `PrimaryOutputs` | 無 | all PO depth |
| `all_dff_d` | `DffD` | 無 | all DFF.D depth |
| `global_critical` | `GlobalCriticalPath` | 無 | global worst depth/path |
| `exceeding` | `EndpointsExceedingDepth` | `threshold` | PO + DFF.D over threshold |
| `po_exceeding` | `PrimaryOutputsExceedingDepth` | `threshold` | only PO bits over threshold |
| `gate_on_critical` | `GateOnCriticalPath` | `gate_name` | yes/no |
| `deepest_output` | `DeepestOutputCone` | 無 | deepest PO |

### 13.2 PO-only wrapper（Batch 5 已完成）

題目原文：

```text
How many outputs have a logic depth greater than 4?
```

`EndpointsExceedingDepth` 同時包含 PO 與 DFF.D，不能直接把 `count` 當 output count。目前已新增：

```text
DepthQueryType::PrimaryOutputsExceedingDepth
depth_query po_exceeding <threshold>
```

不要要求 LLM 從 mixed endpoint report 自行篩選。

### 13.3 Response

```text
report_count
reports[{endpoint_type, endpoint_name, depth, critical_path}]
worst
threshold
gate_name
gate_on_critical_path
```

`gate_on_critical` 必須明確輸出 false，不可只在 true 時顯示欄位。

---

## 14. func_query

### 14.1 已 expose 的 modes

| mode | API type | 參數 |
|---|---|---|
| `equivalence` | `Equivalence` | `net_a`, `net_b` |
| `can_be_value` | `CanBeValue` | `net`, `const_value` |
| `constant` | `ConstantFunction` | `net`, `const_value` |
| `always_zero` | `AlwaysZero` | `net` |
| `always_one` | `AlwaysOne` | `net` |
| `truth_status` | `TruthStatus` | `net` |
| `depends_on` | `FunctionalDependence` | `target_net`, `input_net` |
| `symmetry` | `Symmetry` | `target_net_or_bus`, `input_a`, `input_b` |
| `boolean_expression` | `BooleanExpression` | `net` |
| `simplified_expression` | `SimplifiedBooleanExpression` | `net`, `max_depth` |
| `support_pi` | `PrimaryInputsOfNet` | `net` |

### 14.2 SAT response 規則

SAT 類 mode 必須輸出：

```text
solver_ran
solver_status
solver_timed_out
solver_unknown
unsupported
equivalent
can_be_zero
can_be_one
is_constant
input_in_structural_support
depends_on_input
symmetric
symmetry_input_a_in_structural_support
symmetry_input_b_in_structural_support
counterexample_found
mismatched_target_bit_names
counterexample_assignments
output_values_before_swap
output_values_after_swap
truth_status
```

只有 `solver_status=SAT/UNSAT` 且 `complete=true` 時，LLM 才能把主要 bool 當最終答案。

### 14.3 Expression response

```text
expression
expression_length
max_expression_depth
expression_depth_limited
support_primary_inputs
```

若 expression 太長，tool response 可同時提供檔案輸出，但不可靜默截斷後仍宣稱是完整 equation。

### 14.4 Search API 與剩餘缺口

| Prompt | API / 狀態 |
|---|---|
| 是否存在 internal pair 使 NAND(a,b)==target | `func_search nand_pair <target>`；`FunctionSearchQuery::NandEquivalentInputPairs` |
| arbitrary functional-equivalent gate pair search | SAT-based candidate/search API |

---

## 15. Path separator / mandatory modes

| public path mode | internal Graph API type | 參數 | 主要結果 |
|---|---|---|---|
| `path_query pi_po_cut` | `IsCutNetBetweenPiPo` | `internal_net` | `isSeparator`, `pathExists`, witness PI/PO |
| `path_query mandatory_nodes` | `ArticulationPointsBetween` | `source_net`, `target_net` | `pathExists`, mandatory IDs/names |
| `path_query is_separator` | `ArticulationPointsBetween` | `source_net`, `target_net`, `candidate_net` | `pathExists`, `isSeparator` |

Graph layer 使用 active net directed DAG 與 dominator tree；`runPathQuery()` 負責公開 dispatch 與 `PathQueryResult` 正規化。`NO_PATH`、empty mandatory list、invalid object 與 cycle unsupported 不可混成同一個 false。舊 `graph_query` parser 只保留相容性並從 help/schema 隱藏。

---

## 16. edit_apply

### 16.1 Public command 原則

對外只 expose `EDIT_APPLY_USAGE.md` 中有等價性 certificate 的 public command。以下 internal primitive 永遠不出現在 help/tool schema：

```text
DisconnectGateInput
ConnectGateInput
RemoveGate
ReplaceAllLoadsOfNet
ReplaceGateWithNet
ReplaceGateWithConstant
ReplaceGateWithNotOfNet
ReplaceDriverOfNet
RewireGateOutputToExistingNet
ReplaceNetFunctionKeepingName
MergeNetIntoNet
BypassNetKeepingPortSemantics
RedirectAllLoads
```

### 16.2 第一批 public edit modes

Rename：

```text
rename_gate <old_name> <new_name>
rename_net <old_name> <new_name>
```

Cleanup / simplification：

```text
cleanup_buffers
collapse_double_inverter
local_simplification_fixpoint
safe_cleanup_fixpoint
trim_dead_logic
remove_dangling_logic
remove_unused_nets
remove_net_if_unused <net_name>
merge_structurally_equivalent_gates
simplify_constants
simplify_same_input
```

`merge_equivalent_gates` 不對外開放：legacy 實作只做 structural identity，名稱會誤導 LLM。public structural merge 固定使用 `merge_structurally_equivalent_gates`；未來 SAT-based flow 使用獨立的 `merge_functionally_equivalent_gates`。

Buffer insertion：

```text
insert_buffers_for_fanout <max_fanout>
insert_buffers_for_net <net_name> <max_fanout>
insert_buffers_for_dff_control <max_fanout> <clock/reset flags>
insert_buffers_on_each_load <net_name>
insert_buffer_at_driver <net_name>
insert_buffer_before_gate <net_name> <gate_name>
insert_buffers_by_gate_type <gate_type> <input/output flags>
```

Technology mapping：

```text
convert_basis <scope> [scope_name] <allowed_types> [banned_types]
replace_type <scope> [scope_name] <target_type> <allowed_types>
```

### 16.3 Edit defaults

```text
validate_equivalence = true
rollback_on_failure = true
```

Public tool schema 不提供關閉 rollback 的選項。若只具有 local rewrite certificate，response 必須如實回 `LocalRewriteRule`，不能標成 `WholeDesignSat`。

### 16.4 Edit response

必須完整保留 `NetlistEditReport`：

```text
success
changed
rolled_back
operation_kind
operation_name
before_stats
after_stats
diff
validation
depth_change
fanout_change
mapping_delta
changed_gate_names
changed_net_names
warnings
```

follow-up 計數規則：

| 問題 | 欄位 |
|---|---|
| removed gates | `-diff.activeGateCountDelta`，並確認 delta < 0 |
| specified type eliminated | `-diff.gateTypeCountDelta[type]` |
| specified type added | `diff.gateTypeCountDelta[type]` 或 `mappingDelta.addedCountByType[type]` |
| final type count | `afterStats.gateTypeCounts[type]` / mapping final count |
| max fanout after edit | `fanoutChange.afterMaxFanout` |
| constraint met | `fanoutChange.meetsConstraint` |

---

## 17. equiv_query（Batch 4 已完成）

已新增 command：

```text
equiv_query original [time_budget_seconds]
equiv_query previous_edit [time_budget_seconds]
```

對應：

```cpp
session.current.checkWholeDesignEquivalence(baseline, budget)
```

預設 budget：

```text
240 seconds
```

response 必須輸出完整 `WholeDesignEquivalenceReport`，尤其是：

```text
ok
equivalent
time_budget_exceeded
compared_output_count
skipped_output_count
compared_dff_d_count
skipped_dff_d_count
matched_output_names
mismatched_output_names
skipped_output_names
matched_dff_d_names
mismatched_dff_d_names
skipped_dff_d_names
interface differences
unsupported_reasons
warnings
```

只要有 skipped PO/DFF.D endpoint、timeout 或 unsupported reason，就不可回答「已證明等價」。

---

## 18. report_query

新增：

```text
report_query last_edit
```

用途是穩定回答：

```text
How many BUF gates were added by the buffer insertion just performed?
How many dangling gates were removed?
How many OR gates were eliminated by constant-1 propagation?
What happened in the previous transformation?
```

它只回傳 cached `lastEditReport`，不重新執行 edit，也不重新比較 before/after。

若 session 沒有 edit report，回 `NO_LAST_EDIT_REPORT`。

---

## 19. 目前仍需補高階 API 的 NewTestCase 題型

這些不可只靠補 `tools.cpp` 解決：

| 題型 | 建議 owner API | 狀態 |
|---|---|---|
| cone gate type breakdown | ConeReport extension | 小缺口 |
| shared fanin cone gates | ConeQuery extension | 小缺口 |
| PO-only depth exceeding count | DepthQuery extension | 小缺口 |
| direct PI-to-PO depth-0 connections | PathQuery extension | 小缺口 |
| path invalid endpoint vs no path | PathQueryResult validation fields | 必要修正 |
| symmetry | `FunctionQueryType::Symmetry` | API/report/CLI/mini test22 已完成 |
| DFF enable/hold detection | SequentialPatternQuery | API/report/CLI/pagination 已完成 |
| arbitrary functional duplicate/redundancy removal | SAT candidate + verified edit flow | 缺演算法 |
| critical/max depth optimization | real OptApply | 未完成 |
| cone depth optimization with basis constraint | scope-aware OptApply | 未完成 |

---

## 20. 不應對外 expose 的現有 optimization skeleton

目前 `runOptQuery()` / `runOptApply()` 只支援：

```text
cleanup buffer chain
collapse double inverter
local simplification fixpoint
```

而且目前：

```text
scopeName 尚未真正 enforced
candidateIds 會被接受但實際套用 whole pass
whole-design equivalence 尚未整合
沒有真正 depth optimization
```

因此在完成真正 optimization contract 前，不應將它 expose 成可回答「minimize critical path depth」的 `opt_apply`。這些 cleanup 功能先由 `edit_apply` 對外提供即可。

---

## 21. Prompt Routing 規則

| Prompt 關鍵語意 | Tool command |
|---|---|
| count/list/type/PI/PO/DFF/port width | `structure_query` |
| direct driver/load/fanout/pin connection | `structure_query` |
| reachable/transitive/cone/largest cone | `cone_query` |
| path from A to B/through/avoid/all paths | `path_query` |
| register-to-register path | `path_query` + DFF endpoint preset |
| depth/critical/deepest/output threshold | `depth_query` |
| named-net equivalence/constant/dependence/symmetry/equation | `func_query` |
| unknown NAND-equivalent internal candidate pair search | `func_search` |
| cut/articulation/mandatory/separator | `path_query` |
| DFF enable/hold semantics | `sequential_query` |
| specified rename/cleanup/simplify/buffer/map | `edit_apply` |
| objective/cost-driven minimize/optimize/best | future `opt_apply` |
| current vs original/pre-edit equivalence | `equiv_query` |
| previous edit delta/follow-up count | `report_query last_edit` |

LLM 不應用錯誤替代：

```text
reachable != direct fanout
structurally connected != functionally equivalent
constant input != constant function
local rewrite certificate != whole-design SAT proof
largest cone by gate count != deepest cone by logic depth
all timing endpoints over threshold != only primary outputs over threshold
named function proof != unknown candidate search
specified transformation != cost-driven optimization
structural duplicate merge != SAT functional merge
```

---

## 22. 分批實作順序

### Batch 1：建立可測試的 tools core（已完成）

```text
ToolSession
ToolRequest / ToolResponse
single dispatcher
NO_DESIGN_LOADED guard
atomic read + original snapshot
structured status/error envelope
保留 interactive text adapter
```

這一批不增加新 domain 功能，只整理架構並確保舊 command 行為不退化。

### Batch 2：補齊已完成 query API 的 routing（已完成）

```text
Cone LargestOutputCone
Depth GateOnCriticalPath / DeepestOutputCone
Function BooleanExpression / SimplifiedBooleanExpression / PrimaryInputsOfNet
Function solver status output
Path/RegisterPath safety options and completeness output
all_pi / all_po endpoint tokens
```

### Batch 3：補齊 public EditApply（已完成）

```text
rename
cleanup
simplification
structural merge
buffer insertion
technology mapping
full NetlistEditReport output
lastEditBaseline / lastEditReport state
report_query last_edit
```

### Batch 4：接 whole-design equivalence（已完成）

```text
equiv_query original
equiv_query previous_edit
time budget
partial/timeout/unsupported output
```

### Batch 5：補小型高階 API 缺口並接 tools（已完成）

```text
Cone gateTypeCounts
SharedFaninGates
PrimaryOutputsExceedingDepth
DirectPiPoConnections
PathQueryResult validation fields
```

### Batch 6：特殊分析（進行中）

```text
已完成：Cut / articulation
已完成：DFF enable / hold CLI exposure
已完成：Symmetry API/report/CLI 與 official testcase 驗證
已完成：NAND function pair search high-level API/report/mini test23
已完成：`func_search nand_pair` CLI/report/help 與 mini test24
待完成：arbitrary functional-equivalent gate pair search
```

已實作 command contract：

```text
sequential_query enable_hold <all|dff_name>
  [--summary-only] [--confirmed-only] [--include-no-pattern]
  [--offset N] [--limit N] [--verify-sat]
```

輸入語意：

| 參數 | 語意 |
|---|---|
| `all` | 分析所有 active DFF |
| `<dff_name>` | 只分析指定 DFF instance |
| default | 包含 AND-only semantics-pending candidates；all detail 只列有 pattern 的 DFF |
| `--summary-only` | 只回完整 aggregate，回答 count prompt |
| `--confirmed-only` | 排除 AND-only candidates |
| `--include-no-pattern` | detail 也列無 pattern DFF |
| `--offset/--limit` | detail 分頁；all 預設上限 50 records |
| `--verify-sat` | 只允許指定 DFF；all-DFF SAT 會在 parser 階段拒絕 |

必要輸出：

```text
status / message
DFF 總數 / 已分析 DFF 數量
confirmed unique DFF 數量 / candidate DFF 數量
每顆 DFF：D/Q net、pattern kind、enable/data/feedback、active level
structural/functional proof status、SAT timeout/unknown、evidence gates
pagination completeness、next offset
```

CLI formatter 不得把 `AndGatedDataCandidate` 算入 confirmed matched count；官方回覆前必須輸出 `semantics pending`。

驗證：`mini test/test21` 12/12；test40 summary 約 0.32 秒、1101 characters，預設 detail 固定 50 records 並明確回 partial。

### Batch 7：真正 optimization

```text
scope-aware depth candidate
optimization apply
basis constraint validation
whole-design equivalence
accept/no-improvement/rollback
NetlistEditReport.depthChange
```

---

## 23. 每一批的驗證規則

每一批完成後都必須：

```text
1. 編譯 tools.cpp。
2. 跑該 API 原有 mini test。
3. 跑 tools integration test，從 stdin 餵 command 並驗證 structured output。
4. 測正常名稱、錯誤名稱、缺參數與 0-result。
5. mutation 類同時驗證 current design、report delta 與 write-back。
6. timeout/partial 類確認不會被輸出成 false/no。
7. 移除測試產生的 tools.exe / output Verilog / path files。
```

建議新增獨立 tools integration test，不把 parser 測試混入現有單一 API unit test：

```text
mini test/test9/
  tools integration circuit
  scripted commands
  expected machine responses
```

### 23.1 NewTestCase coverage regression

Batch 5 完成後，下一步應建立 prompt-to-command coverage table，逐條驗證 test31 到 test40 的非 optimization prompt：

```text
prompt 原文
selected command/mode
arguments
required report fields
complete / partial / unsupported expectation
```

不能只檢查「有 command 名稱」，還要檢查 response 是否真的包含題目要求的細節欄位。

---

## 24. Definition of Done

`tools.cpp` 可視為完成，必須同時滿足：

```text
1. 每個已 public 高階 API 都有明確 tool routing，或文件明確說明不對外開放原因。
2. 每個 NewTestCase 非 optimization prompt 都能對應到單一 command 或明確的高階 API 缺口。
3. 不需要 LLM 對大型 gate/path list 自行做交集、過濾與統計。
4. yes/no、timeout、unsupported、invalid input 不會互相混淆。
5. 所有 public edit 都回傳完整 NetlistEditReport。
6. original、previous edit、current 三種 design state 不會混淆。
7. previous edit 的數量追問可由 cached report 回答。
8. path enumeration 與 SAT/whole-design check 都遵守時間與完整性語意。
9. internal low-level mutation 不出現在外部 help/schema。
10. 每批新增 routing 都有 integration test 與既有 regression 證據。
```

---

## 25. 目前下一個實作工作項目

Batch 1 到 Batch 5 已完成並有獨立 integration test，test31 到 test40 command coverage audit 也已整理。Typed/scoped constant propagation、batch PI/PO width report、Basic active-design semantics、exact functional dependence、directed cut/articulation 與 symmetry analysis 已完成。下一步處理剩餘特殊分析與最佳化缺口：

```text
1. Function pair search / observability-aware redundancy analysis。
2. Constrained depth optimization public flow。
```

Batch 5 已遵守高階 API 邊界：cone intersection、PO-only filtering 與 direct depth-0 connection analysis 都在 analysis layer 完成，`tools.cpp` 只做參數解析與輸出。
