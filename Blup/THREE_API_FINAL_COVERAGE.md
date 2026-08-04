# 三個高階 API 最終 Coverage

## 1. 範圍與結論

本文件收斂 Path Query、Function Search 與 Sequential Pattern Query 的責任、實測結果與
尚未處理項目。就目前公開 NewTestCase 與已建立的同類型變形測試而言，三者的非 Boolean
核心功能已完成；尚未宣稱完整支援的是任意 Boolean decomposition/AIG 重構，以及超過
`2^64 - 1` 的精確 path count。

## 2. 責任邊界

| API | 唯一主要責任 | 典型 prompt | 不屬於此 API |
|---|---|---|---|
| Path Query | 已知端點間的 path existence、完整列舉、count、min/max depth、必經/避開條件、cut/mandatory nodes | 是否存在 A 到 B 的路徑、列出所有路徑、所有路徑是否經過 g0、某 net 是否為 PI-to-PO cut | transitive cone、global critical depth、Boolean equivalence |
| Function Search | target function 已知，但要搜尋未知 net pair 或 gate pair/group | 找 `(a,b)` 使 `NAND(a,b)==target`、找功能相同的 gates | 已知 A/B equivalence、結構 duplicate、whole-design equivalence |
| Sequential Pattern Query | 從指定或全部 DFF.D logic 辨識 enable/hold，回報 control polarity、data、feedback 與 unique DFF count | 列出 DFF enable/hold structures、計算具有 enable/hold 的 DFF 數量 | clock fanout、register path/depth、一般 DFF count |

### 相鄰 API 分工

```text
reachable fanin/fanout cone       -> Cone Query
global critical path/depth        -> Depth Query
已知兩條 net 的 equivalence       -> Function Query
修改前後 whole-design equivalence -> Equivalence/Edit flow
DFF clock source/load             -> Structure Query
register-to-register path         -> Path Query
```

## 3. Path Query Coverage

### 已完成

- `exists`、`find`、`enumerate`、`count-only`、`min_depth`、`max_depth`。
- required/avoid constraints、every-through、direct PI-to-PO、mandatory nodes、PI-to-PO cut。
- reverse reachability pruning、count-only DP、完整 path streaming artifact。
- 大型完整結果由工具寫檔，terminal 只回 count、complete 與 path。

### 實測證據

| Testcase | 結果 |
|---|---|
| test12 | 8/0 paths 與 max depth 11/15，complete |
| test14 | 289,366 與 203,810 paths；full streaming 約 2.31 秒 |
| test32 | 1,176,523 register paths；full 約 6.20 秒，count-only 約 0.5 秒 |
| test33/test36/test38 | direct path、cut、mandatory-node semantics 通過 |
| test40 | all DFF.Q 到 all DFF.D maximum depth 105，約 0.09 秒 |

### 未關閉

- `PATH-003`：若精確 path count 超過 `2^64 - 1`，現有 `size_t` 會 overflow。等待官方回答
  是否需要 arbitrary-precision count。
- `PATH-001`：部分錯誤 CLI 參數可能被靜默忽略；目前依先前決議維持 Deferred。
- 題目保證 combinational graph 為 DAG，因此 cycle-specific longest path 不處理。

## 4. Function Search Coverage

### 已完成

- `NandEquivalentInputPairs`：搜尋 `(a,b)` 使 `NAND(a,b)` 等價於 target。
- `EquivalentGatePairs`：搜尋 output function 相同的 gate pairs/groups。
- FindAll 預設不設 result-count 上限；完整 records 自動寫入不覆寫 artifact。
- scope、gate-type filter、same-pair policy、DFF.Q boundary、constant/floating exclusion 已驗證。
- timeout、partial artifact 與 complete semantics 已驗證。

### 實測證據

```text
mini test24：9/9
mini test26：18/18
NewTestCase test29/test30/test35：通過
mixed-boundary / candidate / scope probes：通過
```

### 未關閉

- 任意 hidden Boolean structure、共享 AIG cache、cofactor/decomposition 統一架構維持
  `Boolean-Deferred`。
- 官方 netlist 不含未定義 primitive；MUX 等 prompt 語意由合法 gates 組成，因此 direct unknown
  primitive reader 支援不列入本 API scope。

## 5. Sequential Pattern Coverage

### 已完成

- canonical feedback-MUX：AND/OR、NAND/NAND、AND/OR POS、NOR/NOR forms。
- BUF、NOT、`NAND(x,x)`、`NOR(x,x)`、雙重反相 wrapper normalization。
- active-high/low、data inversion、same-DFF positive-Q feedback ownership。
- `D = EN & DATA` 只回 data-gating diagnostic，不計入 matched/candidate count。
- 指定 DFF optional SAT confirmation、mapping/cleanup 後 canonical detectability。
- all-DFF detail 自動完整寫入 artifact；summary 回 unique DFF count。

### 實測證據

| 測試 | 結果 |
|---|---|
| test19 canonical matrix | 8 DFF、6 matched；所有 form/polarity 正確 |
| wrapper probe | 3 DFF、2 matched；wrong-Q feedback 正確排除 |
| test20 | 13/13；mapping、whole-design equivalence、SAT proof 通過 |
| test21 | 18/18；count、artifact、pagination、timeout/error contract 通過 |
| NewTestCase/test40 prompt 12/13 | 2585 analyzed、1583 unique matched records，約 0.2 秒 |

### 未關閉

- 非 canonical 的任意 Boolean enable/hold、Q-free EN/DATA decomposition 與完整 functional search
  維持 `Boolean-Deferred`；現有 bounded functional fallback 只可視為 opt-in 補充，不能宣稱完整。

## 6. 最終判定

| 項目 | 狀態 |
|---|---|
| 三 API public command 與 report contract | Verified |
| 公開 NewTestCase 對應 prompt | Verified；Sequential 任意 Boolean 情況除外 |
| 大型完整輸出政策 | Verified；Path/Function Search/Sequential 均可寫 artifact |
| 非 Boolean correctness 與主要 timeout regression | Verified |
| arbitrary Boolean/AIG generalized coverage | Boolean-Deferred |
| path count > `2^64 - 1` | Awaiting Official |
| Path CLI malformed-option strictness | Deferred |

因此目前可以進入三個 API 的整體工具使用確認；後續若 LLM 實測失敗，應先依本文件判定是
tool selection、CLI 組裝、既有 deferred 項目，或新的底層 correctness 問題。

