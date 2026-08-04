# Basic Query API 問題與修改紀錄

## 1. 盤查範圍

- `BasicQuery` / `BasicReport` public contract
- gate、net、PI/PO、DFF、gate-type 與 structural-issue queries
- active object / tombstone filtering
- port width、direction、bit name/ID ordering
- merge 後 source build、mini test 與 NewTestCase coverage

`tools.cpp` parser/help/printer 與 `TOOLS_SPEC` 的問題只登記到
`API_SPEC/TOOLS待更新表.md`，不在本階段修改。

## 2. 狀態總覽

| ID | 問題 | 優先級 | 狀態 | 已修正 | 驗證狀態 |
|---|---|---:|---|---|---|
| BASIC-000 | merge 後 baseline 與 Jul performance rewrite | P0 | Verified | 不需修改 | source build；test14/15/16/34 通過 |
| BASIC-001 | PortInfo bit ordering 隨 include flags 改變，且缺少 direction | P1 | Verified | 是 | 5 個 direct API cases、CLI smoke 與既有 regressions 通過 |
| BASIC-002 | PI/PO structured metadata 被錯綁到 `includeNames` | P1 | Verified | 是 | basic-only 22/22；test15 12/12 |

## 3. 問題與修改紀錄

## BASIC-000：merge 後 baseline 與 Jul performance rewrite

- 狀態：`Verified`
- 優先級：P0
- 類型：Merge / Regression / Performance

### 核對結果

Jul commit `121f11b` 將 Summary gate/type count、ListDffs、GatesByType 與
GatesWithConstantInput 改為共用單次掃描結果。這些改寫沒有改變 active-object、gate type、
constant filter 或 count 語意。

### 驗證

| 驗證 | 結果 |
|---|---|
| merge 後完整 source build | 成功；只有既有 third-party warnings |
| `mini test/test14` | 16/16 |
| `mini test/test15` | 12/12 |
| `mini test/test16` | 8/8 |
| `mini test/test34` | 6/6 |

main 內附的 `tools.exe` 並非由完整 merge 後 source 產生，因此上述正式結果以強制重編後
binary 為準。

## BASIC-001：PortInfo ordering 與 direction contract

- 狀態：`Verified`
- 優先級：P1
- 類型：Report Correctness / Port Metadata

### 問題

目前 `includeIds=true` 時，`netIds` 由 `expandNetToBits()` 依 bit index 升冪產生，`netNames`
再由該 ID 陣列推導；`includeIds=false` 時，`netNames` 則由 `Port::netIds` 依 declaration order
產生。對 `[3:0] data_in`，同一個 PortInfo query 可能分別得到 `0,1,2,3` 或 `3,2,1,0`。

此外，base bus name 不能用 NetInfo 判斷方向，而 PortInfo 尚未填入既有的
`isPrimaryInput` / `isPrimaryOutput` 欄位。

### 處理決議

```text
1. PortInfo 只查找一次 Port declaration。
2. netIds 與 netNames 都從同一個 Port::netIds 依 declaration order 產生。
3. 兩個 vectors 使用相同 active-net filter，保持 index alignment。
4. 填入 isPrimaryInput / isPrimaryOutput。
5. 不改 BasicQuery 或 BasicReport public 欄位。
6. tools printer 的 id:-1 另列 tools 待更新，不在本項修改。
```

### 修改內容

| 檔案 | 修改 |
|---|---|
| `src/analysis/BasicAnalysis.cpp` | `PortInfo` 改為直接查找 port declaration；names/IDs 共用 `Port::netIds` declaration order 與 active-net filter；填入 input/output direction flags。 |
| `mini test/test1/tester.cpp` | 新增 descending bus name/ID alignment、names-only、IDs-only、metadata-only 與 missing-port 五組 direct API cases。 |
| `API_SPEC/BASIC_QUERY_API.md` | 明定 PortInfo ordering、alignment 與 direction contract。 |
| `API_SPEC/BASIC_QUERY_USAGE.md` | 更新 PortInfo 欄位、讀取方式與 ordering 範例。 |

### 驗證

| 驗證 | 結果 |
|---|---|
| descending bus default query | 通過；name/ID 依 declaration order 且 index alignment |
| names-only query | 通過；順序不受 `includeIds=false` 影響 |
| IDs-only query | 通過；順序不受 `includeNames=false` 影響 |
| metadata-only query | 通過；方向與 width 正確，list vectors 保持空白 |
| missing port | 通過；回傳 `ok=false`, `exists=false` |
| `mini test/test1` 完整程序 | 上述 Basic cases 全數通過；後段非 Basic 測試發生既有 `std::bad_alloc`，不可記為完整 suite 通過 |
| `TOOLS_SPEC/Makefile` UCRT64 source build | 成功；既有 ABC/CaDiCaL libraries 必須搭配 `MSYSTEM=UCRT64`，不能混用 MINGW64 ABI |
| `mini test/test14` | 16/16 通過 |
| `mini test/test15` | 12/12 通過 |
| `mini test/test16` | 8/8 通過 |
| `mini test/test34` | 6/6 通過 |
| PortInfo CLI smoke | `[3:0] data_in` 依序輸出 `data_in[3]`、`[2]`、`[1]`、`[0]` |

CLI printer 的 `id: -1` 與 direction 顯示仍屬 tools layer，已另外登記為 `BASIC-TOOLS-001`。

## BASIC-002：PI/PO structured metadata 與 includeNames 耦合

- 狀態：`Verified`
- 優先級：P1
- 類型：Report Contract / Port Metadata

### 問題

`BasicQuery::includeNames` 的 public contract 只控制 `gateNames`、`netNames` 與 `portNames`，但
`ListPrimaryInputs` / `ListPrimaryOutputs` 原本把 structured `ports` 也放在同一個條件內。
因此呼叫端只要關閉重複的 flat names，就會連 width、range、bus 與 direction 一起失去。

### 修改內容

| 檔案 | 修改 |
|---|---|
| `src/analysis/BasicAnalysis.cpp` | PI/PO queries 固定建立 declaration-order `PortSummary`；只有 `portNames` 受 `includeNames` 控制。 |
| `include/core/NetlistQueries.h` | 明定 `includeNames` 不控制 structured `ports`，並補正 PortInfo direction flag 註解。 |
| `mini test/test1/tester.cpp` | 新增 PI/PO `includeNames=false` metadata-only regression。 |
| `API_SPEC/BASIC_QUERY_API.md`、`API_SPEC/BASIC_QUERY_USAGE.md` | 同步 structured metadata contract。 |

### 驗證

| 驗證 | 結果 |
|---|---|
| `mini test/test1/tester.exe ... --basic-only` | 22/22；PI/PO metadata-only cases 通過 |
| `TOOLS_SPEC/Makefile` UCRT64 source build | 成功 |
| `mini test/test15` | 12/12；既有 port CLI 行為未退步 |
