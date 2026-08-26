# Path Query Tool

## 1. 責任

`path_query` 負責明確 startpoint 與 endpoint 之間的 combinational path、路徑限制、register-to-register path、mandatory nodes 與 separator/cut。官方 prompt 使用 `articulation points between A and B` 時，也由本工具的 `articulation_between` mode 處理。對外不使用 legacy `reg_path_query` 或 `graph_query`。

完整性規則：`all paths` 預設不設數量上限。只問數量時使用 `-count_only`；要求完整列表時
直接呼叫 `enumerate`，CLI 會自動以 streaming 寫入不覆寫既有結果的唯一檔名，正式答案只回
總數、完成狀態與檔案位置。輸出檔名、terminal 顯示量與執行預算由工具自行管理，LLM
不傳入這些內部控制參數。
詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

路徑依 gate instance 計算 structural edge。同一 net 同時接到同一 gate 的多個 input pins
仍只算一次；只有不同 gate instances 才是不同 path branches。

若 endpoint、gate pin、`-req` 或 `-avoid` 指向 edit 後已移除的 net/gate，工具會回 unresolved
error。這和「物件仍存在，但兩端沒有 path」的成功 `No`/零結果不同，不可混為同一答案。

## 2. 選擇條件

當答案是「某個 start set 與 end set 之間的 directed combinational path relation」時使用本
tool。先辨識 endpoints，再辨識要回答的 path population：

- existence/witness：是否存在、任一條、最短或最長的一條。
- population：所有 paths、path count、depth-filtered paths、Top-K 或 Nth path。
- universal constraint：每條 path 是否經過/避開指定 node、mandatory nodes。
- separation：candidate 是否切斷指定 endpoints，或 PI-to-PO directed cut。

即使題目使用 `logic depth`、`critical` 或 `register` 字樣，只要 start scope 與 end scope 被明確
限制，depth 就是該 endpoint relation 上的 path metric，owner 仍是 `path_query`。反之，若題目
要量測 current design 所有 timing endpoints 的 arrival-depth distribution 或 global maximum，
使用 `depth_query`。

只問某 root 的完整 ancestors/descendants 集合而沒有 end constraint 時使用 `cone_query`；只問
一層 driver/load adjacency 時使用 `structure_query`。不能因 prompt 出現 `path` 字樣，就把
critical endpoint summary 或 rooted cone 強制送到本工具。

## 3. Command Grammar

```text
path_query <mode> <start_endpoint> <end_endpoint>
           [-req <node...>] [-avoid <node...>]

path_query enumerate <start_endpoint> <end_endpoint>
           [-req <node...>] [-avoid <node...>] [-count_only]
           [-depth_eq N] [-depth_ge N] [-depth_le N]

path_query top_k_shortest <start_endpoint> <end_endpoint> <K>
path_query top_k_longest  <start_endpoint> <end_endpoint> <K>
path_query nth_shortest   <start_endpoint> <end_endpoint> <N>
path_query nth_longest    <start_endpoint> <end_endpoint> <N>
           [-req <node...>] [-avoid <node...>]
           [-depth_eq N] [-depth_ge N] [-depth_le N]

path_query is_separator <start_endpoint> <end_endpoint> <candidate_net>
path_query articulation_between <start_endpoint> <end_endpoint>
path_query pi_po_cut <candidate_net>
path_query direct_pi_po
```

`start_endpoint` 與 `end_endpoint` 是 required。`-count_only` 只有 `enumerate` 接受；depth
predicate options 也可縮小 ranked population。K/N 是 prompt 明確給定的 1-based positive integer。
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
| `top_k_shortest` | `<start> <end> <K> [constraints/options]` | 全域前 K 短 paths | ordered ranks、depth、completion |
| `top_k_longest` | `<start> <end> <K> [constraints/options]` | 全域前 K 長 paths | ordered ranks、depth、completion |
| `nth_shortest` | `<start> <end> <N> [constraints/options]` | 第 N 短 path | original rank、path 或 rank missing |
| `nth_longest` | `<start> <end> <N> [constraints/options]` | 第 N 長 path | original rank、path 或 rank missing |
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
| `-depth_eq N` | none | `enumerate`、ranked modes | 只納入 logic depth exactly N 的 paths |
| `-depth_ge N` | none | `enumerate`、ranked modes | inclusive minimum depth |
| `-depth_le N` | none | `enumerate`、ranked modes | inclusive maximum depth |

depth 是穿越的 gate instance 數；NOT、BUF 與其他 gate 各算一層，直接 PI-to-PO wire 是 0。
`-depth_ge L -depth_le U` 表示 between L and U inclusive。條件可與 `-req/-avoid` 組合。

共同輸出判讀：

| Mode | 作答欄位 |
|---|---|
| `exists`, `every_through`, `every_avoids` | `Yes` / `No`；必須先確認 `complete:true` |
| `find_any`, `min_depth`, `max_depth` | path existence、`Depth`, `Nets`, `Gates` |
| `mandatory_nodes`, `articulation_between` | `Path exists`, `Mandatory internal nets` |
| `is_separator`, `pi_po_cut` | `Is separator` 與 witness fields |
| `enumerate` | `Total paths`, `Complete enumeration`, `Timed out`, `Stop reason`, `Output file` |
| ranked modes | rank window/completion、first/last returned depth、`Ranked path depth ranges`；Nth 另有 `Requested rank depth` |
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

ranked modes 不會完整 enumerate 後交給 LLM 排序。小結果直接列出；超過 4096-token 估算時
完整 `rank/depth/literal path` 會自動寫入 artifact。只有 `Ranking complete: yes` 時排名可用；
`Requested rank exists: no` 也必須在 complete 時才能回答不存在。

同 depth 的連續 ranks 會精確壓縮，例如 `ranks 1-200: depth=16`。literal paths 外移時，只要
壓縮後 ranges 仍符合 response budget，這份 mapping 會保留在 `TOOL_RESULT`。Nth-depth 題直接讀
`Requested rank depth`，不需要讀 artifact。若 ranges 本身也太大，回答保留 first/last depth、
range count 與 `output_file`，完整 mapping/path 由 artifact 交付。

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
Prompt: How many paths from a to y have logic depth exactly 5?
Command: path_query enumerate pi:a po:y -depth_eq 5 -count_only
Read: Minimum/Maximum accepted depth, Total paths, Complete enumeration
```

```text
Prompt: List every PI-to-PO path with depth between 3 and 7 inclusive.
Command: path_query enumerate all_pi all_po -depth_ge 3 -depth_le 7
Read: Total paths, Complete enumeration, Output file；完整 literal records 由 artifact 交付，LLM 不開檔
```

```text
Prompt: Does any path from a to y have depth at least 8?
Command: path_query enumerate pi:a po:y -depth_ge 8 -count_only
Read: Total paths；大於 0 表示存在。必須先確認 Complete enumeration=yes
```

```text
Prompt: List the five shortest paths from a to y.
Command: path_query top_k_shortest pi:a po:y 5
Read: ranks 1..5, Ranked path depth ranges, Ranking complete, Population exhausted
```

```text
Prompt: What is the third longest register-to-register path?
Command: path_query nth_longest all_dff_q all_dff_d 3
Read: Requested rank exists；若 yes，depth 讀 Requested rank depth，完整 path 讀 Rank 3 或 output_file
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
- 題目指定 exactly/at-least/at-most/between 時使用 `enumerate` 的 depth options，不要把
  `min_depth/max_depth` 當成 filter。若題目同時要求 Top-K/Nth，則把同一 depth options 加在
  ranked command；`min_depth/max_depth` 仍只回一條 extrema witness。
- 空的 mandatory list 可能表示 path 存在但沒有 mandatory internal net；必須同時讀 `Path exists`。
- `articulation_between` 在本工具中的精確語意，是列出每一條 directed `start -> end`
  combinational path 都必經的 internal nets；不包含 start/end，也不是搜尋整張無向圖的全域
  articulation vertices。它與 `mandatory_nodes` 使用同一個 report 與 backend。
