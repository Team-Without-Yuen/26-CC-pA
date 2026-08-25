# Path Query Tool

## 1. 責任

`path_query` 負責明確 startpoint 與 endpoint 之間的 combinational path、路徑限制、register-to-register path、mandatory nodes 與 separator/cut。官方 prompt 使用 `articulation points between A and B` 時，也由本工具的 `articulation_between` mode 處理。對外不使用 legacy `reg_path_query` 或 `graph_query`。

完整性規則：`all paths` 預設不設數量上限。只問數量時使用 `-count_only`；要求完整列表時
直接呼叫 `enumerate`，CLI 會自動以 streaming 寫入不覆寫既有結果的唯一檔名，正式答案只回
總數、完成狀態與檔案位置。輸出檔名、terminal 顯示量與執行預算由工具自行管理，LLM
不傳入這些內部控制參數。
詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

prompt 出現 `path from A to B`、`through`、`avoid`、`every path`、`shortest`、`longest between endpoints`、`register-to-register`、`mandatory`、`articulation points between A and B`、`separator` 或 `cut` 時使用本 tool。即使題目使用 `logic depth` 字樣，只要明確限定 start scope 與 end scope（例如 `from any primary input to any DFF D-pin`），仍使用本 tool 的 `max_depth`，不能改用包含其他 startpoints 的全域 depth query。

只問某物件可到達的完整範圍使用 `cone_query`；全設計 maximum depth/critical endpoint 使用 `depth_query`。

## 3. Command Grammar

```text
path_query <mode> <start_endpoint> <end_endpoint>
           [-req <node...>] [-avoid <node...>]

path_query enumerate <start_endpoint> <end_endpoint>
           [-req <node...>] [-avoid <node...>] [-count_only]

path_query is_separator <start_endpoint> <end_endpoint> <candidate_net>
path_query articulation_between <start_endpoint> <end_endpoint>
path_query pi_po_cut <candidate_net>
path_query direct_pi_po
```

`start_endpoint` 與 `end_endpoint` 是 required。只有 `enumerate` 接受 `-count_only`。
`-req`/`-avoid` 遇到下一個 option 或行尾結束。

### Endpoint 與 Node

endpoint tokens：

| Token | 意義 |
|---|---|
| `net:<n>` 或 bare net | 指定 net |
| `pi:<p>`, `po:<p>` | 指定 PI/PO |
| `all_pi`, `all_po` | 所有 PI/PO |
| `dff_q:<ff>`, `dff_d:<ff>` | 指定 DFF Q/D boundary |
| `all_dff_q`, `all_dff_d` | 所有 DFF Q/D boundaries |
| `dff_clk:<ff>[:pin]` | 指定 DFF clock pin |
| `dff_reset:<ff>[:pin]` | 指定 DFF reset/set pin |
| `gate_out:<g>` | gate output |
| `gate_in:<g>:<index_or_pin>` | gate input pin |

required/avoided nodes 使用 `gate:<g>`、`net:<n>` 或 bare net；bare token 預設視為 net。

## 4. Modes

| Mode | 語法 | 用途 | 主要 data |
|---|---|---|---|
| `exists` | `<start> <end> [-req ...] [-avoid ...]` | 是否至少存在一條符合限制的 path | `Yes` / `No` |
| `find_any` | `<start> <end> [constraints]` | 回傳任一條 path | `Depth`, `Nets`, `Gates` |
| `enumerate` | `<start> <end> [constraints/options]` | 列舉或計數所有 paths | `Total paths`, `Complete enumeration`, stop fields |
| `min_depth` | `<start> <end> [constraints]` | 最短 path | path `Depth`, `Nets`, `Gates` |
| `max_depth` | `<start> <end> [constraints]` | 最長 path | path `Depth`, `Nets`, `Gates` |
| `every_through` | `<start> <end> -req <node...>` | 每條 path 是否都通過 required nodes | `Yes` / `No` |
| `every_avoids` | `<start> <end> -avoid <node...>` | 每條 path 是否都避開指定 nodes | `Yes` / `No` |
| `mandatory_nodes` | `<start> <end>` | 所有 path 都經過的 internal nets | `Path exists`, `Mandatory internal nets` |
| `articulation_between` | `<start> <end>` | `mandatory_nodes` 的官方 prompt 對應別名 | `Path exists`, `Mandatory internal nets` |
| `is_separator` | `<start> <end> <candidate_net>` | candidate 是否切斷指定 endpoints | `Is separator`, witness fields |
| `pi_po_cut` | `<candidate_net>` | candidate 是否為 PI-to-PO directed cut | `Is separator`, witness fields |
| `direct_pi_po` | 無必要 option | 所有 depth-0 PI-to-PO direct connections | 完整 connection list 與總數 |

## 5. Enumeration Options 與輸出判讀

| Option | Default | 適用 mode | 用途 |
|---|---|---|---|
| `-req <node...>` | empty | endpoint path modes | path 必須經過的 nodes；遇到下一個 option 結束 |
| `-avoid <node...>` | empty | endpoint path modes | path 必須避開的 nodes |
| `-count_only` | false | `enumerate` | 只計數，不保存/顯示每條 path |

共同輸出判讀：

| Mode | 作答欄位 |
|---|---|
| `exists`, `every_through`, `every_avoids` | `Yes` / `No`；必須先確認 `complete:true` |
| `find_any`, `min_depth`, `max_depth` | path existence、`Depth`, `Nets`, `Gates` |
| `mandatory_nodes`, `articulation_between` | `Path exists`, `Mandatory internal nets` |
| `is_separator`, `pi_po_cut` | `Is separator` 與 witness fields |
| `enumerate` | `Total paths`, `Complete enumeration`, `Timed out`, `Stop reason`, `Output file` |
| `direct_pi_po` | total count；大型結果另附完整 artifact 與 `output_file` |

對 `enumerate`，只有 envelope `complete:true` 且 data `Complete enumeration: yes` 時，
`Total paths` 才是精確完整總數。非 `-count_only` 查詢會在 DFS 過程中自動 streaming 寫檔，
檔名以目前 design 名稱與序號組成，且不覆寫工作目錄內的既有檔案。
不會先把所有 paths 保存在記憶體後再一次寫出。遇到 timeout 時先保留已找到數量、
停止原因與輸出檔，再依 `LLM_NOTES.md` 的 Competition Answer Policy 產生正式候選答案；
不得把 partial count 偽裝成精確總數。

完整檔案格式為 `LITERAL_PATH_V1`，每筆可直接閱讀，不需要額外 mapping 或 decoder：

```text
Path 0: a -> g0(BUF) -> n0 -> g2(AND) -> n2 -> g7(BUF) -> y
```

只有 header count、`Written paths`、`Complete: yes` 與 envelope 完整性一致時，才能宣稱已列出全部 paths。

`direct_pi_po` 的小型結果直接完整顯示；保守估算超過 4096 response tokens 時由工具自動寫入 self-contained artifact，
response 回傳總數、完整性、格式與 `output_file`，不需要先取得總數後重跑。

## 6. Prompt Examples

```text
Prompt: Is there a path from input a to output y that avoids n8?
Command: path_query exists pi:a po:y -avoid net:n8
Read: Yes/No；先確認 complete=true
```

```text
Prompt: Count all register-to-register paths.
Command: path_query enumerate all_dff_q all_dff_d -count_only
Read: Total paths；只有 Complete enumeration=yes 才是完整答案
```

```text
Prompt: Find the longest path from ff1.Q to ff2.D.
Command: path_query max_depth dff_q:ff1 dff_d:ff2
Read: Depth, Nets, Gates
```

```text
Prompt: What is the maximum logic depth from any primary input to any DFF D-pin?
Command: path_query max_depth all_pi all_dff_d
Read: Depth, Nets, Gates
Do not use: depth_query all_dff_d（它允許 DFF.Q 等 sequential startpoints，scope 不同）
```

```text
Prompt: Is n10 a PI-to-PO cut net?
Command: path_query pi_po_cut n10
Read: Is separator
```

```text
Prompt: Find all articulation points in the combinational graph between n2 and n14.
Command: path_query articulation_between net:n2 net:n14
Read: Path exists, Mandatory internal nets；若 Status=NO_PATH，表示兩端原本沒有 directed combinational path
```

## 7. 限制

- 大型設計的 all-path enumeration 可能產生大型檔案。只問數量時使用 `-count_only`；完整
  列舉時直接使用 `enumerate`，由工具自動完整寫檔並在 terminal 回傳摘要與路徑。
- 輸出檔名、terminal sample 數、path 保存策略與執行時間是工具內部政策，不是公開 command
  參數。完成性只看 `complete`、`Complete enumeration` 與 stop fields。
- bus bit endpoint 必須寫成 `net:n0[0]`、`net:n63[1]`；不要把 bus bit 當成 `pi:` / `po:` endpoint。
- `max_depth` 是指定 endpoints 間的最長 path；全域 critical path 應使用 `depth_query global_critical`。
- 空的 mandatory list 可能表示 path 存在但沒有 mandatory internal net；必須同時讀 `Path exists`。
- `articulation_between` 在本工具中的精確語意，是列出每一條 directed `start -> end`
  combinational path 都必經的 internal nets；不包含 start/end，也不是搜尋整張無向圖的全域
  articulation vertices。它與 `mandatory_nodes` 使用同一個 report 與 backend。
