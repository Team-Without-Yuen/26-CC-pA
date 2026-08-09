# Function Search AIG 前基準

## 1. 目的

本文件固定目前 named-netlist simulation + SAT 實作的功能與效能基準。之後合併 AIG
functional index 或改寫 Boolean engine 時，必須以相同 netlist、command 與輸出語意重跑，
用來區分既有差異與 AIG 改動造成的 regression。

## 2. 基準環境

| 項目 | 值 |
|---|---|
| branch | `blup` |
| commit | `3a32cd3ee8b73f1a5b8bcdd96aec9c7de3c794d4` |
| `tools.exe` SHA256 | `C8F393B44F0407A530A2B5FFFDB56B99B2625BBA05B8C7674F2122E3E3746CAE` |
| 執行日期 | 2026-08-10 |
| 原始結果目錄 | `Blup/function_search_baseline_before_aig/` |

## 3. 固定測試

| Case | Command | 用途 | 結果 | API elapsed | 外部 elapsed |
|---|---|---|---|---:|---:|
| test29 | `func_search equivalent_pairs whole --all` | 官方 merge prompt 的搜尋底層 | 7 classes / 7 pairs | 2.575 s | 2.64 s |
| test30 | `func_search equivalent_pairs whole --all` | 官方 merge prompt 的搜尋底層 | 1 class / 1 pair | 0.109 s | 0.17 s |
| test35 | `func_search nand_pair n25` | 官方 existence prompt | witness `n26080`, `n6359` | 7.699 s | 7.82 s |
| test35 | `func_search nand_pair n25 --all --time-limit 55` | 非官方路由；FindAll 壓力測試 | 512 pairs | 41.154 s | 41.24 s |

test35 官方原文只問是否存在，因此正確行為是 FindAny。FindAll 只用來觀察完整列舉的效能與
語意，不能讓 router 因為工具支援 `--all` 就自行擴張官方要求。

## 4. 完整性核對

四個 query 都符合：

- `ok=true`
- `complete=true`
- `timed_out=false`
- `truncated=false`
- `unsupported=false`
- `sat_unknown_count=0`

Artifact 逐筆核對如下：

| Artifact | class records | match records | completion marker | SHA256 |
|---|---:|---:|---|---|
| `test29_function_search_1.txt` | 7 | 7 | `Complete: yes` | `33BB3F2B428CA9FDE8FB44EBC0D2967ABFE5F49441CBC025D07B6D949ED8F2D7` |
| `test30_function_search_1.txt` | 1 | 1 | `Complete: yes` | `8EFE226E1BC0349B9CA34E7818159EF9E16B1C2E0FB8F0E71A0C538B75F1203D` |
| `test35_function_search_1.txt` | 0 | 512 | `Complete: yes` | `94A1BB85867CA22597370EED97BCB34F5E3A249DE60E9F8825605EA167C88BBF` |

FindAny 不產生 artifact；完整 witness 保留在 `test35_nand_find_any.log`。

## 5. 合併 AIG 前待釐清差異

目前 test35 FindAll 與舊盤查紀錄不一致：

| 指標 | 舊紀錄 | 本次固定基準 |
|---|---:|---:|
| candidate signals | 39,283 | 34,639 |
| simulation-eligible signals | 12,003 | 11,832 |
| candidate pairs considered | 72,030,003 | 69,992,196 |
| SAT checks | 12,003 | 11,831 |
| matches | 514 | 512 |

本次 artifact 的 512 筆與 report 完全一致，所以不是輸出截斷。差異在 AIG 合併前已存在，
必須先追查 candidate eligibility、tombstone/active-net 過濾或 testcase/reader 版本變動；在原因
確認前，不能把 512 或 514 任一數值直接宣告為正確答案，也不能把差異歸因於後續 AIG。

## 6. AIG 合併後驗收規則

1. 先確認 public command grammar 與 report 欄位不變。
2. 重跑本文件四個 command，不先調整 timeout 或候選條件。
3. test29、test30 的 classes/pairs 與 test35 FindAny witness 必須保持 SAT-proven 語意。
4. FindAll 必須完整、無 timeout、無 truncation、無 SAT unknown；match set 應以排序後內容比較，
   不能只比較總數。
5. 若輸出順序改變但集合相同，記為 deterministic-order 差異；若集合不同，逐筆列出新增與遺失
   pair，再判斷是修正或 regression。
6. 效能比較同時記錄 API elapsed 與外部 elapsed，避免把 process startup 或檔案 I/O 混入 SAT
   engine 成本。
