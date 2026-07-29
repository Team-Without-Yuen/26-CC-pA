# tools.cpp / TOOLS_SPEC 待更新表

這份文件只記錄需要修改 `tools.cpp` parser/help/envelope printer 或
`TOOLS_SPEC/*.md` 的事項。

API facade、Netlist/engine、optimizer core、sequential pattern engine 等底層待辦，
請放在：

```text
API_SPEC/API_CORE_BACKEND_TODO.md
```

已公開且已同步完成的 CLI schema 不再列為 tools.cpp 待修改項。

## 目前已同步完成

以下功能目前不屬於 tools.cpp 待辦，只保留作為同步狀態摘要：

```text
sequential_query enable_hold functional fallback CLI 已完成
TOOLS_SPEC/SEQUENTIAL_QUERY_TOOL.md 已同步
tools.cpp parser / help / envelope printer 已同步

opt_query critical_path_depth 已完成
opt_apply critical_path_depth 已完成
TOOLS_SPEC/OPTIMIZATION_TOOL.md 已同步
report_query last_edit / equiv_query previous_edit cache 串接已完成
```

## 待 tools.cpp / TOOLS_SPEC 同步項目

### T1：DFF.Q fanin boundary 的公開語意同步

依官方 QA，DFF.Q/register output 的 fanin cone 應視為 combinational boundary。
底層行為完成後，tools 層需同步：

```text
- cone_query net_fanin <DFF.Q> 的 help / spec / envelope 範例改為 empty cone。
- scoped edit/optimization 對 empty cone 的 response 語意改為 verified no-op。
- 移除或 deprecated resolved_through_dff_data_pin 公開輸出欄位。
- 更新 TOOLS_SPEC/CONE_QUERY_TOOL.md。
- 更新 TOOLS_SPEC/OPTIMIZATION_TOOL.md。
- 更新相關 CLI regression expected output。
```

底層實作細節請見 `API_CORE_BACKEND_TODO.md` 的 P0-1。

### T2：跨 prompt persistent constraints 的 tools 顯示與文件

底層 session constraint state 完成後，tools 層需同步：

```text
- edit_apply / opt_apply envelope 顯示本次 commit 前驗證了哪些 accumulated constraints。
- write envelope 顯示 final constraint validation 結果。
- report_query last_edit 顯示 constraint validation failure reason。
- TOOLS_SPEC/EDIT_APPLY_TOOL.md 補 accumulated constraint 判讀規則。
- TOOLS_SPEC/OPTIMIZATION_TOOL.md 補 constraint-preserving optimization 判讀規則。
- TOOLS_SPEC/SESSION_AND_IO_TOOL.md 補 write 前 final validation 語意。
```

底層設計請見 `API_CORE_BACKEND_TODO.md` 的 P0-2。

### T3：AND-only enable/hold candidate 的公開輸出同步

底層 sequential engine 更新後，tools 層需同步：

```text
- sequential_query 預設輸出不得把 AND-only data gating 顯示為 enable/hold match。
- matched_dff_count / candidate_dff_count 的 help 與文件需明確排除 AND-only non-match。
- 若仍輸出 diagnostic record，欄位名稱需明確標示 non-match，不得使用 confirmed/candidate ambiguity。
- 更新 TOOLS_SPEC/SEQUENTIAL_QUERY_TOOL.md。
- 更新 sequential_query CLI regression expected output。
```

底層語意請見 `API_CORE_BACKEND_TODO.md` 的 P1-1。

### T4：官方 QA 已符合項目的文件同步

以下事項目前行為已符合或主要由底層保證；若文件仍有舊描述，tools 文件需同步：

```text
- 名稱查詢只看 current transformed netlist，不維護 original aliases。
- verified no-op / zero replacement 可接受。
- cost 必須讀 final constrained candidate metrics。
- intermediate helper gate 不受 final basis 限制。
- transformation log 可簡短；generated Verilog 與 final validation 才是評分依據。
```

### T5：Path enumerate 效能與 endpoint 寫法文件同步

底層 `path_query enumerate` 已改成 streaming output，並新增 reverse reachability pruning 與
`count_only` memoized path-count DP。
核心測試完成後，tools/LLM-facing 文件需同步：

```text
- 說明 -max_paths / maxEnumeratedPaths 只保留相容，不再截斷完整 enumeration。
- 說明大型 enumerate 主要由 -time_limit 控制；timeout/partial 不可回答成完整列表。
- 說明 bus-bit endpoint 應使用 net:n0[0] / net:n63[1]，不要用 pi:n0[0] / po:n63[1]。
- 補上 count_only 與 -out -max_print 0 的建議使用情境。
- 說明 count_only 在無 required nodes 時會走 DP count；完整列出 paths 仍受輸出檔大小限制。
- 說明大型完整 path list 若 timeout，LLM 只能回報 partial count / complete=false / output_file，不可宣稱已列出全部。
- 記錄 test37 all-DFF streaming 30 秒只寫出約 2.94M paths、暫存檔約 1.5GB，完整列出不適合放進 answer log。
```

### T6：Edit apply report 判讀與 redundant-gate dispatch 文件同步

底層代表性 edit command 已重跑確認：常用 rename / buffer insertion /
constant simplification / double-inverter collapse / technology mapping 目前皆能回
`functionally_equivalent: true`，無不等價 committed state。tools/LLM-facing 文件需同步：

```text
- edit 類回答必須檢查 status、complete、report_success、report_changed、functionally_equivalent。
- status=timeout/error 或 report_success=false 時，不得宣稱 transformation completed。
- report_query last_edit 在上一個 edit 失敗時只能回答失敗/未套用/rollback，不得拿舊 delta 回答本題。
- redundant gates prompt 優先使用 merge_structurally_equivalent_gates。
- merge_functionally_equivalent_gates whole 屬較重 SAT/functional search；只有 prompt 明確要求 functional-equivalent merge 時才使用，且必須處理 timeout。
- 已測 test38 structural merge 約 0.14 秒、active_gate_count_delta=-15、equivalent=true；同 case functional merge 30 秒 timeout 但 changed=false。
```

### T7：Equivalence query 判讀與 timeout detail 文件同步

新版 `equiv_query original/previous_edit` 代表 testcase 已重跑確認可用。
tools/LLM-facing 文件需同步：

```text
- 只有 status=ok、complete=true、equivalent=true 時，才能回答「已證明等價」。
- status=timeout/error/unsupported 或 complete=false 時，不得把 equivalent=false 解讀成「已證明不等價」。
- previous_edit 需要先有成功 edit/opt baseline；沒有 baseline 時是 request error。
- original baseline 由 read 建立；edit 後可用 equiv_query original 驗證 current vs loaded design。
- timeout envelope 目前可能列出大量 skipped PO / DFF.D names；後續 printer/spec 應限制 detail 或建議 summary-only。
- 已測 test33/test37/test40 representative original equivalence 均可在 60 秒內 complete=true/equivalent=true。
```

### T8：Cone query 大型輸出 policy 同步

已重跑 released cone prompts，runtime 不是 blocker，但 printer 目前會直接輸出完整
`Cone gates` / `Cone nets`，hidden 大 cone 可能造成 answer log 膨脹。tools/LLM-facing
文件需同步：

```text
- cone_query 目前可穩定回 gate_count、net_count、gate type counts。
- 若 prompt 只問數量或 type breakdown，LLM 不應把完整 Cone gates / Cone nets 搬進答案。
- 若 prompt 明確要求 list all gates，才使用完整 list。
- 後續可考慮在 tools.cpp parser/printer 加 max_print 或 summary_only；在此之前先由 LLM policy 控制。
- 已測最大 released representative：test18 net_fanin n12，約 3,752 gates / 3,818 nets，envelope 約 65KB。
```

### T9：Boolean expression 長輸出 policy 同步

released Boolean-expression prompts 已重跑確認可用，但 full expression printer 目前會直接輸出完整字串。
tools/LLM-facing 文件需同步：

```text
- func_query boolean_expression 會回 expression 與 expression length。
- released representative expression 很短；最大為 test35 n25，expression length=60。
- hidden 大 cone 可能產生很長 expression；LLM 必須先檢查 expression length。
- expression 太長時，改用 func_query simplified_expression <net> <max_depth> 或只回摘要。
- simplified_expression 會標示 expression depth limited=yes，不能把它宣稱為完整 Boolean equation。
- 後續可考慮在 tools.cpp parser/printer 加 max_length 或 file-output；目前先由 LLM policy 控制。
```

## 不放在本表的事項

下列項目不是 tools.cpp 待修改，請不要再混入本文件：

```text
- Netlist public API facade 設計。
- optimizer core runtime / cancellation。
- mockturtle primitive cooperative cancellation。
- cone-isolated resynthesis。
- sequential pattern engine candidate ranking / SAT session 策略。
- accumulated hard constraint 的 session model。
- 底層 structure validation / rollback transaction 行為。
```
