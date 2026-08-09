# TOOLS_SPEC LLM 文件完整性盤查

## 1. 盤查目標

確認只取得 `TOOLS_SPEC` 與 `tools.exe` 的 LLM，能從 prompt 判斷 public tool、組裝
必要輸入，並從實際輸出欄位形成答案。盤查依
`API_SPEC/DOCUMENTATION_FORMAT/TOOL_DOCUMENT_TEMPLATE.md`，但不要求為了章節數量重複內容。

## 2. 必要資訊

每份 `*_TOOL.md` 至少必須清楚提供：

1. 責任與非責任。
2. 選擇條件與易混淆工具。
3. 可直接送入 `tools.exe` 的 command grammar。
4. required/optional arguments、default 與適用 mode。
5. 實際輸出 labels、complete/status 判讀與作答欄位。
6. 接近官方 prompt 的 command examples。
7. 組合流程與已知限制。

## 3. 文件狀態

| 文件 | 初始狀態 | 本輪處理 |
|---|---|---|
| `STRUCTURE_QUERY_TOOL.md` | mode tables 有輸出，但缺集中判讀 | 補 port/bit、active count 與 structural constant 判讀 |
| `CONE_QUERY_TOOL.md` | 完整 | 不增加重複內容 |
| `DEPTH_QUERY_TOOL.md` | 完整 | 不增加重複內容 |
| `FUNCTION_QUERY_TOOL.md` | 完整 | 不增加重複內容 |
| `FUNCTION_SEARCH_TOOL.md` | 完整 | 不增加重複內容 |
| `SEQUENTIAL_QUERY_TOOL.md` | 完整 | 不增加重複內容 |
| `OPTIMIZATION_TOOL.md` | 完整 | 不增加重複內容 |
| `PATH_QUERY_TOOL.md` | grammar/output 分散 | 整理 grammar、options/default 與輸出判讀 |
| `EDIT_APPLY_TOOL.md` | 各 mode 有語法，但缺統一 grammar/輸入規則 | 補統一 command grammar 與 required inputs |
| `EQUIVALENCE_QUERY_TOOL.md` | 缺選擇條件 | 補 original/previous_edit 與 func_query 邊界 |
| `REPORT_QUERY_TOOL.md` | 缺選擇條件與 follow-up 優先規則 | 補 cache/report routing 規則 |
| `SESSION_AND_IO_TOOL.md` | 缺選擇條件與統一輸出判讀 | 補 session prerequisite、status 與 output fields |

## 4. 驗證要求

- 文件 grammar 必須與 `tools.exe` 的 `help` 一致。
- 範例 command 必須通過 public parser；物件不存在可以回 object error，但不得回 grammar error。
- 文件列出的主要 data label 必須在對應成功結果中存在。
- routing-only 測試需保存 prompt、raw command、scope、constraint 與 objective。
- 文件修改後先重跑受影響 prompt，再進行 40 個 testcase 的完整封閉測試。

## 5. 本輪驗證結果

### Public help 與文件 grammar

已執行 `tools.exe help`，確認下列 public families 與更新文件一致：

```text
read / write
structure_query / cone_query / path_query / depth_query
func_query / func_search / sequential_query
edit_apply / opt_query / opt_apply
report_query / equiv_query
```

12 份 `*_TOOL.md` 均已具備責任、選擇條件、command grammar、輸出判讀、prompt examples
與限制。章節可合併，但 required/default/作答欄位必須可直接找到。

### 代表性 runtime 驗證

使用 `NewTestCase/test31` 與 `test35` 實際驗證：

```text
read
edit_apply rename_net
report_query last_edit
equiv_query previous_edit
write
path_query exists pi:<name> po:<name>
```

結果皆為 `status:ok`, `complete:true`。rename report 的 `report_success/report_changed`、
equivalence 的 `equivalent`、write 的 `output_file/written` 與 path 的 Yes/No data 均與文件一致。

驗證 log：

```text
Testing/tools-spec-doc-validation/session_edit_report_equiv.log
Testing/tools-spec-doc-validation/session_path.log
```

### NewTestCase routing-only

使用同步後的 `TOOLS_SPEC` 與保守 literal router，對 `test01` 至 `test40` 全部 prompt
執行 routing-only，不呼叫 API core：

| 項目 | 結果 |
|---|---:|
| Testcase | 40 |
| Prompt | 459 |
| Routed command | 426 |
| Unresolved | 0 |
| Runner/tool timeout | 0 |

額外檢查 scoped gate count、constant 0/1、global/cone objective、PI/PO endpoint 與
`net_fanin output/input` 非法 scope token，違規數為 0。

結果位置：

```text
isoCadTest/routing_only_all_v2/
```

## 6. 結論

目前個別工具文件的輸入、輸出與 routing 資訊已足以覆蓋公開 NewTestCase prompt；沒有
發現新的文件型 command 缺口。下一步不再繼續增加文件篇幅，應處理 sidecar 歸檔與
Path Query 大型 enumeration timeout，再以完整 execution run 驗證 API core。
