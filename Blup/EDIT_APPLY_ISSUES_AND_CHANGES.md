# Edit Apply API 盤查與修改紀錄

本文件記錄 Edit Apply facade、mutation/cleanup backend、report、timeout 風險、
mini regression 與官方 testcase 驗證。正式程式修改前，先登記問題、影響與預定修法。

## 盤查範圍

```text
EditApplyRequest validation / runEditApply dispatch
rename / cleanup / simplification
fanout buffer insertion
technology mapping facade
transaction / rollback / structure validation
NetlistEditReport consistency
large-netlist complexity and timeout
```

Boolean FunctionSearch 與 whole-design SAT equivalence 的重構項目只記錄，
本輪不直接修改其演算法。

## 總覽

| ID | 優先級 | 狀態 | 是否修改 | 驗證 |
|---|---|---|---|---|
| EDIT-001 | P0 | Fixed | 是 | 專用 regression PASS；test2 32/32 PASS |
| EDIT-002 | P1 | Verified | 附件已修改 | direct dedup + repeated mapping PASS |
| EDIT-003 | P1 | Fixed / Verified | 是 | 100 萬 gate public EditApply PASS（0.96 秒） |
| EDIT-004 | P1 | Fixed / Verified | 是 | 100 萬 NOT chain + safety regression PASS |
| EDIT-005 | P1 | Fixed / Verified | 是 | 100 萬 gate same-group/cascade + safety PASS |
| EDIT-006 | Deferred Boolean | 否 | 否 | 等 FunctionSearch / AIG / equivalence 重構 |
| EDIT-007 | P0 | Fixed | 是 | low/high-level rejection + writer regression PASS |
| EDIT-008 | P0 | Fixed | 是 | pin-level replace-all-loads regression PASS |
| EDIT-009 | P1 | Fixed / Verified | 是 | test1 142/142 PASS |

## 附件核對結果

`EDIT_APPLY_REVIEW.md` 所列四項修改均存在於目前 active source：

```text
1. collectFanoutSinkPins() 已改用 Net::loadGateIds。
2. addNet() 已將 1'b0 / 1'b1 納入 netNameToId 去重。
3. getConst0NetId() / getConst1NetId() 原先先使用 O(1) name lookup，
   但查無 canonical constant 時仍會 fallback 掃描全部 nets。
4. collapseBackToBackInverters() 已改為 worklist。
```

但附件沒有附 repository 內可重跑的完整 regression；目前只能確認程式存在，
不能直接視為 correctness 與 timeout 均已驗證。

## EDIT-001：pin-level loadGateIds 被重複展開

優先級：P0

狀態：Fixed

位置：

```text
src/transformation/NetlistTransformation.cpp
collectFanoutSinkPins()
getQaFanoutLoadCount()
moveFanoutSinkPinsToBuffer()
```

問題：

```text
connectGateInput() 每連接一個 input pin，就在 net.loadGateIds 加入一次 gateId。
因此同一顆 gate 的兩個 pins 都接同一 net 時，loadGateIds 會含兩筆相同 gateId。

目前 collectFanoutSinkPins() 對 loadGateIds 的每一筆 gateId，
又掃描該 gate 的全部 matching input pins。
兩個真實 pins 因此會被展開成 2 x 2 = 4 筆 FanoutSinkPin。
```

影響：

```text
1. QA fanout load 被高估。
2. 不必要地插入更多 buffer。
3. 同一 pin 可能同時出現在 keptSinks 與 movedSinks，造成雙向 edge 不一致。
4. buildEditReport() 雖可能在事後 validation rollback，但無法防止 mutation 中途錯接或越界。
```

修改內容：

```text
`src/transformation/NetlistTransformation.cpp::collectFanoutSinkPins()` 新增
`visitedGateIds`，先將 pin-level loadGateIds 去重成候選 gate IDs，再對每顆候選
gate 掃描 matching pins 一次；同時略過 UNKNOWN gate。

仍以 pin 為 fanout 計數單位，未修改 QA fanout 語意、public API、request 或 report。
```

驗證：

```text
修正前 `fanout_tied_pin_regression`：
- query fanout = 2
- backend 嘗試插入 4 顆 BUF
- structure validation 發現 2 條 stale load edge
- report success=false、rolledBack=true

修正後 `fanout_tied_pin_regression`：
- query fanout = 2
- backend 插入 2 顆 BUF
- report success=true、rolledBack=false
- structure_valid=true

完整 `mini test/test2`：32 passed、0 failed。
```

## EDIT-002：constant net dedup

優先級：P1

狀態：Verified

附件修改位於 `src/core/Netlist.cpp::addNet()`。

驗證結果：

```text
1. 重複 addNet("1'b0") / addNet("1'b1") 回傳同一 ID。
2. getNetId() 可直接命中 canonical constant。
3. 8 顆 NOT 與 8 顆 BUF 映射成 XOR basis，分別共用唯一 const1 / const0。
4. 同一設計再次執行 ConvertToBasis，net slot count 不再增加。
5. 完整 mini test/test2：34 passed、0 failed。
```

## EDIT-003：constant lookup complexity

優先級：P1

狀態：Fixed / Verified

問題：

```text
1. simplifyGateWithConstant() 對每個 candidate 都取得 const0 與 const1；若其中一種
   constant 不存在，getConst*NetId() 會對全部 nets 做 fallback scan，形成 O(G x N)。
2. 每化簡一顆共用高 fanout input 的 gate，都在 loadGateIds 上 erase/remove，形成 O(E^2)。
3. public EditApply report 會計算 before/after depth；computeNetLevels() 對已由
   loadGateIds 取得的 edge 再掃一次完整 fanout，也會在高 fanout circuit 形成 O(E^2)。
```

修改內容：

```text
1. `src/optimization/NetlistOptimization.cpp`：getConst0NetId() / getConst1NetId()
   僅使用 canonical name index。addNet() 保證 constant dedup 與 name index，EDIT-007
   又禁止 constant rename，因此找不到 canonical name 時可直接回 -1。
2. bulk SimplifyConstants 啟用 deferred load-list maintenance；rewrite 完成後由所有
   active gate input pins 一次重建 Net::loadGateIds，保持 pin-level multiplicity。
3. `include/core/Netlist.h` 新增只在同步 bulk mutation 期間使用的 deferred state；
   public request、參數與 report schema 均未改變。
4. `src/analysis/DepthAnalysis.cpp::computeNetLevels()` 不再對 traversal 已知 edge
   重掃完整 fanout；完整雙向 adjacency correctness 仍由 validateStructure() 驗證。
```

驗證：

```text
修正前：100,000 gates 超過 30 秒；分段定位後核心 simplify 約 18.01 秒。
修正後：100,000 gates 核心 simplify 0.0088 秒，完整 runEditApply 0.052 秒。
官方上限規模：1,000,000 gates，build 1.25 秒、runEditApply 0.96 秒，
simplified_count=1,000,000、success=true、rolled_back=false、structure_valid=true。
完整 mini test/test2：37 passed、0 failed。
Depth Query mini test/test5：11 passed、0 failed。
```

## EDIT-004：double-inverter worklist

優先級：P1

狀態：Fixed / Verified

已確認問題：

```text
1. worklist 已消除每 collapse 一對就全圖重掃的 timeout 風險，正常方向正確。
2. g1.outputNetId、g1 input、g2id、g2.outputNetId 與 outNet loads 缺少完整上界檢查，
   malformed/stale adjacency 可能在 validation 前直接越界。
3. 未確認 midNet.driverGateId == g1、g2 的唯一 input == midNet、
   outNet.driverGateId == g2，stale edge 可能把不相連的 NOT 誤判為一對並刪除。
4. worklist 與 findDoubleInverterPairs()/bypassDoubleInverter() 各自維護不同條件，
   finder 回報、單對 mutation 與 public bulk mutation 可能產生不同答案。
5. outNet fanout rewiring 手寫 load-list append，沒有集中沿用 pin-level rewiring helper。
```

修改內容：

```text
建立共用的 safe-collapse predicate，統一檢查 active/tombstone、ID range、NOT arity、
driver/load/input 雙向一致與 PO guard。finder、bypass 與 worklist 共用同一語意；
實際 rewiring 統一呼叫 replaceAllLoadsOfNet()，public request/report schema 不變。
```

涉及檔案與內容：

```text
1. `src/optimization/NetlistOptimization.cpp`：新增 isSafeDoubleInverterPair()；
   findDoubleInverterPairs()、bypassDoubleInverter()、collapseBackToBackInverters()
   統一使用同一 predicate。
2. bypass mutation 改用 replaceAllLoadsOfNet()，保留 pin-level fanout multiplicity。
3. `mini test/test2/test2.cpp`：新增 PO guard、stale driver/load、tombstone、
   invalid net/gate ID 的 safe rejection regression。
4. `mini test/test2/double_inverter_scalability.cpp`：新增公開 EditApply deep-chain test。
```

驗證：

```text
1. 正常 EditApply collapse 與新增安全拒絕案例：test2 38 passed、0 failed。
2. 100,000 NOT chain：collapse 50,000 pairs，runEditApply 0.060 秒。
3. 1,000,000 NOT chain：collapse 500,000 pairs，runEditApply 0.741 秒。
4. 99,999 NOT chain：collapse 49,999 pairs，正確保留 1 顆 NOT。
5. 偶數規模皆 success=true、rolled_back=false、active_not_count=0、
   structure_valid=true。
```

## EDIT-005：structural merge worst-case

優先級：P1

狀態：Fixed / Verified

已確認問題：

```text
1. 每次有 merge 就重建全設計 structural groups；級聯 duplicate 每輪只新增下一層
   candidate 時，最壞為 O(G^2)。
2. 同組大量 gates 共用 input nets 時，每移除一顆 gate 都在巨大 loadGateIds 上
   erase/remove，另一條路徑形成 O(G^2)。
3. pass 結尾隱含 trimDeadLogic()，會額外刪除不是 structural duplicate 的 dangling
   gates，讓 report active gate delta 與 prompt 所問 redundant gate 數量失真。
4. 不同具名 PO 的 duplicate drivers 不能直接把兩個 Port.netIds 指向同一 named net；
   否則可能破壞 writer 的 output-name/driver 語意，應保守保留各 PO driver。
5. rewiring 對 stale/invalid output/load/input IDs 缺少完整 mutation 前檢查。
6. merge 後仍保留大量 canonical gates 時，validateStructure() 對每個 input pin
   重掃完整 fanout list；高 fanout common input 使 public report 再次形成 O(E^2)。
```

修改內容：

```text
改成單次初始化 + affected-load worklist；key map 命中時重新驗證 canonical gate 的
current key，避免 input rewiring 後的 stale hash。只處理通過結構安全檢查的 active gates，
保留多個 PO drivers，不隱含 trim dead logic；input load tombstone 於 pass 結束一次
重建，output rewiring 保持 pin-level adjacency。public request/report schema 不變。
validateStructure() 改為一次建立 expected pin-level loads，再逐 net 比對 actual/expected。
```

涉及檔案與內容：

```text
1. `src/optimization/NetlistOptimization.cpp::mergeStructurallyEquivalentGates()`：
   改為 canonical key map + affected-load worklist，不再反覆全圖 grouping。
2. canonical map 每次命中都重新核對 active gate 與 current structural key，避免
   downstream input rewiring 後使用 stale hash。
3. 只在 mutation 前驗證 output/load adjacency；移除 gate 的 input tombstone load
   於 pass 結束由 rebuildNetLoadGateIds() 一次清理，避免 shared-input vector erase。
4. 兩個不同具名 PO drivers 保守不合併；PO driver 與 internal duplicate 衝突時，
   選 PO driver 為 canonical，保留 output 名稱與 driver。
5. 移除 pass 內隱含 trimDeadLogic()；dangling cleanup 仍由明確的 cleanup command
   或 SafeCleanupFixpoint 後續步驟執行。
6. `validateStructure()` 改為 expected/actual pin-level adjacency 比對，並檢查
   multiplicity；不再對每個 input pin 重掃完整 fanout。
7. public request、參數與 report schema 均未改變。
```

效能與驗證：

```text
修正前：
- same_group 100,000 gates：2.22 秒。
- cascade 1,000 levels：0.25 秒；5,000 levels：10.97 秒。
- cascade 250,000 levels：13.26 秒（主要卡在 validateStructure）。

修正後：
- same_group 100,000 gates：0.061 秒。
- same_group 1,000,000 gates：0.615 秒，merge 999,999 gates。
- cascade 5,000 levels：0.009 秒；10,000 levels：0.024 秒。
- cascade 250,000 levels：0.536 秒。
- cascade 500,000 levels / 1,000,000 gates：1.141 秒，merge 500,000 gates。
- 所有 scalability case 均 success=true、rolled_back=false、structure_valid=true。
- test2：39 passed、0 failed；Depth test5：11 passed、0 failed。
```

## EDIT-006：Boolean / whole-design equivalence dependency

狀態：Deferred Boolean

`MergeFunctionallyEquivalentGates` 的 FunctionSearch、representative selection 與
whole-design SAT equivalence 依賴後期 Boolean/AIG 重構。本輪只保留問題，不修改。

`MergeStructurallyEquivalentGates` 目前 structural key 仍可能涵蓋 DFF；同 input DFF
是否可合併涉及 initial state 與 sequential functional equivalence，本輪依規定只標記，
待 Boolean/AIG sequential proof 重構時一併決定，不在 EDIT-005 修改。

## EDIT-007：RenameNet 可破壞 constant literal 語意

優先級：P0

狀態：Fixed

問題：

```text
public RenameNet 目前允許將 "1'b0" / "1'b1" 改成一般 signal name，
並仍以 StructuralIdentity 宣告 functional equivalence。

VerilogWriter 對 gate inputs 直接輸出 net.name；同時 isConst net 不會被宣告成 wire。
因此 constant rename 後會輸出未宣告的一般名稱，而不是 Verilog constant literal，
導致輸出設計無效或功能改變。
```

修改內容：

```text
1. `src/transformation/EditFlow.cpp`：RenameNet request validation 拒絕來源
   `isConst`，也拒絕目標名稱為 `1'b0` / `1'b1`。
2. `src/transformation/NetlistTransformation.cpp::renameNet()`：加入相同底層防護，
   並排除 removed/invalid net，避免繞過 facade。
3. 一般 internal net / PI / PO rename 語意維持不變，未修改 public API。
```

驗證：

```text
1. 修正前：底層與 runEditApply constant rename 兩項 regression 皆 FAIL。
2. 修正後：`renameNet("1'b0", "renamed_zero")` 回 false。
3. 一般 net 改名成 `1'b1` 亦回 false，canonical name index 不受污染。
4. runEditApply 回 failed/no change/no rollback。
5. writer output 保留 `1'b0`，不含 `renamed_zero`。
6. 一般 net rename 既有測試維持 PASS。
7. 完整 mini test/test2：36 passed、0 failed。
```

## EDIT-008：replaceAllLoadsOfNet 破壞 pin-level multiplicity

優先級：P0

狀態：Fixed

問題與修改：

```text
舊版 rewiring 完成後會 sort/unique newNet.loadGateIds，把同一 gate 的多個 input pins
壓成單一 gate ID，違反目前 Net::loadGateIds 的 pin-level adjacency 語意。

`src/optimization/NetlistOptimization.cpp::replaceAllLoadsOfNet()` 現在針對每個實際
rewired input pin 加入一筆 loadGateId，不再進行 gate-level unique；bulk constant
simplification 則由最後一次 linear rebuild 產生相同 pin-level 結果。
```

驗證：

```text
新增 test2 regression：AND 的兩個 inputs 同接 old_source，replace all loads 後兩個
inputs 均接 new_source、old loads 為空、new loads 保留兩筆 gateId，且
validateAfterMutation() 通過。完整 test2：37 passed、0 failed。
```

## EDIT-009：redirectAllLoads 舊 gate-level dedup 語意

優先級：P1

狀態：Fixed / Verified

問題：

```text
redirectAllLoads(old,new,false) 會把 gate 的所有 matching input pins 改接 new net，
但 newNet.loadGateIds 只保留一筆 gate ID。若 gate 原本另一個 pin 已接 new net，
mutation 後有兩個實際 pins，adjacency 卻只有一筆，違反 pin-level invariant。

完整 test1 在加強 multiplicity validation 後為 141 passed、1 failed，唯一失敗即
redirectAllLoadsWithReport()，net[1] 回報 pin-level load multiplicity mismatch。
```

修改內容：

```text
redirectAllLoads() 復用 replaceAllLoadsOfNet()，loadGateIds 永遠依實際 rewired pins
維護；保留 allowDuplicateLoads 參數以維持 ABI/API，但不再允許它破壞 adjacency。
WithReport 的 changed gate IDs/names 依 gate 去重。
```

驗證：

```text
1. redirect 後 AND 的兩個 input pins 均正確接到 new net。
2. newNet.loadGateIds 保留兩筆 pin-level gate ID。
3. report.changedGateIds 只列一次實際受影響 gate。
4. validateAfterMutation() 通過。
5. 完整 test1：142 passed、0 failed。
```

## 非 Boolean Edit Apply 最終 Coverage

狀態：Complete

public command 正向 regression：

| 類別 | 已覆蓋 commands | 結果 |
|---|---|---|
| Rename | `RenameGate`, `RenameNet` | PASS |
| Cleanup / Simplification | `CleanupBuffers`, `CollapseDoubleInverter`, `LocalSimplificationFixpoint`, `SafeCleanupFixpoint`, `TrimDeadLogic`, `RemoveDanglingLogic`, `RemoveUnusedNets`, `MergeStructurallyEquivalentGates`, `SimplifyConstants`, `SimplifySameInput` | PASS |
| Buffer Insertion | `InsertBuffersForFanout`, `InsertBuffersForSpecificNet`, `InsertBuffersForDffControl`, `InsertBuffersOnEachLoad`, `InsertBufferAtDriver`, `InsertBufferBeforeGate`, `InsertBuffersByGateType` | PASS |
| Technology Mapping | `ConvertToBasis`, `ReplaceGateType` | PASS |
| Safe Single-Net Cleanup | `RemoveNetIfUnused` | PASS |

合計 22 個非 Boolean public commands 均有正向 regression。另覆蓋：

```text
1. internal low-level primitive 經 runEditApply 一律拒絕。
2. legacy MergeEquivalentGates public request 一律拒絕。
3. Unknown command、缺少 rename 參數、missing net、invalid fanout limit 等失敗 report。
4. constant literal rename rejection、PO guard、tombstone/stale/invalid-ID safety。
5. pin-level tied-input fanout、replace-all-loads、redirect-all-loads multiplicity。
6. 100 萬 gate constant simplification、double inverter、structural merge scalability。
```

最終 regression：

```text
mini test/test1：142 passed、0 failed。
mini test/test2：39 passed、0 failed。
mini test/test5：11 passed、0 failed。
git diff --check：PASS。
```

不納入本輪完成條件：

```text
MergeFunctionallyEquivalentGates、DFF initial-state、sequential functional merge、
whole-design Boolean/AIG proof 重構；全部維持 Deferred Boolean。
```

`rollbackOnFailure=false` 目前只保留 request 相容性；public WithReport wrapper 在
validation failure 時仍強制 rollback，這是對外 Edit Apply 的固定安全策略。
