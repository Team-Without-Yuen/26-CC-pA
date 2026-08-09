# NewTestCase 封閉模擬測試結果

## 1. 測試目的

確認目前 `tools.exe` 與 `TOOLS_SPEC` 是否足以讓能力較保守的 LLM 依照題目 prompt
選擇工具、完成查詢或修改，並產生題目要求的答案與 output Verilog。

本輪主要驗證工具整合、command routing、timeout 行為與輸出完整性，不代表實際
GPT-4.5-mini 的最終得分。

## 2. 封閉環境

測試在 `isoCadTest` 執行，模擬端只能讀取：

- `tools.exe`
- `TOOLS_SPEC`
- `NewTestCase`
- 封閉模擬 runner

模擬端不能讀取原始碼、`API_SPEC`、Git 歷史或舊答案。command routing 採保守的
literal matching，不使用專案內部知識補答案。

## 3. 執行摘要

| 項目 | 結果 |
|---|---:|
| Testcase | 40/40 |
| Prompt | 459 |
| 成功產生 output Verilog | 40/40 |
| Runner 外部強制截斷 | 0 |
| `status:ok` | 386 |
| `status:no_change` | 27 |
| Router unresolved | 8 |
| Tool timeout | 2 |
| Tool error | 1 |
| Tool unsupported | 1 |

完整執行時間約 250.5 秒。這是 40 個 testcase 的總時間，不包含後續人工盤查。

## 4. 輸出產物

本輪原始結果位於：

```text
isoCadTest/testcase_toolans/
```

每個 `testXX` 子資料夾包含：

```text
testXX_ans
testXX_out.v
tool_calls.txt
```

根目錄另有 `summary.csv`。40 份 output Verilog 合計約 36.9 MB，答案與 testcase
內紀錄合計約 38.3 MB。

部分工具會自動產生大型 sidecar，這些檔案目前位於 `isoCadTest` 根目錄，總計約
3.64 GB。最大檔案如下：

| 檔案 | 大小 | 結果 |
|---|---:|---|
| `test37_path_query_1.txt` | 約 2.46 GB | 4,542,463 條已發現 paths，但 enumeration timeout，結果不完整 |
| `test32_path_query_1.txt` | 約 781 MB | 工具回傳 complete |
| `test14_path_query_1.txt` | 約 237 MB | 工具回傳 complete |
| `test14_path_query_2.txt` | 約 155 MB | 工具回傳 complete |
| `test40_sequential_query_1.txt` | 約 4.13 MB | 工具回傳 complete |

## 5. 功能結果

目前已確認以下流程能在封閉環境中執行：

- design load、write 與基本統計。
- gate/net/PI/PO/DFF structure query。
- direct connectivity、cone 與 depth query。
- 多數 endpoint path query 與大型結果寫檔。
- 多數 cleanup、constant simplification、buffer insertion、technology mapping。
- critical-path optimization command 與統一 edit/optimization report。
- sequential pattern query 與 function query/search command routing。

`status:no_change` 不直接代表失敗。27 次 no-change 多數表示沒有符合 transformation 的
candidate、scope 為空，或 optimizer 保留 original；仍需依 report 的 constraint 與
equivalence 欄位逐題判讀。

## 6. 首輪未通過項目

本輪不能視為完整通過，主要原因為：

- 8 個 prompt 因模擬 LLM/router 漏判而未呼叫工具。
- 部分 prompt 使用錯誤 scope、objective 或過度寬鬆的 constant filter。
- `test35` function search 只得到 partial matches。
- `test37` register-to-register path enumeration timeout，未列完所有 paths。
- `test33` whole-design equivalence SAT 回傳 UNKNOWN，沒有完成 endpoint 比較。
- sidecar 尚未集中到對應 testcase 子資料夾。
- `_ans` 目前是工具結果的輕量整理，不是實際 LLM 生成的自然語言答案。

## 7. Routing 修正重跑

在 `TOOLS_SPEC/README.md` 增加 routing 優先規則，並修正封閉 runner 後，重新執行：

```text
test25, test31, test33, test34, test35,
test36, test37, test39, test40
```

`test37` 只執行到 #6，避免 routing 驗證再次觸發已知的 #9 大型 path enumeration。

重跑結果：

- 原先 8 個 unresolved prompt 全部取得正確 command。
- scoped cone count、optimization objective、constant 0/1 filter 與 endpoint type 均已修正。
- 14 筆原錯誤 LLM/router 呼叫逐筆核對通過。
- `test40 #16` 不再把 `output` 當成 net name，改以 `n14` 執行並回 `no_change`。
- `test25 #8` 正確使用 `net_fanin n11[0]` 與 `--objective global`。
- `test35 #13` 的 prompt 只問 existence，runner 錯用 `--all`；正確 FindAny 約 5.5 秒完成。29.46 秒結果只用來保留 hidden FindAll 題型的效能風險。
- `test33 #14` 仍回 whole-design SAT unsupported，確認不是 routing 問題。

重跑產物位於：

```text
isoCadTest/routing_retest_main/
isoCadTest/routing_retest_test37/
isoCadTest/routing_retest_test25/
```

問題、實際錯誤 command、預期 command 與處理狀態詳見：

```text
Blup/NEWTESTCASE_CLOSED_SIM_ISSUES.md
```

## 8. 結論

目前 tools 整體整合已能完成大多數 NewTestCase 流程，且 40 個 testcase 都能維持
session 並輸出 Verilog。已知 LLM/router 錯誤已修正並通過局部重跑；尚未完成的主要
項目是 Path Query 大型 enumeration、sidecar 整理，以及後期 Boolean/AIG equivalence。
