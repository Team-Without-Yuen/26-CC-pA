# Depth Query API 盤查與修改紀錄

本文件只記錄 Depth Query backend、API 文件、mini regression 與官方 testcase 驗證。
修改程式前先登記問題與建議方案，經討論後再實作。

## 盤查範圍

```text
computeNetLevels / computeGateLevels
SpecificNet / PrimaryOutputs / DffD
GlobalCriticalPath
EndpointsExceedingDepth / PrimaryOutputsExceedingDepth
GateOnCriticalPath / DeepestOutputCone
critical path reconstruction
```

## 總覽

| ID | 優先級 | 狀態 | 是否修改 | 驗證 |
|---|---|---|---|---|
| DEPTH-001 | P0 | Fixed | 是 | tombstone/stale-edge regression PASS |
| DEPTH-002 | P1 | Fixed | 是 | includeCriticalPath=false matrix PASS |
| DEPTH-003 | P1 | Fixed | 是 | 30000-level GateOn deep-chain PASS |
| DEPTH-004 | P1 | Regression Passed | 否 | test5、test9 與官方 test04/31/32/34/35/36/39/40 已實跑 |
| DEPTH-005 | P1 | API Documented / Tools Pending | API 文件是 | 待同步 tools routing 說明 |

## 附件核對結果

`DEPTH_ANALYSIS_CHANGES_SUMMARY.md` 所列修改已存在於 active `DepthAnalysis.cpp`：

```text
1. getDffsWithDDepthGreaterThan() 已改為共用一次 computeNetLevels()。
2. makeDepthReportForNet() 已增加內部 buildCriticalPath 參數，公開簽章未改。
3. PrimaryOutputs、DffD、threshold、DeepestOutputCone 與 GateOnCriticalPath
   已在部分路徑共用 netLevels 並可跳過 path reconstruction。
```

8000 DFF 的 54 秒到 13 毫秒結果來自附件外部 harness，目前 repository 內沒有可重跑案例。

## DEPTH-001：level/path traversal 未完整遵守 tombstone 與雙向 edge

優先級：P0

狀態：Fixed

觀察：

```text
1. PI/constant/DFF.Q source 建立沒有排除 removed net/gate。
2. computeNetLevels() 直接使用 net.loadGateIds，沒有反查 gate.inputNetIds。
3. gate output 沒有驗證 net.driverGateId 是否指回該 gate。
4. makeDepthReportForNet()/SpecificNet 只檢查 ID 範圍，removed net 仍可能成為 endpoint。
5. PO/DFF.D collection 與 critical-path reconstruction 未統一排除 removed object。
6. GateOnCriticalPath downstream traversal 也只部分檢查 UNKNOWN/bounds，未驗證雙向 edge。
```

風險：

```text
edit/cleanup 後可能把 tombstone endpoint 納入 count/worst，或由 stale driver 計入不存在的深路徑。
無效 cached ID 雖有部分 bounds guard，但各入口規則不一致。
```

修改內容：

```text
`src/analysis/DepthAnalysis.cpp` active 區段新增 active net/gate、consistent driver/load helper。
source marking、level propagation、endpoint collection、path reconstruction 與 GateOnCriticalPath
已統一使用同一套規則；未修改 DepthQuery/DepthReportSet 公開型別。

`mini test/test5/test5.cpp` 新增 removed net、removed gate、stale driver edge regression，
確認 tombstone 不會成為 endpoint，失效 driver 不會被計入 depth/path。
```

## DEPTH-002：includeCriticalPath=false 的效能承諾只完成一部分

優先級：P1

狀態：Fixed

修改內容：

```text
SpecificNet、PrimaryOutputs、DffD、GlobalCriticalPath、兩種 threshold query、
GateOnCriticalPath 與 DeepestOutputCone 均依 includeCriticalPath 決定是否建 path。
Global 與 threshold query 先共用一次 netLevels 收集 endpoint metadata，
只替 global winner 或 threshold 入選項目重建 path。
public findGlobalCriticalPath()/findEndpointsExceedingDepth() 也改為單次 level computation。

`mini test/test5/test5.cpp` 新增八種 query mode 的 path suppression matrix，
確認 includeCriticalPath=false 時 reports/worst 均不含 path。
```

## DEPTH-003：GateOnCriticalPath downstream analysis 仍使用遞迴 DFS

優先級：P1

狀態：Fixed

問題：

```text
maxRemainingDepthFromNet 使用 std::function recursive DFS。
極深 combinational chain 仍會消耗 C++ call stack；Cone longest path 已改 iterative，
但 Depth GateOnCriticalPath 尚未採用相同防護。
```

修改內容：

```text
`src/analysis/DepthAnalysis.cpp` 改用已計算的 netLevels 排序 active nets，
在 DAG 上由高 level 往低 level 執行 iterative reverse-depth DP，
保留「任一 maximum-depth path」語意且不再依賴 C++ recursion stack。

`mini test/test5/test5.cpp` 新增 30000 個 BUF 的 combinational chain，
查詢第一顆 gate 時通過，global depth 為 30000。
```

## DEPTH-004：既有與官方 testcase baseline

優先級：P1

狀態：Regression Passed

應實跑：

```text
mini test/test5：GateOnCriticalPath / DeepestOutputCone。
NewTestCase/test04：指定 output fanin depth。
NewTestCase/test31、test36：PI 到 DFF.D maximum depth、PO depth > 4 count。
NewTestCase/test32：gate g0 是否位於任一 maximum-depth path。
NewTestCase/test34、test35：PI-to-PO maximum/deepest output。
NewTestCase/test39：PO depth > 4 count / global maximum depth。
NewTestCase/test40：register-to-register maximum depth / global maximum depth。
```

實跑結果：

| Testcase / command | 結果 |
|---|---|
| `mini test/test5` | `11/11 PASS`（含 30000-level deep chain） |
| `mini test/test9` | `20/20 PASS` |
| test04 `depth_query net n3` | depth `14` |
| test31 `depth_query po_exceeding 4` | `32` outputs，worst depth `14` |
| test31 `depth_query all_dff_d` | `756` DFF.D reports，worst depth `58` |
| test32 `depth_query gate_on_critical g0` | `no`，global depth `58` |
| test34 `depth_query deepest_output` | `343` PO bits，worst `n30` depth `0` |
| test35 `depth_query global_critical`（原始設計） | `g8589.D`，depth `125` |
| test36 `path_query max_depth all_pi all_dff_d` | depth `19`；此 prompt 明確限定 PI，故不用 DepthQuery |
| test39 `depth_query po_exceeding 4` | `47` outputs，worst depth `77` |
| test39 `depth_query global_critical` | depth `77` |
| test40 `depth_query net n14` | depth `2` |
| test40 `depth_query global_critical` | `g2153.D`，depth `104` |
| test40 `path_query max_depth all_dff_q all_dff_d` | depth `104` |

先前 `Blup/THREE_API_FINAL_COVERAGE.md` 與 Path 盤查文件記錄 test40 depth `105`，
但目前工具重跑 Path/Depth 都是 `104`；這是舊紀錄過期，不是本輪 Depth off-by-one。

## DEPTH-005：PI-only prompt 與 timing-global depth 的責任邊界

優先級：P1

狀態：API Documented / Tools Pending

```text
DepthQuery 的 arrival sources 是 PI、constant、DFF.Q，timing endpoints 是 PO、DFF.D。
因此 GlobalCriticalPath 回答全設計 timing-boundary maximum，不等於限定 PI -> PO。

「from any primary input to any primary output」應使用 PathQuery：all_pi -> all_po max depth。
「from any primary input to any DFF D-pin」應使用 PathQuery：all_pi -> all_dff_d max depth。
「maximum combinational logic depth in the design」才使用 DepthQuery::GlobalCriticalPath。
```

`TOOLS_SPEC/DEPTH_QUERY_TOOL.md` 的 `gate_on_critical` 表格目前寫成「代表性 global critical path」，
但 backend 實際語意是「任一 global maximum-depth path」，後續 tools 文件同步時需修正。

本輪已修正 `API_SPEC/DEPTH_QUERY_USAGE.md` 的 PI-only prompt routing；
`tools.cpp` 與 `TOOLS_SPEC` 依既定整批同步流程暫不修改。
