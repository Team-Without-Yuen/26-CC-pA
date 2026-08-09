# Path Query Tool

## 1. 責任

`path_query` 負責明確 startpoint 與 endpoint 之間的 combinational path、路徑限制、register-to-register path、mandatory nodes 與 separator/cut。對外不使用 legacy `reg_path_query` 或 `graph_query`。

完整性規則：`all paths` 預設不設數量上限。只問數量時使用 `-count_only`；要求完整列表時
直接呼叫 `enumerate`，CLI 會自動以 streaming 寫入不覆寫既有結果的唯一檔名，正式答案只回
總數、完成狀態與檔案位置。除非 prompt 明確要求，LLM 不主動傳入輸出或截斷控制參數。
詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

prompt 出現 `path from A to B`、`through`、`avoid`、`every path`、`shortest`、`longest between endpoints`、`register-to-register`、`mandatory`、`separator` 或 `cut` 時使用本 tool。

只問某物件可到達的完整範圍使用 `cone_query`；全設計 maximum depth/critical endpoint 使用 `depth_query`。

## 3. Command Grammar

```text
path_query <mode> <start_endpoint> <end_endpoint>
           [-req <node...>] [-avoid <node...>]

path_query enumerate <start_endpoint> <end_endpoint>
           [-req <node...>] [-avoid <node...>] [-count_only]
           [-out <file>] [-max_print N] [-max_paths N]
           [-time_limit seconds]

path_query is_separator <start_endpoint> <end_endpoint> <candidate_net>
path_query pi_po_cut <candidate_net>
path_query direct_pi_po
```

`start_endpoint` 與 `end_endpoint` 是 required。只有 `enumerate` 接受輸出、顯示、legacy
path limit、time limit 與 count-only options。`-req`/`-avoid` 遇到下一個 option 或行尾結束。

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
| `is_separator` | `<start> <end> <candidate_net>` | candidate 是否切斷指定 endpoints | `Is separator`, witness fields |
| `pi_po_cut` | `<candidate_net>` | candidate 是否為 PI-to-PO directed cut | `Is separator`, witness fields |
| `direct_pi_po` | 無必要 option | 所有 depth-0 PI-to-PO direct connections | 完整 connection list 與總數 |

## 5. Enumeration Options 與輸出判讀

| Option | Default | 適用 mode | 用途 |
|---|---|---|---|
| `-req <node...>` | empty | endpoint path modes | path 必須經過的 nodes；遇到下一個 option 結束 |
| `-avoid <node...>` | empty | endpoint path modes | path 必須避開的 nodes |
| `-out <file>` | 自動唯一檔名 | `enumerate` | 明確指定輸出檔 |
| `-max_print <N>` | 工具摘要策略 | `enumerate`/`direct_pi_po` | 最多在 terminal data 顯示 N 筆，不限制完整檔案 |
| `-max_paths <N>` | 不限制 | `enumerate` | Legacy 相容參數；不作為完整列舉的截斷條件 |
| `-time_limit <seconds>` | 題目/session budget | `enumerate` | enumeration 時間上限 |
| `-count_only` | false | `enumerate` | 只計數，不保存/顯示每條 path |

共同輸出判讀：

| Mode | 作答欄位 |
|---|---|
| `exists`, `every_through`, `every_avoids` | `Yes` / `No`；必須先確認 `complete:true` |
| `find_any`, `min_depth`, `max_depth` | path existence、`Depth`, `Nets`, `Gates` |
| `mandatory_nodes` | `Path exists`, `Mandatory internal nets` |
| `is_separator`, `pi_po_cut` | `Is separator` 與 witness fields |
| `enumerate` | `Total paths`, `Complete enumeration`, `Timed out`, `Stop reason`, `Output file` |
| `direct_pi_po` | total count 與完整 connection list |

對 `enumerate`，只有 envelope `complete:true` 且 data `Complete enumeration: yes` 時，
`Total paths` 才是精確完整總數。非 `-count_only` 查詢會在 DFS 過程中自動 streaming 寫檔，
檔名以目前 design 名稱與序號組成，且不覆寫工作目錄內的既有檔案。
不會先把所有 paths 保存在記憶體後再一次寫出。遇到 timeout 時先保留已找到數量、
停止原因與輸出檔，再依 `LLM_NOTES.md` 的 Competition Answer Policy 產生正式候選答案；
不得把 partial count 偽裝成精確總數。

`direct_pi_po` 預設直接顯示所有 depth-0 connections，不需要先取得總數後重跑。
只有 prompt 明確限制顯示筆數時才使用 `-max_print`。

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
Prompt: Is n10 a PI-to-PO cut net?
Command: path_query pi_po_cut n10
Read: Is separator
```

## 7. 限制

- 大型設計的 all-path enumeration 可能產生大型檔案。只問數量時使用 `-count_only`；完整
  列舉時直接使用 `enumerate`，由工具自動完整寫檔並在 terminal 回傳摘要與路徑。
- `-out`、`-max_print`、`-max_paths`、`-time_limit` 都是特殊控制參數；除非 prompt 明確要求
  對應控制，LLM 不主動使用。
- `-max_paths` 僅為 legacy compatibility，不得把它當成完整 enumeration 的限制或完成保證；完成性只看 `complete`、`Complete enumeration` 與 stop fields。
- bus bit endpoint 必須寫成 `net:n0[0]`、`net:n63[1]`；不要把 bus bit 當成 `pi:` / `po:` endpoint。
- `max_depth` 是指定 endpoints 間的最長 path；全域 critical path 應使用 `depth_query global_critical`。
- 空的 mandatory list 可能表示 path 存在但沒有 mandatory internal net；必須同時讀 `Path exists`。
