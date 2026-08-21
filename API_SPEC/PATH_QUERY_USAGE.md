# Path Query 使用說明

這份文件只負責說明 `Netlist::PathQuery` 與公開 `path_query` command 的使用方式。

設計背景與內部 contract 請看：

```text
API_SPEC/PATH_QUERY_API.md
```

---

## 1. 基本概念與入口

C++ 最小流程：

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::Exists;
query.startpoints.emplace_back(
    Netlist::PathEndpointType::SpecificNet, "a");
query.endpoints.emplace_back(
    Netlist::PathEndpointType::SpecificNet, "y");

const Netlist::PathQueryResult result = netlist.runPathQuery(query);
```

公開 CLI：

```text
path_query <mode> <start_endpoint> <end_endpoint> [options]
path_query pi_po_cut <candidate_net>
path_query direct_pi_po
```

標準判讀順序：

```text
1. 先看 ok / unsupported。
2. 再看 mode-specific exists、status、depth 或 count。
3. Enumeration 必須檢查 completeEnumeration、timeout 與 stop reason。
4. 有輸出檔時讀 outputFilePath，不把 terminal 省略的 samples 當完整列表。
```

## 2. Mode / Command 總表

| Mode | 必要輸入 | 主要輸出 | 語意 |
|---|---|---|---|
| `exists` | start、end | `exists` | 至少一條合法 path |
| `find_any` | start、end | `path`, `depth` | 任一條合法 path |
| `enumerate` | start、end | `pathCount`, completion/file fields | 完整列舉或計數 |
| `min_depth` | start、end | `path`, `depth` | 最短合法 path |
| `max_depth` | start、end | `path`, `depth` | 最長合法 path |
| `every_through` | start、end、`-req` | `exists` | 每條既有 path 都通過 required nodes |
| `every_avoids` | start、end、`-avoid` | `exists` | 每條既有 path 都避開指定 nodes |
| `mandatory_nodes` | start、end | `pathExists`, mandatory names | 所有 path 共用的 internal nets |
| `is_separator` | start、end、candidate | `isSeparator` | candidate 是否切斷指定 connectivity |
| `pi_po_cut` | candidate | `isSeparator`, witness | 是否切斷至少一組原本相連的 PI/PO |
| `direct_pi_po` | 無 | `paths`, `pathCount` | 所有 depth-0 PI-to-PO wires |

## 3. 輸入欄位

### 3.1 Endpoint tokens

| CLI token | C++ endpoint | 意義 |
|---|---|---|
| `net:n1` 或 `n1` | `SpecificNet("n1")` | 指定 net |
| `pi:a` | `PrimaryInput("a")` | 指定 PI port |
| `po:y` | `PrimaryOutput("y")` | 指定 PO port |
| `all_pi` / `all_po` | 空名稱 PI/PO | 所有 PI/PO bits |
| `dff_q:ff1` / `dff_d:ff1` | `DffQ/DffD("ff1")` | 指定 register boundary |
| `all_dff_q` / `all_dff_d` | 空名稱 DffQ/DffD | 所有 register boundaries |
| `dff_clk:ff1[:pin]` | `DffClock` | clock pin net |
| `dff_reset:ff1[:pin]` | `DffReset` | reset/set pin net |
| `gate_out:g1` | `GateOutput("g1")` | gate output net |
| `gate_in:g1:0` | `GateInput("g1", index=0)` | positional gate input |
| `gate_in:ff1:D` | `GateInput("ff1", pinName="D")` | named input pin |

Bus bit 使用完整 net 名稱，例如 `net:n0[0]`。不要把 bus bit 寫成 `pi:n0[0]` 或
`po:n63[1]`，因為 `pi:`/`po:` 解析的是 port 名稱。

### 3.2 Constraint tokens

```text
-req gate:g1 net:n3
-avoid net:n8 gate:g4
```

bare constraint token 預設視為 net。多個 `-req` 必須全部經過；任一 `-avoid` 都不可經過。

### 3.3 Enumeration options

| Option | 語意 | 正常 LLM 使用政策 |
|---|---|---|
| `-count_only` | 只計數，不建立完整 path file | prompt 只問數量時使用 |

`outputFilePath`、`maxPrintedPaths`、`maxEnumeratedPaths` 與
`enumerationTimeLimitSeconds` 仍是 C++ facade 的內部執行欄位，可供 regression、printer 與
runner policy 使用；它們不是 LLM-facing command grammar。公開 CLI 會自動選擇 artifact
檔名、terminal 顯示量與 request deadline。

非 count-only 的 CLI `enumerate` 會自動 streaming 寫入唯一的
`<design>_path_query_<sequence>.txt`，並在 terminal 回傳檔案位置。核心 C++ facade 若沒有
設定 `outputFilePath`，相容 fallback 是 `path_enumeration_output.txt`。

`direct_pi_po` 的結果超過自動門檻時，也會改寫完整 list artifact；terminal 只保留總數、
artifact 完整性、格式與 `output_file`。小型結果仍直接完整列出。

完整檔案採 `LITERAL_PATH_V1`。每一條 path 都直接使用 current named netlist 的 net name、
gate instance name 與 gate type：

```text
Path 0: a -> g0(BUF) -> n0 -> g2(AND) -> n2 -> g7(BUF) -> y
```

artifact 不需要 dictionary、base36 或額外 decoder。正式判讀必須同時確認：

```text
Total paths == Expected paths == Written paths
Complete: yes
Timed out: no
```

若 footer 為 `Complete: no`，檔案只包含 timeout 前已完成的 records，不是完整答案。

## 4. 回傳欄位與狀態判讀

### 4.1 成功、false 與 invalid input

```text
ok=false
```

表示 endpoint/constraint 解析失敗或 request 無效；應讀 `message` 與 `unresolved*`。

```text
ok=true, exists=false
```

表示輸入合法且確定沒有 path，或 every-path 條件不成立。EveryPath modes 不採 vacuous
truth；原始 path 不存在時也回 false。

```text
unsupported=true
```

表示 request 合法，但目前設定不支援，例如 `combinationalOnly=false`。

### 4.2 Enumeration completeness

精確完整答案必須同時符合：

```text
ok == true
completeEnumeration == true
enumerationTimedOut == false
enumerationPathLimitReached == false
```

若 `completeEnumeration=false`，`pathCount` 只能視為 partial count，必須附上
`enumerationStopReason`。`SIZE_MAX` 目前也不可直接解讀成精確答案，因為 64-bit overflow
規格仍在等待官方回覆。

### 4.3 Mandatory / separator

- `pathExists=false`：原始 endpoints 沒有 path。
- mandatory list 空但 `pathExists=true`：有 path，但沒有共同 internal net。
- `isSeparator=true`：candidate 確實切斷原本存在的 directed connectivity。
- source 與 target 不計入 mandatory internal nodes。

## 5. 各 Mode 使用範例

### 5.1 Exists

```text
path_query exists pi:a po:y -avoid net:n8
```

讀取 `exists`；CLI data 為 `Yes` 或 `No`。

### 5.2 FindAny

```text
path_query find_any net:a net:y
```

讀取 `path.netIds`、`path.gateIds` 與 `depth`。

### 5.3 EnumerateAll

完整列表：

```text
path_query enumerate net:a net:y
```

工具自動寫檔；讀取 `pathCount`、completion fields 與 `outputFilePath`。

只問數量：

```text
path_query enumerate all_dff_q all_dff_d -count_only
```

讀取 `pathCount`，並確認完整性。

### 5.4 MinDepth

```text
path_query min_depth pi:a dff_d:ff1
```

讀取 `depth` 與代表 path。

### 5.5 MaxDepth

```text
path_query max_depth all_dff_q all_dff_d
```

讀取 `depth` 與 longest witness path。全域 design critical depth 改用 `depth_query`。

### 5.6 EveryPathThrough

```text
path_query every_through net:n2 net:n12 -req gate:g0
```

只有原始 path 存在且每條都通過 `g0` 時回 true。

### 5.7 EveryPathAvoids

```text
path_query every_avoids net:n2 net:n12 -avoid net:n10
```

只有原始 path 存在且每條都避開 `n10` 時回 true。

### 5.8 FindMandatoryNodes

```text
path_query mandatory_nodes net:n2 net:n14
```

讀取 `pathExists`、`status` 與 `mandatoryNetNames`。底層使用 dominator，不列舉 paths。

### 5.9 IsSeparator

```text
path_query is_separator net:n2 net:n14 n10
```

讀取 `pathExists` 與 `isSeparator`。

### 5.10 PI-to-PO Cut

```text
path_query pi_po_cut n55104
```

若 candidate 切斷至少一組原本相連的 PI/PO，回 `isSeparator=true` 並提供 witness pair。

### 5.11 Direct PI-to-PO

```text
path_query direct_pi_po
```

回傳全部 PI/PO 共用同一 net 的 depth-0 connections；每筆 path 沒有 gate，depth 為 0。

### 5.12 C++ Register-to-Register

```cpp
Netlist::PathQuery query;
query.mode = Netlist::PathQueryMode::MaxDepth;
query.startpoints.emplace_back(Netlist::PathEndpointType::DffQ, "");
query.endpoints.emplace_back(Netlist::PathEndpointType::DffD, "");

const auto result = netlist.runPathQuery(query);
```

`RegisterPathQuery` 只保留 legacy/internal wrapper；新串接直接使用上述 endpoints。

## 6. Prompt 對應

| Prompt 語意 | Mode / Command | 主要讀取 |
|---|---|---|
| Is there a path from A to B? | `exists` | `exists` |
| Find one path from A to B | `find_any` | `path`, `depth` |
| List every path from A to B | `enumerate` | file、`pathCount`、completion |
| How many paths from A to B? | `enumerate -count_only` | `pathCount`、completion |
| Shortest path/depth between A and B | `min_depth` | `depth`, `path` |
| Longest/critical path between named endpoints | `max_depth` | `depth`, `path` |
| All register-to-register paths | `enumerate all_dff_q all_dff_d` | file/count/completion |
| Number of register-to-register paths | 同上加 `-count_only` | `pathCount` |
| Does every path pass through X? | `every_through -req X` | `exists` |
| Does every path avoid X? | `every_avoids -avoid X` | `exists` |
| Articulation/mandatory nets between A and B | `mandatory_nodes` | `pathExists`, mandatory names |
| Is X a separator between A and B? | `is_separator` | `isSeparator` |
| Is X a PI-to-PO cut? | `pi_po_cut` | `isSeparator`, witness |
| Directly wired PI-to-PO signals | `direct_pi_po` | `paths`, `pathCount` |

## 7. 何時不要使用

| 問題 | 改用 |
|---|---|
| 從 n2 可到達的所有 gates/nets | `cone_query net_fanout n2` |
| n2 的完整 transitive fanin | `cone_query net_fanin n2` |
| 全設計 maximum logic depth | `depth_query global_critical` |
| 有多少 outputs depth > N | `depth_query po_exceeding N` |
| gate/net driver、load、fanout | `structure_query` |
| net 是否 constant/equivalent | `func_query` |
| DFF enable/hold pattern | `sequential_query` |

## 8. 實作與測試狀態

```text
Public types: include/core/PathTypes.h
Implementation: src/analysis/PathAnalysis.cpp
CLI: tools.cpp
```

所有 `PathQueryMode` 已完成 dispatch。官方 testcase 已驗證 existence、avoid、enumeration、
count-only、min/max depth、every-path、mandatory、separator/cut、direct PI-to-PO 與 all-DFF
endpoints。

已知限制：

- `combinationalOnly=false` 不支援。
- combinational graph contract 為 DAG。
- 64-bit count overflow 等待官方規格回覆。
- literal path artifact footer 與 report envelope 都必須標示 complete，才能視為完整答案。
