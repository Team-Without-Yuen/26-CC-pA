# tools.cpp / TOOLS_SPEC 待更新表

這份文件只記錄需要修改 `tools.cpp` parser/help/envelope printer 或
`TOOLS_SPEC/*.md` 的事項。

API facade、Netlist/engine、optimizer core、sequential pattern engine 等底層待辦，
請放在：

```text
API_SPEC/API_CORE_BACKEND_TODO.md
```

已公開且已同步完成的 CLI schema 不再列為 tools.cpp 待修改項。

## 短期盤查：完整輸出與 best-effort 作答

競賽策略已改為「優先取得完整答案；完整計算失敗時仍提交明確候選答案」。文件層已在
`TOOLS_SPEC/LLM_NOTES.md` 定義 Competition Answer Policy。tools 層仍需逐一盤查：

```text
- 所有 list/find-all mode 是否有 silent hard cap。
- 有 hard cap 的輸出是否提供 records_truncated、next offset、streaming file 或可重入續查。
- terminal max_print / summary_only 是否只影響顯示，不影響實際計算。
- timeout/partial 是否保留 confirmed records、partial count、stop reason 與 output file。
- tools help 是否能讓 LLM 找到完整輸出與 pagination 的正確參數。
```

在盤查完成前，不宣稱「任何 list 題都不可能截斷」。best-effort inference 屬 LLM/testcase
driver 策略，不應在 tool envelope 內把 partial 改標為 complete。

### 盤查結果

```text
已可由現有 CLI 取得完整結果：
- Structure/Cone/Depth/Edit/Equivalence lists：目前 printer 全量輸出。
- path_query enumerate：count_only 或 streaming -out。
- path_query direct_pi_po：先讀 total，再以 total 重送 -max_print。
- sequential_query detail：all-DFF 預設自動完整寫入唯一 artifact；明確 offset/limit 才使用 pagination。
- func_search FindAll：預設不限量，完整 records 自動 streaming 至唯一 artifact。

仍需評估 tools.cpp/parser/help/envelope：
- func_query boolean_expression 沒有 file-output / output-length policy。
- Structure/Cone/Edit/Equivalence 大清單雖不截斷，但只有 terminal 全量輸出，
  尚無 summary/file/pagination 選項。
```

### 可能產生大型結果的功能

| Tool / mode | 大型內容 | 現有控制 | 目前作答方式 | 是否需要討論修改 |
|---|---|---|---|---|
| `path_query enumerate` | 所有 A-to-B paths，可能指數成長 | `-out` streaming、`-count_only`、time limit | 有 `-out` 時只簡答 total/complete/file | 否，現有架構可用 |
| `path_query direct_pi_po` | 所有 direct PI-PO connections | `-max_print` 只控制顯示 | 先取 total，再以 total 重送 | 可考慮補 `-out` |
| `sequential_query enable_hold all` | 全部 DFF detail records | 預設自動 artifact；offset/limit compatibility | 回 artifact count / complete / output_file | 否，已完成 |
| `func_search nand_pair --all` | 大量 signal pairs | 預設不限量；streaming artifact | 回 match_count / complete / output_file | 否，已完成 |
| `func_search equivalent_pairs --all` | equivalence classes 與展開 pair records | 緊湊 classes + streaming pair artifact | 回 class/pair count / complete / output_file | 否，已完成 |
| `structure_query list_* / gates_by_type` | 全部 gate/net/port names | terminal 全量輸出 | count 題只簡答；list 題目前全量讀 terminal | 視 hidden size 決定是否補 `-out` |
| `structure_query structural_issues / fanout_violations` | 大量問題 nets/gates | terminal 全量輸出 | count 題只簡答；list 題全量輸出 | 可考慮共用 list file output |
| `cone_query net/gate fanin/fanout` | 大型 cone gate/net lists | terminal 全量輸出 | count/type 題只簡答；list 題全量輸出 | 建議討論 `summary_only` / `-out` |
| `depth_query all_po/all_dff_d/exceeding` | 大量 endpoint/depth records | terminal 全量輸出 | count 題只簡答；list 題全量輸出 | 可考慮 `-out` |
| `func_query boolean_expression` | expression 可能極長 | terminal 全量輸出 | 目前無 file output | 後期 Boolean/AIG 重構處理 |
| `func_query support_pi` | 大型 support list | terminal 全量輸出 | list 題全量輸出 | 低優先，可共用 `-out` |
| `edit_apply` / `report_query last_edit` | changed names、merge records、validation messages | terminal 全量輸出 | delta 題只簡答；detail 題全量輸出 | 可考慮 report file output |
| `equiv_query` | matched/skipped boundary names | terminal 全量輸出 | yes/no 題只簡答；mismatch detail 全量輸出 | 可考慮只印 mismatch 或 `-out` |

討論優先順序：

```text
P0: cone_query / structure_query 的共用 summary-only 與 file-output。
P1: depth/edit/equivalence 的大型 detail file-output。
P2: Boolean expression，留待 AIG/Boolean 重構。
```

## 已完成的 tools.cpp 同步項目

本輪已完成並通過 Full regression：

```text
- T1 DFF.Q fanin boundary：公開移除 resolved_through_dff_data_pin；
  DFF.Q net fanin 不含 combinational gate，root net 仍可出現在 cone report。
- T2 persistent constraints：由 LLM/testcase driver 保存並在後續 command 重送；
  規則已寫入 LLM_NOTES、EditApply、Optimization 與 Session 文件。
- T3 AND-only data gating：DataGatingWithoutHoldFeedback 是 non-match，
  不計入 matched_dff_count 或 candidate_dff_count。
- T4 QA 規則：no-op、constraint 與 final validation 判讀已同步。
- T5 path enumerate：streaming/count_only/time-limit 說明完成；-max_paths
  明確是 legacy compatibility，不截斷完整 enumeration；test10 已更新。
- T6 edit report：status/complete/report/rollback 與 functional merge timeout
  判讀已同步。
- T7 equivalence：original/previous_edit、timeout/partial/mismatch 判讀已同步。
- T8 cone 大輸出：summary-first LLM policy 已同步；尚未新增 CLI max_print 或
  summary_only，因目前由 LLM policy 控制。
- T9 Function Search FindAll：預設移除 hidden result cap，完整 records 自動
  streaming 至不覆寫的 artifact；envelope 回 match_count、stored_match_count 與
  output_file；test24 9/9、test26 18/18，NewTestCase test29/30/35 實測通過。
- T10 Sequential Pattern all-DFF detail：未指定 summary/offset/limit 時自動完整寫入
  不覆寫的 artifact；single-DFF 與明確 pagination 維持原行為。test21 18/18，test40
  1583/1583 records，envelope 由約 3.23 MB 降至 1292 characters。
```

驗證：`scripts/run_tools_regression.ps1 -Profile Full`，24/24 PASS。

## 後期 AIG / Boolean 重構前暫不推進

Boolean-function 類問題目前不列為短期 tools.cpp / TOOLS_SPEC 擴充目標。
現有 command 可照目前能力使用，但以下「完整化」工作等 AIG functional index
重構後再統一處理：

```text
- sequential_query enable_hold 的 functional fallback default policy。
- Q-free EN/DATA decomposition 與任意 Boolean-equivalent enable/hold pattern。
- func_query boolean_expression 的 canonical expression、large-output、max-length/file-output。
- func_query symmetry / equivalence / constant function 的共用 SAT/AIG cache policy。
- merge_functionally_equivalent_gates 等 functional-equivalent cleanup/edit 的大型 testcase 策略。
```

## 不放在本表的事項

下列項目不是 tools.cpp 待修改，請不要混入本文件：

```text
- Netlist public API facade 設計。
- optimizer core runtime / cancellation。
- mockturtle primitive cooperative cancellation。
- cone-isolated resynthesis。
- sequential pattern engine candidate ranking / SAT session 策略。
- accumulated hard constraint 的 session model。
- 底層 structure validation / rollback transaction 行為。
```
