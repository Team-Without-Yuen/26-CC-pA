# AIG Primitives 問題與修改紀錄

## 1. 文件目的

本文件記錄 AIG Phase A backend 在合併盤查中確認的正確性問題、修改內容與驗證結果。
範圍限於 `AigModel`、`Primitives`、CEC safety gate 與對應 regression；不包含高階 API
AIG 化、Phase B SatEngine/Fraig、`tools.cpp` 或 LLM routing。

相關文件：

```text
API_SPEC/AIG_PRIMITIVES_BACKEND_GUIDE.md
Blup/AIG_INTEGRATION_REVIEW.md
API_SPEC/API_CORE_BACKEND_TODO.md
```

## 2. 狀態總覽

| ID | 問題 | 嚴重度 | 狀態 | 驗證 |
|---|---|---:|---|---|
| AIG-SAFE-001 | PO-only floating net 被綁成 constant 0 | Critical | 已修正 | test36 |
| AIG-SAFE-002 | malformed gate input 未必計入 unresolved | Critical | 已修正 | test36 |
| AIG-SAFE-003 | DFF D/control 無法解析時仍建立可查詢模型 | Critical | 已修正 | test36 |
| AIG-SAFE-004 | topo-dropped/unresolved 只警告，仍可回 proven result | Critical | 已修正 | test36 |
| AIG-SAFE-005 | `UnknownPolicy::AsEqual` 可能掩蓋 invalid model | Critical | 已修正 | test36 |
| AIG-SAFE-006 | invalid snapshot/after-model 仍可能進入 CEC | High | 已修正 | test36 |
| AIG-SAFE-007 | ownership 文件仍使用舊 session 語意 | Medium | 已修正 | 文件核對 |
| AIG-SAFE-008 | Phase B 尚未完成 | High | 已知限制 | 本輪不修改 |
| AIG-SAFE-009 | revision freshness 只依賴 dirty | Medium | 已知限制 | 本輪不修改 |

## 3. AIG-SAFE-001：PO-only floating 被默認成 0

### 問題

原始 free-PI 掃描只將 gate input 視為 referenced net。若一條 undriven net 只連到 primary
output，它不會被建立成 free PI；建立 PO 時會直接使用 constant 0。

這可能讓 constant/equivalence/CEC 回傳錯誤的確定答案。

### 修改

檔案：`src/SATEngine/AigBuilder.cpp`

```text
- free-PI referenced 集合納入所有 primary-output net。
- PO-only floating net 建成具名 free PI。
- 模型狀態標記為 Conservative，仍允許保守 Boolean proof。
```

### 修改後語意

```text
undriven PO y -> free Boolean variable y
不再假設 y = 0
```

## 4. AIG-SAFE-002：必要 gate input 未完整驗證

### 問題

原始程式對所有組合 gate 都先取得兩個 input。當 binary gate 缺少第二個 input 時，可能直接
回傳 constant 0，卻沒有增加 `num_unresolved`；unary gate 與 binary gate 的 arity 也沒有明確
區分。

### 修改

檔案：`src/SATEngine/AigBuilder.cpp`

```text
- NOT/BUF 明確視為 unary，其餘合法組合 gate 視為 binary。
- 缺少必要 input 時增加 unresolved counter 並記錄具體 gate/input index。
- unsupported gate type、invalid output net 與 multiple driver 均標記 Invalid。
```

替代用的 constant 0 只保留在 diagnostic AIG 中，不能再用於 proven answer。

## 5. AIG-SAFE-003：DFF 必要訊號解析失敗

### 問題

原始 DFF 建模會將無法解析的 D 默認成 0，RN/SN 則默認成 inactive，沒有區分：

```text
pin 本來就沒有連接
pin 有連接但 net 無效或無法解析
```

兩者語意不同。前者可依 cell 定義使用 inactive value；後者代表模型不完整。

### 修改

檔案：`src/SATEngine/AigBuilder.cpp`

```text
- D 缺失或無法解析：Invalid。
- RN/SN 未接：合法，維持 inactive value。
- RN/SN 已連接但無法解析：Invalid。
- Q output invalid 或與既有 signal 衝突：Invalid。
```

## 6. AIG-SAFE-004：模型不健全仍允許證明

### 問題

原本 `num_topo_dropped > 0` 或 `num_unresolved > 0` 只在 verbose 模式印出警告，
`equiv()`、`is_const()`、support、cut 與 CEC 仍可繼續執行。

### 修改

檔案：

```text
include/SATEngine/AigBuilder.h
include/SATEngine/Primitives.h
src/SATEngine/AigBuilder.cpp
src/SATEngine/Primitives.cpp
src/SATEngine/Session.cpp
```

新增：

```cpp
enum class ModelHealth {
    Sound,
    Conservative,
    Invalid
};
```

| 狀態 | 條件 | 是否可證明 |
|---|---|---|
| `Sound` | 所有必要訊號均忠實建模 | 是 |
| `Conservative` | floating signal 已轉成 free PI | 是 |
| `Invalid` | unresolved、topo failure 或必要結構錯誤 | 否 |

診斷文字最多保留前八項具體問題，其餘以 `additional issues omitted` 收斂，避免大型損壞
netlist 因錯誤訊息本身耗用過多記憶體。

## 7. AIG-SAFE-005：Invalid 與 solver Unknown 分離

### 問題

`UnknownPolicy::AsEqual` 是 solver resource-limit 的折疊策略。若 Invalid model 也走相同 policy，
可能將「模型不可信」轉成 `true`。

### 修改後語意

checked API 在 Invalid model 上固定回 `EquivResult::Unknown`：

```text
equiv_checked
is_const_checked
equiv_under
is_const_under
depends_on_checked
```

無三態回傳的 Boolean API 會丟出 `UnsoundModel`：

```text
equiv / is_const / is_const0 / is_const1
is_free_var / is_constant / is_complemented
cofactor / functional_support / is_unate / is_symmetric
make_and / make_or / make_xor / make_mux
enumerate_cuts / truth_of / npn_matches / cut access
equivalence_classes / raw / wrap
```

因此 `AsEqual` 無法繞過 model-health gate。未來高階 API 接入時，必須捕捉
`UnsoundModel` 並轉成統一 report。

## 8. AIG-SAFE-006：CEC safety

### 修改

檔案：`src/SATEngine/Session.cpp`

```text
- Invalid model 產生的 snapshot 固定 valid=false。
- current after-model Invalid 時，CEC 直接回 Unknown。
- CEC message 附上 model-health reason。
- 不建立 miter，也不啟動 SAT proof。
```

`Unknown` 不得被解讀成 equivalent 或 not-equivalent。

## 9. Ownership 與導入邊界

程式註解與文件已統一為：一份 current named `Netlist` 最終由 backend analysis context 持有
一個共用 `Primitives`。高階 API 只能接收共用 reference，不應各自建立長生命週期 AIG cache。

本輪只定義規則，尚未建立 analysis context；等第一個高階 API 接入 AIG 時再落實所有權。

## 10. 未修改事項

### Phase 切換

依目前決議，不修改 `enable_phase_b()`。SatEngine/Fraig 仍是未完成的 Phase B stub，不能作為
目前可用能力。

### Revision freshness

依目前決議，不新增 `builtRevision` 比對。現有 freshness 仍依賴 `dirty`；未來必須透過
「一個 Netlist 對應一個共用 Primitives」避免多 cache 互相 `clearDirty()`。

### 高階 API 與 tools

```text
- 尚未讓 Function Search、Function Analysis 或 Sequential Pattern 使用 Primitives。
- 未修改任何 public query/report schema。
- 未修改 tools.cpp 或 TOOLS_SPEC。
```

## 11. 測試與建置結果

### AIG model-health regression

檔案：`mini test/test36/test36.cpp`

```text
19 passed, 0 failed
```

覆蓋：

```text
Sound DAG
floating gate input
PO-only floating
missing binary-gate input
UnknownPolicy::AsEqual safety
合法未接 RN/SN
missing DFF D
combinational loop
Invalid CEC
```

### 既有 regression

| 驗證 | 結果 |
|---|---|
| MSYS2 UCRT64 full incremental compile/link | 成功 |
| NetlistTool equivalence/snapshot/CEC smoke | 成功 |
| `mini test/test2` | 40 passed, 0 failed |
| Function Search test29 | 7 classes / 7 pairs，complete |
| Function Search test30 | 1 class / 1 pair，complete |
| Function Search test35 FindAny | `n26080`, `n6359`，complete |
| Function Search test35 FindAll | 512 pairs，complete，無 timeout/Unknown |

Windows 必須使用 MSYS2 UCRT64。以舊 `C:/MinGW/bin/g++` 連結 UCRT64 static library 會出現
`stat64i32` / `fstat64i32` ABI 錯誤，這不是 AIG source failure。

## 12. 後續工作

1. 建立高階 API/AIG 能力盤點表。
2. 使用 testcase 量測現有演算法的答案、時間與記憶體。
3. 優先評估 single-net constant、指定兩 net equivalence 與 whole-design CEC。
4. Phase A 不完整或可能更慢的能力不得直接替換既有 API。
5. 第一個 API 接入時建立 backend analysis context，落實共用 ownership。
