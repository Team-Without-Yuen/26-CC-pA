# NewTestCase 封閉模擬問題盤查

## 1. 本輪範圍

- 測試環境：`isoCadTest`
- 可見內容：`tools.exe`、`TOOLS_SPEC`、`NewTestCase`、封閉模擬 runner
- 測試範圍：`test01` 至 `test40`
- 總 prompt 數：459
- 產生 output Verilog：40/40
- runner 外部強制截斷：0
- 本輪結果位置：`isoCadTest/testcase_toolans`

本輪使用 literal router 模擬能力較保守的 LLM。它只依 prompt 字面與
`TOOLS_SPEC` 選擇 command，不讀原始碼、`API_SPEC` 或既有答案。因此本報告需將
「模擬 router 問題」和「tools/API 後端問題」分開判讀。

## 2. 狀態摘要

| Tool status | 數量 | 判讀 |
|---|---:|---|
| `ok` | 386 | command 完整成功 |
| `no_change` | 27 | 成功保留原設計，需逐題確認語意 |
| `unresolved` | 8 | router 未選出 command |
| `timeout` | 2 | tools/API 自己回傳 timeout，不是 runner 截斷 |
| `error` | 1 | router 傳入錯誤 scope name |
| `unsupported` | 1 | whole-design SAT 無法完成證明 |

大型 sidecar 合計約 3.64 GB；其中 `test37_path_query_1.txt` 約 2.46 GB。

## 3. 問題清單

| ID | 類型 | 嚴重度 | Test / Prompt | 問題 | 狀態 |
|---|---|---|---|---|---|
| SIM-001 | Router coverage | 高 | test31 #12/#13、test33 #9/#16、test34 #8、test35 #6、test37 #5、test39 #17 | 8 個已有 public tool 可處理的 prompt 被判為 unresolved，包括 rename、net connectivity、PI width、path exists/avoid 與 fixed-basis cone conversion | 已修正並驗證 |
| SIM-002 | Wrong scope | 高 | test33 #10、test37 #6 | 問的是 cone 內 NAND 數量，卻呼叫 whole-design `structure_query count_by_type NAND`；應查 `cone_query net_fanin n8` 的 gate-type breakdown | 已修正並驗證 |
| SIM-003 | Wrong argument | 高 | test40 #16 | regex 將 `output` 當成 net name，送出 `--scope net_fanin output`，造成 scope resolution error；正確 net 是 `n14` | 已修正並驗證 |
| SIM-004 | Wrong objective | 中 | test25 #8 | cost 是 final design maximum depth，卻送出 `--objective cone`；依文件應為 global objective。此 testcase 因 n11[0] 為空 combinational cone 而 no-change，但路由語意仍錯 | 已修正並驗證 |
| SIM-005 | Over-broad edit filter | 中 | test36 constant-0 AND、test39 constant-1 OR | prompt 明確限定 constant value，runner 卻使用 `simplify_constants <TYPE> any`，可能修改題目未要求的其他 candidate | 已修正並驗證 |
| SIM-006 | Wrong search mode | 高 | test35 #13 | prompt 只問是否存在，runner 卻加上 `--all`；正確命令是 FindAny 的 `func_search nand_pair n25` | 已確認，交由 LLM/router 文件處理 |
| CORE-001 | Function Search FindAll risk | 中 | hidden「列出全部 NAND pairs」題型 | FindAll 需檢查 72,030,003 組 simulation pairs，並對其中 12,003 組各自建立 SAT proof；29.46 秒完成但距 API 內建 30 秒上限僅約 0.54 秒 | 非 test35 blocker，保留效能風險 |
| CORE-002 | Path enumeration timeout | 高 | test37 #9 | exact count 16,548,172；改用 V3 delta artifact 與 unconstrained DAG fast path 後 17.81 秒完整完成 | 已修正並驗證 |
| CORE-003 | Whole-design equivalence unsupported | 高 | test33 #14 | `equiv_query original 240` 實際被單次 SAT 的硬編碼 30 秒上限提前終止；87 個 PO 與 4,211 個 DFF.D 全部 skipped | 根因已確認，Boolean/AIG 或證明鏈後續處理 |
| OUT-001 | Sidecar placement | 中 | 多個 path/function/sequential query | tools 產生的 sidecar 位於 `isoCadTest` 根目錄，不在各自 testcase 子資料夾 | 非 backend 範圍，由 LLM/測試整合端歸檔 |
| OUT-002 | Answer synthesis | 中 | 全部 testcase | `_ans` 是封閉 runner 的工具結果整理，不是真正 LLM 自然語言答案 | 非 backend 範圍，由 LLM 負責人處理 |

### 3.1 CORE 問題根因補充

#### CORE-001：Function Search FindAll 效能風險

- test35 #13 只問 existence，正確模式是 FindAny；現有 API 約 5.5 秒即可找到 SAT witness 並完整回答。
- 封閉 runner 錯加 `--all` 才觸發下列 FindAll 成本，因此 test35 timeout 應歸類為 SIM-006 routing 問題。
- `FunctionSearchQuery::timeLimitSeconds` 預設為 30 秒，低於官方 basic operation 的 60 秒限制。
- 12,003 個 simulation-eligible signals 形成 72,030,003 組候選；目前用雙層迴圈逐組比較 signature。
- 12,003 組通過 simulation 後，`solveNandPairEquivalenceDetailed()` 會逐組重新建立 cone CNF 與 SAT solver。
- hidden prompt 若明確要求列出全部，29.46 秒的完整結果仍不是穩定餘裕；目前同時存在預設 budget 過短與重複 SAT 建模成本。

#### CORE-002：test37 Path Query

- count-only DP 在約 1.32 秒得到 exact total 16,548,172，證明路徑數本身與 endpoint 解析正確。
- 正常文字 sidecar 在 55 秒完成 4,542,463 條、約 2.46 GB，只覆蓋總數約 27.4%。
- 改寫到 `NUL` 後，同樣 55 秒完成 13,926,858 條、約 84.2%，證明主要瓶頸是逐條文字格式化與磁碟 I/O；DFS 的 per-path allocation/record handling 仍有次要成本。
- 依現有平均 record 大小估算，完整未壓縮文字結果接近 9 GB。
- 已修正：`COMPACT_PATH_V3` 以 dictionary、base36、prefix/suffix delta 保留每條 path；
  unconstrained DAG 使用低開銷 DFS fast path。最終 16,548,172 records 在 17.81 秒完成，
  artifact 670.1 MB，獨立掃描 record count 與 footer 均一致。

#### CORE-003：test33 Whole-design Equivalence

- `compareEndpointsBySat()` 雖接收 240 秒 total budget，但每次 solve 使用 `min(30.0, remaining)`。
- global endpoint miter 在約 30 秒被 terminator 中止後回 UNKNOWN；因 total budget 尚未耗盡，report 同時出現 `timeBudgetExceeded=false`，容易誤導上層。
- test33 在查詢前只有一個成功的 XNOR-to-NOR 等價轉換，其餘相關 edit 為 no-change；若 session 保存由成功驗證 edit 組成的 equivalence certificate chain，可避免此處重新執行大型 SAT。

## 4. LLM 呼叫錯誤明細

以下問題屬於 LLM/router 選擇或組裝 command 錯誤，不應登記成 API core bug。

| Test / Prompt | 實際呼叫 | 應使用 | 問題 |
|---|---|---|---|
| test31 #12 | `none` | `edit_apply rename_net n1289 renamed_sig` | 未辨識 internal signal rename |
| test31 #13 | `none` | `structure_query net_driver renamed_sig`，再呼叫 `structure_query net_loads renamed_sig` | 未辨識「所有連到 net 的 gates」需組合 driver/load |
| test33 #9 | `none` | `edit_apply convert_basis net_fanin n8 -allow NAND NOT` | 將 fixed-basis restructuring 漏判成無工具 |
| test33 #10 | `structure_query count_by_type NAND` | `cone_query net_fanin n8`，讀 cone breakdown 的 NAND count | 錯把 scoped count 當 whole-design count |
| test33 #16 | `none` | `structure_query list_pi` | `the primary inputs` 句型未被辨識 |
| test34 #8 | `none` | `path_query exists net:n2 net:n30 -avoid net:n4552` | 未辨識 `avoids` 句型 |
| test35 #6 | `none` | `path_query exists pi:n2 po:n25` | 未辨識一般 path-existence yes/no prompt |
| test35 #13 | `func_search nand_pair n25 --all` | `func_search nand_pair n25` | existence prompt 應使用 FindAny，不應強制列舉全部 matches |
| test36 #7 | `edit_apply simplify_constants AND any` | `edit_apply simplify_constants AND 0` | 將明確 constant-0 constraint 放寬成 any |
| test37 #5 | `none` | `edit_apply convert_basis net_fanin n8 -allow NAND NOT` | fixed-basis restructuring 漏判 |
| test37 #6 | `structure_query count_by_type NAND` | `cone_query net_fanin n8`，讀 cone breakdown 的 NAND count | 錯把 scoped count 當 whole-design count |
| test39 #5 | `edit_apply simplify_constants OR any` | `edit_apply simplify_constants OR 1` | 將明確 constant-1 constraint 放寬成 any |
| test39 #17 | `none` | `structure_query list_pi` | `the primary inputs` 句型未被辨識 |
| test40 #16 | `opt_apply critical_path_depth --scope net_fanin output --objective cone --allowed NAND NOT` | `opt_apply critical_path_depth --scope net_fanin n14 --objective cone --allowed NAND NOT` | 將英文單字 `output` 誤擷取為 net name |
| test25 #8 | `opt_apply critical_path_depth --scope net_fanin n11[0] --objective cone --allowed NAND NOT` | `opt_apply critical_path_depth --scope net_fanin n11[0] --objective global --allowed NAND NOT` | cost 明確是 final-design maximum depth，objective 應為 global |

以上 14 筆 LLM/router 呼叫已使用保守封閉模擬重新驗證。重跑結果皆為
`unresolved:0`，實際 raw command 與「應使用」欄一致。`test37` 只執行到 #6，避免在
routing 驗證階段再次觸發已知的 #9 大型 path enumeration。

## 5. 已確認可用的部分

- 40 個 design 都能 load 並 write，40 份 output Verilog 均已產生。
- runner 沒有發生外部 timeout 或整批卡死。
- Basic、structure、cone、depth、direct connectivity 與多數 endpoint path prompt 能完成。
- 多數固定 edit、cleanup、technology mapping 與 optimization command 能得到完整 envelope。
- 27 個 `no_change` 本身不等於失敗；目前多數代表找不到 candidate 或保留 original，仍需以各 report 的 validation/constraint 欄位判讀。

## 6. 後續順序

1. CORE-002 已完成：保留 V3 artifact regression 與 test37 壓力測試作為後續回歸基準。
2. 將 SIM-006 的 existence→FindAny 規則交給 LLM/router 文件；CORE-001 只針對 hidden FindAll 題型評估 budget 與候選/SAT context 重用。
3. CORE-003 保留為 Boolean/AIG 或 session equivalence certificate chain 階段；先修正硬編碼 30 秒與 report 語意的設計方案。
4. sidecar 歸檔及自然語言答案格式交由 LLM/測試整合端處理，不列入 API backend 修改範圍。
