# LLM Tool Notes

本文件定義所有 tools 共用的 LLM 執行與作答規則。每個 command 的 grammar、mode、參數與
`data` 欄位仍以對應的 `*_TOOL.md` 為準；若個別文件與本文件的跨工具規則衝突，以本文件為準。

## 1. LLM 可見範圍與角色

LLM 可以讀取：

- 使用者 prompt 與同一題的對話上下文。
- 自己送出的 command 與參數。
- tools 回傳的 `TOOL_RESULT` envelope、`data` 與其中的 `output_file` 路徑。
- 小型結果直接放在 `data` 中的 inline records。

LLM 預設不能開啟或讀取 artifact 檔案內容。因此：

- artifact 是交付完整大型 payload 的檔案，不是 LLM 的第二個資料來源。
- LLM 可以在正式答案中提供 `output_file`，但不能依賴檔案內的 records 做比較、計數、篩選、
  routing 或後續推理。
- artifact 不是 session result handle，也不能直接傳給下一個 tools command。
- tools 必須把作答、判斷與後續 routing 所需的 scalar、identity、scope、完整性與關鍵結論保留在
  `TOOL_RESULT`；完整大型 records 才移到 artifact。

tools command 不知道原始 prompt，也不負責撰寫最終自然語言答案。LLM 必須以 `TOOL_RESULT`
中的可見證據組裝答案；artifact path 只負責交付 prompt 要求的完整資料。

## 2. 標準執行流程

每個 prompt 依下列順序處理：

```text
解析 operation、object、scope、direction、metric、quantifier、state 與 output shape
-> 選擇 authoritative public command
-> 送出最少且語意完整的參數
-> 核對 result 的 command/mode/source/target/scope/revision
-> 檢查 status、complete 與 semantic data fields
-> 必要時呼叫其他高階 query 完成衍生判斷
-> 直接回答結論；大型完整 payload 另附 output_file
```

不要在 command grammar 後附加自然語言、註解或未定義 token。固定 arity mode 必須嚴格遵守
其文件列出的參數數量。

### 2.1 高容錯 command 選擇 invariant

LLM 應選擇「最少額外推理即可直接回答原 prompt」的 public mode。以下規則適用所有 tools：

1. 有 batch/ranking/filter mode 能直接回傳完整 winner、ties、threshold matches 或衍生集合時，
   不使用只回一個 representative 的 legacy mode，也不逐物件 loop 後自行拼接。
2. Prompt 使用單數 `which` 不代表答案一定唯一。Extrema query 必須保留所有同一 metric level 的
   ties，最後答案可明確說明並列。
3. 未指定方向的 `logic cone of output X` 預設為 feeding X 的 transitive fanin；
   `affected/reachable/downstream from X` 才是 transitive fanout。`status:ok` 不能彌補方向錯誤。
4. Prompt 明確指定 start 與 end scopes 時，extrema/depth 必須使用同一 endpoint-scoped path mode，
   不可改用會納入其他 endpoints 的 global summary。
5. 優先使用能在單一 report 同時提供 prompt 所需 scalar 與 identities 的 mode，例如
   `fanout_load` 同時提供 QA pin-level fanout 與 `Direct load gates`。
6. Edit/optimization 後詢問 `current/now/after` 的性質時，重新查 current design；只有詢問上一步
   delta、changed names 或 operation status 時才讀 `report_query last_edit`。
7. 每個 RUN 只能有一個 public command。禁止使用 `;`、`&&`、`||`、pipe 或換行串接命令；
   複合題以多個 LLM turns 逐一呼叫，再組合 complete semantic fields。
8. 不主動加入 prompt 未要求的 `--with-pins`、path detail、result limit、time limit 或相容 flag。
   額外 payload 不能提高正確性，反而可能造成 parser、token 與 routing 風險。
9. 不以單一關鍵字決定 family/mode。`output` 不等於 fanout、`depth` 不一定是 DepthQuery、
   `count` 不一定是 whole-design summary、`equivalent` 也不一定是 whole-design equivalence；
   必須由完整 semantic tuple 決定 owner。

## 3. Envelope 與完整性

每次 command 都先讀 `status` 與 `complete`，再讀具語意名稱的 `data` 欄位：

```text
status: ok/no_change and complete: true
    -> 可作為完整結果判讀。
status: partial/timeout/unsupported/error or complete: false
    -> 不可把結果當成已證明的「不存在」、「0」、「否」或完整列表。
```

`ok:true` 只表示 request 已被處理，不保證 design 已改變，也不保證某個 yes/no 性質成立。

以下規則不可違反：

- 回答物件數量時使用 mode 文件指定的 semantic count，不使用通用 serialization count。
- `list entry count`、`artifact_record_count` 等欄位描述檔案或 records 數量，不一定等於 prompt
  所問的 gate、net、path、pin 或 issue 數量。
- `complete:false`、timeout 或 unknown 不能當成 0、false、NotEqual 或完整證明。
- artifact 的完整性由 envelope 中對應的 artifact status 與 `complete` 判讀；LLM 不開檔檢查 footer。
- 若寫檔失敗但工具完整回退為 inline records，依 envelope 與 inline 完整性判讀。

## 4. Result Assembly 與 Artifact

### 4.1 Prompt 要求分類

| 要求類型 | 例子 | LLM 使用的可見資料 |
|---|---|---|
| scalar / decision | count、yes/no、maximum、depth、winner | envelope 的 semantic metric/status |
| exhaustive payload | all gates、all paths、all equations | 小型 inline records，或完整 artifact 的 path |
| scalar + payload | count and list all matching gates | envelope scalar + inline records/artifact path |
| record-derived result | difference、intersection、依 record 屬性分組 | 專用高階 query 回傳的衍生結果 |
| analyze-then-act | 對分析出的集合再修改或查詢 | 專用 scoped/batch command 在 backend 重新計算 |

### 4.2 Artifact contract

1. Artifact 只承載超過自然語言輸出預算的完整可列舉 payload。
2. Prompt 要求的 count、yes/no、winner、maximum、scope、filter、result identity 與 completeness
   必須留在 envelope；不能只寫進 artifact。
3. Prompt 要求完整 records 且產生 artifact 時，正式答案直接給 scalar/關鍵結論、是否完整與
   `output_file`。不要把檔案全文貼回 response。
4. 沒有 artifact metadata 時，代表完整小型 records 位於 `data`；prompt 要求列出全部時不得
   擅自省略。
5. 任何需要檢查 membership、逐筆屬性或兩份 records 才能得到的結論，都不能假設 LLM 能從
   artifact 推導。必須使用現有專用 query，或登記為工具功能缺口。
6. 不可依賴「這次結果應該很小、會 inline」來設計功能。Hidden case 可能觸發 artifact，因此
   關鍵語意必須在小型與大型結果下都能由 envelope 或專用 query 得到。
7. Artifact 應可供 evaluator 或使用者單獨解讀，但 self-contained 格式不代表 LLM 能讀取它。

### 4.3 正式答案格式

```text
小型 scalar：直接回答 scalar 或結論。
小型完整清單：直接回答總數與完整 inline records。
大型完整清單：直接回答總數/結論、complete 狀態與 output_file。
大型 expression/path/detail：回答可見摘要與 output_file，不重貼 payload。
```

不能只回一條路徑而省略 prompt 明確要求的 scalar 或判斷；也不能把 artifact path 描述成 LLM
已檢查過的內容。

## 5. 比較、集合與複合推理

比較前先拆出 operands、metric、scope、filter 與量詞。每個 operand 使用相同語意的 query，並以
回傳的 source/target/object、mode 與 design revision 綁定，不能因呼叫順序混淆 A/B。

可以安全組合多個 analysis command 的前提是：所有 command 都能由原 prompt 直接決定，最終答案
只依賴各 envelope 中可見的 semantic scalar、Boolean、status 或 identity。即使兩邊都產生
artifact，只要比較欄位仍在 complete envelope，就可直接比較；不需要也不得開啟 artifact。
若下一個 command 的 target、scope 或 filter 必須先讀取前一份 records 才能決定，則屬於
adaptive chaining，必須改用專用 batch/scoped query，不能套用本節的 scalar 組合規則。

| 比較型態 | 正確作法 |
|---|---|
| 數值大小、相等、winner/tie | 取得兩邊 complete 且同語意的 envelope metric 後直接比較 |
| winner 的完整名單 | 先由 metric 判定 winner；再交付該 operand 的 inline records 或 artifact path |
| 集合相同、交集、差集 | 使用 `shared_fanin` 等專用集合 query；不能只比較 counts，也不能讀 artifacts 自行運算 |
| 依每筆 record 的屬性排序/分組 | 使用 ranking/filter/scoped aggregate mode；沒有時視為工具缺口 |
| before/after scalar | 修改前保存 semantic scalar，修改後對 current design 重查並比較 |
| before/after object delta | 優先使用 `report_query last_edit` 的正式 delta；不得用 artifact entry count 推測 |

例如：

```text
Compare the fanin cone sizes of A and B and report which is larger.
-> cone_query net_fanin A
-> cone_query net_fanin B
-> 核對兩份 complete、source 與相同 gates metric，再回答 A、B 或 tie。

Which cone, A or B, contains more NAND gates?
-> cone_query net_fanin A --gate-types NAND
-> cone_query net_fanin B --gate-types NAND
-> 比較兩份明確的 filtered gates；不要從只列非零類型的 breakdown 猜缺少類型為 0。

Which signal, A or B, has more direct loads, and by how many?
-> structure_query fanout_load A
-> structure_query fanout_load B
-> 比較兩份 fanout load count；即使兩份完整 load lists 都位於 artifact，差值仍由 envelope 計算。

Which gates are present in A's fanin cone but not B's?
-> counts 不足以回答。
-> 必須使用能直接計算 difference 的高階 query；若沒有，這是 API coverage gap。
```

同一句包含多個要求時，每個子要求都必須有可見且 authoritative 的結果。工具若把 records 改寫
到 artifact，其他比較或後續動作仍必須由工具直接計算，不可交給 LLM 開檔補做。

## 6. Routing Checklist

送出 command 前重新讀取 prompt 中的量詞、否定詞、限制、scope、object kind 與輸出要求：

| Prompt 語意 | 必須保留的 command 語意 |
|---|---|
| `any`、`exists`、`find one` | find-any；一個 proven witness 即可完整回答，不使用 `--all` |
| `all`、`every`、`list each` | 完整列舉；不得用第一個 witness 代替 |
| `avoid`、`without passing` | 使用 `-avoid`，保留被避開物件的種類與名稱 |
| `through`、`must pass`、`every path passes` | 使用對應 through/every-through mode |
| `only TYPE...`、`at most N` | 原樣保留 allowed basis 或數值 constraint |
| `original`、`previous edit`、`current` | 選正確 baseline，不得互換 |

送出後仍需反查 result 的 mode、source/target、scope、constraints 與 complete。Prompt 有 avoid X，
但 command/result 沒有 X，即使 `status:ok` 仍是 routing 錯誤。

常見 routing 規則：

- `Which of A or B...`：分別查相同 metric，再明答 winner 或 tie；不能只貼兩份 report。
- 單一 gate 的 type 與 pin connections：使用 `gate_info`。
- 某些 gate types 的完整 gate/pin 清單：使用 `gates_by_type --gate-types ... --with-pins`。
- 多個 include types 放在同一個 `--gate-types` 後，語意為 OR；排除條件使用
  `--exclude-gate-types`，exclude 優先。
- constant-input gate 若要求 pin、其他 inputs 或 output，使用 `const_input_gates ... --with-pins`。
- 成功的 list/filter query 明確回傳 semantic count 0 時可回答沒有 matching objects；欄位缺失
  不能自行當作 0。
- net driver/load 要 exact pin、gate type 或 DFF pin role 時加 `--with-pins`；gate count 與 pin
  connection count 不得互換。
- `List DFFs driven by clock n0` 讀 DFF clock-pin loads，不使用全部 load 或總 fanout count。
- cone gate-type count 為空且 query complete、root 是 DFF.Q boundary 時，各 combinational gate
  type 為 0；不可穿透到 DFF.D。
- `structural_issues` 各分類直接讀對應 semantic count，不使用 `list entry count`。

## 7. Large Output 與時間

- 單一 prompt 的自然語言輸出預算以 4096 tokens 為基準。工具可自動將大型 payload 改寫為
  artifact，但不可移走 LLM 作答需要的 metadata。
- 除 prompt 明確要求前 N 筆、最多 N 筆或一個 witness，不得自行設定結果數量上限。
- LLM 不主動設定 `max_print`、`limit`、`max_paths`、`--max-results`、輸出檔名或 time limit。
- `max_print`、`limit` 等 display/page 參數不得改變 `all` 的語意；若有 pagination，必須取完。
- 官方 read/write 等 basic operation 上限為 60 秒，其餘 query/edit/optimization 為 300 秒；
  backend 預設保留 55/290 秒。官方 runner/watchdog 管理總時間，LLM 不分配跨 tool call budget。
- 一個 edit/optimization prompt 只呼叫一次主要 `edit_apply` 或 `opt_apply`；後續確認使用
  `report_query`、`depth_query` 或其他較輕 query。
- 只問 count 時優先使用 summary/count-only；只有 prompt 明確要求完整 records 才要求 list。
- backend 若有不可關閉的 hard cap，結果必須標成 incomplete，不能把達到 cap 當成全部。
- `path_query enumerate` 完整性讀 `complete:true` 與 `Complete enumeration: yes`。只問 path
  數量時使用 `-count_only`；要求全部 paths 時直接 enumerate，由 CLI 自動管理 artifact。
- `FunctionSearch FindAll` 使用 `--all`，不加入 `--max-results`；完整性讀 envelope 的
  `complete`、`truncated` 與 timeout/status。
- Boolean expression 的完整性讀 envelope 的 expression artifact status 與 counts；LLM 不讀
  artifact footer，也不重貼大型 equations。

## 8. Compound Prompt 執行順序

### Read-only analysis

```text
確認 design 已 read
-> 拆成 count/list/connectivity/depth/function 子問題
-> 分別呼叫 authoritative tool
-> 檢查每個 envelope
-> 完成可由 envelope/inline records 支持的最終推導
-> 大型 payload 附 output_file
```

### Transformation

```text
必要時先查 baseline
-> edit_apply
-> report_query last_edit
-> 檢查 report_success / rolled_back / validation
-> 題目要求功能不變時確認 operation certificate
-> 重新查詢 final property
-> write
```

### Optimization

```text
depth_query 取得 before depth
-> opt_apply 並重送 scope/basis/target constraints
-> report_query last_edit
-> 檢查 candidate_accepted、before/after、constraints、certificate 與 warnings
-> 必要時用 equiv_query 取得獨立 whole-design proof
-> 驗證 final design
-> write
```

Follow-up 問「上一步改了什麼」時使用 `report_query last_edit`，不要重做 edit。後續 prompt 要延續
先前 constraint 時，從對話中的前次 command 取回並在新 command 明確重送。

## 9. Error Recovery 與競賽作答

| 狀況 | 處理方式 |
|---|---|
| unknown/unresolved object | 用 StructureQuery info/list 確認正式名稱與物件種類後重試 |
| bus/port 解析失敗 | 保留完整 bit token；確認題目問 port count 還是 bit count |
| records paginated | 持續使用 next offset，直到結果完整 |
| path count timeout | 只問數量時改用 `-count_only` |
| path list timeout | 保留 all-path 語意；不自行加數量上限 |
| function search timeout | existence 改 find-any；all-pairs 保留完整性狀態 |
| SAT timeout/unknown | 縮小合法 scope，或用其他 completed query 交叉推定 |
| edit/optimization rollback | 不輸出失敗候選；保留 original 或最後接受版本 |
| `status:no_change` | 判斷 already optimal、原本符合要求或沒有合法 transformation |
| output file error | 改用可寫入的明確相對路徑後重試 |
| unsupported mode | 重新 routing 到可組合 public tools，不猜不存在的 command |

相同 command、參數與 design revision 已 timeout 時，不要原樣無限重試。先改 query 策略或合法
scope；仍無法完成時依下列 best-effort 規則作答。

作答優先順序：

```text
1. complete=true 的直接結果。
2. 較便宜但語意等價的 query，例如 count_only、summary、DP 或指定 scope。
3. 組合其他 completed query 交叉推導。
4. 根據 partial result、已證明下界、結構統計與相鄰 query，提交最可能的單一答案。
```

最後一級為 `BEST_EFFORT_INFERENCE`。正式答案仍需直接給數值、名稱、yes/no 或 netlist，不輸出
`unknown`、`maybe` 或只描述工具失敗。但 best-effort 不可竄改工具事實：不得把 partial 稱為
complete，也不得宣稱未完成的 SAT 已證明等價。

若精確答案需要讀 artifact records，而目前沒有專用 query，該能力是 coverage gap。競賽時可以
依上述規則猜測，但不能把猜測描述成工具已完成的精確分析。

## 10. Sequential、Edit 與 Equivalence

### Sequential boundaries

- DFF.Q 是 combinational start boundary；fanin 不回到同顆 DFF 的 D input。
- `D = EN & DATA` 沒有 Q feedback，不是 enable/hold，只能是
  `DATA_GATING_WITHOUT_HOLD_FEEDBACK` diagnostic。
- `sequential_query enable_hold` 使用 Q-cofactor functional proof；數量讀 `matched_dff_count`，
  並先確認 `complete:true`。
- enable/data 名稱為空不代表 non-match；可能是已證明但無單一具名 net 的 Q-free function。

### Edit and optimization

- 同時檢查 `status`、`complete`、`report_success`、`report_changed`、`rolled_back` 與 validation。
- 未通過 structure、constraint 與 operation certificate contract 的候選不得輸出；rollback 或
  rejected 時保留 original 或最後一個已接受版本。
- `CertifiedRewrite` 可描述為 qualified function-preserving rewrite；只有完整 `equiv_query` 的
  `WholeDesignSat` 可描述成 SAT-proven。
- `status:no_change` 可是合法 no-op 或 already optimal；不可假裝完成新的 rewrite。
- session 不會自動保存自然語言 constraint；後續 edit/opt 必須明確重送。
- `report_query last_edit` 只代表最近一次 edit/optimization，不能沿用更早的 delta。

### Equivalence scope

- `equiv_query original`：current 對最近一次 `read` 建立的 original snapshot。
- `equiv_query previous_edit`：current 對最近一次 edit/opt 前 snapshot。
- whole-design equivalence 比較 PO 與 DFF.D combinational boundaries，不含 initial state 或
  multi-cycle sequential equivalence。
- timeout、partial、unsupported 不是不等價證明；只有 complete mismatch 才能回答 NotEqual。

## 11. Functional Analysis Boundary

Boolean/function 類 tool 只能依各自文件已列出的 mode 與 complete status 使用。不得自行假設存在
任意 expression canonicalization、unbounded symmetry 或大型 functional merge。Boolean expression
若包含 DFF.Q，應描述為 top-level PI 加 current-state variables，不得把 state variable 稱為 PI，
也不得沿 Q 回追 D pin。DFF enable/hold 依 `SEQUENTIAL_QUERY_TOOL.md` 的 Q-cofactor 契約處理。
