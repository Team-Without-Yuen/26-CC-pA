# Path Query API 問題與修改紀錄

## 1. 盤查範圍

- Public `PathQuery` API、資料型別與 register-path wrapper
- `tools.cpp` 的 parser、dispatch、report printer 與輸出政策
- existence、enumeration、count、min/max depth、mandatory node 與 separator/cut
- sequential boundary、cycle、結果完整性、timeout、記憶體與大型輸出
- `API_SPEC`、`TOOLS_SPEC` 與官方 `testcase` prompt coverage

## 2. 狀態總覽

| ID | 問題 | 優先級 | 狀態 | 已修正 | 驗證狀態 |
|---|---|---:|---|---|---|
| PATH-000 | 完整列舉的 CLI 輸出政策 | P0 | Verified | 是 | 官方 testcase 通過 |
| PATH-001 | CLI 靜默忽略錯誤參數 | P2 | Deferred | 否 | 負向測試已重現 |
| PATH-002 | API 文件過期或互相矛盾 | P1 | Verified | 是 | contract/欄位/CLI 核對通過 |
| PATH-003 | Path count 超過 64-bit 時誤報精確完成 | P1 | Awaiting Official | 否 | overflow probe 已重現 |
| PATH-004 | Combinational cycle 下的 max-depth 正確性 | P1 | Won't Fix | 不適用 | 題目保證 combinational graph 為 DAG |
| PATH-005 | Partial path file 缺少完成性資訊 | P2 | Won't Fix | 不處理 | timeout 結果不具正式得分價值 |

狀態語意：

| 狀態 | 意義 |
|---|---|
| `Pending` | 問題已登記，尚未開始完整處理 |
| `Investigating` | 正在確認實作或建立重現測試 |
| `Deferred` | 問題存在，但目前風險可接受，延後處理 |
| `Awaiting Official` | 等待官方規格回覆後決定作法 |
| `Approved` | 修改方案已討論並確認 |
| `Fixed` | 已修改，尚未完成完整驗證 |
| `Verified` | 已修改且通過對應測試 |
| `Won't Fix` | 已確認不需修改，並保留原因 |

修改紀錄規則：每個已動過的項目都必須列出檔案、函式或文件章節、修改內容與外部行為；
測試檔和正式程式碼分開記錄。若尚未修改，必須明確寫出「正式程式碼：無」。

## 3. 問題與修改紀錄

## PATH-000：完整列舉的 CLI 輸出政策

- 狀態：`Verified`
- 優先級：P0
- 類型：CLI / Report / Documentation
- Boolean 相關：否

### 問題

LLM 原本必須自行決定 `-out`、`-max_print` 與 time limit。若未正確設定，完整路徑可能大量進入 terminal envelope；固定 fallback 檔名也可能覆寫舊結果。

### 預期行為

prompt 沒有指定輸出控制時，工具仍完整產生所有路徑，但大型列表寫入檔案；terminal 只回總數、完成狀態與檔案位置。

### 處理決議

- 完整列表由 CLI 自動 streaming 寫檔，不要求 LLM 主動控制輸出。
- 只問數量時由 LLM 使用 `-count_only`。
- `-out`、`-max_print`、`-max_paths`、`-time_limit` 只在 prompt 明確要求時使用。
- 保留既有輸入參數與核心 API 相容性。

### 修改內容

| 檔案 | 函式 / 章節 | 實際修改 | 外部行為 |
|---|---|---|---|
| `tools.cpp` | include 區 | 加入 `<filesystem>`、`<limits>` | 支援安全檔名檢查與完整顯示上限 |
| `tools.cpp` | `ToolSession` | 新增 `pathArtifactSequence` | 同一 session 的自動輸出檔使用遞增序號 |
| `tools.cpp` | `makeAutomaticPathOutputPath()` | 依 design stem 建立候選檔名，並用 `filesystem::exists()` 避免覆寫 | 新舊程序都不覆寫既有 path file |
| `tools.cpp` | `path_query` enumerate dispatch | 非 `count_only` 且沒有 `-out` 時，自動設定 `writePathsToFile` 與唯一輸出路徑；自動模式將 terminal sample 設為 0 | 完整列表自動 streaming 寫檔，terminal 只回摘要與檔案位置 |
| `tools.cpp` | `path_query direct_pi_po` dispatch | 預設 `maxPrintedPaths` 設為可完整顯示 | 不需先查總數再重跑，直接回完整 depth-0 connections |
| `tools.cpp` | unified help 的 Path Query 區 | 補上自動寫檔與特殊控制參數政策 | LLM 可從 `help` 得知正常情況不傳 `-out/-max_print/-max_paths/-time_limit` |
| `TOOLS_SPEC/PATH_QUERY_TOOL.md` | 責任、modes、enumeration options、examples、限制 | 將 LLM 使用流程改為工具自動寫檔，移除正常 prompt 主動設定輸出限制的建議 | 工具文件與 CLI 行為一致 |
| `API_SPEC/PATH_QUERY_USAGE.md` | 第 7 節與 register-path CLI example | 區分核心 facade 的固定 fallback 檔名與 `tools.cpp` 的唯一自動檔名政策 | API 維護者不會混淆 core 與 CLI 預設 |
| `tools.exe` | 編譯產物 | 由更新後的 `tools.cpp` 重新編譯 | 根目錄執行檔包含新輸出政策 |

未修改內容：`include/core/PathTypes.h`、`src/analysis/PathAnalysis.cpp`、`PathQuery` 輸入參數、
`PathQueryResult` 型別、路徑定義與底層搜尋演算法。

測試產物：

| 路徑 | 用途 |
|---|---|
| `Blup/path_output_policy_test/` | 驗證同程序與跨程序唯一檔名、count-only 不寫檔、明確 `-out` 相容性 |
| `Blup/path_query_runs/output_policy_regression/` | 使用官方 test12、test14、test32 重跑輸出政策與計數 |

### 驗證結果

- 同一程序建立 `_1`、`_2`；新程序再次執行建立 `_3`，既有檔案未被覆寫。
- `-count_only` 不建立 path file；明確 `-out` 仍依指定路徑輸出。
- `test12` 完整列舉得到 8 與 0 paths，約 0.21 秒，皆 `complete:true`。
- `test14` count-only 得到 289,366 與 203,810，約 0.04 秒。
- `test32` all-DFF count-only 得到 1,176,523，約 0.45 秒。
- `TOOLS_SPEC make check` 通過。
- Path Query 相關程式未使用 `std::thread`、`std::async`、OpenMP 或 parallel execution；DFS/BFS/DP/streaming 均為單執行緒。

## PATH-001：CLI 靜默忽略錯誤參數

- 狀態：`Deferred`
- 優先級：P2
- 類型：CLI / API Semantics
- Boolean 相關：否

### 問題

未知 option、缺少 option value 或 mode 不接受的額外 token，可能被 parser 忽略並回傳成功，形成 silent wrong-answer 風險。

### 重現方式

```text
path_query exists net:n31[0] net:n6476 -bogus unexpected
path_query enumerate net:n31[0] net:n6476 -out
path_query direct_pi_po unexpected
```

三個命令目前都可能回 `status:ok, complete:true`。

### 原因

`tools.cpp` 的 option loop 沒有統一拒絕 unknown token；部分 option 讀值失敗後也沒有立即回 validation error。

### 處理決議

目前 LLM 應依 `TOOLS_SPEC` 組合合法命令，此問題不影響合法參數，因此延後。若後續 LLM/testcase 實測出現錯誤參數被靜默接受，再提高優先級。

### 修改內容

正式程式碼：無。文件僅記錄問題與延後決議。未來可在不變更參數的前提下修改
`tools.cpp` 的 `path_query` option parser，加入 strict validation。

### 驗證狀態

負向案例已穩定重現；合法 `test32` path commands 正常。

## PATH-002：API 文件過期或互相矛盾

- 狀態：`Verified`
- 優先級：P1
- 類型：Documentation
- Boolean 相關：否

### 問題

- `API_SPEC/PATH_QUERY_API.md` 的 enum/result 範例尚未完整列出 mandatory、separator、completion 與 file fields。
- 文件仍有只能由 enumerate size 計數的舊描述，但目前已有 `countOnly` DP。
- `API_SPEC/PATH_QUERY_USAGE.md` 最後的 coverage 清單漏列 `EnumerateAll`、`FindMandatoryNodes`、`IsSeparator`。
- 部分 helper 同時被描述成缺少與已新增。

### 影響

不影響目前執行結果，但維護者可能依過期規格誤用 API。

### 處理決議

不修改 API；依 `PathTypes.h`、`PathAnalysis.cpp`、`tools.cpp` 與既有 testcase 結果，按照
`API_SPEC/DOCUMENTATION_FORMAT` 的 API/Usage 模板重寫文件。

### 修改內容

| 檔案 | 章節 / 內容 | 實際修改 |
|---|---|---|
| `API_SPEC/PATH_QUERY_API.md` | 全文件，依 API template 的 1~8 節 | 重寫核心定位、DAG/DFF boundary、endpoint/query types、完整 report contract、演算法流程、API 責任界線、限制與實作位置 |
| `API_SPEC/PATH_QUERY_API.md` | Query mode table | 補齊 `FindMandatoryNodes`、`IsSeparator`、`DirectPiPoConnections`；移除不存在的獨立 `CountPaths` mode，改說明 `EnumerateAll + countOnly` |
| `API_SPEC/PATH_QUERY_API.md` | Report Contract | 補齊 mandatory/separator、cycle、streaming/completion、timeout、count-only、unresolved fields |
| `API_SPEC/PATH_QUERY_API.md` | 舊 helper/function 段落 | 移除「helper 缺少／已新增」矛盾描述，以及誤放的 constant/Boolean expression 問題 |
| `API_SPEC/PATH_QUERY_USAGE.md` | 全文件，依 Usage template 的 1~8 節 | 重寫標準呼叫流程、mode/command 表、endpoint/options、狀態判讀、每個 mode 範例、prompt routing 與何時改用其他 API |
| `API_SPEC/PATH_QUERY_USAGE.md` | Enumeration usage | 明確區分 core 固定 fallback 與 CLI 唯一自動檔名；LLM 只在 prompt 明確要求時傳輸出控制參數 |
| `API_SPEC/PATH_QUERY_USAGE.md` | Coverage/limitations | 補齊全部 mode，加入 DAG、64-bit overflow pending 與 partial-file completion 限制 |

正式程式碼：無。此次只修改上述兩份 Markdown。

### 驗證狀態

- 十個 `PathQueryMode` 均可在文件中找到。
- `PathQueryResult` public fields 已逐項與 `include/core/PathTypes.h` 核對，無缺漏。
- 過期 `CountPaths`、helper 矛盾與 Function Query 內容掃描均無殘留。
- `git diff --check` 通過；CLI help smoke test 通過。

## PATH-003：Path count 超過 64-bit 時誤報精確完成

- 狀態：`Awaiting Official`
- 優先級：P1
- 類型：Algorithm / Report
- Boolean 相關：否

### 問題

count-only DP 超過 `size_t` 上限時會飽和為 `SIZE_MAX`，但結果仍標示完整且精確。

### 重現方式

- 測試電路：`Blup/path_query_runs/overflow_probe/overflow_probe.v`
- 結構：65 層二分支後匯合的 DAG，共 196 gates
- 命令：`path_query enumerate pi:a po:y -count_only`
- 正確結果：`2^65 = 36,893,488,147,419,103,232`
- 實際結果：`18,446,744,073,709,551,615`（`SIZE_MAX`）
- 錯誤狀態：同時回報 `complete:true`、`Complete enumeration:yes`
- 執行時間：約 0.14 秒，並非 timeout

### 原因

`saturatingAdd()` 發生溢位時只回傳 `SIZE_MAX`，沒有向 `PathQueryResult` 傳遞 overflow 狀態。

### 處理決議

先詢問官方正式 count 是否可能超過 `2^64 - 1`。待寄內容見 `Blup/OFFICIAL_QUESTIONS.md`。

### 修改內容

正式程式碼：無。

| 測試檔 | 內容 |
|---|---|
| `Blup/path_query_runs/overflow_probe/overflow_probe.v` | 65 層二分支匯合 DAG，用最小規模穩定觸發超過 64-bit 的 path count |
| `Blup/OFFICIAL_QUESTIONS.md` | 記錄數值上限的英文詢問信及官方回覆後的兩種處理策略 |

若官方不保證上限，再規劃修改 `src/analysis/PathAnalysis.cpp` 的 count representation，以及
`include/core/PathTypes.h`、`tools.cpp` report printer 和對應文件；目前均未動。

### 驗證狀態

overflow probe 已穩定重現問題。

## PATH-004：Combinational cycle 下的 max-depth 正確性

- 狀態：`Won't Fix`
- 優先級：P1
- 類型：Algorithm
- Boolean 相關：否

### 問題

無 constraint 的 max-depth fast path 偵測到 combinational cycle 後，會退回 pairwise longest-path DFS。fallback 的 memoization key 只包含目前 net，沒有包含目前 simple path 已造訪的節點集合，因此不同 DFS 狀態可能錯誤共用結果。

### 潛在影響

含 cycle 的 hidden netlist 中，`max_depth` 可能回傳錯誤深度或錯誤 witness path。一般 DAG 不受影響。

### 處理決議

題目已確定 combinational netlist 為 DAG，不會進入 cycle fallback。此風險不影響正式測資，
不建立 cycle probe，也不修改 longest-path memoization。若未來 API 要支援任意含 loop 的外部
netlist，再重新開啟此項。

### 修改內容

正式程式碼：無。尚未新增 cycle probe；目前只有靜態風險紀錄。

### 驗證狀態

官方 testcase 與題目 contract 均採 combinational DAG；現有 DAG max-depth testcase 已通過。

## PATH-005：Partial path file 缺少完成性資訊

- 狀態：`Won't Fix`
- 優先級：P2
- 類型：Report
- Boolean 相關：否

### 問題

timeout 時 CLI envelope 會回 `complete:false` 與 stop reason，但 path file header 只有 `Total paths`。若後續流程只讀檔案，可能把 partial count 認成完整結果。

### 原因

path file finalization 目前只回填 count，沒有寫 completion、timeout 或 stop reason metadata。

### 處理決議

不處理。內部 timeout 時 report envelope 已提供 `complete:false`、timeout 與 stop reason；正式
答案已不是完整結果。若遭外部 deadline 強制終止，path file 也未必有機會完成 finalization，
增加 file metadata 無法恢復完整答案或提高得分，只對離線除錯有幫助。因此維持現有格式，
把時間優先投入避免 timeout 的演算法與 testcase regression。

### 修改內容

正式程式碼：無。尚未修改 `src/analysis/PathAnalysis.cpp` 的 path-file header/finalization。

### 驗證狀態

已由靜態程式碼確認 envelope 具備 completion/timeout fields；不建立專門的 file-format probe。

## 4. 官方 Testcase Coverage

| 類型 | 對應 prompt |
|---|---|
| exists + avoid | test06~20、test34、test35 |
| enumerate all | test08~20、test32、test33、test37 |
| endpoint max depth | test10~20、test40 register-to-register |
| every path through | test32 |
| direct PI-to-PO depth-0 | test33、test38 |
| mandatory/articulation | test38 |
| PI-to-PO cut | test33、test36 |
| reachable fanout | 屬 `Cone Query`，不是 Path Query |
| global critical path/depth | 屬 `Depth Query`，不是 Path Query |

## 5. Testcase 與效能回歸

| Testcase | 查詢 | 結果 | 耗時 / 產物 |
|---|---|---|---|
| test12 | `n24[1] -> n26[1]` enumerate | 8，complete | 約 0.21 秒，自動寫檔 |
| test12 | `n24[2] -> n26[0]` enumerate | 0，complete | 同一 session |
| test12 | max depth | 11、15 | complete |
| test14 | 兩組 count-only | 289,366、203,810 | 約 0.04~0.06 秒 |
| test14 | 兩組 full streaming | record count 與 count-only 一致 | 約 2.31 秒；約 237 MB、155 MB |
| test14 | 三組 max depth | 5、54、51 | complete |
| test32 | all DFF.Q -> all DFF.D full enumerate | 1,176,523，complete | 約 6.20 秒；約 781 MB |
| test32 | 同 endpoints count-only | 1,176,523 | 約 0.45~0.50 秒 |
| test32 | path mode matrix | exists/find/min/max/every modes 正常 | 約 0.45 秒 |
| test33 | direct PI-to-PO、enumerate、cut | 0、0、not cut | 約 0.46 秒 |
| test36 | `pi_po_cut n10239` | not cut | 約 0.06 秒 |
| test38 | direct PI-to-PO、mandatory | 0、`NO_PATH` | 約 0.06 秒 |
| test40 | all DFF.Q -> all DFF.D max depth | 105 | 約 0.09 秒 |

舊版 `test12` 曾兩次在 280 秒 timeout，`test14` 也未完成；加入 reverse reachability pruning 與 count-only DP 後皆已在上述時間內完成。大型完整列表目前主要成本是輸出資料量。

## 6. 後續處理順序

1. 等待官方回覆後處理 `PATH-003`。
2. `PATH-001` 維持延後，除非 LLM 實測出現 silent wrong answer。
3. `PATH-004` 因題目保證 DAG，不處理 cycle-specific longest path。
4. `PATH-005` 不修改 partial-file 格式，持續以避免 timeout 為優先。

## 7. Boolean 延後項目

目前 Path Query 盤查尚未發現需要 Boolean function、AIG 或 SAT 才能處理的項目。
