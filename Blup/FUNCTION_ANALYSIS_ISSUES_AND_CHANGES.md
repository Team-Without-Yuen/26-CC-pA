# Function Analysis API 問題與修改紀錄

## 1. 盤查範圍

| 項目 | 內容 |
|---|---|
| 高階入口 | `Netlist::runFunctionQuery()` |
| 主要程式 | `src/analysis/FunctionAnalysis.cpp`、`src/analysis/Booleanexpression.cpp` |
| 公開型別 | `include/core/NetlistQueries.h`、`include/core/Netlist.h` |
| API 文件 | `API_SPEC/FUNCTION_ANALYSIS_API.md`、`API_SPEC/FUNCTION_ANALYSIS_USAGE.md` |
| 既有測試 | `mini test/test4/test4.cpp` |
| 外部盤查 | `C:/Users/user/Downloads/FUNCTION_ANALYSIS_REVIEW.md` |

本階段遵守以下邊界：SAT、Boolean expression、functional equivalence、constant
function、functional dependence、symmetry 等 Boolean 功能問題先盤查與標記，留待後期
AIG functional index / SAT 架構重整；不在本階段修改其核心語意。

## 2. 狀態總表

| ID | 優先級 | 狀態 | 問題摘要 | 本階段處理 |
|---|---:|---|---|---|
| FUNCAN-001 | P0 | Deferred Boolean | `SimplifiedBooleanExpression` 的無上限遞迴可能先於 size budget 發生 stack overflow | 登記，待 AIG/Boolean 重構 |
| FUNCAN-002 | P1 | Deferred Boolean | SAT query 每次依 whole-design net count 配置 solver，對大量小 cone 查詢成本偏高 | 登記，待 SAT/AIG 重構 |
| FUNCAN-003 | P1 | Deferred Boolean | bus 名稱在 dispatcher 與 solver 內重複 `expandNetToBits()`，可能重複全表掃描 | 登記，待 SAT/AIG 重構 |
| FUNCAN-004 | P1 | In Progress | expression 的 depth/size 截斷與新增 support 分類缺少 regression | support 分類測試已補，截斷測試待補 |
| FUNCAN-005 | P2 | Fixed / Verified | 文件仍稱 expression 不做代數化簡，與目前 local identity rewrite 不一致 | API/Usage 已同步真實語意 |
| FUNCAN-006 | P2 | Deferred Boolean | `expressionDepthLimited` 同時承擔「採用 depth limit」與「實際被截斷」兩種語意 | 登記，待 report 重整 |
| FUNCAN-007 | P2 | Verified / No Change | support BFS 對正式 tombstone 流程是否安全 | valid netlist regression 通過；損壞內部 ID 不列公開契約 |
| FUNCAN-008 | P2 | Deferred Boolean | scalar-only constant/truth query、缺少 complement equivalence、batch query 與一致 witness | 登記，待 SAT/AIG 重構 |

## 3. 已確認基準

### 3.1 附件修改已存在於目前分支

目前原始碼已包含：

1. `kExpressionSizeBudget = 2,000,000`。
2. `getBooleanExpression(..., bool* wasTruncated)`。
3. same-input、constant-input、complementary-input、double-negation 的 local rewrite。
4. `PrimaryInputSupport` 與 real PI / DFF.Q / undriven leaf 三類欄位。

上述檔案目前沒有工作樹差異，表示不是本輪尚待套用的 patch。

### 3.2 基準測試

`mini test/test4/test4.cpp`：

```text
Summary: 17 passed, 0 failed.
```

已覆蓋基本 expression、depth-limited expression、DFF.Q boundary、AlwaysZero、
AlwaysOne、TruthStatus、CanBeValue、Equivalence 與基本錯誤參數。

## 4. 問題詳情

### FUNCAN-001：depth-limited expression 仍可能 stack overflow

狀態：Deferred Boolean

涉及位置：

```text
src/analysis/Booleanexpression.cpp
  buildExprLimited()
  getSimplifiedBooleanExpression()
```

`maxExpressionDepth` 只要求非負數，沒有安全上限。`sizeBudget` 在 child recursion
返回、組出 `result` 後才扣除，因此它只能限制部分字串建構成本，不能限制尚未 unwind
的遞迴深度。極深 BUF chain、NOT chain，或被 local rewrite 成 `PassA` 的同輸入 gate
chain，可能在預算開始發揮作用前就耗盡 call stack。

可能受影響的 prompt：

```text
Show the Boolean expression of nX with a very large expansion depth.
Expand the complete logic expression of a deeply chained output.
```

預定方向：AIG/Boolean 重構時改為 iterative traversal / expression DAG，或在 facade
內設定不依賴 LLM 的安全深度；本階段不改 Boolean expression 行為。

### FUNCAN-002：SAT solver 依 whole-design 規模重複配置

狀態：Deferred Boolean

每次 CanBeValue、Equivalence、FunctionalDependence、Symmetry 等查詢都可能依整個
netlist 規模配置 solver，即使實際 cone 很小。大型設計連續查多條 net 時，固定成本
會被查詢次數放大。

預定方向：AIG functional index 建立 cone-local literal mapping，並研究可重用的 solver
context 或 batch query。

### FUNCAN-003：bus 展開可能重複掃描名稱表

狀態：Deferred Boolean

dispatcher 驗證名稱與底層 solver 取 bit IDs 時可能各自呼叫一次
`expandNetToBits()`。bus base name 若需 prefix scan，會重複付出 `O(net count)` 成本。

預定方向：驗證階段保存 resolved bit IDs，交給底層 proof 使用。

### FUNCAN-004：新增行為缺少 regression

狀態：Pending Test

原本 test4 尚未驗證：

```text
expression size/depth safety limit 的 report
supportRealPrimaryInputs
supportDffPseudoInputs
supportUndrivenLeaves
same/constant/complementary/double-negation expression 結果
極深 chain 的 timeout / crash 風險
```

已新增 real PI、DFF.Q、undriven leaf 三分類與正式 gate tombstone boundary 的
characterization tests。expression 截斷、local identity 與極深 chain 仍待補；Boolean
預期結果若有爭議，只記錄而不修改。

### FUNCAN-005：文件與目前 expression 行為不一致

狀態：Fixed / Verified

原文件寫「Boolean expression 是結構展開結果，不代表已做代數化簡」，但目前程式會
執行數種 local Boolean identity rewrite。API/Usage 已同步並明確區分：

```text
會做：固定、局部、結構可證明的 identity rewrite。
不會做：一般 Boolean minimization、SAT-based canonicalization 或全域代數化簡。
```

### FUNCAN-006：`expressionDepthLimited` 語意混合

狀態：Deferred Boolean

完整 expression 時，此欄位代表「實際觸發 depth/size safety limit」；simplified
expression 時則無條件為 true，代表「這個 mode 採用 depth limit」。同一欄位存在兩種
語意，而且 simplified mode 無法區分因 size budget 截斷或只是設定了 depth。

預定方向：後續 report 重整時拆分 `depthLimitApplied`、`truncated`、
`truncationReason`；本階段不改公開欄位。

### FUNCAN-007：support traversal 的 tombstone 防護

狀態：Verified / No Change

`getPrimaryInputSupportBreakdown()` 直接讀取 `gates[net.driverGateId]`，並將 gate 的
input IDs 加入 queue。正常、已通過 `validateStructure()` 的 netlist 不應出錯，但 API
本身尚未明確拒絕 removed net、UNKNOWN tombstone gate、越界 driver 或越界 input。

正式 `removeGateAndDetachPins()` 流程會清空 gate pins 並把原 output net 的 driver 設為
`-1`；新增 regression 驗證 traversal 會在該 net 停止並回報 undriven leaf，不會穿越
UNKNOWN gate。越界 driver/input ID 代表 netlist 已違反內部結構契約，應由 mutation
validation 阻止；本階段不讓每個 read-only traversal 重複承擔損壞圖的修復責任。

### FUNCAN-008：其餘 Boolean coverage 缺口

狀態：Deferred Boolean

```text
ConstantFunction / AlwaysZero / AlwaysOne / TruthStatus 主要為 scalar 語意。
沒有直接的 A == NOT(B) complement-equivalence query。
多 target 查詢需由呼叫端反覆建立 SAT query。
大部分非等價結果沒有一致的 witness/counterexample schema。
FunctionalDependence / Symmetry 的可切換 input 類型有限制。
```

以上統一留待 AIG functional index 與 SAT proof facade 重構。

## 5. 下一步

1. support 三分類、DFF.Q、undriven leaf 與正式 tombstone regression 已完成。
2. API/Usage 的 support 欄位、local identity 與測試數字已同步。
3. expression 截斷、local identity 與深鏈安全測試留待 Boolean/AIG 階段補齊。
4. SAT/AIG、Boolean identity 與 report schema 的實質重構全部維持 Deferred Boolean。
