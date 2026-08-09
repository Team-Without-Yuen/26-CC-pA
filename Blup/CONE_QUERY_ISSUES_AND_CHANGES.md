# Cone Query API 盤查與修改紀錄

本文件只記錄 Cone Query API backend、API 文件、mini regression 與官方 testcase 驗證。
修改程式前先記錄問題與建議方案，經討論後才實作。

## 盤查範圍

```text
NetTransitiveFanin
NetTransitiveFanout
GateTransitiveFanin
GateTransitiveFanout
LargestOutputCone
SharedFaninGates
findLongestPathInCone / findShortestPathInCone
```

## 總覽

| ID | 優先級 | 狀態 | 是否修改 | 驗證 |
|---|---|---|---|---|
| CONE-001 | P0 | Fixed | 是 | test6 active/tombstone/bus/stale edge PASS |
| CONE-002 | P1 | Fixed | 是 | SharedFanin empty result/flags PASS |
| CONE-003 | P1 | Fixed | 是 | local path edge cases與 100000-level chain PASS |
| CONE-004 | P1 | Baseline Passed | 否 | test15/test31/test32/test37/test40 已實跑 |

## 已確認語意

### DFF.Q fanin boundary

依官方 Q65 與現有專案規格，DFF.Q/register output 是 sequential boundary：

```text
fanin traversal 不得穿越同一顆 DFF 回到 D/CK/RN/SN。
combinational gate count = 0。
local depth = 0。
ConeResult/report 可以保留使用者查詢的 root net，因此 net count 可為 1。
```

目前 `getTransitiveFaninCone()` 在 root driver 是 DFF 時停止，符合這項規則。

### addNet 冪等性

真實 `Netlist::addNet()` 會先查 `netNameToId`；同名 active net 直接回傳既有 ID，
removed tombstone 則在原 ID 上重建。附件提出的 VerilogReader 重複 net/斷線風險目前不存在。

## CONE-001：traversal 與高階 dispatch 未完整套用 active-object/edge consistency

優先級：P0

狀態：Fixed

觀察：

```text
1. getTransitiveFaninCone()/getTransitiveFanoutCone() 直接使用 expandNetToBits() 結果，
   沒有排除 removed net。
2. fanin traversal 直接以 net.driverGateId 索引 gates，未先驗證 ID、removed gate，
   或 gate.outputNetId 是否仍指回目前 net。
3. fanout traversal 直接以 net.loadGateIds 索引 gates，未先驗證 ID、removed gate，
   或 gate.inputNetIds 是否仍包含目前 net。
4. GateTransitiveFanin/Fanout dispatch 只做 isValidGateId() bounds check，
   GateType::UNKNOWN tombstone 仍可能被視為來源。
5. getConeNetIds() 只檢查 ID 範圍，removed net 仍可能出現在 report/count。
6. LargestOutputCone 只檢查 PO net ID 範圍，未明確排除 removed PO net。
```

風險：

```text
cleanup/edit 後可能回報已刪除物件或 stale connectivity。
若 stale cached gate ID 超出範圍，traversal 可能直接越界。
```

修改內容：

```text
在 ConeAnalysis.cpp 內建立 active net/gate、consistent driver/load 與 active bus-root helper。
所有 traversal、gate wrapper、count/name conversion、dispatch 與 LargestOutputCone 共用同一套規則。
所有 gate/net vector indexing 前先驗證 active object；公開參數與回傳型別未改。
```

Regression：

```text
removed scalar net、all-removed/partial-removed bus、removed gate與 public stale driver。
load cache 的單邊破壞沒有 public API 可直接建立，實作仍以雙向 helper 防護。
```

## CONE-002：SharedFaninGates 的 exists 與 include flags 語意不一致

優先級：P1

狀態：Fixed

觀察：

```text
1. 兩個來源 net 都存在但 intersection 為空時，程式回 ok=true、exists=false。
   ConeReport.exists 的宣告語意是「指定來源是否存在並成功建立 cone」，
   因此 empty result 不應等同來源不存在。
2. rootNetIds/rootNetNames 目前無條件填入，沒有遵守 includeIds/includeNames。
3. bus sourceId fallback 可能取到未經 active-object 過濾的第一個 bit。
```

修改內容：

```text
來源都合法時固定 ok=true、exists=true；gateCount=0 表示沒有 shared gates。
includeIds/includeNames 只控制對應 payload，不影響 count 與成功狀態。
bus roots 只使用 active bit nets。empty intersection 與 flags=false regression 已通過。
```

## CONE-003：iterative longest-path 缺少 repository 內可重跑的壓力測試

優先級：P1

狀態：Fixed

現況：

```text
findLongestPathInCone() 已由 recursive DFS 改為 iterative post-order traversal。
附件聲稱 28 個等價案例與 100 萬層 chain 通過，但 harness/fixture 未納入 repository。
現有 mini test/test15/test15.ps1 測的是 Basic port metadata，不是 Cone Query。
```

已補充：

```text
mini test/test6 加入不依賴巨大 Verilog 檔的程式化 ConeResult regression：
empty、reconvergent DAG、multi-root tie、cycle guard 與 100000-level chain。
深鏈測試驗證 depth、path size 與兩端 endpoints。
```

## CONE-004：官方 testcase prompt baseline

優先級：P1

狀態：Baseline Passed

直接屬於 Cone Query 的已公開 prompt：

```text
Compute the transitive fanin cone of output ...
Compute the transitive fanout cone of input ...
How many gates are in the fanin/logic cone of ...?
Report all gates shared between the fanin cones of ... and ...
Report the number of each gate type in the cone of ...
Compute the fanin logic cone ... and list all gates that contribute ...
Which output has the largest fanin cone?
```

`deepest fanin logic cone` 與 `maximum logic depth` 主要由 Depth Query 負責；
cone transformation/optimization 則由 Edit/Opt API 負責，本文件只確認 Cone Query 提供的 scope/count。

實跑結果：

| Testcase / command | 結果 |
|---|---|
| `NewTestCase/test15`, `net_fanin n3` | `nets=1`, `gates=0` |
| `NewTestCase/test15`, `net_fanout n0[0]` | `nets=190`, `gates=189` |
| `NewTestCase/test31`, `shared_fanin n16 n17` | 無 shared gates，command 正常完成 |
| `NewTestCase/test32`, `net_fanout n0` | DFF boundary，`nets=1`, `gates=0` |
| `NewTestCase/test32`, `net_fanin n12` | DFF boundary，`nets=1`, `gates=0` |
| `NewTestCase/test37`, `net_fanin n8` | DFF.Q boundary，`nets=1`, `gates=0` |
| `NewTestCase/test40`, `largest_output` | `n11[23]`, `nets=29`, `gates=22`, checked PO bits `134` |

現有 `mini test/test6` 重新編譯並執行：

```text
Summary: 16 passed, 0 failed.
```

Tools CLI 整合 regression：

```text
mini test/test9/test9.ps1 -Executable tools.exe
Summary: 20 passed, 0 failed.
```

以 `TOOLS_SPEC/Makefile` 重編 `tools.exe` 成功；只有既有第三方 library warnings。
重編後再跑 test15/test31/test37/test40，正常 netlist 的 cone count、gate-type breakdown
與 largest output 結果均和修正前一致。

## 本輪修改檔案

| 檔案 | 修改內容 |
|---|---|
| `src/analysis/ConeAnalysis.cpp` | active-object、雙向 edge、active bus roots、SharedFanin report 與 active PO 掃描。 |
| `mini test/test6/test6.cpp` | 16-case Cone API regression，包含 100000-level chain。 |
| `API_SPEC/CONE_QUERY_API.md` | backend 規則、SharedFanin 與測試狀態。 |
| `API_SPEC/CONE_QUERY_USAGE.md` | 對外使用語意、exists/include flags 與測試狀態。 |

`tools.cpp` / `TOOLS_SPEC` 本輪不需修改：command、參數、輸出 envelope 與可見欄位均未改變。

## 附件核對結果

```text
附件 CONE_QUERY_REVIEW.md 與 active findLongestPathInCone() 修改方向一致。
真實 addNet() 已確認為冪等。
附件的外部 28-case/深鏈 harness 未納入 repository，需補成正式 regression 才能重現。
```
