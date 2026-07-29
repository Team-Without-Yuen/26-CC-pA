# 待盤查 Functions

本文件整理目前從 `testcase_toolans` 掃描到的可疑工具函式與模式。判斷依據包含：

- `tool_calls.txt` 出現 `Result status: error / timeout / partial / unsupported`
- `Complete: false`
- tool call 缺少 `Result status` 或 `Complete`，疑似外部截斷
- answer log 出現 `Functional equivalence: false`
- answer log 缺少對應的 `#RESPONSE`

## P0：優先盤查

### Boolean / AIG 重構後再處理項目

目前決策：

- Boolean-function 類問題暫時不列為短期修正目標。
- 後期若要完整支援 hidden testcase 中的任意 Boolean equivalent pattern，應先建立 AIG functional index，再統一處理 SAT / cofactor / equivalence / symmetry / enable-hold decomposition。
- 現階段只保留既有可回答的部分答案；不為這類問題再補臨時 heuristic，避免局部修補和後期 AIG 架構衝突。

包含項目：

- `sequential_query enable_hold` 的 functional fallback / Q-free EN-DATA decomposition。
- `func_query boolean_expression` 的大型 expression 控制與 canonical Boolean 表示。
- `func_query symmetry` 的完整 Boolean-level symmetry。
- `func_query equivalence` / constant function 類 SAT cache 與共用 functional index。
- `merge_functionally_equivalent_gates` 等依賴全域 Boolean equivalence 的 edit/cleanup。

### `sequential_query enable_hold`（後期 AIG 重構）

相關 QA：

- Q46：MUX enable/hold 應以 Boolean equivalence 判斷，不限特定 structural pattern。
- Q47：`D = EN & DATA` 不算 enable/hold；必須是含同一 DFF.Q feedback 的 hold structure。
- Q69.1：EN/DATA 不一定要是 netlist 中現成 net；Q-free Boolean decomposition 可接受。

相關 testcase：

- `test19`
- `test21`
- `test40`，特別是 Q69 提到的 `g1274`

盤查結果：

- 已確認：底層 `FunctionalPatternEngine` 有 functional fallback，可用 SAT 證明 control 在 hold level 時 `D == Q`。
- 已確認：若 hold cofactor 可證明，但找不到現成 named DATA net，目前仍可回 functional match，狀態類似 `HOLD_PROVEN_DATA_UNRESOLVED`；這符合 Q69.1 的 Q-free decomposition 方向。
- 已實測：`NewTestCase/test40` 的 `sequential_query enable_hold g1274 --functional-fallback ...` 可找到 enable/hold pattern。
- 仍有問題：`SequentialPatternQuery::enableFunctionalFallback` 預設是 `false`，LLM 若只呼叫 `sequential_query enable_hold all`，可能漏掉 hidden 中非 canonical / De Morgan / function-level MUX-hold。
- 已修正：AND-only data gating 改為 `DataGatingWithoutHoldFeedback` non-match diagnostic，不再使用 `semanticsPending=true`，也不計入 `matched_dff_count` / `candidate_dff_count`。

後期處理方向：

- 不在現階段調整 default policy。
- 等 AIG functional index 建立後，再統一支援 `D = EN ? DATA : Q` 的 Boolean decomposition。
- 後期 regression 應包含 `test40 g1274`、De Morgan / NAND / NOR restructuring，以及 `EN`、`DATA` 不是實體 net 的情況。

### `cone_query net_fanin <DFF.Q>` / scoped rewrite scope

相關 QA：

- Q21.2：fanin depth 只看 combinational depth，DFF.Q 視為 primary input。
- Q65：若分析或最佳化 DFF.Q/register output 的 fanin cone，應把它當作 boundary，不得轉到同顆 DFF 的 D-input cone。

盤查結果：

- 已修正：`resolveRewriteScope()` 的 `TargetScope::NET_FANIN` 已移除 DFF.Q -> D pin data cone 特例。
- 已確認：`cone_query net_fanin <DFF.Q>` 仍停在 sequential boundary。
- 已確認：scoped edit / scoped optimization 若 scope 是 DFF.Q fanin cone，會視為 empty combinational cone，不再改到 D-input cone。

目前驗證：

- `NewTestCase/test26` 的 `n10` 是 DFF.Q。
- `cone_query net_fanin n10` 回 `gates: 0`、`nets: 1`。
- `edit_apply convert_basis net_fanin n10 -allow NOR NOT --validate_equivalence` 回 `no_change`、`changed=false`。
- `opt_apply critical_path_depth --scope net_fanin n10 --objective cone --allowed NOR NOT --time-limit 5` 回 already optimal、`before_depth=0`、`after_depth=0`、`resolved_through_dff_data_pin=false`。

後續只剩 tools-facing 文件同步：

- 移除或 deprecated `resolved_through_dff_data_pin` 類公開語意。
- 將 `TOOLS_SPEC` 中 DFF.Q-to-D-pin 舊描述改為 boundary no-op。此項已列入 `API_SPEC/TOOLS待更新表.md`。

### persistent constraints across prompts

相關 QA：

- Q63：多個 transformation request 之間，前面建立的 structural constraints 在後續 transformation 後仍是 hard constraints。

目前決策：

- 先不做底層 automatic persistent constraint state。
- 「前一題 constraint 是否延續」屬於 prompt/testcase 語意，應由 LLM 或 testcase driver 維護。
- 工具層只保證單次 request 明確傳入的 constraint 會被驗證；若後續 edit/opt 需要保留前題限制，LLM 必須重送 allowed/banned/fanout 等 constraint，或在修改後額外呼叫 query/equiv 驗證。

後續文件處理：

- 已移出 core/backend 待辦。
- 已在 `API_SPEC/TOOLS待更新表.md` 標記為 LLM 使用策略文件項目。
- 之後統一更新 tools-facing 文件時，加入「LLM 必須記住並重送前題 hard constraint」的 policy。

### `path_query enumerate`

相關 testcase：

- `test12 #6/#7`
- `test14 #5/#6`
- `test18 #5`
- `test19 #6`
- `test32 #6`
- `test37 #9`

疑似問題：

- 已修正：`EnumerateAll` 改為 streaming 寫檔，不再先累積所有 paths 後一次寫出。
- 已修正：`maxEnumeratedPaths` / `-max_paths` 不再作為截斷條件；完整列舉主要由內部 `-time_limit` 控制。
- 已修正：新增 reverse reachability pruning，每個 start/end pair 先反向標記可抵達 endpoint 的 nets，DFS 不再探索無法抵達 endpoint 的分支。
- 已修正：`count_only` 且沒有 required nodes 時使用 memoized path-count DP；all-DFF 多 start/end count 可重用同一 endpoint 的計數結果。
- 已驗證：`test14 net:n0[0] -> net:n63[1]` 可完整列舉 289,366 paths；`count_only` 約 0.4 秒，streaming `-out` 約 1.7 秒，輸出檔約 226MB。
- 已驗證：`test12 #5/#6`、`test18 #5/#6`、`test19 #5/#6`、`test32 #19` 的 `count_only -time_limit 30` 均完整完成。
- 已驗證：`test32 #6 all_dff_q -> all_dff_d` 可完整 count 1,176,523 paths；`count_only` 約 0.4-0.6 秒，streaming `-out` 約 10 秒，輸出檔約 745MB。
- 已驗證：`test37 #8 all_dff_q -> all_dff_d` 可完整 count 16,548,172 paths；`count_only -time_limit 30` 約 1.2-1.7 秒完成。
- 已驗證：`test37 #8 all_dff_q -> all_dff_d` streaming `-out -time_limit 30` 會正確回傳 `timeout/complete=false`；30 秒 partial 約 2,936,293 paths，暫存輸出檔約 1.5GB。
- 新增證據：`run_tools_regression.ps1 -Profile Full` 目前只剩 `mini test/test10` 的 path-limit expected failure；工具輸出為完整列舉 `status=ok / complete=true / Path limit reached: no`，但舊測試仍期待 `-max_paths` 造成 partial/truncated。
- 仍需注意：若 prompt 要完整列出 `test37` 等級的所有 paths，時間與檔案大小受輸出量下限限制；count 已解決，但 streaming 大輸出不能靠 DP 消除。

建議檢查：

- 決定 `-max_paths` 的正式語意：若保留為 display/printing cap，則更新 `test10` expected；若要恢復 enumeration cap，需重新確認與「完整算完」策略不衝突。
- 確認 `-time_limit` 是否真的在工具內部生效。
- 確認 timeout 時會回傳完整 `timeout` envelope，且不依賴外部截斷。
- 對完整 path list 題目確認 output file policy，避免 LLM 把巨大 `-out` 檔案內容搬進答案 log。
- LLM 工具說明需明確禁止大型 testcase 直接做完整 path enumeration，除非工具能證明完整完成。

### `edit_apply`

相關 mode：

- `replace_type`
- `convert_basis`
- `collapse_double_inverter`
- `remove_dangling_logic`
- `trim_dead_logic`
- `merge_functionally_equivalent_gates`
- `rename_net`
- `insert_buffers_on_each_load`
- `insert_buffers_for_net`
- `simplify_constants`

相關 testcase：

- `test27 #4/#5/#6`
- `test29 #4/#5/#6/#7`
- `test31 #5/#12/#14`
- `test38 #4/#7/#9/#13/#16`
- `test39 #5/#8/#12/#15/#18`
- `test40 #4/#5/#7`

疑似問題：

- 已重跑代表命令：`collapse_double_inverter`、`rename_net`、`insert_buffers_on_each_load`、`insert_buffers_for_net`、`simplify_constants OR/NOR`、`replace_type` 目前皆回傳 `success/no_change` 且 `functionally_equivalent: true`。
- 舊 `testcase_toolans` 中多個 `error false` 較可能來自舊版工具或舊 LLM 呼叫流程；新版工具未重現不等價 commit。
- 已確認：`merge_functionally_equivalent_gates whole --time-limit 30` 在 `test38` 會 timeout，report 為 `success=false / changed=false / complete=false`，屬安全 failure，不會留下 committed 不等價設計。
- 已確認：`test38` 的「redundant gates」類 prompt 可由 `edit_apply merge_structurally_equivalent_gates` 快速處理，約 0.14 秒，`active_gate_count_delta=-15`，`functionally_equivalent=true`。
- 仍需注意：若 edit 失敗或 timeout，後續 `report_query last_edit` 只能用來回答「上一個 edit 失敗/未套用」，不能回答「移除了幾個 / 新增了幾個」。

建議檢查：

- 對 redundant gate prompt，優先 dispatch 到 `merge_structurally_equivalent_gates`；只有 prompt 明確要求 functional-equivalent merge 時才用 `merge_functionally_equivalent_gates`。
- LLM answer policy 必須檢查 `status/complete/report_success/report_changed/functionally_equivalent`，不得只看 command 是否有回傳 envelope。
- 若 `status=timeout/error` 或 `report_success=false`，必須宣告未套用或已 rollback，不得宣稱 transformation completed。

### `opt_apply critical_path_depth`

相關 testcase：

- `test33 #19`
- `test40 #16`

疑似問題：

- `test33` 出現 timeout。
- `test40` tool call 缺少完整 status，疑似外部截斷。
- 需同步官方 Q65：DFF.Q/register output 的 fanin cone 是空的 combinational boundary，不應自動展開到同一個 DFF 的 D input cone。

建議檢查：

- 修正 DFF.Q fanin boundary 語意。
- 若 scope 是 DFF.Q 的 fanin cone，cone depth 應為 0，optimization 應 verified no-op。
- Optimization 必須在內部時限內回傳完整 report。

## P1：需要盤查

### floating primary output preservation

相關 QA：

- Q69.3：undriven primary outputs must remain undriven in the output netlist。

相關 testcase：

- `test25`：`n13[3:0]` 是 undriven/floating primary output。

盤查結果：

- 已確認：`VerilogWriter` 只宣告 floating PO，不會補 driver。
- 已確認：`MockturtleConverter` 對 old PO 無 driver 且非 PI 的情況會保留 floating form，不插 BUF/NOT。
- 已確認：`removeDanglingLogic()` 會把 PO net 當 useful root，不會移除 floating PO。
- 已實測：`NewTestCase/test25` read/write 後 `n13[3:0]` 仍是 undriven/floating。
- 已實測：`trim_dead_logic` 後 write/read，`n13[3:0]` 仍是 undriven/floating。
- 已補 regression：`mini test/test34` 覆蓋 scalar floating PO、bus floating PO、`trim_dead_logic`、`remove_unused_nets`、write/read preservation，以及 CLI response envelope。
- 已接入 regression runner：`scripts/run_tools_regression.ps1 -Profile Tools/Full` 會執行 `test34`。

建議檢查：

- 目前不需要大改 core。
- 後續若修改 writer / mockturtle conversion / cleanup / optimization，需重跑 `mini test/test34` 或 Tools profile，避免改壞 floating PO preservation。

### `cone_query net_fanin / net_fanout`

相關 testcase：

- `test16 #13/#14`
- `test17 #13/#14`
- `test18 #12/#13`
- `test19 #14`
- `test20 #14`

疑似問題：

- 已重跑代表 released cone prompts：`test16/test17/test18/test20/test31/test32/test33/test37/test40` 相關 `net_fanin/net_fanout/shared_fanin/largest_output` 皆回 `status=ok / complete=true`。
- runtime 目前不是 blocker；所有代表測試約 0.02-0.23 秒完成。
- 最大代表輸出為 `test18 cone_query net_fanin n12`，約 3,752 gates / 3,818 nets，answer envelope 約 65KB。
- 仍需注意：`cone_query` printer 目前會直接輸出完整 `Cone gates` / `Cone nets`，沒有 max-print 或 summary-only CLI；hidden 大 cone 仍可能讓 answer log 膨脹。

建議檢查：

- 後續 tools/LLM-facing 層應加入或文件化 summary-only / max-print policy。
- LLM 若只問 cone gate count / gate type count，應只使用 summary 欄位，不要把完整 `Cone gates` / `Cone nets` 搬進答案。
- 若 prompt 明確要求 list all gates，允許列出；但大型 hidden case 應有 output cap 或 file-output policy。

### `equiv_query original`

相關 testcase：

- `test33 #14`
- `test40 #8`

疑似問題：

- 已重跑代表場景：`test32 original after rename`、`test31 previous_edit after rename`、`test33 original after edits`、`test37 original after trim`、`test40 original after NAND/NOT mapping` 皆回 `status=ok / complete=true / equivalent=true / method=WholeDesignSat`。
- 已確認：沒有 previous edit baseline 時，`equiv_query previous_edit` 會回 `status=error / complete=false`，不會誤判為 false。
- 已確認：極短 time budget 會回 `status=timeout / complete=false / time_budget_exceeded=true`，不能解讀為不等價。
- 仍需注意：timeout envelope 目前會列出大量 skipped PO / DFF.D endpoint names；大型 design 可能造成 answer log 膨脹，tools/LLM-facing 層應限制 detail 或改用 summary。

建議檢查：

- LLM answer policy 必須只在 `status=ok && complete=true && equivalent=true` 時回答「已證明等價」。
- `status=timeout/error/unsupported` 或 `complete=false` 時，不得把 `equivalent=false` 欄位解讀成「已證明不等價」。
- timeout / skipped endpoint detail 需要後續在工具文件或 printer 中設定輸出上限。

### `func_query boolean_expression`（後期 AIG 重構）

相關 testcase：

- `test31 #10`
- `test34 #7`
- `test35 #16`
- `test37 #13`
- `test39 #11`

盤查結果：

- 已重跑 released Boolean-expression prompts，皆回 `status=ok / complete=true`。
- runtime 目前不是 blocker，代表測試約 0.05-0.22 秒。
- released full expression 都很短：`test31 n16` 長度 3、`test34 n30` 長度 3、`test35 n25` 長度 60、`test37 n8` 長度 2、`test39 n12` 長度 3。
- `simplified_expression <net> <max_depth>` 已可用，會標示 `expression depth limited: yes`；代表測試可穩定回傳 depth-limited expression。
- 仍需注意：`boolean_expression` printer 會直接輸出完整 expression，hidden 大 cone 可能 expression 爆炸。

建議檢查：

- 現階段不擴充完整 Boolean expression engine。
- 後期 AIG 重構後，再定義 expression length policy、canonical expression、depth-limited expression，以及必要的 file-output / max-length 機制。

### `path_query max_depth all_dff_q all_dff_d`

相關 testcase：

- `test40 #11`

盤查結果：

- 已修正：無 required/avoid constraints 的 `PathQueryMode::MaxDepth` 已加入 multi-source / multi-endpoint DP-style longest-path engine，不再逐 start/end pair 呼叫 DFS。
- 已驗證：`path_query max_depth all_dff_q all_dff_d` 在 `NewTestCase/test40` 由修正前 70 秒外層 timeout，降為約 0.5 秒完成。
- 已驗證：修正後 `NewTestCase/test40` 回傳 `Depth: 104`，與 `depth_query all_dff_d` 的 worst depth 104 一致。
- 已驗證：`mini test/test33` 已加入 test40 all-register max-depth regression，目前 10 passed / 0 failed。
- 已驗證：Tools regression profile 9 passed / 0 failed。

仍需注意：

- 帶 required/avoid constraints 的 MaxDepth 仍保留原本 pair-wise DFS 語意；這類 prompt 若出現在大型 hidden case，仍需要另行盤查。
- 若 hidden netlist 含 combinational cycle，fast path 會偵測 cycle 並 fallback 到原本 DFS。

## P2：連鎖錯誤

### `structure_query net_loads renamed_sig`

相關 testcase：

- `test31 #13`

疑似問題：

- 前一步 `rename_net n1289 renamed_sig` 失敗，因此查詢 `renamed_sig` 也跟著失敗。
- 這不一定是 `structure_query` 本身錯。

建議檢查：

- 修正 `rename_net` 後再重測。
- 若上一個 edit 失敗，LLM 不應假設 renamed signal 已存在。

## 目前看起來較穩定的功能

以下功能目前從執行狀態看沒有明顯 timeout/error，但仍需逐題重算比對答案數值：

- `read`
- `write`
- `structure_query count_by_type`
- `structure_query list_pi`
- `structure_query list_po`
- `structure_query gate_info`
- `structure_query gate_fanout`
- `structure_query const_input_gates`
- `depth_query net`
- `depth_query global_critical`
- `depth_query po_exceeding`
- `func_query equivalence`
- `func_query depends_on`
- `func_query boolean_expression`
- `func_query symmetry`

## 建議盤查順序

1. 同步 tools/LLM-facing 文件：large output、equivalence timeout、edit report 判讀、persistent constraint memory policy。
2. 處理 `path_query enumerate -max_paths` 的正式語意與 `mini test/test10` expected。
3. 重跑 `testcase_toolans`，確認不再出現缺 `#RESPONSE`、`Complete: false`、`Functional equivalence: false`。
4. 後期再做 Boolean / AIG functional index 重構，統一處理 enable-hold、large Boolean expression、symmetry、SAT cache、functional-equivalent merge。
