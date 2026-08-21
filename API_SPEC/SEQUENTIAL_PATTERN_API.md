# Sequential Pattern Query API 整理

這份文件整理 `SequentialPatternQuery` 的設計、責任與內部 contract。

使用方式與 prompt 範例請看：

```text
API_SPEC/SEQUENTIAL_PATTERN_USAGE.md
```

---

## 1. 核心定位

`SequentialPatternQuery` 從 DFF D-input Boolean function 判斷是否存在同顆 DFF 的
enable/hold 語意：

```text
D = EN ? DATA : Q
```

`EN` 與 `DATA` 必須是不依賴該顆 DFF.Q 的 Boolean function，但不一定對應單一
具名 net。API 不負責一般 DFF 列表、clock/reset 直接連線、netlist 修改或時序最佳化。

## 2. 資料模型與邊界

- named netlist 是唯一正式設計狀態。
- DFF.Q 在 Boolean model 中是自由 sequential boundary，不從 D 穿越到 Q。
- 內部使用 Netlist 唯一共用的 `eqeng::Primitives`；AIG/SAT 不暴露給 caller。
- structural/canonical matcher 只提供快速候選、evidence 與名稱提示，不能單獨產生
  confirmed match。
- floating/undriven 或受污染的 Boolean signal 不得默認為 constant。

## 3. Query 型別

```cpp
enum class SequentialPatternQueryType {
    DffEnableHold
};

struct SequentialPatternQuery {
    SequentialPatternQueryType type;
    std::string dffName; // empty = all active DFFs
    bool includeAndGatedCandidates;
    bool verifyCanonicalMatchesWithSat; // compatibility switch
    bool enableFunctionalFallback;      // compatibility switch
    size_t maxFunctionalCandidates;
    size_t maxFunctionalMatchesPerDff;
    bool findAllFunctionalMatches;
    bool resolveFunctionalDataNets;
    size_t maxFunctionalDataCandidatesPerMatch;
    bool enableFunctionalSimulationFilter; // compatibility field
    size_t functionalSimulationPatternCount;
    double functionalPerDffTimeLimitSeconds;
    double functionalTimeLimitSeconds;
};
```

現行版本無論 compatibility switches 的值為何，都會對每顆 DFF 執行正式功能判定。
candidate/data 上限只限制證明後的具名 role mapping，不會截斷 Boolean classification。
`functionalPerDffTimeLimitSeconds=0` 代表使用剩餘共用 request deadline；正值才是額外的
單顆 DFF cap。

## 4. Report Contract

### 4.1 整體 report

| 欄位 | 語意 |
|---|---|
| `ok` | request 合法且沒有功能證明 timeout |
| `exists` | 至少一顆 DFF 有 confirmed enable/hold |
| `complete` | 所有目標 DFF 的 Boolean classification 皆已完成 |
| `timedOut` | 共用功能證明 deadline 已到 |
| `matchedDffCount` | 以 DFF instance 去重後的 confirmed 數量 |
| `candidateDffCount` | 具有 enable/hold pattern record 的 DFF 數；data-gating diagnostic 不計 |
| `functionalMatchCount` | 透過通用 cofactor path 證明的 pattern 數 |
| `reports` | 每顆 DFF 的細節、proof 與可選名稱映射 |

### 4.2 每顆 DFF / pattern

`matched=true` 只代表至少一個 `confirmed=true` pattern。`enableNetName` 或
`dataNetName` 為空不代表 non-match；它只代表 derived Boolean role 無法映射到單一具名 net。

`functionalCandidateCount`、`functionalCandidatesExamined`、`functionalCandidateLimitReached`
與 data candidate 欄位現在是 optional named-role mapping 的成本診斷，不是 Boolean match
搜尋的 completeness。因此 mapping 不完整時，已證明的 pattern 仍可
`confirmed=true` 且整體 `complete=true`。

## 5. 高階入口與執行流程

```cpp
SequentialPatternReportSet runSequentialPatternQuery(
    const SequentialPatternQuery& query) const;
```

### 5.1 Canonical 安全快速路徑

1. 以結構 matcher 取得 control/data/evidence hint。
2. 檢查 control 與 data branch 的結構 cone 不含同顆 DFF.Q。
3. 使用 `FunctionalPatternEngine::proveSpecifiedOperands()` 建立 MUX AIG function。
4. 只有 AIG literal equality 或 SAT UNSAT proof 證明 target 等價時才 confirmed。

若 data/control 可能依賴 Q，不會因外觀像 MUX 就直接接受，而是轉入通用
cofactor path。

### 5.2 通用 Q-cofactor 路徑

```text
D0 = D | Q=0
D1 = D | Q=1
EnableFunction = XNOR(D0, D1)
DataFunction   = D0
```

必須證明：

1. `D0 & !D1` 為 constant 0，排除無法表示成 hold/update MUX 的 transition。
2. `!D0 & D1` 可達，證明 hold mode 存在。
3. `D0 == D1` 可達，證明 update mode 存在。

這些條件會排除 `D=EN&DATA`、`D=Q`、`D=!Q` 與使用另一顆 DFF.Q 作為
同顆 feedback 的情況。功能確認後才嘗試將 derived EN/DATA 映射回具名 net。

### 5.3 通用 pattern proof core

內部 `FunctionalPatternEngine` 已登錄 `BUF/NOT/AND/NAND/OR/NOR/XOR/XNOR/MUX`
pattern builder。給定 target 與 operands 時，會以同一套 AIG/SAT proof 回傳
`ProvenMatch`、`ProvenNonMatch`、`Unknown` 或 `Unsupported`。這是其他高階 API 可重用的
內部能力，不是新的 public query family。

## 6. 與其他 API 的責任界線

| 問題 | 應使用 |
|---|---|
| 列出所有 DFF | `BasicQuery::ListDffs` |
| 列出 clock/reset 直接驅動的 DFF | `DirectConnectivityQuery` |
| 判斷 DFF D-input enable/hold | `SequentialPatternQuery` |
| 已知兩條 net 的一般功能等價 | `FunctionQuery::Equivalence` |
| 搜尋未知 Boolean operand pair | `FunctionSearchQuery` |

## 7. 限制與安全規則

- `Unknown`/timeout 不得當成 match 或 non-match。
- 正式 classification 與 optional role mapping 分離；mapping 預算用完不影響已證明 count。
- 目前每顆 functional match 只回傳一個 canonical derived decomposition，不窮舉所有可能
  EN/DATA 表示。
- 受污染、floating 或無法建模的 target 會回 incomplete/unsupported，不會猜 constant。
- 所有計算單執行緒，共用 `functionalTimeLimitSeconds` request deadline。

## 8. 實作與測試狀態

```text
include/core/NetlistQueries.h
include/core/FunctionalPatternEngine.h
src/analysis/SequentialPatternAnalysis.cpp
src/analysis/FunctionalPatternEngine.cpp
```

- `mini test/test21`：18/18。
- `mini test/test58`：11/11；含 canonical、XOR/NAND/NOR restructuring、derived unnamed enable、data gating、
  permanent hold、toggle 與 cross-DFF feedback。
- `NewTestCase/test91`：2585 DFF 全數完成，1796 matched，`complete=true`、無 timeout，
  最近兩次 query 約 21.55–29.63 秒。
