# Direct Connectivity Query API 盤查與修改紀錄

本文件只記錄 Direct Connectivity Query API backend / API 文件 / mini regression。
`ConnectivityAnalysis.cpp` 後半部 `/* ... */` 內為註解保留的原始版本，不參與本次修正。

## 總覽

| ID | 優先級 | 狀態 | 是否修改 | 驗證 |
|---|---|---|---|---|
| DIRECT-001 | P0 | Fixed | 是 | direct-only 30/30 PASS |
| DIRECT-002 | P0 | Fixed | 是 | stale output public regression PASS |
| DIRECT-003 | P0 | Fixed | 是 | direct-only 30/30 PASS |
| DIRECT-004 | P0 | Fixed | 是 | include flag matrix regression PASS |
| DIRECT-005 | P1 | Fixed | 是 | stale driver public regression PASS；load 端 code-level 防護 |

## DIRECT-001：removed scalar / all-removed bus 被誤判為存在

優先級：P0

狀態：Fixed

問題重現：

```text
NetDriverGates / NetLoadGates 對 removed scalar net 可能回 ok=true、exists=true、count=0。
bus query 若所有 bit 都 removed，也可能因 expandNetToBits() 非空而被視為存在。
```

原因：

```text
高階 dispatch 只檢查 getNetId() 是否是合法 vector index，或 expandNetToBits() 是否非空。
但 tombstone net 的 ID/name map 仍保留，因此合法 ID 不代表 active net。
```

修改檔案：

| 檔案 | 修改內容 |
|---|---|
| `src/analysis/ConnectivityAnalysis.cpp` | 新增 `activeNetIdsForName()`，NetDriverGates / NetLoadGates 以 active bit existence 判斷 net 是否存在。 |
| `mini test/test1/tester.cpp` | 新增 removed scalar net、partial removed bus、all-removed bus regression。 |
| `API_SPEC/DIRECT_CONNECTIVITY_QUERY_API.md` | 明定 removed scalar net 與 all-removed bus 的存在性語意。 |
| `API_SPEC/DIRECT_CONNECTIVITY_QUERY_USAGE.md` | 補上 tombstone 與 bus active-bit 使用規則。 |

驗證：

```text
mini test/test1/tester.exe mini test/test1/mini_circuit.v --direct-only
Summary: 30 passed, 0 failed.
```

tools 層同步：否。

## DIRECT-002：GateOutput 可能暴露 removed output net

優先級：P0

狀態：Fixed

問題重現：

```text
GateOutput dispatch 直接使用 getGateOutputNetId() 後只檢查 isValidNetId()。
若 output net 已 tombstone，可能填入 removed net 的 id/name。
```

原因：

```text
低階 getGateOutputNetName() 已排除 removed net，但高階 dispatch 沒有沿用相同 active-net 語意。
```

修改檔案：

| 檔案 | 修改內容 |
|---|---|
| `src/analysis/ConnectivityAnalysis.cpp` | GateOutput 改用 `hasConsistentActiveDriver()`；active gate 若沒有有效且一致的 active output，report 保持 ok=true、exists=true，但 count=0、netId=-1 且不填 output payload。 |
| `mini test/test1/tester.cpp` | 以 public `connectGateOutput()` 建立 driver 被取代後的 stale gate output，確認 GateOutput 不洩漏 stale net ID/name。 |
| `API_SPEC/DIRECT_CONNECTIVITY_QUERY_API.md` | 明定 GateOutput 對 active gate 無有效 output 時回 count=0。 |

驗證：

```text
public API 連續將兩顆 gate 的 output 接到同一條 net，可形成舊 gate 仍保留 outputNetId、
但 net.driverGateId 已指向新 gate 的不一致狀態。regression 確認舊 gate 的 GateOutput
回 count=0、netId=-1 且 payload 為空。
```

tools 層同步：否。

## DIRECT-003：DirectlyConnected 對 tombstone gate/net 的 exists 語意錯誤

優先級：P0

狀態：Fixed

問題重現：

```text
DirectlyConnected 只檢查 gate/net ID 範圍。
removed gate 或 removed net 可能被回報成 ok=true、exists=true、connected=false。
```

原因：

```text
高階 dispatch 沒使用 active gate/net 判斷。
```

修改檔案：

| 檔案 | 修改內容 |
|---|---|
| `src/analysis/ConnectivityAnalysis.cpp` | DirectlyConnected 改用 `isActiveGate()` / `isActiveNet()` 判斷存在性。 |
| `mini test/test1/tester.cpp` | 新增 removed scalar net 與 removed gate 的 DirectlyConnected regression。 |
| `API_SPEC/DIRECT_CONNECTIVITY_QUERY_USAGE.md` | 明定 removed gate/net 為 not found，active gate/net 但不直接相連才是 connected=false。 |

驗證：

```text
mini test/test1/tester.exe mini test/test1/mini_circuit.v --direct-only
Summary: 30 passed, 0 failed.
```

tools 層同步：否。

## DIRECT-004：GateInputs count 受到 include flags 影響

優先級：P0

狀態：Fixed

問題重現：

```text
includeIds=false 且 includeNames=false 時，GateInputs 的 count 可能錯誤為 0。
```

原因：

```text
舊 dispatch 由已填入的 netIds 或 netNames 長度推導 count。
include flags 關閉 payload 後，count 也被連帶影響。
```

修改檔案：

| 檔案 | 修改內容 |
|---|---|
| `src/analysis/ConnectivityAnalysis.cpp` | GateInputs 先計算完整 inputNetIds，count 永遠取完整長度；include flags 只控制 payload。 |
| `mini test/test1/tester.cpp` | 新增四種 include flag matrix regression。 |
| `API_SPEC/DIRECT_CONNECTIVITY_QUERY_USAGE.md` | 明定 includeIds/includeNames 不改變 count。 |

驗證：

```text
mini test/test1/tester.exe mini test/test1/mini_circuit.v --direct-only
Summary: 30 passed, 0 failed.
```

tools 層同步：否。

## DIRECT-005：cached driver/load edge 缺少雙向一致性驗證

優先級：P1

狀態：Fixed

問題重現：

```text
若 edit flow 留下 stale net.driverGateId 或 net.loadGateIds，Direct Connectivity helper
可能只因 cache 仍存在而回報不存在的直接連線。
```

原因：

```text
部分 helper 只確認 cached gate ID 是 active gate，沒有反查 gate.outputNetId 或 gate.inputNetIds。
```

修改檔案：

| 檔案 | 修改內容 |
|---|---|
| `src/analysis/ConnectivityAnalysis.cpp` | 新增 `hasConsistentActiveDriver()` 與 `isConsistentActiveLoad()`；driver/load/fanin/fanout/direct-check helper 全部使用雙向一致性。 |
| `mini test/test1/tester.cpp` | 新增 stale driver/output、目前 driver 唯一性、DirectlyConnected 拒絕 stale edge，以及 tied-input gate-level/pin-level 計數 regression。 |
| `API_SPEC/DIRECT_CONNECTIVITY_QUERY_API.md` | 明定 cached edge 必須與 gate 端 output/input pin 互相吻合。 |
| `API_SPEC/DIRECT_CONNECTIVITY_QUERY_USAGE.md` | 補上 stale cached edge 不會被回報的使用語意。 |

驗證：

```text
driver 端可由 public API 建立 stale output 情境，三個對應 regression 均 PASS。
load 端沒有公開 API 可單獨破壞 net.loadGateIds 與 gate.inputNetIds 的對應，
因此 missing/stale load cache 仍以 `isConsistentActiveLoad()` 的 code-level 防護為主。
```

tools 層同步：否。

## 本輪驗證

編譯：

```text
UCRT64 g++ mini tester build: PASS
```

Direct API regression：

```text
mini test/test1/tester.exe mini test/test1/mini_circuit.v --direct-only
Summary: 30 passed, 0 failed.
```

Basic API 隔離 regression：

```text
mini test/test1/tester.exe mini test/test1/mini_circuit.v --basic-only
Summary: 26 passed, 0 failed.
```

Tools CLI 整合 regression：

```text
mini test/test9/test9.ps1 -Executable tools.exe
Summary: 20 passed, 0 failed.
```

`tools.exe` 由 `TOOLS_SPEC/Makefile` 重新編譯成功；編譯只出現既有第三方函式庫 warning。
本輪未修改 `tools.cpp` / `TOOLS_SPEC`，因 public command、參數與 report envelope 均未改變。

## 尚未處理風險

```text
1. `tools.cpp` / `TOOLS_SPEC` 本輪未同步，因 Direct backend 的 public query 參數與輸出欄位未改。
2. load 端的單邊 stale private cache 無法用現有 public API 直接造出；目前以 helper 雙向一致性防護。
3. 本輪以 Direct-only 與 Basic-only 做範圍驗證；未將完整 test1 的其他 API 視為本項通過條件。
```
