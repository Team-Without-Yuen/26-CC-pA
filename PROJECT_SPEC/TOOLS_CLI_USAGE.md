# Tools CLI Usage

這份文件說明目前 `tools.cpp` 產生的互動式 EDA query tool 如何使用。

目前 CLI 設計重點：

```text
- 不直接暴露大量低階 API command。
- 使用「分類式高階入口」：
  basic_query / conn_query / cone_query / path_query / depth_query / func_query
- 每個入口再用 mode 決定實際查詢種類。
- CLI command 主要作為 LLM dispatch、人工 debug、testcase prompt 驗證的介面。
```

## 1. Build

在專案根目錄執行：

```powershell
g++ -std=c++20 -I. -Ilib tools.cpp src/core/*.cpp src/io/*.cpp src/analysis/*.cpp src/optimization/*.cpp src/transformation/*.cpp -L./include/lib/win/ucrt64 -lcadical -o tools.exe
```

注意：

```text
- 目前 Windows 環境建議使用 include/lib/win/ucrt64/libcadical.a。
- mingw64 版本目前可能遇到 stat64i32 linker error。
```

## 2. Start Tool

```powershell
.\tools.exe
```

進入互動模式後會看到：

```text
eda>
```

輸入 `help` 可以看到目前支援的高階指令。

離開工具：

```text
quit
exit
```

## 3. I/O Commands

| Command | 用途 |
| --- | --- |
| `read <verilog_file>` | 讀入 restricted gate-level Verilog netlist |
| `write <verilog_file>` | 將目前 netlist 寫回 Verilog |

路徑如果有空白，請用引號包起來：

```text
read "mini test\mini_circuit.v"
write "mini test\out.v"
```

## 4. Unified Command Overview

目前主要只有六個高階查詢入口：

| Entry Command | 負責問題類型 | 對應 C++ 高階 API |
| --- | --- | --- |
| `basic_query` | 基本物件、ID、type、summary、structural issue | `runBasicQuery()` |
| `conn_query` | gate/net 直接連線關係 | `runDirectConnectivityQuery()` |
| `cone_query` | transitive fanin/fanout cone | `runConeQuery()` |
| `path_query` | startpoint-to-endpoint path analysis | `runPathQuery()` |
| `depth_query` | timing endpoint depth / critical path | `runDepthQuery()` |
| `func_query` | Boolean function / equivalence / constant status | `runFunctionQuery()` |

設計原則：

```text
LLM 不應優先呼叫 getGateCount、hasCombinationalPath、checkEquivalence 這類低階函式。
應先判斷 prompt 屬於哪一類問題，再轉成對應的 unified query command。
```

## 5. Basic Query

格式：

```text
basic_query <mode> [args]
```

| Mode | Args | 用途 |
| --- | --- | --- |
| `summary` | 無 | 回傳 gate/net/PI/PO/wire 數量與 gate type breakdown |
| `list_gates` | 無 | 列出所有 gate instance names |
| `list_nets` | 無 | 列出所有 net names |
| `list_pi` | 無 | 列出所有 primary input port names |
| `list_po` | 無 | 列出所有 primary output port names |
| `list_dffs` | 無 | 列出所有 DFF instance names |
| `list_comb` | 無 | 列出所有 combinational gates，不包含 DFF |
| `gate_info` | `<gate>` | 查單一 gate 的 type、ID、連線資訊 |
| `net_info` | `<net>` | 查單一 net 是否 PI/PO/constant/internal |
| `port_info` | `<port>` | 查 port width、bus/scalar、bit net 展開 |
| `count_by_type` | `[gate_type]` | 統計特定 gate type；不給 type 時列全部 |
| `gates_by_type` | `<gate_type>` | 列出指定 type 的 gate names |
| `const_input_gates` | `[gate_type] [0|1]` | 找 input 接 constant 的 gates |
| `structural_issues` | 無 | 回報 undriven/no-load/floating/unconnected |

範例：

```text
basic_query summary
basic_query gate_info g1
basic_query net_info n16
basic_query port_info data
basic_query count_by_type AND
basic_query gates_by_type DFF
basic_query const_input_gates AND 0
basic_query structural_issues
```

## 6. Direct Connectivity Query

格式：

```text
conn_query <mode> [args]
```

| Mode | Args | 用途 |
| --- | --- | --- |
| `net_driver` | `<net>` | 查 net 的直接 driver gate |
| `net_loads` | `<net>` | 查 net 直接 load 到哪些 gates |
| `fanout_load` | `<net>` | 依 Problem A QA 定義回報 net 的 pin-level fanout loads |
| `fanout_report` | `<net>` | `fanout_load` 的別名 |
| `global_fanout` | `[limit]` | 掃描全設計 fanout max；給 limit 時同時列 violations |
| `pi_fanout` | `[limit]` | 只掃 primary inputs，找最高 fanout PI |
| `fanout_violations` | `<limit>` | 列出 fanout 超過 limit 的 nets |
| `gate_inputs` | `<gate>` | 查 gate 的 input nets |
| `gate_output` | `<gate>` | 查 gate 的 output net |
| `gate_fanin` | `<gate>` | 查直接驅動 gate inputs 的上一層 gates |
| `gate_fanout` | `<gate>` | 查 gate output 直接 fanout 到哪些 gates |
| `is_connected` | `<gate> <net>` | 判斷 gate 和 net 是否直接相連 |

範例：

```text
conn_query net_driver n16
conn_query net_loads n2
conn_query fanout_load n2
conn_query fanout_report clk
conn_query global_fanout
conn_query global_fanout 16
conn_query pi_fanout
conn_query fanout_violations 16
conn_query gate_inputs g5
conn_query gate_output g5
conn_query gate_fanin g5
conn_query gate_fanout g5
conn_query is_connected g5 n2
```

注意：

```text
conn_query 只回答一層直接相連。
如果題目問 reachable、fanin cone、fanout cone，不應使用 conn_query，應改用 cone_query。
如果題目問 fanout load count，尤其包含 DFF CK/RN/SN 或 primary output load，應使用 conn_query fanout_load。
```

## 7. Cone Query

格式：

```text
cone_query <mode> <name> [with_paths]
```

| Mode | Args | 用途 |
| --- | --- | --- |
| `net_fanin` | `<net>` | 從 net 往上游追 transitive fanin cone |
| `net_fanout` | `<net>` | 從 net 往下游追 transitive fanout cone |
| `gate_fanin` | `<gate>` | 從 gate output 對應 net 往上游追 cone |
| `gate_fanout` | `<gate>` | 從 gate output 對應 net 往下游追 cone |

選項：

| Option | 用途 |
| --- | --- |
| `with_paths` | 額外回傳 cone 內 shortest/longest local path 資訊 |

範例：

```text
cone_query net_fanin n16
cone_query net_fanout n2
cone_query gate_fanin g10 with_paths
cone_query gate_fanout g10 with_paths
```

注意：

```text
- DFF 目前視為 sequential boundary。
- cone traversal 不穿越 DFF。
- cone_query 回答的是可達集合，不是指定 start/end 的路徑。
```

## 8. Path Query

格式：

```text
path_query <mode> <start_endpoint> <end_endpoint> [-req node...] [-avoid node...] [-out file] [-max_print n]
```

### 8.1 Modes

| Mode | 用途 |
| --- | --- |
| `exists` | 判斷是否至少存在一條合法路徑 |
| `find_any` | 回傳任意一條合法路徑 |
| `enumerate` | 列出所有合法路徑；預設會將完整結果寫入檔案 |
| `min_depth` | 找最短 logic depth path |
| `max_depth` | 找最長 logic depth path |
| `every_through` | 判斷所有路徑是否都經過指定節點 |
| `every_avoids` | 判斷所有路徑是否都避開指定節點 |

### 8.2 Endpoint Format

| Endpoint | 意義 |
| --- | --- |
| `net:<name>` | 指定 net |
| `<name>` | 沒有 prefix 時預設視為 `net:<name>` |
| `pi:<port>` | 指定 primary input port；可支援 bus port 展開 |
| `po:<port>` | 指定 primary output port；可支援 bus port 展開 |
| `dff_q:<ff>` | DFF Q/output pin |
| `dff_d:<ff>` | DFF D input pin |
| `dff_clk:<ff>` | DFF clock pin，預設 pin name 為 `CK` |
| `dff_clk:<ff>:<pin>` | 指定 DFF clock pin name |
| `dff_reset:<ff>` | DFF reset/set pin，預設嘗試 `RN` / `SN` |
| `dff_reset:<ff>:<pin>` | 指定 DFF reset/set pin name |
| `gate_out:<gate>` | gate output net |
| `gate_in:<gate>:<index>` | gate positional input net |
| `gate_in:<gate>:<pin>` | named input pin net |

### 8.3 Required / Avoided Node Format

| Node | 意義 |
| --- | --- |
| `net:<name>` | 指定必經/避開 net |
| `gate:<name>` | 指定必經/避開 gate |
| `<name>` | 沒有 prefix 時預設視為 net |

### 8.4 Examples

```text
path_query exists net:a net:y
path_query find_any pi:a po:y
path_query enumerate net:a net:y
path_query enumerate net:a net:y -out paths_output.txt -max_print 0
path_query min_depth net:a net:y
path_query max_depth net:a net:y
path_query exists net:a net:y -avoid gate:g3 net:n5
path_query exists net:a net:y -req gate:g2 -avoid net:n4
path_query every_through net:a net:y -req gate:g2
path_query every_avoids net:a net:y -avoid gate:g3
path_query max_depth pi:a dff_d:ff1
path_query exists dff_q:ff1 po:y
```

注意：

```text
- 目前 path query 預設 combinationalOnly=true。
- DFF 是 sequential boundary，路徑搜尋不會穿越 DFF。
- 如果要查 register-to-register path，應使用 dff_q:<ff> 作為起點、dff_d:<ff> 作為終點。
- `enumerate` 會把完整路徑列表寫到檔案；CLI 只印摘要與前 `-max_print` 條。
- `-out <file>` 可覆蓋輸出檔名；未指定時使用 `path_enumeration_output.txt`。
- `-out` 建議使用不含空白的路徑。
```

## 9. Depth Query

格式：

```text
depth_query <mode> [args]
```

| Mode | Args | 用途 |
| --- | --- | --- |
| `net` | `<net>` | 查指定 net 的最大 depth 與 critical path |
| `all_po` | 無 | 分析所有 primary output endpoint depth |
| `all_dff_d` | 無 | 分析所有 DFF D-pin endpoint depth |
| `global_critical` | 無 | 找全設計 PO/DFF.D 中最深 endpoint |
| `exceeding` | `<depth>` | 找 depth 大於門檻的 endpoints |

範例：

```text
depth_query net n16
depth_query all_po
depth_query all_dff_d
depth_query global_critical
depth_query exceeding 4
```

注意：

```text
depth_query 偏 timing endpoint analysis。
如果只是問 A 到 B 是否有路徑，應使用 path_query。
```

## 10. Function Query

格式：

```text
func_query <mode> [args]
```

| Mode | Args | 用途 |
| --- | --- | --- |
| `equivalence` | `<net_a> <net_b>` | 判斷兩個 net/bus 是否功能等價 |
| `can_be_value` | `<net> <0|1>` | 判斷 scalar net 是否存在輸入組合可為指定值 |
| `constant` | `<net> <0|1>` | 判斷 scalar net 是否恆等於指定值 |
| `always_zero` | `<net>` | 判斷 scalar net 是否永遠為 0 |
| `always_one` | `<net>` | 判斷 scalar net 是否永遠為 1 |
| `truth_status` | `<net>` | 回報 always zero / always one / non-constant |

範例：

```text
func_query equivalence n1 n2
func_query can_be_value n16 0
func_query constant n16 1
func_query always_zero n16
func_query always_one n16
func_query truth_status n16
```

注意：

```text
- function query 目前以 combinational fanin cone 建 SAT model。
- DFF.Q 視為 pseudo primary input。
- scalar constant status 請優先使用 func_query。
```

## 11. Prompt Dispatch 建議

LLM / parser 可以先用下面規則判斷要呼叫哪個入口：

| Prompt 類型 | 建議入口 |
| --- | --- |
| gate/net/PI/PO/DFF 數量、列出物件、查 type | `basic_query` |
| 某 net 的 driver/load、某 gate 的 input/output | `conn_query` |
| 某 net 依 QA 定義的 fanout load 數、DFF clock/reset loads、是否 drive PO | `conn_query fanout_load` |
| reachable、transitive fanin/fanout、cone size | `cone_query` |
| A 到 B 是否有路徑、找路徑、避開/必經節點 | `path_query` |
| critical path、depth、depth > threshold endpoints | `depth_query` |
| equivalence、always 0/1、constant status | `func_query` |

常見判斷範例：

```text
Determine all gates reachable from n2.
=> cone_query net_fanout n2

Report every gate connected to the output of g0.
=> conn_query gate_fanout g0

Is output n16 always 0 regardless of all inputs?
=> func_query always_zero n16

What is the maximum logic depth from any primary input to any DFF D-pin?
=> depth_query all_dff_d

Find a path from n1 to n8 avoiding g3.
=> path_query find_any net:n1 net:n8 -avoid gate:g3
```

## 12. Current Boundary

目前這份 tool 是 analysis/query CLI，不是完整 transformation CLI。

已可使用：

```text
- 讀寫 netlist
- basic/direct/cone/path/depth/function query
- 用統一 command 減少 LLM dispatch 錯誤
```

尚未在 tools CLI 中統一包裝：

```text
- cleanup / simplification execution
- optimization candidate report
- transformation result report
- rollback / validation / accept decision flow
```

後續若要補 transformation/optimization CLI，建議新增：

```text
edit_query / edit_apply / opt_query / opt_apply
```

不要把修改型 command 混進目前 read-only query command 裡。
