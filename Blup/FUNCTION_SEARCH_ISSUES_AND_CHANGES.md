# Function Search API 問題與修改紀錄

## 1. 盤查範圍

- `FunctionSearchQuery`、`FunctionSearchReport` 與 public entry
- NAND-equivalent signal-pair search
- functionally equivalent gate-pair/class search
- candidate generation、simulation filter、SAT proof、scope 與 gate-type filter
- FindAny/FindAll、完整性、result limit、timeout 與大型輸出
- `tools.cpp` parser/printer、`API_SPEC`、`TOOLS_SPEC` 與官方 testcase coverage
- Boolean/AIG 重構需求只記錄，不在本階段實作

## 2. 狀態總覽

| ID | 問題 | 優先級 | 狀態 | 已修正 | 驗證狀態 |
|---|---|---:|---|---|---|
| FUNCSEARCH-000 | 目前兩種 public mode 的 baseline coverage | P0 | Verified | 不需修改 | test35/test29/test30 通過 |
| FUNCSEARCH-001 | FindAll 預設 `maxResults=256` 且沒有自動檔案輸出 | P1 | Verified | 是 | probe、test24/test26、test29/30/35 通過 |
| FUNCSEARCH-002 | Boolean engine/AIG 重構與任意 hidden structure coverage | P2 | Boolean-Deferred | 否 | 保留現有 SAT 能力，後期再重構 |
| FUNCSEARCH-003 | timeout 停止誤差與 partial artifact 保存 | P1 | Verified | 不需修改 | NewTestCase 29/35 的 0.01 秒與 1 秒 probe 通過 |
| FUNCSEARCH-004 | 未知 primitive 在 reader 階段靜默遺失 | P3 | Out of Scope | 不需修改 | 官方 netlist 不含額外 gate types |
| FUNCSEARCH-005 | candidate、boundary、scope 與 gate-type filter 語意 | P1 | Verified | 不需修改 | mixed-boundary probe 全部符合預期 |
| FUNCSEARCH-006 | NAND FindAll 貼近 30 秒內建上限 | P1 | Root Cause Confirmed | 否 | test35 診斷：72,030,003 pairs；29.46 秒完成，僅餘約 0.54 秒 |

狀態語意沿用 `PATH_QUERY_ISSUES_AND_CHANGES.md`。每個已修改項目必須列出檔案、函式或
章節、修改內容及外部行為；測試檔與正式程式碼分開記錄。

## 3. 問題與修改紀錄

## FUNCSEARCH-000：目前兩種 public mode 的 baseline coverage

- 狀態：`Verified`
- 優先級：P0
- 類型：Coverage / Algorithm / Performance
- Boolean 相關：使用現有 simulation + SAT，不進行 AIG 重構

### 責任與官方 Prompt

直接由 Function Search 回答：

```text
NewTestCase/test35 prompt 13:
Does there exist any pair of internal signals (a, b) already in the netlist such that NAND(a, b) is equivalent to n25?
```

由 EditApply 使用 Function Search equivalence classes 作為底層：

```text
NewTestCase/test29 prompt 7:
Find and merge all gate pairs in the design that are functionally equivalent (produce the same function). Make sure nothing changes functionally.

NewTestCase/test30 prompt 7:
Find and merge all gate pairs in the design that are functionally equivalent (produce the same function). Make sure nothing changes functionally.
```

已知兩個 signal 名稱的 equivalence prompts 屬 `FunctionQuery::Equivalence`，不是 Function
Search。Technology mapping 與 whole-design equivalence 也不屬本 API。

### 實作流程

| Mode | 候選與證明流程 |
|---|---|
| `NandEquivalentInputPairs` | 收集 internal scalar signals -> deterministic simulation filter -> 對 surviving pairs 建立 `NAND(a,b) XOR target` miter -> UNSAT 才回 match |
| `EquivalentGatePairs` | 依 scope 收集 active combinational gate outputs -> simulation signature 分桶 -> bucket representative SAT comparison -> 建立 proven equivalence classes |

不同 simulation bucket 已有 concrete pattern 證明不等價；相同 bucket 仍需 SAT，不會把 simulation
collision 當成 match。DFF 不作為 candidate gate，DFF.Q 作為 Boolean boundary。

### 驗證結果

| Testcase | Command | 結果 | Function search 時間 |
|---|---|---|---:|
| test35 | `func_search nand_pair n25` | `MATCH_FOUND`；`n26080`,`n6359`；SAT UNSAT；complete | 約 5.55 秒 |
| test29 | `func_search equivalent_pairs whole --all` | 7 classes / 7 pairs；3444 SAT checks；complete | 約 3.33 秒 |
| test30 | `func_search equivalent_pairs whole --all` | 1 class / 1 pair；269 SAT checks；complete | 約 0.12 秒 |

三組皆為 `ok:true`、`complete:true`、`timedOut:false`、`truncated:false`、
`unsupported:false`、`satUnknownCount=0`。

### 修改內容

正式程式碼：無。本項只建立 baseline 與 testcase 證據。

| 文件 | 內容 |
|---|---|
| `Blup/FUNCTION_SEARCH_ISSUES_AND_CHANGES.md` | 新增 Function Search 標準化盤查、prompt routing 與實測結果 |

## FUNCSEARCH-001：FindAll 預設 256 筆且沒有自動檔案輸出

- 狀態：`Verified`
- 優先級：P1
- 類型：Completeness / Report / CLI / Scalability
- Boolean 相關：否；這是輸出與完整性政策問題

### 問題

`FunctionSearchQuery::maxResults` 預設為 256。FindAll 若 proven pair records 超過此值，會設
`truncated=true`、`complete=false`、`ok=false`。`func_search` 目前沒有 pagination 或自動檔案
輸出，因此 prompt 要求完整列表時，LLM 必須自行推算 pair 上界並傳入 `--max-results`。

這與目前共通政策不一致：prompt 沒有明確指定數量上限時，LLM 不應主動截斷，也不應負責
猜測內部輸出上限；大型完整結果應由工具寫檔並回傳路徑。

### 靜態根因

| 檔案 | 位置 | 目前行為 |
|---|---|---|
| `include/core/NetlistQueries.h` | `FunctionSearchQuery::maxResults` | 固定預設 256 |
| `src/analysis/FunctionAnalysis.cpp` | NAND FindAll match append | 達 `maxResults` 後停止搜尋並標記 truncated |
| `src/analysis/FunctionAnalysis.cpp` | EquivalentGatePairs pair expansion | classes 分類完成後只展開前 `maxResults` pair records，並把 report 標成 partial |
| `tools.cpp` | `buildFunctionSearchQuery()` / printer | CLI 可手動提高上限，但不會自動寫完整結果到檔案 |
| `TOOLS_SPEC/FUNCTION_SEARCH_TOOL.md` | completeness policy | 要求 LLM 計算 `N*(N-1)/2` 後重送，增加串接責任 |

### 影響

- 目前 test29/test30 分別只有 7/1 pairs，不會觸發。
- test35 只問是否存在，FindAny 找到 witness 即完整，不會觸發。
- hidden prompt 若要求列出全部 NAND pairs 或 equivalent gate pairs，可能得到 partial result。

### 處理決議

兩種 FindAll mode 統一採用以下政策：

```text
預設 maxResults=0，不設隱藏數量上限
逐筆計數並 streaming 寫檔，不把完整 records 堆入 terminal 或 report vector
公開 CLI 自動產生不覆寫舊檔的 artifact 名稱
terminal 只回 summary、總數與檔案路徑
正值 maxResults 只保留給 prompt 明確指定前 N 筆的情況
```

未加入 thread、async 或 parallel execution；搜尋與檔案寫入皆維持單執行緒。

### 修改內容

正式程式碼：

| 檔案 | 函式 / 型別 | 修改內容 |
|---|---|---|
| `include/core/NetlistQueries.h` | `FunctionSearchQuery` | `maxResults` 預設改為 0；新增 `maxStoredMatches`、`writeMatchesToFile`、`outputFilePath` |
| `include/core/NetlistQueries.h` | `FunctionSearchReport` | 新增 `matchCount`、`wroteMatchesToFile`、`outputFilePath`；`matches` 明定為 bounded samples |
| `src/analysis/FunctionAnalysis.cpp` | Function Search output helpers | 新增 artifact 開啟、match/class record streaming 與尾端 completion summary |
| `src/analysis/FunctionAnalysis.cpp` | equivalent-pair FindAll | 先保留緊湊 classes，再逐筆展開、計數與 streaming；只有 `maxResults>0` 才截斷 |
| `src/analysis/FunctionAnalysis.cpp` | NAND FindAll | 每個 SAT-proven pair 立即計數與 streaming；只有 `maxResults>0` 才停止 |
| `tools.cpp` | `ToolSession` / automatic path helper | 新增 Function Search artifact sequence，建立 `<design>_function_search_<sequence>.txt` 並跳過既有檔案 |
| `tools.cpp` | `func_search` dispatch | FindAll 自動啟用寫檔、`maxStoredMatches=0`，保持既有 command grammar |
| `tools.cpp` | `printFunctionSearchReport()` | 輸出總數、sample 數、是否寫檔與路徑；寫檔時不在 envelope 重複列 records/classes |

測試與文件：

| 檔案 | 修改內容 |
|---|---|
| `mini test/test24/test24.ps1` | 驗證 NAND FindAll artifact record 數等於 `match_count`，並保留 explicit limit 測試 |
| `mini test/test26/test26.ps1` | 驗證 equivalent classes、完整 pair artifact、type filter 與 explicit limit |
| `Blup/function_search_runs/result_limit_probe/result_limit_probe.v` | 24 個等價 BUF outputs，同時產生 276 組 equivalent pairs 與 276 組 NAND pairs |
| `API_SPEC/FUNCTION_SEARCH_API.md` | 同步 unlimited、sample、streaming 與 report contract |
| `API_SPEC/FUNCTION_SEARCH_USAGE.md` | 同步 C++/CLI 使用方式與完整性判讀 |
| `TOOLS_SPEC/FUNCTION_SEARCH_TOOL.md` | 移除 LLM 推算 pair 上界重試流程，改為 `--all` 自動完整寫檔 |

### 驗證狀態

| 驗證 | 結果 |
|---|---|
| 276-match NAND probe | `match_count=276`，artifact 276 records，`complete=true` |
| 276-match equivalent-pair probe | 1 class / 276 pairs，artifact 276 records，`complete=true` |
| `mini test/test24/test24.ps1` | 9 passed / 0 failed |
| `mini test/test26/test26.ps1` | 18 passed / 0 failed |
| NewTestCase test29 | 7/7 equivalent-pair records，約 3.48 秒，complete |
| NewTestCase test30 | 1/1 equivalent-pair record，約 0.14 秒，complete |
| NewTestCase test35 | NAND FindAny 找到 1 組 SAT witness，約 5.66 秒，complete |

明確指定 `--max-results` 的 regression 仍回 `RESULT_LIMIT_REACHED`、`truncated=true` 與
partial，證明外部 grammar 與原有顯式限制能力皆保留。

## FUNCSEARCH-006：NAND FindAll 30 秒邊界風險

- 狀態：`Root Cause Confirmed`
- 優先級：P1
- 類型：Performance / Time Budget
- Boolean 相關：現階段只盤查，不進行 AIG 重構

### 問題

test35 #13 原文只問是否存在，正確命令是 FindAny 的 `func_search nand_pair n25`，現有 API
約 5.5 秒即可完整回答。封閉 runner 錯誤加上 `--all`，才觸發本節的 FindAll 診斷。

FindAll 重跑雖完成，但 report elapsed 約 29.46 秒，而
`FunctionSearchQuery::timeLimitSeconds` 預設只有 30 秒。首次執行已在約 30.00 秒回 partial，
因此 hidden prompt 若真的要求列出全部，結果會受機器負載與快取狀態影響，不能視為穩定通過；
此問題不阻擋目前 test35 的 existence prompt。

### 根因

| 階段 | 數量 / 行為 |
|---|---|
| candidate signals | 39,283 |
| simulation-eligible signals | 12,003 |
| candidate pairs | 72,030,003，雙層迴圈逐組檢查 signature |
| simulation-pass / SAT checks | 12,003 |
| SAT 建模 | 每一組呼叫 `solveNandPairEquivalenceDetailed()`，重新收集 cone、建立 CNF 與 solver |
| 完整 matches | 514 |

官方 basic operation 時限為 60 秒，但 API 的無參數預設值為 30 秒。短期可以討論將內部預設
調整為保留官方外層餘裕的值；中長期則應以候選索引、共享 CNF/SAT context，或後期 AIG
functional index 消除重複建模。修改前需先決定本階段是否只修 budget，或同時動演算法。

## FUNCSEARCH-002：Boolean engine/AIG 重構與 hidden structure coverage

- 狀態：`Boolean-Deferred`
- 優先級：P2
- 類型：Architecture / Boolean Analysis
- Boolean 相關：是

### 問題

目前 Function Search 直接使用 named netlist simulation 與 SAT encoding。若未來要統一處理更多
Boolean decomposition、arbitrary functional candidates、cofactor 或共享 cache，較適合建立由
current named netlist 衍生的 AIG functional index。

### 處理決議

本階段保留已能通過 testcase 的 simulation + SAT 實作。遇到 unsupported Boolean cone 或需要
新功能時先記錄，不做零散擴充；待後期 AIG 重構時統一處理。

### 修改內容

正式程式碼：無。

## FUNCSEARCH-003：timeout 停止誤差與 partial artifact 保存

- 狀態：`Verified`
- 優先級：P1
- 類型：Performance / Timeout / Partial Result
- Boolean 相關：否

### 問題

FindAll 在大型候選集合中可能於 time limit 前完成部分 SAT proofs。需要確認實際停止時間不會
嚴重超出 budget，且 timeout 前已證明的 matches 仍寫入 artifact，同時不得把 partial 結果
標成 complete。

NAND pair loop 每 4096 個 considered pairs 檢查一次外層時間；進入 SAT 前仍會把剩餘時間
傳給 solver。Equivalent-pair search 每個 candidate/class comparison 都重新計算剩餘時間。

### 修改內容

正式程式碼：無。現有 timeout 與 streaming 收尾行為符合 contract。

測試證據保留於：

```text
Blup/function_search_runs/timeout_probe/
```

### 驗證狀態

| Case / query | Budget | Report elapsed | 外部 elapsed | 已證明 / artifact | 結果 |
|---|---:|---:|---:|---:|---|
| test35 NAND FindAll | 0.01 秒 | 0.0107 秒 | 0.098 秒 | 0 / 0 | TIMEOUT、complete=false |
| test29 equivalent FindAll | 0.01 秒 | 0.0114 秒 | 0.028 秒 | 0 / 0 | TIMEOUT、complete=false |
| test35 NAND FindAll | 1 秒 | 1.0069 秒 | 1.097 秒 | 0 / 0 | TIMEOUT、complete=false |
| test29 equivalent FindAll | 1 秒 | 1.0010 秒 | 1.019 秒 | 7 / 7 | TIMEOUT、complete=false |

四個 artifact footer 均正確記錄 `Complete: no` 與 `Status: TIMEOUT`。test29 的 1 秒 probe
證明 timeout 前已完成的 7 組 SAT-proven pairs 會保留，且 artifact record 數等於
`match_count`。外部時間包含 process 啟動、read/parse 與 command envelope，因此略高於 report
內部計時屬合理現象。

## FUNCSEARCH-004：未知 primitive 在 reader 階段靜默遺失

- 狀態：`Out of Scope`
- 優先級：P3
- 類型：Reader / Design Integrity / Unsupported Detection
- Boolean 相關：direct MUX 的功能支援可留待 AIG，但 instance 不可靜默遺失

### 問題

`VerilogReader::read()` 對無法轉成既有 `GateType` 的 instance line 直接略過，仍回 read
成功。Function Search 接收到的 named netlist 已經缺少該 gate，因此無法增加
`unsupportedSignalCount`，反而可能回 `complete:true / NO_MATCH`。

### Probe 結果

```text
Blup/function_search_runs/unsupported_probe/unknown_primitive_probe.v
```

| 操作 | 結果 |
|---|---|
| `read unknown_primitive_probe.v` | `ok:true`，但 `gate_count:0` |
| `func_search equivalent_pairs whole --all` | `complete:true`、`NO_MATCH`、`unsupported:false` |
| `func_search nand_pair y --all --include-boundary-signals` | `complete:true`、`NO_MATCH`、`unsupported:false` |

### 責任與處理決議

此問題發生在 Function Search 之前，不能由搜尋 API 判斷原始 Verilog 中是否曾有未知
instance。不過官方 netlist 只使用題目規定的 gate types；prompt 提到的 MUX 或其他功能結構
會由合法 gates 組成的 Boolean expression 表示，不會出現 direct `mux` primitive。因此此項
標記為 `Out of Scope / No Action`，不列為競賽 blocker，也不在 Function Search 內加入
parser-specific workaround。

同一 probe 也確認：一般 undriven `floating_input` 會被現有 simulation/SAT 當成
unconstrained Boolean leaf，因此 supported gates 的 floating cone 仍可被完整分析，不會被當成
constant 0。這和「未知 primitive 整顆消失」是兩個不同問題。

## FUNCSEARCH-005：candidate、boundary、scope 與 gate-type filter 語意

- 狀態：`Verified`
- 優先級：P1
- 類型：Candidate Semantics / Scope / Filter
- Boolean 相關：否

### 測試設計

```text
Blup/function_search_runs/candidate_scope_probe/candidate_scope_probe.v
```

電路同時包含 PI `a/b/clk`、兩條等價 internal BUF nets、由 `a` 驅動的 PO、DFF.Q、floating
net、NAND target，以及包含/不包含等價 gates 的 fanin scopes。

### 驗證結果

| Query | Candidate count | Match / pair count | 結果 |
|---|---:|---:|---|
| NAND internal distinct | 4 | 1 | 只有 `n_a1,n_a2` |
| NAND internal allow-same | 4 | 3 | 加入 `n_a1,n_a1`、`n_a2,n_a2` |
| NAND include boundary | 9 | 6 | 正確納入 PI `a` 與 PO `po_copy` |
| NAND boundary allow-same | 9 | 10 | 四個 a-equivalent signals 的 unordered-with-self pairs |
| Equivalent whole | 6 gates | 1 class / 3 pairs | `g_a1/g_a2/g_po` |
| Equivalent whole BUF-only | 3 gates | 1 class / 3 pairs | filter 只限制 candidate gate type |
| Equivalent net_fanin po_logic | 2 gates | 0 | complete NO_MATCH |
| Equivalent net_fanin po_copy | 1 gate | 0 | complete NO_MATCH |

八個 FindAll artifact 的 record 數皆等於 `match_count`。Target 本身、constant、floating
undriven net 不會進入預設 NAND candidates；DFF.Q 可作為 internal Boolean boundary candidate。
Pairs 維持 unordered，只有 `--allow-same` 增加 self pairs。Scope 與 gate-type filter 行為符合
API contract，因此不需修改程式。

## 4. Prompt Responsibility Matrix

| Prompt 類型 | 負責 API |
|---|---|
| 未知 `(a,b)`，要求 `NAND(a,b)==target` | `FunctionSearchQuery::NandEquivalentInputPairs` |
| 未知 gate pairs/groups，output functions 相同 | `FunctionSearchQuery::EquivalentGatePairs` |
| 已知 net A/B，判斷 equivalence | `FunctionQuery::Equivalence` |
| 搜尋後實際 merge equivalent gates | `EditApply::MergeFunctionallyEquivalentGates`，內部使用 Function Search classes |
| 結構 duplicate cleanup | EditApply structural merge |
| observability-aware arbitrary redundant gate removal | 目前不屬於 Function Search equivalent-output contract |
| 修改前後 whole-design equivalence | Whole-design equivalence flow |

## 5. 後續處理順序

1. FUNCSEARCH-001 已完成，不再以 LLM 推算 pair 上界作為正常流程。
2. 後續只追蹤新的非 Boolean correctness、timeout 或 hidden-case coverage 問題。
3. Boolean engine/AIG 重構維持 `Boolean-Deferred`，不在本階段零散改動。
