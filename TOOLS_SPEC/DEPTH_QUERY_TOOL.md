# Depth Query Tool

## 1. 責任

`depth_query` 負責量測 current design 的 combinational arrival depth、PO/DFF.D endpoint depth、critical path，以及 endpoint depth 比較/範圍篩選。它是 read-only analysis，不會降低或最佳化 depth。

完整性規則：all/threshold/filter mode 不限制 endpoint 筆數。record 數或預估字元量超過 terminal 門檻時，工具自動
完整寫入 artifact；response 只回 count、worst summary、完整性、格式與 `output_file`。
詳見 [`LLM_NOTES.md`](LLM_NOTES.md)。

## 2. 選擇條件

當答案來自 current design 的 timing endpoint arrival depth 時使用本 tool。它負責的 object/scope
組合是：指定 net、所有 PO、所有 DFF.D、PO+DFF.D 的全域 endpoint 集合，以及全域
maximum-depth paths 上的 gate 聯集。

mode 由 quantifier 與 output shape 決定：

- 單一 endpoint depth：`net`。
- 完整 endpoint distribution：`all_po` / `all_dff_d`。
- exact、threshold 或 inclusive range 的 endpoint subset：`filter`、`po_exceeding` 或 `exceeding`。
- current global maximum 與一條 witness：`global_critical`；僅比較 PO 時是 `deepest_output`。
- 某 gate 是否位於任一 global maximum path：`gate_on_critical`；完整 gate 聯集或分類計數：
  `critical_gates`。

若 start scope 與 end scope 都被明確限定，問題是在該 endpoints relation 上量測 path depth，
應使用 `path_query`，即使句中也有 `logic depth` 或 `critical path`。若 operation 是降低、最小化、
改善或達成 target cost，屬於 optimization；`depth_query` 只能量測，不修改設計。

## 3. Command Grammar

```text
depth_query <mode> [args]
```

## 4. Modes

| Mode | 參數 | 用途 | 主要 data |
|---|---|---|---|
| `net` | `<net>` | 指定 net 的 depth 與代表性 critical path | `Report count`, endpoint depth, path |
| `all_po` | 無 | 所有 PO depths | endpoint/depth list, `Worst endpoint` |
| `all_dff_d` | 無 | 所有 DFF.D depths | endpoint/depth list, `Worst endpoint` |
| `global_critical` | 無 | PO 與 DFF.D 中的全域最大 depth | `Worst endpoint`, `depth`, path |
| `exceeding` | `<depth>` | 超過 threshold 的 PO 與 DFF.D endpoints | `Report count`, endpoint/depth list |
| `po_exceeding` | `<depth>` | 只計算超過 threshold 的 primary outputs | `Report count`, endpoint/depth list |
| `filter` | `<all\|po\|dff_d> <eq\|ne\|gt\|ge\|lt\|le> <depth>` | 通用單值比較 | checked/matched/unavailable counts，typed endpoint list |
| `filter` | `<all\|po\|dff_d> between <lower> <upper>` | inclusive depth range | checked/matched/unavailable counts，typed endpoint list |
| `gate_on_critical` | `<gate>` | gate 是否位於任一條 global maximum-depth path | `Gate on critical path`；顯示的 worst path 仍只是一條代表路徑 |
| `critical_gates` | 無 | 所有位於任一 global maximum-depth path 的 combinational gates | gate summary counts、完整 critical gate records、`Worst endpoint` |
| `deepest_output` | 無 | 完整 PO-bit population 中 arrival logic depth 最大的 primary output；量測 logic levels，不是 cone gate/net 數 | `Worst endpoint`, depth, path |

## 5. 輸出判讀

| 題目 | 應讀欄位 |
|---|---|
| How many outputs have depth greater than 4? | `po_exceeding 4` 的 `Report count` |
| How many endpoints exceed depth 4? | `exceeding 4` 的 `Report count` |
| Maximum logic depth | `global_critical` 的 `Worst endpoint ... depth=` |
| Critical path gates/nets | `global_critical` 的 path `Gates` / `Nets` |
| Specific net depth | `net` report 中對應 endpoint depth |
| Exact/at-least/at-most/less-than/range | `filter` 的 `Matched endpoint count` 與 typed endpoint records |
| 所有 critical gates / critical gate 數量 | `critical_gates` 的 `Critical gate count` 與 gate records |
| 指定 type 的 critical gate 數量 | `critical_gates` 的 `Critical gate type counts`；大型 artifact 時仍留在 envelope |

`exceeding` 包含 PO 與 DFF.D；題目明確說 outputs 時必須使用 `po_exceeding`。

新題型不是 strict greater-than 時使用 `filter`。scope `all` 是 PO bits 後接 DFF.D；
`po` 只含 outputs；`dff_d` 只含 register D pins。`between` 包含上下界。讀取時優先使用
`Matched endpoint count`。`No timing path endpoint count` 表示 endpoint 合法但沒有可量測
timing path，它不參與數值比較，也不會使 envelope 變成 partial。只有
`Graph inconsistent endpoint count` 或 `Analysis failure endpoint count` 非零時，
`Analysis complete: no` / `status: partial` 才表示分析可靠性不足。

小型結果直接完整列出。大型 endpoint list 或 critical path 使用
`QUERY_LIST_ARTIFACT_V1`；artifact 的 `Endpoint depth reports` 保存每個 endpoint 與 depth，
`Worst path nets/gates in start-to-end order` 保存代表性 critical path。LLM 不開檔檢查 artifact
footer，而是使用 envelope 的 `complete:true`、artifact complete 與 `analysis_complete:yes`
判斷；producer 的 `Complete: yes` footer 僅供檔案接收端驗證 records 已完整寫入。
artifact 的每筆 endpoint record 都含 `status=available|no_timing_path|graph_inconsistent|analysis_failure`；
`depth=-1` 必須搭配 status 解讀，不能直接視為工具失敗。

`critical_gates` 的每筆 record 包含：

```text
gate, id, type, output, output_id, arrival_depth, remaining_depth, path_depth
```

其中 `path_depth = arrival_depth + remaining_depth`，且所有回傳 gate 的 `path_depth` 都等於
`Worst endpoint` 的 global maximum depth。list 是所有 tied maximum-depth paths 的聯集，依
gate ID 穩定排序且不重複。DFF 是 boundary，不列入；無法到達 PO/DFF.D 的合法 logic 計入
`No timing path gate count`。大型 gate list 自動寫入 artifact 的
`Critical gates on global maximum-depth paths` section，LLM 應回報 `output_file`，不可只轉述
terminal sample。`Critical gate type counts` 固定保留 AND、OR、NAND、NOR、NOT、BUF、XOR、
XNOR 八種完整統計與零值，不需要讀 artifact 重新計數。

## 6. Prompt Examples

```text
Prompt: How many outputs have a logic depth greater than 4?
Command: depth_query po_exceeding 4
Read: Report count
```

```text
Prompt: Report the current maximum logic depth and critical path.
Command: depth_query global_critical
Read: Worst endpoint, depth, Nets, Gates
```

```text
Prompt: Is gate g10 on the critical path?
Command: depth_query gate_on_critical g10
Read: Gate on critical path
```

```text
Prompt: List every gate that lies on at least one maximum-depth path.
Command: depth_query critical_gates
Read: Critical gate count and all gate records; if artifact exists, report output_file
```

```text
Prompt: How many NAND gates are part of any critical path?
Command: depth_query critical_gates
Read: Critical gate type counts 的 NAND 值；不需要讀取 artifact
```

```text
Prompt: List all outputs whose logic depth is at most 4.
Command: depth_query filter po le 4
Read: Matched endpoint count, typed endpoint records
```

```text
Prompt: How many DFF D-pins have logic depth between 2 and 5 inclusive?
Command: depth_query filter dff_d between 2 5
Read: Checked endpoint count, Matched endpoint count, Analysis complete
```

若 prompt 將 startpoint 明確限制為 PI，例如「PI 到 PO 的最長路徑」或「PI 到 DFF.D
的最大 depth」，不可直接使用 timing-global `depth_query global_critical`，因為後者同時允許
DFF.Q start boundary。應改用：

```text
path_query max_depth all_pi all_po
path_query max_depth all_pi all_dff_d
```

## 7. 限制

- 同一 maximum depth 可能有多條 paths；`global_critical` 回傳一條代表性 path，不代表唯一
  critical path。`gate_on_critical` 與 `critical_gates` 都以任一條 maximum-depth path 為準，
  不只檢查該代表路徑。
- `critical_gates` 回傳 critical gate 聯集，不枚舉所有 paths，也不計算 critical path 數量；
  需要特定 start/end path enumeration 時仍使用 Path Query。
- depth traversal 以 combinational boundary 處理 DFF，DFF.Q 為 start boundary，DFF.D 為 endpoint boundary。
- `deepest_output` 只比較 PO；全域最差 endpoint 使用 `global_critical`。
- deepest、logic depth、path depth、logic levels 都是 depth metric；即使 prompt 以 output 的 fanin logic
  描述 target，也不能改用計算 cone gate/net 數量的 `cone_query largest_output`。
- `filter` 是 summary-only，不回傳每個 match 的 critical path；如需指定 endpoint path，再使用 `net` 或 Path Query。
