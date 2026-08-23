# LLM Tool Notes

本文件只記錄跨 tool 的判讀規則。每個 command 的 grammar、mode 與 data 欄位仍以對應 `*_TOOL.md` 為準。

## 1. 先判讀 Envelope

每次 command 都先讀 `status` 與 `complete`，再讀 data：

```text
status: ok/no_change and complete: true
    -> 可作為完整結果判讀。
status: partial/timeout/unsupported/error or complete: false
    -> 不可把 tool result 當成已證明的「不存在」、「0」、「否」或完整列表；
       但正式競賽答案仍須依下方 best-effort 規則輸出明確候選答案。
```

`ok:true` 只表示 request 有被處理，不保證 design 已改變，也不保證某個 yes/no 性質成立。

## 2. Competition Answer Policy

LLM 不應把 `unknown`、`maybe`、`cannot determine` 或只描述工具失敗原因當成正式答案。
作答優先順序如下：

```text
1. 使用 complete=true 的直接結果。
2. 改用較便宜但語意等價的 query，例如 count_only、summary、DP 或指定 scope。
3. 調整剩餘時間後重試，或組合其他已完成 query 交叉推導。
4. 仍無法完成時，根據 partial result、已證明下界、結構統計與相鄰 query，
   選出最可能的單一答案並提交。
```

最後一級稱為 `BEST_EFFORT_INFERENCE`。正式答案必須符合題目要求的格式並直接給出
數值、名稱、yes/no 或 netlist，不加入模稜兩可的措辭。LLM 應在自己的推理狀態中保留
觸發原因、使用的完整或部分結果，以及得到候選答案的假設，但不得把這些內部資訊加入
題目要求的正式輸出。

best-effort 不能改寫工具事實：不得把 partial 標成 complete，也不得宣稱未執行或未完成的
SAT 已證明等價。對 transformation/optimization，未通過 structure、constraint 與該
operation certificate contract 的候選不得輸出；應保留 original 或最後一個已接受版本。
`opt_apply critical_path_depth` 的 changed candidate 使用 `CertifiedRewrite`，本 command
不執行 whole-design SAT；需要獨立 proof 時使用 `equiv_query previous_edit/original`。
詳細判讀以 [`OPTIMIZATION_TOOL.md`](OPTIMIZATION_TOOL.md) 為準。

## 2.1 Routing And Answer Checklist

送出每個 command 前，重新讀取 prompt 中的量詞、否定詞、限制條件、scope、object kind 與
輸出要求。以下詞不能在 routing 時遺失：

| Prompt 語意 | 必須保留的 command 語意 |
|---|---|
| `any`、`exists`、`find one` | find-any；找到一個 proven witness 即完整回答，不使用 `--all` |
| `all`、`every`、`list each` | 完整列舉；不得用第一個 witness 代替 |
| `avoid`、`without passing`、`does not traverse` | 使用 `-avoid` 並保留被避開物件的種類與名稱 |
| `through`、`must pass`、`every path passes` | 使用對應 through/every-through mode，不得改成普通 `exists` |
| `only TYPE...`、`at most N` | 原樣保留 allowed gate basis 或數值 constraint |
| `original`、`previous edit`、`current` | 選正確 baseline，不得互換 |

`status:ok` 只證明送出的 command 合法。送出後仍需反查 result 的 mode、source/target、scope、
constraints 與 complete status；若 prompt 有 avoid X，但回傳 command/result 沒有 X，即使結果
為 `ok` 仍屬 routing 錯誤。

不要在 command grammar 後附加自然語言、註解或未定義 token。Structure Query 的固定 arity
modes 會 fail closed：零參數 mode 不接受任何 token，object-info mode 只接受一個名稱。

複合答案必須完成最後一步推導：

- `Which of A or B...`：分別查 A、B 的相同 metric，再明確回答 winner 或 tie；不能只貼兩份 report。
- 單一 gate 的 `gate type and pin connections`：使用 `gate_info` 組合 type、input nets 與 output net。
- 某 type 的全部 gates 並附 pin connections：使用
  `structure_query gates_by_type --gate-types <type...> --with-pins`，不得逐顆呼叫 `gate_info`。
- prompt 指定多個 gate type 時，把它們放在同一個 `--gate-types` 後，語意為 OR；prompt 說
  `except`、`excluding`、`not TYPE` 時使用 `--exclude-gate-types`。未指定 include 代表全部，
  exclude 優先，不要自行取得全名單後在文字中過濾。
- constant-input gates 若還要求 constant 所在 pin、其他 inputs 或 output signal：使用
  `structure_query const_input_gates [type|all] [0|1|any] --with-pins`；只問數量或名稱時不要加旗標。
- 成功的 list/filter query 明確回傳 `gates: 0` 時，回答沒有 matching gates；若欄位根本未輸出，
  不能自行當作 0，應依 mode 文件判斷或改用正確 query。
- net driver/load 題目若要求 exact pin、gate type 或 DFF pin role，加 `--with-pins`；回答 gate 數讀
  `count`，回答 pin/edge 數讀 `pin connection count`，兩者不得交換。
- `List DFFs driven by clock n0`：使用 fanout-load report 的 `DFF clock-pin loads`，不能把所有 load
  類型或只有總 fanout count 當成 DFF 名單。
- cone gate-type count 為空時，若 query complete 且 root 是 DFF.Q boundary，明答各 gate type
  為 0；不要回 `unknown`，也不要穿透到 DFF.D。
- 大型完整結果位於 artifact 時，答案提供題目要求的 count/yes-no/關鍵結論、完整性與
  `output_file`；不要把 artifact 全文貼回自然語言 response。

## 3. Compound Prompt 執行順序

一個 prompt 同時包含多個要求時，依相依關係執行，不要只呼叫一個 summary command。

### Read-only analysis

```text
確認 design 已 read
→ 將 prompt 拆成 count/list/connectivity/depth/function 子問題
→ 分別呼叫 authoritative tool
→ 檢查每個 envelope 的 complete
→ 組合正式答案
```

### Transformation

```text
必要時先查 baseline
→ edit_apply
→ report_query last_edit
→ 檢查 report_success / rolled_back / validation
→ 題目要求功能不變時確認 equivalence certificate
→ 重新查詢題目要求的 final property
→ write
```

### Optimization

```text
depth_query 取得 before depth
→ opt_apply 並重送所有 scope/basis/target constraints
→ report_query last_edit
→ 檢查 candidate_accepted、before/after depth、constraints、equivalence method 與 warnings
→ `CertifiedRewrite` 不得稱為 SAT proof；題目要求獨立 proof 時再呼叫 equiv_query previous_edit
→ depth_query 與 structure_query 驗證 final design
→ write
```

### Follow-up

題目詢問「上一步改了什麼」時使用 `report_query last_edit`，不要重新執行 edit。題目要求
延續先前 constraint 時，從對話中的前次 command 取回限制並在新 command 明確重送。

## 4. Error Recovery

| 狀況 | 處理方式 |
|---|---|
| unknown/unresolved object | 用 StructureQuery 的 info/list mode 確認正式名稱與物件種類後重試 |
| bus/port 解析失敗 | 保留完整 bit token；確認題目要 port count 還是 bit count |
| paginated/records truncated | 使用 next offset 續查，直到沒有下一頁 |
| path count timeout | 若只問數量，改用 `-count_only`；不要先要求完整 path file |
| path list timeout | 保留原 `all paths` 語意；使用 `enumerate` 自動 artifact，不自行加入輸出量、檔名或時間控制 |
| function search timeout | existence 題改 find-any；all-pairs 題保留 confirmed pairs 並依完整性規則處理 |
| SAT timeout/unknown | 縮小合法 scope、增加可用 budget 或改用其他已完成 query 交叉推定 |
| edit/optimization rollback | 不輸出失敗候選；保留 original 或最後一個已驗證版本 |
| `status:no_change` | 檢查是否 already optimal、原本已符合要求或沒有可套用 transformation |
| output file error | 改用可寫入的明確相對路徑後重試 |
| unsupported mode | 回到 README 重新 routing，使用可組合的 public tools，不猜不存在的 command |

相同 command、參數與 design revision 已 timeout 時，不要原樣無限重試。先改變 query 策略、
scope、輸出模式或時間配置；仍無法完成才使用 best-effort inference。

## 5. Large Output And Time Budget

- 除 prompt 明確要求前 N 筆、最多 N 筆或只取一個 witness 外，不得自行設定結果數量上限。
- `max_print`、`limit` 等 display/page 參數不能改變「all」的語意；有下一頁就必須取完。
- 官方 read/write 等 basic operation 上限為 60 秒，其餘 query/edit/optimization 上限為
  300 秒；backend 預設保留 55/290 秒供運算。prompt 未指定較短 budget 時，不為了方便
  自行加入 `-time_limit`、`--time-limit` 或 equivalence budget。
- 一個 edit/optimization prompt 只呼叫一次主要 `edit_apply` 或 `opt_apply`；不要用多次昂貴
  apply 嘗試碰運氣。高階 apply 已在內部完成必要流程，後續確認優先使用 `report_query`、
  `depth_query` 或其他輕量 query。
- 官方時間由 runner/watchdog 管理。LLM 不負責計算跨 tool call 的剩餘秒數，也不要自行替
  每次呼叫切割 budget。
- 支援完整 file output 時，把大型結果寫入檔案；正式答案只回總數、是否完整、關鍵摘要與
  output file，不複製整份內容。
- 若 backend 具有無法關閉的 hard result cap，必須把結果視為 incomplete，依該 tool 文件
  的重試方式處理；不能因為達到預設上限就宣稱已找到全部。
- 先選 summary/count-only mode；只有題目明確要求列出全部物件或路徑時才要求 list。
- `path_query enumerate` 的完整性只看 `complete:true` 與 `Complete enumeration: yes`。
- all-path count 只加 `-count_only`；完整 path list 直接使用 `enumerate`，由 CLI 自動寫入唯一 artifact。不要自行加入輸出量、檔名、截斷或時間控制，也不要在答案中使用省略號冒充完整列表。
- 有 pagination 的 list query 必須持續讀取直到 `records_truncated:false` 或沒有 next offset。
  若因總時限無法完成，正式答案仍依 Competition Answer Policy，根據 partial count、
  stop reason 與已取得 records 提交最可能答案。
- `cone_query` 若 prompt 指定一種或多種 gate type，必須原樣傳給 `--gate-types <type...>`，
  再讀 `filtered gates`；不可只查完整 cone 後由自然語言自行估算。合法 type 為 `AND OR
  NOT NAND NOR XOR XNOR BUF DFF`。若只問數量或 type breakdown，不搬運完整 gate/net 清單。
- `cone_query --with-pins` 只在 prompt 要求 connection/pin/detail 時使用。大型 details 會自動
  寫入 artifact，答案回 `filtered gates`、完整性與 `output_file`。

完整輸出配方：

| 題型 | 作法 |
|---|---|
| Structure/Cone name list | 小型清單直接輸出；大型清單自動完整寫入 `QUERY_LIST_ARTIFACT_V1`，答案回 count、完整性與 `output_file` |
| Depth name list | printer 會全量輸出；只問數量時不要把清單搬入答案 |
| direct PI-to-PO connections | 直接使用 `path_query direct_pi_po`；讀取完整 connection records 與總數，不自行加入 display limit |
| all paths count | `path_query enumerate ... -count_only` |
| all paths list | `path_query enumerate ...`；工具自動完整寫入唯一 artifact |
| sequential DFF detail | all-DFF 預設自動完整寫入 artifact；答案回 `artifact_record_count`、完整性與 `output_file` |
| FunctionSearch FindAll | 使用 `--all`；工具自動完整寫入唯一 artifact，答案回 `match_count`、完整性與 `output_file` |
| Boolean expression | 呼叫 `func_query boolean_expression <net>`；工具自動完整寫入 named-DAG artifact，答案回 equation/boundary counts、完整性與 `output_file` |

Boolean expression 若回 `primary-input-only combinational expression available: no`，必須查看
`DFF.Q boundary count` 與 `undriven boundary count`。DFF.Q 是 current-state pseudo input，不是
top-level PI。完整 artifact 的 `Current-state variables` 會把 `state_qN` 對照到
`<DFF>.Q` 與原 net；回答時應以「top-level PI + current-state variables」描述，不得把
state variable 說成 primary input，也不得沿 Q 回追 D pin。
此時應明確回答在 current combinational frame 下無法只用 top-level PI 表示，並指出實際
boundary。

FunctionSearch 不再要求 LLM 推算 unordered pair 上界。Prompt 未明確限制數量時不得加入
`--max-results`；`--all` 的完整 records 從 `output_file` 取得，只有 `complete:true`、
`truncated:false` 且未 timeout/unsupported 時才能宣稱完整。

Boolean equation artifact 不設 gate/depth/字元上限。只有 envelope `complete:true`、
`expression artifact complete: yes` 且檔案 footer 為 `Complete: yes` 時才可宣稱完整；
自然語言 response 不重貼大型 equations，只提供簡答與 `output_file` 路徑。

通用自動 list artifact 以 4096-token response 上限與保守 token estimate 決定 terminal 或檔案
呈現，且不限制結果數量。看到 `list artifact complete:yes` 時，完整名單位於 `output_file`，自然語言答案提供
總數與路徑。若沒有 artifact metadata，代表結果規模小，完整清單已直接出現在 data。

## 6. Sequential Boundaries

- DFF.Q 是 combinational boundary。`cone_query net_fanin <dff_q_net>` 不會回到同一顆 DFF 的 D input，且不含 combinational gate；report 可能仍列出 root net 本身。
- `D = EN & DATA` 沒有 Q feedback，不是 enable/hold。它只能當 `DATA_GATING_WITHOUT_HOLD_FEEDBACK` diagnostic，不能計入 matched/candidate DFF 數。
- `sequential_query enable_hold` 預設已使用 canonical safe fast path 與 Q-cofactor
  functional proof。數量讀 `matched_dff_count`，並先確認 `complete:true`。
- enable/data 名稱為空不代表 non-match；可能是已證明但無單一具名 net 的
  Q-free Boolean function。

## 7. Edit, Optimization And Constraints

- edit/optimization 結果必須同時檢查 `status`、`complete`、`report_success`、`report_changed`、`rolled_back` 與題目要求的 validation 欄位。
- 題目要求功能不變時，必須依 operation 的 certificate contract 判讀；`CertifiedRewrite` 可回報 qualified function-preserving rewrite，但只有完整 `equiv_query` 的 `WholeDesignSat` 才能描述成 SAT-proven。
- `status:no_change` 可以是合法 verified no-op 或 already-optimal 結果；必須如實回報 original retained，不可假裝已完成新的 rewrite。
- session 不會自動保留自然語言中的 previous constraint。後續 prompt 若要求 preserve/maintain constraint，LLM 必須從前題 call 找回限制，並在新的 edit/opt command 重新傳入；final write 前再驗證。
- `report_query last_edit` 只回答最近一次 edit/optimization。最近一次若失敗、rollback 或 timeout，不能沿用較早操作的 delta 回答目前問題。

## 8. Equivalence Scope

- `equiv_query original` 比較 current 與最近一次 `read` 建立的 original snapshot。
- `equiv_query previous_edit` 比較 current 與最近一次 edit/opt 前 snapshot。
- whole-design equivalence 比較 PO 與 DFF.D combinational boundaries；不包含 initial state 或 multi-cycle sequential equivalence。
- `timeout`、`partial`、`unsupported` 不是不等價證明；只有 complete result 的 mismatch 才能回答不等價。

## 9. Functional Analysis Boundary

目前 Boolean/function 類 tool 只可依各自文件中已列出的 mode 與 complete status 使用。任意 Boolean expression canonicalization、unbounded symmetry 與大型 functional-merge 策略不應由 LLM 自行假設存在；DFF enable/hold 則依 `SEQUENTIAL_QUERY_TOOL.md` 的 Q-cofactor 契約處理。
