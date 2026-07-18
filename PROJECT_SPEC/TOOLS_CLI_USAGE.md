# Tools CLI Usage

這份文件是目前 `tools.cpp` 的對外使用手冊，內容只包含現在已經可以呼叫的 command。

完整實作規格、未來 tool schema、尚未 expose 的 API 與分批開發順序請看：

```text
API_SPEC/TOOLS_CPP_IMPLEMENTATION_SPEC.md
```

公開 facade 與 legacy compatibility 的責任表請看 `API_SPEC/PUBLIC_API_CONSOLIDATION.md`。

---

## 1. 目前架構

`tools.cpp` 提供分類式高階入口：

```text
read / write
structure_query
cone_query
path_query
depth_query
func_query
func_search
edit_apply
report_query
equiv_query
```

目前所有 command 會經過同一個 dispatcher，並共用同一個 design session。

LLM 或人工使用時，不應直接呼叫 `getGateCount()`、`connectGateInput()`、`replaceDriverOfNet()` 等低階 helper。查詢應走 query command，修改應走 public `edit_apply`。

### 1.1 Input Grammar

CLI 每次讀取一行，第一個 token 是 command，第二個 token 通常是 mode。command 與 mode 不分大小寫；gate、net、port、DFF 名稱依 netlist 原名精確比對。

```text
<command> <mode> <positional args...> [options...]
```

`read`、`write` 會把剩餘整行視為路徑，因此支援以單引號或雙引號包住含空白路徑。其他參數目前以空白切分，名稱與 `-out` path 不應包含空白。

Gate type token 不分大小寫，例如 `and`、`AND` 都會解析為 `GateType::AND`。數值參數必須使用十進位整數或文件指定的秒數格式。

---

## 2. Build

目前 MinGW 編譯 mockturtle 相關 translation unit 時可能因 template 展開耗費大量時間或產生過大的 object file。現階段 `tools.cpp` 尚未 expose depth optimizer，也不直接使用 mockturtle converter，因此 tools CLI 應排除 `DepthOptimizer.cpp` 與 `MockturtleConverter.cpp`。

在專案根目錄執行：

```powershell
$allSources = Get-ChildItem `
    src\core\*.cpp, `
    src\io\*.cpp, `
    src\analysis\*.cpp, `
    src\optimization\*.cpp, `
    src\transformation\*.cpp

$excludedSources = @('DepthOptimizer.cpp', 'MockturtleConverter.cpp')

$toolSources = @('tools.cpp') + `
    ($allSources | Where-Object { $_.Name -notin $excludedSources } |
        ForEach-Object { $_.FullName })

g++ -std=c++20 `
    -DFMT_HEADER_ONLY -DWIN64 -DLIN64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
    -fpermissive `
    -Wno-unknown-pragmas -Wno-narrowing -Wno-sign-compare `
    -Wno-unused-parameter -Wno-misleading-indentation -Wno-unused-variable `
    -Wno-unused-but-set-variable -Wno-format -Wno-dangling-else `
    -Wno-class-memaccess -Wno-int-to-pointer-cast -Wno-overflow `
    -I. -Iinclude -Iinclude/lib -Iinclude/lib/nauty `
    -Iinclude/lib/abcsat -Iinclude/lib/abcesop `
    $toolSources `
    -Linclude/lib/cadical/win/ucrt64 -lcadical `
    -o tools.exe
```

若未來 `tools.cpp` 正式 expose mockturtle/depth optimizer，應先完成這兩個 translation unit 的獨立建置驗證，再從排除清單移除。

---

## 3. Start And Exit

啟動：

```powershell
.\tools.exe
```

互動提示字元：

```text
eda>
```

顯示 command help：

```text
help
```

離開：

```text
quit
exit
```

---

## 4. Tool Response

每個非空 command 都會回傳一個統一 envelope：

```text
TOOL_RESULT_BEGIN
ok: true
status: ok
command: structure_query
mode: summary
message: Design summary
design_revision: 0
complete: true
data:
...
TOOL_RESULT_END
```

### 4.1 Envelope 欄位

| 欄位 | 意思 |
|---|---|
| `ok` | command 是否成功 |
| `status` | 統一狀態碼 |
| `command` | 本次執行的 command family |
| `mode` | query/apply mode；沒有 mode 時省略 |
| `message` | 簡短結果或錯誤原因 |
| `design_revision` | current design 已成功修改的次數 |
| `complete` | 結果是否完整，可否直接當成最終答案 |
| `data` | 高階 API report 的詳細內容 |

欄位判讀順序：

```text
1. 先看 status 與 complete。
2. status=error 時讀 message，不解讀 data 為有效答案。
3. complete=false 時只能回報部分結果或未知原因，不能猜成 no/0。
4. edit 的 ok=true 只表示 apply 成功；是否真的修改需再看 report_changed。
```

### 4.2 Status

```text
ok           request 成功且結果完整
no_change    edit 成功，但 design 沒有改變
partial      有部分有效結果，但結果不完整
timeout      SAT 或 enumeration 超時
unsupported  request 合法，但目前演算法不支援
error        參數、名稱、I/O 或執行失敗
```

`partial`、`timeout`、`unsupported` 不可解讀成 yes/no 題目的 `no`。

### 4.3 Common Error Semantics

| 情況 | Status | Session 是否改變 |
|---|---|---|
| 尚未 read 就 query/edit/write | `error` + `NO_DESIGN_LOADED` | 否 |
| mode 不存在或缺少參數 | `error` | 否 |
| gate/net/port 名稱不存在 | 通常為 `error` | 否 |
| edit 合法但沒有修改機會 | `no_change` | revision 不增加，但保存 edit report |
| enumeration 達 path limit | `partial` | 否 |
| enumeration 或 solver 超時 | `timeout` | 否 |
| 演算法不支援該功能 | `unsupported` | 否 |

---

## 5. Session State

CLI 目前保存：

```text
current design
original snapshot
last edit baseline
last edit report
design revision
```

成功 `read` 後：

```text
current = 新載入設計
original = 同一份設計的 snapshot
last edit state = 清空
design_revision = 0
```

`read` 會先讀入 temporary netlist，成功後才替換 current。若檔案不存在或 parsing 丟出錯誤，原本 current design 會保留。

目前 original、last edit baseline 與 last edit report 已由 session 保存。`report_query last_edit` 可讀取上一筆修改報告；`equiv_query original/previous_edit` 可比較 current 與對應 baseline 的 PO、DFF.D combinational-boundary equivalence。

| Command | current | original | last edit cache | revision |
|---|---|---|---|---|
| successful `read` | 替換 | 建立 snapshot | 清空 | 歸零 |
| failed `read` | 保留 | 保留 | 保留 | 保留 |
| query / `report_query` | 不變 | 不變 | 不變 | 不變 |
| successful changed edit | 更新 | 不變 | 更新 | `+1` |
| successful no-change edit | 不變 | 不變 | 更新 | 不變 |
| failed edit request | 不變或 rollback | 不變 | parser 接受並執行時保存 failed report | 不變 |

---

## 6. I/O Commands

### 6.1 Read

```text
read <verilog_file>
```

範例：

```text
read NewTestCase/test31/test31.v
read "C:\path with spaces\design.v"
```

成功時 `data` 會包含：

```text
loaded_file
gate_count
net_count
primary_input_count
primary_output_count
original_snapshot_available
```

語意：先讀入 temporary netlist，成功後才一次替換 session；因此 failed read 不會清掉已載入的 design。輸入檔不存在或 parser 失敗時回 `error`。

### 6.2 Write

```text
write <verilog_file>
```

範例：

```text
write NewTestCase/test31/test31_out.v
```

輸入是輸出 Verilog 路徑。成功時 `data` 包含 `output_file` 與 `written: true`；write 不會改變 design、snapshot、last report 或 revision。

尚未載入 design 時，query、edit 與 write 會回：

```text
status: error
message: NO_DESIGN_LOADED: ...
```

---

## 7. structure_query：基本結構模式

格式：

```text
structure_query <mode> [args]
```

語意：回答 design inventory、單一物件資訊及結構問題，不執行 graph reachability、SAT 或修改。

| Mode | Input | 主要輸出 | 語意 |
|---|---|---|---|
| `summary` | 無 | active gates、active nets、logical wires、PI/PO counts、gate type counts | counts-only current-design summary，過濾 edit tombstones，不列全部 gate/net names |
| `list_gates` | 無 | `Gate names` | 所有 active gates |
| `list_nets` | 無 | `Net names` | 所有 active nets |
| `list_pi` | 無 | `Port names`、port summaries | PI ports 的 name、width、msb/lsb、bus 與 direction；不是展開後 path endpoints |
| `list_po` | 無 | `Port names`、port summaries | PO ports 的 name、width、msb/lsb、bus 與 direction |
| `list_dffs` | 無 | `Gate names` | 所有 DFF instances |
| `list_comb` | 無 | `Gate names` | 所有 combinational gates |
| `gate_info` | `<gate>` string，必填 | object、id、type、formatted pin/connection info | 單一 gate 詳細資料 |
| `net_info` | `<net>` string，必填 | object、id、type、formatted PI/PO/constant info | 單一 net 詳細資料 |
| `port_info` | `<port>` string，必填 | object、width、formatted bus/scalar info | port width 與 bits |
| `count_by_type` | `[gate_type]` | gate type counts | 指定 type；省略時回全部 |
| `gates_by_type` | `<gate_type>`，必填 | count、gate names | 指定 type instances |
| `const_input_gates` | `[gate_type\|all] [0\|1\|any] [--inputs N]` | count、gate names | 可依 type/value/input count 篩選；結構上接 constant 不代表 output 是 constant function |
| `structural_issues` | 無 | undriven、no-load、floating nets、unconnected gates | 結構檢查清單 |

`structure_query` 的 total/list/type/structural 結果都以 current active design 為準。edit 留下的 `UNKNOWN` gate 與 removed net 不會出現在答案中；對它們執行 `gate_info` / `net_info` 會明確回報 not found。

範例：

```text
structure_query summary
structure_query gate_info g0
structure_query port_info n9
structure_query count_by_type NOT
structure_query gates_by_type XOR
structure_query const_input_gates NAND 1
structure_query const_input_gates NAND 1 --inputs 2
structure_query structural_issues
```

名稱不存在、必要名稱缺失或 gate type 不合法時回 `error`。合法查詢沒有符合項目時應解讀為空清單或 count 0，而不是錯誤。

---

## 8. structure_query：直接連線模式

格式：

```text
structure_query <mode> [args]
```

語意：只回答一層直接連線與 Problem A fanout load 定義，不回答 transitive reachability。

| Mode | Input | 主要輸出 | 語意 |
|---|---|---|---|
| `net_driver` | `<net>` | count、gate names | 直接 driver gates |
| `net_loads` | `<net>` | count、gate names | 直接 load gates |
| `fanout_load` | `<net>` | total load、PO loads、分類後 gate/DFF pin loads | Problem A QA pin-level fanout |
| `fanout_report` | `<net>` | 同 `fanout_load` | alias |
| `global_fanout` | `[limit]` integer | checked nets、max fanout、max/violating nets、limit result | 全設計 fanout；省略 limit 只找最大值 |
| `pi_fanout` | `[limit]` integer | 同 global，但只掃 PI nets | primary input fanout |
| `fanout_violations` | `<limit>` integer，必填 | violating nets、satisfies limit | 超過限制的 nets |
| `gate_inputs` | `<gate>` | count、net names | gate input nets |
| `gate_output` | `<gate>` | count、net names | gate output net |
| `gate_fanin` | `<gate>` | count、gate names | immediate predecessor gates |
| `gate_fanout` | `<gate>` | count、gate names | immediate successor gates |
| `is_connected` | `<gate> <net>` | `connected: yes/no` | gate 與 net 是否直接相連；false 也固定輸出欄位 |

範例：

```text
structure_query fanout_load n0
structure_query pi_fanout
structure_query gate_fanout g0
structure_query gate_inputs g0
structure_query is_connected g0 n1
```

`structure_query` 的 connectivity modes 只回答一層直接連線。多層 reachable/cone 問題應使用 `cone_query`。

內部會依 mode dispatch 到 `BasicQuery` 或 `DirectConnectivityQuery`。舊 `basic_query`、`conn_query` 仍可執行以相容既有腳本，但不屬於公開 help/schema。

Fanout 的 `totalLoadCount` 計算 combinational input pins、DFF D/clock/reset/set/other pins 及 PO connections；同一 gate 的不同 input pin 可分別計數，因此不一定等於 unique load gate 數量。

---

## 9. cone_query

格式：

```text
cone_query <mode> [name] [with_paths]
```

語意：回答多層 transitive fanin/fanout cone。`with_paths` 是可選 token，要求補充 cone 內最長/最短 local path depth。

| Mode | Input | 主要輸出 | 語意 |
|---|---|---|---|
| `net_fanin` | `<net> [with_paths]` | source、gate/net counts、root nets、cone names、optional depths | net 的 transitive fanin |
| `net_fanout` | `<net> [with_paths]` | 同上 | net 的 transitive fanout |
| `gate_fanin` | `<gate> [with_paths]` | 同上 | gate 的 transitive fanin |
| `gate_fanout` | `<gate> [with_paths]` | 同上 | gate 的 transitive fanout |
| `largest_output` | 無 | selected output、cone counts/names、checked PO count | 以 fanin cone gate count 找最大的 PO bit |
| `shared_fanin` | `<net_a> <net_b>` | two sources、shared gate count/names、gate type counts | 兩個 fanin cones 共有的 combinational gates |

範例：

```text
cone_query net_fanout n2
cone_query net_fanin n16
cone_query gate_fanin g0 with_paths
cone_query largest_output
cone_query shared_fanin n16 n17
```

所有 cone modes 都會回傳 `Gate type counts`。`largest_output` 會回傳被選中的 output、cone gate/net count，以及實際掃描的 primary output bit 數量。

`largest_output` 的「最大」是 gate count，不是 logic depth；最深 output 應使用 `depth_query deepest_output`。

---

## 10. path_query

格式：

```text
path_query <mode> <start_endpoint> <end_endpoint> [options]
path_query direct_pi_po [-max_print count]
path_query mandatory_nodes <start_endpoint> <end_endpoint>
path_query is_separator <start_endpoint> <end_endpoint> <candidate_net>
path_query pi_po_cut <candidate_net>
```

語意：在 combinational graph 中查詢 start endpoint 到 end endpoint 的 path。start/end 可代表單點或整組 PI/PO/DFF endpoints。

| Mode | 主要輸出 | 語意 |
|---|---|---|
| `exists` | `Yes/No` | 是否至少存在一條合法 path |
| `find_any` | representative path 的 depth、nets、gates | 找任意一條 path，不保證最短或最長 |
| `enumerate` | total paths、完整性欄位、optional displayed paths/file | 列舉或計數所有 paths |
| `min_depth` | minimum-depth path | gate 數最少的 path |
| `max_depth` | maximum-depth path | gate 數最多的 path |
| `every_through` | `Yes/No` | 每一條 path 是否都經過 `-req` nodes |
| `every_avoids` | `Yes/No` | 每一條 path 是否都避開 `-avoid` nodes |
| `direct_pi_po` | count、depth-0 connection witnesses | 找出 PI 與 PO 共用同一 net 的直接連線；不需 start/end |
| `mandatory_nodes` | path status、mandatory net count/names | 求 source/target 間所有 directed paths 必經的 internal nets |
| `is_separator` | candidate、path status、yes/no | 判斷 candidate 是否為指定 source/target separator |
| `pi_po_cut` | candidate、checked endpoints、witness、yes/no | 判斷 candidate 是否切斷至少一組 directed PI-to-PO connectivity |

Endpoint：

```text
net:<name>
pi:<port>
po:<port>
dff_q:<ff>
dff_d:<ff>
all_pi
pi:*
all_po
po:*
all_dff_q
dff_q:*
all_dff_d
dff_d:*
dff_clk:<ff>[:pin]
dff_reset:<ff>[:pin]
gate_out:<gate>
gate_in:<gate>:<index_or_pin>
```

沒有 prefix 的 endpoint 會當成 specific net。`all_pi`/`pi:*`、`all_po`/`po:*`、`all_dff_q`/`dff_q:*`、`all_dff_d`/`dff_d:*` 分別代表所有對應 endpoints。

| Option | Type/Default | 語意 |
|---|---|---|
| `-req <node...>` | node list | path 必須經過的 gate/net nodes |
| `-avoid <node...>` | node list | path 不可經過的 gate/net nodes |
| `-out <file>` | path token | 將完整保留的 paths 寫入檔案 |
| `-max_print <count>` | non-negative integer，default 20 | terminal 最多顯示幾條 |
| `-max_paths <count>` | non-negative integer，default 100000 | enumeration 上限 |
| `-time_limit <seconds>` | number，default 55 | enumeration 時間上限 |
| `-count_only` | flag | 只計數，不在 memory 保存 literal paths |

Node：

```text
gate:<name>
net:<name>
```

範例：

```text
path_query exists net:n2 net:n12
path_query find_any pi:n2 po:n12
path_query every_through pi:n2 po:n12 -req gate:g0
path_query exists net:n2 net:n30 -avoid net:n4552
path_query enumerate pi:n2 po:n12 -out paths.txt -max_print 20
path_query enumerate all_pi all_po -count_only -max_paths 100000 -time_limit 55
path_query direct_pi_po
path_query enumerate all_dff_q all_dff_d -count_only
path_query max_depth all_dff_q all_dff_d
path_query mandatory_nodes net:n2 net:n14
path_query is_separator net:n2 net:n14 n8
path_query pi_po_cut n55104
```

`-count_only` 只統計 paths，不把每一條 path 保存在 memory；若同時需要 literal path listing，不要使用此選項。

enumerate result 會明確輸出：

```text
Total paths
Complete enumeration
Count only
Timed out
Path limit reached
Stop reason
Wrote paths to file / Output file
```

只要 `Complete enumeration: no`，envelope 就會回 `partial` 或 `timeout`，不可把已列舉數量當成全部 paths。

合法 endpoints 但無 path 會回 `status: ok` 與 `No`；endpoint 或 required/avoided node 無法解析時則回 `status: error`，並列出 `Unresolved startpoints/endpoints/required nodes/avoided nodes`。`combinationalOnly=false` 在 API 層會標記為 unsupported。

---

## 11. Register path（由 path_query 統一提供）

格式：

```text
path_query <mode> all_dff_q all_dff_d [options]
```

語意：以 `all_dff_q` / `all_dff_d` endpoint preset 分析 register boundary 間的 combinational logic。

| Mode | 主要輸出 | 語意 |
|---|---|---|
| `exists` | start/end DFF counts、`Yes/No` | 是否存在 register-to-register path |
| `find_any` | representative DFF pair 與 path | 任意一條 register path |
| `enumerate` | total paths、完整性欄位、paths/file | 列舉或計數 register paths |
| `min_depth` | representative DFF pair、minimum path | 最小 combinational depth |
| `max_depth` | representative DFF pair、maximum path | 最大 combinational depth |

Options：

```text
-from <dff...>
-to <dff...>
-req <node...>
-avoid <node...>
-out <file>
-max_print <count>
-max_paths <count>
-time_limit <seconds>
-count_only
```

`-from` 與 `-to` 接 DFF instance name list；`-req`、`-avoid`、enumeration safety options 的型態與語意和 `path_query` 相同。

沒有指定 `-from` / `-to` 時，使用所有 DFF.Q 到所有 DFF.D。

範例：

```text
path_query enumerate all_dff_q all_dff_d -out register_paths.txt -max_print 20
path_query enumerate all_dff_q all_dff_d -count_only -max_paths 100000 -time_limit 55
path_query max_depth all_dff_q all_dff_d
path_query exists dff_q:ff0 dff_d:ff1
```

enumerate 同樣必須先檢查 `Complete enumeration`、`Timed out` 與 `Path limit reached`；partial count 不能當成全部 register paths。

舊 `reg_path_query` 與 `runRegisterPathQuery()` 保留供既有程式相容；新的 LLM/tool schema 不再暴露獨立 register path command。

---

## 12. depth_query

格式：

```text
depth_query <mode> [args]
```

語意：logic depth 是 combinational path 上的 gate 數；PI、DFF.Q 與 constant leaf 不增加 combinational gate depth。

| Mode | Input | 主要輸出 | 語意 |
|---|---|---|---|
| `net` | `<net>` string，必填 | report count、endpoint depth、worst endpoint/path | specific net depth |
| `all_po` | 無 | 每個 PO bit 的 depth、worst PO/path | all primary output depths |
| `all_dff_d` | 無 | 每個 DFF.D 的 depth、worst DFF/path | all sequential endpoints |
| `global_critical` | 無 | worst endpoint、maximum depth、critical path nets/gates | PO 與 DFF.D 中的 global critical path |
| `exceeding` | `<threshold>` integer | matching endpoint count/names/depths、worst | depth 大於 threshold 的 PO + DFF.D |
| `po_exceeding` | `<threshold>` integer | matching PO count/names/depths、worst | 只統計 depth 大於 threshold 的 primary output bits |
| `gate_on_critical` | `<gate>` string | gate、gate ID、`Gate on critical path: yes/no` | 是否位於任一 global maximum-depth path |
| `deepest_output` | 無 | selected PO、depth、critical path | fanin logic depth 最大的 PO bit |

範例：

```text
depth_query net n16
depth_query all_dff_d
depth_query global_critical
depth_query exceeding 4
depth_query po_exceeding 4
depth_query gate_on_critical g0
depth_query deepest_output
```

`gate_on_critical` 會固定輸出 `Gate on critical path: yes/no`，false 不會以缺少欄位表示。

注意：`exceeding` 同時包含 PO 與 DFF.D；回答「多少 primary outputs depth > N」必須使用 `po_exceeding`。批次 PO/DFF.D depth 共用一次 net-level 計算，不會對每個 endpoint 重算整張 graph。

同一最大深度可能存在多條 path；目前 report 回傳一條代表性 critical path。題目若要求列出所有同深度 paths，應改用受限制的 path enumeration，而不能把代表性 path 當成唯一解。

---

## 13. func_query

格式：

```text
func_query <mode> [args]
```

語意：回答 internal net 的 Boolean function，不等同於只看 gate type 或結構連線。SAT 類 mode 會把 PI 與 DFF.Q 視為可控制 leaves；目前不納入 DFF initial state/time-frame behavior。

| Mode | Input | 主要輸出 | 語意 |
|---|---|---|---|
| `equivalence` | `<net_a> <net_b>` | status、solver fields、`equivalent` | 兩個 internal signals 是否同 Boolean function |
| `can_be_value` | `<net> <0\|1>` | solver fields、`answer` | 是否存在 leaf assignment 使 net 達指定值 |
| `constant` | `<net> <0\|1>` | solver fields、`is constant`/status | 是否恆等於指定值 |
| `always_zero` | `<net>` | solver fields、answer/status | 是否恆為 0 |
| `always_one` | `<net>` | solver fields、answer/status | 是否恆為 1 |
| `truth_status` | `<net>` | status、can be 0/1、is constant | ALWAYS_ZERO、ALWAYS_ONE 或 NON_CONSTANT |
| `depends_on` | `<target_net> <input_net>` | structural-support flag、solver fields、`depends on input` | exact SAT/cofactor dependence；input 必須為 PI 或 DFF.Q pseudo-PI |
| `functional_dependence` | `<target_net> <input_net>` | 同 `depends_on` | alias |
| `symmetry` | `<target_net_or_bus> <input_a> <input_b>` | symmetric、support flags、solver fields、非對稱反例 | exact swapped-cofactor SAT；inputs 必須是 scalar PI 或 DFF.Q pseudo-PI |
| `boolean_expression` | `<net>` | expression、length、support PI | 完整展開 Boolean expression |
| `expression` | `<net>` | 同 `boolean_expression` | alias |
| `simplified_expression` | `<net> <max_depth>` | expression、length、depth-limited flag/max depth、support PI | 達 max depth 後以 internal symbol 截止展開 |
| `support_pi` | `<net>` | support primary inputs | fanin cone 的 PI / DFF.Q pseudo-PI leaves |
| `primary_inputs_of_net` | `<net>` | 同 `support_pi` | alias |

範例：

```text
func_query equivalence n1287 n2404
func_query always_zero n16
func_query truth_status n16
func_query depends_on n8 n1
func_query symmetry n11 n3 n9[0]
func_query boolean_expression n16
func_query simplified_expression n16 10
func_query support_pi n16
```

expression result 會回傳 expression、字串長度、是否套用 depth limit、使用的 max depth 與 support PI names。

SAT 類 query 會輸出 solver ran/status/timeout/UNKNOWN/unsupported。expression/support query 不會輸出無意義的 SAT yes/no 欄位。

`support_pi` 只是 structural candidate set；題目若問特定 output 是否「functionally depend on」特定 input，必須使用 `depends_on`。

若 response 為 `timeout`、`partial` 或 `unsupported`，不可把主要 bool 當成 `false`。

只有 solver status 已得到確定 SAT/UNSAT 且 `complete: true` 時，才能把 equivalence、constant 或 reachability bool 當成最終答案。`NotChecked`、UNKNOWN、timeout 和 unsupported 都是未知，不是 false。

---

## 14. func_search

格式：

```text
func_search nand_pair <target_net>
    [--all] [--max-results N]
    [--patterns 1..4096] [--time-limit seconds]
    [--allow-same] [--include-boundary-signals]
```

語意：從候選 signals 中搜尋 `(a,b)`，並以 SAT 證明 `NAND(a,b)` 是否與 target function 等價。預設 `FindAny`；`--all` 才要求完整列舉。

| 輸出 | 判讀 |
|---|---|
| `found: true`, `complete: true` | 至少一組 match 已由 UNSAT miter 證明 |
| `found: false`, `complete: true` | 完整搜尋後確定沒有 match |
| `status: partial` | result limit 或 solver unknown；matches 不是完整集合 |
| `status: timeout/unsupported` | 不得解讀成 no-match |

每筆 match 都包含 `net_a`、`net_b`、`proven_equivalent`、`proof_method` 與 `solver_status`。simulation 只負責篩選，不能單獨構成 proof。

範例：

```text
func_search nand_pair n25
func_search nand_pair n25 --all --max-results 256 --time-limit 30
```

---

## 15. Directed separator / mandatory nodes（由 path_query 統一提供）

格式：

```text
path_query pi_po_cut <internal_net>
path_query mandatory_nodes <source_net> <target_net>
path_query is_separator <source_net> <target_net> <candidate_net>
```

| Mode | Input | 主要輸出 | 語意 |
|---|---|---|---|
| `pi_po_cut` | internal net | path/cut bool、checked PI/PO bits、witness PI/PO | candidate 是否切斷至少一組 directed PI-to-PO pair |
| `mandatory_nodes` | source net、target net | path bool、mandatory count/names | 列出位於所有 directed source-to-target paths 的 internal nets |
| `is_separator` | source net、target net、candidate | path bool、separator bool | 判斷指定 candidate 是否為 source-to-target mandatory net |

DFF 是 graph boundary；result 是 net-level directed dominator 語意。`NO_PATH`、connected-but-empty、invalid input 與 combinational cycle 會使用不同 status。

實作仍重用 `GraphQuery` dominator engine。舊 `graph_query` 保留供既有腳本相容，但不屬於公開 help/schema。

---

## 16. sequential_query

格式：

```text
sequential_query enable_hold <all|dff_name>
    [--summary-only]
    [--confirmed-only]
    [--include-no-pattern]
    [--offset N] [--limit N]
    [--verify-sat]
```

`enable_hold` 分析 DFF D-input logic 中的 canonical feedback-MUX enable/hold pattern。預設包含 AND-only candidate，但 candidate 會標成 `semantics_pending:true`，不會計入 `matched_dff_count`。

| 輸入 | 語意 |
|---|---|
| `all` | 分析所有 active DFF |
| `<dff_name>` | 只分析指定 DFF instance；不存在時回 `DFF_NOT_FOUND` |
| `--summary-only` | 只輸出完整 aggregate count，不輸出 DFF records |
| `--confirmed-only` | 不建立 AND-only candidate，只保留 confirmed canonical pattern |
| `--include-no-pattern` | all-DFF detail 也輸出沒有 pattern 的 DFF |
| `--offset N` | detail records 從第 N 筆開始，N 必須 >= 0 |
| `--limit N` | 本頁最多輸出 N 筆，N 必須 > 0；預設 50 |
| `--verify-sat` | 對指定 DFF 的 canonical pattern 補 conditional SAT proof；不接受 `all` |

Aggregate 主要欄位：

```text
report_ok / report_status / report_message / exists
total_dff_count / analyzed_dff_count
matched_dff_count / candidate_dff_count
available_dff_record_count / reported_dff_count
omitted_no_pattern_count
record_offset / record_limit / records_truncated / next_record_offset
```

每筆 DFF record 會輸出 D/Q net、status、matched、Q feedback 與 patterns。每個 pattern 包含 kind、detection method、enable/data/feedback、active level、confirmed/semantics-pending、solver 狀態與 evidence gates。

### 15.1 Summary 與分頁

數量題固定使用：

```text
sequential_query enable_hold all --summary-only
```

這會完整分析所有 DFF，但不把數千筆 records 放入 LLM context。若 prompt 要列出結構，使用預設 50 筆分頁：

```text
sequential_query enable_hold all
sequential_query enable_hold all --offset 50 --limit 50
```

尚有下一頁時 response 為 `status:partial`、`complete:false`，並回傳 `records_truncated:true` 與 `next_record_offset`。Aggregate counts 仍是完整設計結果，不是本頁 count。

### 15.2 Result Semantics

| 情況 | Envelope | 解讀 |
|---|---|---|
| summary 完成 | `status:ok`、`complete:true` | aggregate 可直接回答 |
| detail 尚有下一頁 | `status:partial`、`complete:false` | 本頁有效，但不可宣稱已列完 |
| 指定 DFF 無 pattern | `status:ok`、`complete:true`、`matched:false` | 確定沒有支援的 pattern，不是 error |
| AND-only candidate | `status:ok`、`semantics_pending:true` | 不可算入 confirmed enable/hold count |
| SAT timeout/unknown | `timeout` 或 `partial` | 不可宣稱 SAT 已證明 |
| DFF/option 無效 | `status:error` | request 未執行 |

test40 的兩個 prompt 應分別路由：

```text
Report D-input structures -> paged detail
How many flip-flops...     -> --summary-only，讀 matched_dff_count
```

---

## 17. edit_apply

所有對外 edit 都經過 public `runEditApply()`，預設要求 equivalence certificate 並在 validation 失敗時 rollback。CLI 不提供關閉 rollback 或直接呼叫低階 rewiring 的選項。

### 17.1 Rename

```text
edit_apply rename_gate <old_name> <new_name>
edit_apply rename_net <old_name> <new_name>
```

| Mode | Input | 主要輸出 | 語意 |
|---|---|---|---|
| `rename_gate` | old/new gate names | changed names、zero structural diff、`StructuralIdentity` | 只改 instance name |
| `rename_net` | old/new net names | changed names、zero structural diff、`StructuralIdentity` | 更新 signal name 與相關 port/reference |

舊名稱必須存在，新名稱不可和現有 active object 衝突。Rename 不改 Boolean function，但仍會增加 revision，因為輸出的 netlist 文字已改變。

### 17.2 Cleanup And Simplification

```text
edit_apply cleanup_buffers
edit_apply collapse_double_inverter
edit_apply local_simplification_fixpoint
edit_apply safe_cleanup_fixpoint
edit_apply trim_dead_logic
edit_apply remove_dangling_logic
edit_apply remove_unused_nets
edit_apply remove_net_if_unused <net_name>
edit_apply merge_structurally_equivalent_gates
edit_apply simplify_constants [gate_type|all] [0|1|any] [--inputs N]
edit_apply simplify_same_input
```

| Mode | Input | 語意 | 主要 report 欄位 |
|---|---|---|---|
| `cleanup_buffers` | 無 | 移除可安全 bypass 的 BUF | diff、depth change、LocalRewriteRule |
| `collapse_double_inverter` | 無 | 套用 `NOT(NOT(x)) = x` | diff、depth change、LocalRewriteRule |
| `local_simplification_fixpoint` | 無 | 反覆執行局部 Boolean simplification 到穩定 | diff、depth change、LocalRewriteRule |
| `safe_cleanup_fixpoint` | 無 | 組合安全 simplification、dead/dangling/unused cleanup 到穩定 | diff、depth change、LocalRewriteRule |
| `trim_dead_logic` | 無 | 移除不影響 observable endpoints 的 dead logic | removed gate/net delta、StructuralIdentity |
| `remove_dangling_logic` | 無 | 移除無有效 load 的 dangling logic | removed gate/net delta、StructuralIdentity |
| `remove_unused_nets` | 無 | 掃描並移除所有無 loads 且非 PI/PO/constant 的 nets | changed net names、StructuralIdentity |
| `remove_net_if_unused` | `<net_name>` | 只移除指定且符合 unused 條件的 net | changed IDs/names、StructuralIdentity |
| `merge_structurally_equivalent_gates` | 無 | 合併相同 gate type 與相同 inputs 的 structural duplicates | active gate delta、StructuralIdentity |
| `simplify_constants` | 可選 gate type、constant value、input count | 只對符合 filter 的候選套用 constant-input identities；省略參數時為全域單輪 | constant simplification、gate type delta、LocalRewriteRule |
| `simplify_same_input` | 無 | 套用相同 input 的 Boolean identities | gate type delta、LocalRewriteRule |

這些 pass 沒找到修改機會時回 `no_change`，仍可從 before/after/diff 確認確實沒有變化。不能用 cleanup command 代替 arbitrary SAT functional redundancy removal。

### 17.3 Buffer Insertion

```text
edit_apply insert_buffers_for_fanout <max_fanout>
edit_apply insert_buffers_for_net <net_name> <max_fanout>
edit_apply insert_buffers_for_dff_control <max_fanout> -clock|-reset
edit_apply insert_buffers_on_each_load <net_name>
edit_apply insert_buffer_at_driver <net_name>
edit_apply insert_buffer_before_gate <net_name> <target_gate_name>
edit_apply insert_buffers_by_gate_type <gate_type> [-inputs] [-outputs] [-both]
```

`insert_buffers_for_dff_control` 至少要指定 `-clock` 或 `-reset`；兩者可同時使用。`insert_buffers_by_gate_type` 未指定方向時預設同時處理 inputs 與 outputs。

| Mode | Input | 語意 | 主要 report 欄位 |
|---|---|---|---|
| `insert_buffers_for_fanout` | max fanout，integer >= 2 | 對全設計超標 nets 建 buffer tree | fanout change、BUF delta、changed names |
| `insert_buffers_for_net` | net name、max fanout >= 2 | 只處理指定 net 的 loads | fanout change、changed names |
| `insert_buffers_for_dff_control` | max fanout、clock/reset flags | 處理 DFF clock/reset/set control loads | BUF delta、changed names |
| `insert_buffers_on_each_load` | net name | 每個 load 前各插入專用 BUF | BUF/net delta、changed names |
| `insert_buffer_at_driver` | net name | 在 net driver 端插入一層 BUF | BUF/net delta、changed names |
| `insert_buffer_before_gate` | net name、target gate name | 只在指定 load gate 前插入 BUF | BUF/net delta、changed names |
| `insert_buffers_by_gate_type` | gate type、input/output direction | 在指定 type gates 的選定邊界插入 BUF | BUF/net delta、changed names |

Buffer insertion 以 `BUF(x)=x` 取得 `LocalRewriteRule` certificate。`fanout_change` 是全設計 before/after max fanout 摘要；執行 specific-net command 後若題目要求確認該 net 的確切 final fanout，應再呼叫 `structure_query fanout_load <net>`。

### 17.4 Technology Mapping

```text
edit_apply convert_basis <scope> [scope_name] \
    -allow <gate_type...> [-ban <gate_type...>] [--validate_equivalence]

edit_apply replace_type <scope> [scope_name] <target_type> \
    -allow <gate_type...> [--validate_equivalence]
```

Scope：

```text
whole
net_fanin <net>
net_fanout <net>
gate_fanin <gate>
gate_fanout <gate>
```

| Mode | 必要輸入 | 可選輸入 | 語意 |
|---|---|---|---|
| `convert_basis` | scope、`-allow` and/or `-ban` gate types | scope name、`--verbose` | 將 scope 轉成符合 allowed/banned basis |
| `replace_type` | scope、target gate type、`-allow` replacement basis | scope name、`--verbose` | 只替換 scope 內指定 target type |

非 `whole` scope 必須緊接 scope target name。`replace_type` 在 scope 內沒有 target type 時為成功的 `no_change`，不是 error。

主要輸出是 `mapping_delta.removed_count_by_type`、`added_count_by_type`、`final_gate_count_by_type` 與 validation certificate。Cone scope 的 basis constraint 只檢查該 scope，不會要求 cone 外 gate 也符合。

範例：

```text
edit_apply convert_basis whole -allow AND NOT
edit_apply convert_basis net_fanin n10 -allow NOR NOT
edit_apply replace_type whole XOR -allow NAND
edit_apply replace_type net_fanin n15 XOR -allow AND OR NOT
```

### 17.5 Edit Report

每次 parser 接受的 edit 執行前，session 會保存 `lastEditBaseline`；執行後保存完整 `lastEditReport`。只有 `report.success && report.changed` 時，`design_revision` 才會增加。

每個 edit response 會完整輸出：

```text
report_success / report_changed / rolled_back
operation_kind / operation_name / report_message
before_stats / after_stats / diff
validation 與 equivalence_method
optional depth_change / fanout_change / mapping_delta
optional constant_simplification
changed_gate_ids / changed_net_ids
changed_gate_names / changed_net_names / warnings
```

| Report 區塊 | 用途 |
|---|---|
| `before_stats` / `after_stats` | 修改前後完整 gate/net/PI/PO/DFF/type counts |
| `diff` | 所有 count delta；負數表示移除，正數表示新增 |
| `validation` | structure、Problem A constraints、functional certificate |
| `depth_change` | 修改前後 global worst depth、是否改善 |
| `fanout_change` | 修改前後 global max fanout、target、violations |
| `mapping_delta` | mapping 特有的 removed/added/final type counts |
| `constant_simplification` | filter、candidate/simplified/skipped 清單與指定 gate type 的 eliminated count |
| changed IDs/names | 實際受影響物件；某些 whole pass 目前可能只提供 aggregate diff |
| `warnings` | 成功但仍需注意的限制；不可忽略 |

Problem A structural validation 同時輸出 baseline/after 的絕對狀態與 `problem_a_constraints_regressed`。若 testcase 原本就有 floating/undriven PO，無關 edit 不會因此失敗；只有此次 edit 新增 `new_problem_a_constraint_violations` 才會 rollback。

因此後續問題不必重新掃描 design，例如「剛才新增多少 BUF」、「移除多少 OR」或「fanout 是否達標」，可直接讀取 `diff`、`mapping_delta` 或 `fanout_change`。

### 17.6 Previous Edit Follow-Up

```text
report_query last_edit
```

此命令只回傳 cached `lastEditReport`，不重跑 edit，也不重新計算 before/after。成功 `read` 新 design 後 cache 會清空；尚無 edit report 時回 `NO_LAST_EDIT_REPORT`。

範例流程：

```text
read design.v
structure_query const_input_gates OR 1
edit_apply simplify_constants OR 1
report_query last_edit
```

若追問「剛才多少 OR 被 constant-1 propagation 消除」，優先讀 `constant_simplification.eliminated_target_gate_count`；也可用 `diff.gate_type_count_delta[OR]` 的負值交叉確認。`simplified_count` 包含只移除 neutral constant input、但仍保留原 gate type 的 rewrite，因此不能直接當 eliminated count。

成功的 public edit 必須回傳 `StructuralIdentity` 或 `LocalRewriteRule` certificate；若 requested certificate 缺失，envelope 會是 `partial` 且 `complete: false`。低階 arbitrary rewiring 不會放進 tools CLI。

---

## 18. equiv_query

格式：

```text
equiv_query original [time_budget_seconds]
equiv_query previous_edit [time_budget_seconds]
```

語意：使用 batched combinational SAT miter 比較 current design 與 session baseline 的每個同名 PO bit 與 DFF.D next-state function。PI 依 net name 共享 SAT variable，DFF.Q 依 DFF instance name 共享；DFF initial state 與跨 cycle behavior 不在目前模型內。

| Mode | Baseline | 前置條件 |
|---|---|---|
| `original` | 最近一次成功 `read` 建立的原始 snapshot | design 已載入 |
| `previous_edit` | 上一筆 parser 接受並執行的 `edit_apply` 之前 snapshot | 至少執行過一次 edit |

`time_budget_seconds` 必須是大於 0 的有限數字，省略時預設 240 秒。所有 PO 與 DFF.D 共用同一個總 budget；先執行 global endpoint miter，只有發現 mismatch 時才逐 endpoint 定位，每次 solve 最多使用 30 秒。

主要輸出：

```text
report_ok / equivalent / method / report_message
time_budget_seconds / time_budget_exceeded
compared_output_count / skipped_output_count
matched_output_names / mismatched_output_names / skipped_output_names
compared_dff_d_count / skipped_dff_d_count
matched_dff_d_names / mismatched_dff_d_names / skipped_dff_d_names
missing_input_names / extra_input_names
missing_output_names / extra_output_names
missing_dff_names / extra_dff_names
unsupported_reasons / warnings
```

### 18.1 Result Semantics

| Report 情況 | Envelope | 可得結論 |
|---|---|---|
| 全部 PO 與 DFF.D matched | `status:ok`、`complete:true`、`equivalent:true` | 已證明 combinational-boundary equivalent |
| 至少一個 PO 或 DFF.D mismatch，且比較完整 | `status:ok`、`complete:true`、`equivalent:false` | 已證明不等價 |
| PI/PO/DFF names 不一致 | `status:ok`、`complete:true`、`report_ok:false`、`equivalent:false` | 嚴格介面不等價；SAT 不會繼續比較全部 endpoints |
| total budget exceeded | `status:timeout`、`complete:false` | 未能完成等價證明 |
| unsupported logic 或 endpoint SAT UNKNOWN | `status:unsupported`、`complete:false` | 未知，不可回答 equivalent=false |
| 尚無 previous baseline | `status:error` + `NO_PREVIOUS_EDIT_BASELINE` | request 無法執行 |

`report_ok` 表示 SAT comparison 流程是否完整執行；它和最終 `equivalent` 不同。Interface mismatch 雖使 `report_ok:false`，但已足以確定兩個嚴格同名介面的 designs 不等價，因此外層 response 仍是 completed answer。

### 18.2 Examples

```text
read design.v
equiv_query original 30
edit_apply safe_cleanup_fixpoint
equiv_query previous_edit 30
equiv_query original 60
```

等價結果的主要輸出：

```text
status: ok
complete: true
data:
  report_ok: true
  equivalent: true
  method: WholeDesignSat
  compared_output_count: 2
  skipped_output_count: 0
  compared_dff_d_count: 4
  skipped_dff_d_count: 0
  mismatched_output_names (0):
  mismatched_dff_d_names (0):
  unsupported_reasons (0):
```

若 report 含 DFF warning，結論只代表以 DFF.Q 為 boundary leaves 的 combinational equivalence，不代表 initial-state 或 sequential equivalence。

---

## 19. Input/Output Examples

以下只保留與語意判讀有關的欄位；實際 response 仍包含完整 envelope。

### 19.1 Explicit False

輸入：

```text
structure_query is_connected g0 z
```

主要輸出：

```text
status: ok
complete: true
data:
  gate: g0
  net: z
  count: 0
  connected: no
```

`connected: no` 是已完成查詢的 false；它和 `status:error` 或 `complete:false` 不同。

### 19.2 Partial Enumeration

輸入：

```text
path_query enumerate all_pi all_po -count_only -max_paths 2
```

主要輸出：

```text
status: partial
complete: false
data:
Total paths: 2
Complete enumeration: no
Count only: yes
Timed out: no
Path limit reached: yes
Stop reason: Path enumeration stopped after reaching the path limit.
```

此處的 2 是停止前找到的數量，不能回答「總共有 2 條 paths」。

### 19.3 Edit And Follow-Up

輸入：

```text
edit_apply simplify_constants OR 1
report_query last_edit
```

主要輸出結構：

```text
status: ok
complete: true
data:
  report_success: true
  report_changed: true
  operation_name: edit_apply:simplify_constants
  constant_simplification:
    target_gate_type: OR
    target_const_value: 1
    candidate_count: 2
    simplified_count: 2
    skipped_count: 0
    eliminated_target_gate_count: 2
  diff:
    active_gate_count_delta: -3
    gate_type_count_delta:
      OR : -2
  validation:
    equivalence_checked: true
    functionally_equivalent: true
    equivalence_method: LocalRewriteRule
```

後續題目問「剛才移除多少 OR」時，答案取 `-gate_type_count_delta[OR] = 2`。數值僅示範欄位讀法，實際結果由 testcase report 決定。

### 19.4 Error

```text
status: error
complete: false
message: NO_DESIGN_LOADED: use read before running this command.
```

error response 不可從缺少的 `data` 欄位推導任何 design 結論。

---

## 20. Prompt Routing

| Prompt 語意 | Command |
|---|---|
| count/list/type/port width/structural issue | `structure_query` |
| direct driver/load/fanout/pin connection | `structure_query` |
| reachable/transitive fanin/fanout/cone | `cone_query` |
| path/through/avoid/enumeration | `path_query` |
| register-to-register path | `path_query` + `all_dff_q` / `all_dff_d` |
| depth/critical path/timing endpoint | `depth_query` |
| internal equivalence/constant/truth status | `func_query` |
| unknown internal pair satisfying `NAND(a,b)==target` | `func_search` |
| directed PI-to-PO cut / source-target mandatory/separator nets | `path_query` |
| DFF D-input enable/hold structure 與 confirmed count | `sequential_query` |
| rename/cleanup/simplification/buffer/mapping | `edit_apply` |
| previous edit delta/follow-up count | `report_query last_edit` |
| current vs original/previous edit whole-design equivalence | `equiv_query` |
| write current design | `write` |

常見區分：

```text
direct fanout != transitive fanout
structural connection != functional equivalence
constant input != constant function
largest cone by gate count != deepest cone by logic depth
local rewrite certificate != whole-design SAT proof
named-net Boolean proof != unknown functional candidate search
specified transformation != objective-driven optimization
structural duplicate merge != functional-equivalent merge
```

---

## 21. 尚未對外開放

目前尚不能從 CLI 直接使用：

```text
true depth optimization
```

以上會依 `TOOLS_CPP_IMPLEMENTATION_SPEC.md` 的後續批次逐步加入，每一批完成後再更新本 usage。
