# Sequential Pattern Query 使用說明

這份文件只負責說明 `SequentialPatternQuery` 與對應公開 command 的使用方式。

設計背景與內部 contract 請看：

```text
API_SPEC/SEQUENTIAL_PATTERN_API.md
```

---

## 1. 基本概念與入口

```cpp
Netlist::SequentialPatternQuery query;
query.type = Netlist::SequentialPatternQueryType::DffEnableHold;
const auto report = netlist.runSequentialPatternQuery(query);
```

```text
sequential_query enable_hold <all|dff_name> [options]
```

預設就會對 canonical 與非 canonical 實作做功能判定。不需要加
`--functional-fallback` 才能辨識 restructuring。

## 2. Mode / Command 總表

| Mode | 必要輸入 | 主要輸出 | 語意 |
|---|---|---|---|
| `enable_hold all` | 無 | counts；detail artifact | 分析所有 active DFF |
| `enable_hold <dff>` | DFF instance | 單顆 DFF detail | 分析指定 DFF |

## 3. 輸入欄位

| 欄位 / option | 預設 | 說明 |
|---|---:|---|
| `dffName` | empty | empty 表示 all |
| `includeAndGatedCandidates` | true | 回報 `D=EN&DATA` data-gating non-match diagnostic |
| `resolveFunctionalDataNets` | true | 嘗試將 derived DATA 映射為具名 net |
| `maxFunctionalCandidates` | 64 | optional control-name mapping 上限 |
| `maxFunctionalDataCandidatesPerMatch` | 16 | optional data-name mapping 上限 |
| `functionalPerDffTimeLimitSeconds` | 0 | 0 使用剩餘共用 deadline；正值新增單 DFF cap |
| `functionalTimeLimitSeconds` | 290 | 整次 query 總 wall-clock 預算 |

`verifyCanonicalMatchesWithSat`、`enableFunctionalFallback`、simulation fields 為相容舊 caller
保留。它們不再能關閉正式功能判定。CLI 仍要求明確調整 functional mapping
參數時同時帶 `--functional-fallback`，這只是 parser 相容規則。

## 4. 回傳欄位與狀態判讀

1. 先讀 `ok` / `complete` / `timedOut`。
2. 數量題讀 `matchedDffCount`，不要用 `patterns.size()` 或 `functionalMatchCount`。
3. 單顆 DFF 只有 `confirmed=true` 才是正式 match。
4. `enableNetName` / `dataNetName` 為空是「功能存在，但無單一具名 net」，不是失敗。
5. `DATA_GATING_WITHOUT_HOLD_FEEDBACK` 是明確 non-match diagnostic，不計入 matched count。
6. `complete=false` 時不能將未找到的項目解讀為不存在。

candidate/examined/unexamined 欄位是 role mapping 診斷。即使
`functionalCandidateLimitReached=true`，只要 Boolean proof 已完成，report 仍可
`complete=true`。

## 5. 使用範例

### 5.1 計算所有具 enable/hold 的 DFF

```text
sequential_query enable_hold all --summary-only
```

讀取 `matched_dff_count`、`complete`、`timed_out`。

### 5.2 列出所有 DFF 的分析

```text
sequential_query enable_hold all
```

CLI 會將完整 DFF records 寫入唯一 artifact，envelope 只回 summary 與
`output_file`。回答時可簡述數量並提供該路徑。

### 5.3 查詢單顆 DFF

```text
sequential_query enable_hold ff1
```

讀取 `matched`、`kind`、`confirmed`、`enable_net_name`、`data_net_name`、
`active_level_name`、`feedback_net_name` 與 `solver_status`。

### 5.4 只需功能數量，不需要 DATA 名稱

```text
sequential_query enable_hold all --summary-only \
  --functional-fallback --no-resolve-functional-data
```

只略過 optional DATA mapping，不會關閉 enable/hold Boolean proof。

## 6. Prompt 對應

| Prompt 語意 | Command | 主要讀取 |
|---|---|---|
| How many flip-flops have enable or hold? | `sequential_query enable_hold all --summary-only` | `matched_dff_count` |
| Report D input enable/hold logic | `sequential_query enable_hold all` | artifact 內的 confirmed patterns |
| Does ff1 have enable/hold? | `sequential_query enable_hold ff1` | `matched`, `confirmed` |
| Report enable/data names | `sequential_query enable_hold ff1` | role names；空值表示 derived function 無具名 net |

## 7. 何時不要使用

- 單純列 DFF：用 `structure_query list_dffs`。
- 查 clock/reset load：用 Direct Connectivity command。
- 查 DFF.D depth 或 register-to-register path：用 Depth/Path Query。
- 查一般 net 的 equivalence/constant/symmetry：用 Function Query。

## 8. 實作與測試狀態

- `mini test/test21`：18/18。
- `mini test/test58`：11/11，含 NAND/NOT 與 NOR/NOT restructuring。
- `NewTestCase/test91`：2585 DFF、1796 matched、complete，最近兩次約 21.55–29.63 秒。
