# Function Search 使用說明

這份文件只負責說明 `FunctionSearchQuery` 與公開 `func_search` command 的使用方式。

設計背景與內部 contract 請看：

```text
API_SPEC/FUNCTION_SEARCH_API.md
```

---

## 1. 基本概念與入口

```cpp
const Netlist::FunctionSearchReport report =
    netlist.runFunctionSearchQuery(query);
```

公開 CLI：

```text
func_search nand_pair ...
func_search equivalent_pairs ...
```

標準流程是先選 search type，再設定 FindAny/FindAll、搜尋限制與 type-specific 欄位，最後必須檢查 `ok`、`complete` 與 status，不能只看 `matches.empty()`。

---

## 2. Mode / Command 總表

| Mode | 必要輸入 | 主要輸出 | 語意 |
|---|---|---|---|
| `nand_pair` | scalar target net | `found`, `matches` | 搜尋 `NAND(a,b)==target` |
| `equivalent_pairs` | scope，必要時 scope name | `equivalenceClasses`, `matches` | 搜尋 gate output function 等價類 |

`FindAny` 找到一組 SAT-proven witness 即停止。`FindAll` 嘗試完整搜尋，但可能受 time limit、unsupported logic 或 `maxResults` 影響。

---

## 3. 輸入欄位

共同欄位：

| 欄位 | 預設 | 條件 |
|---|---:|---|
| `mode` | `FindAny` | `FindAny` 或 `FindAll` |
| `maxResults` | 256 | 必須大於 0 |
| `simulationPatternCount` | 256 | 1..4096 |
| `timeLimitSeconds` | 30 | 必須大於 0 |
| `expandEquivalentPairs` | true | 一般 query 保持 true；批次 merge 可設 false，只保留 classes 與總 pair count |

`EquivalentGatePairs` scope：

| Scope | `scopeName` |
|---|---|
| `WholeDesign` | 不使用 |
| `NetFanin` | existing net |
| `NetFanout` | existing net |
| `GateFanin` | existing gate |
| `GateFanout` | existing gate |

`gateTypeFilter = GateType::UNKNOWN` 表示不限制 type；可設定 AND、OR、NAND、NOR、NOT、BUF、XOR 或 XNOR。

---

## 4. 回傳欄位與狀態判讀

安全判讀：

```cpp
if (report.ok && report.complete && report.found) {
    // 已證明至少存在一組。
} else if (report.ok && report.complete && !report.found) {
    // 完整搜尋後確定不存在。
} else {
    // partial / timeout / unsupported / invalid，不得回答不存在。
}
```

主要狀態：

| `status` | 判讀 |
|---|---|
| `MATCH_FOUND` | FindAny 找到 witness，exists 已完整回答 |
| `MATCHES_FOUND` | FindAll 完整分類且至少一組 |
| `NO_MATCH` | 完整搜尋後無結果 |
| `RESULT_LIMIT_REACHED` | `matches` 只含前 N 組 |
| `TIMEOUT` | 時間內無法完成 |
| `SOLVER_UNKNOWN` | SAT 無法完成部分比較 |
| `UNSUPPORTED_OR_PARTIAL` | 存在無法分析的候選 |
| `TARGET_NOT_FOUND` | NAND search target 不存在 |
| `SCOPE_NOT_FOUND` | equivalent-pair scope 不存在 |
| `INVALID_ARGUMENT` | limit、filter 或必要輸入不合法 |

`EquivalentGatePairs` 優先讀：

```text
equivalenceClassCount
equivalentPairCount
equivalenceClasses
matches
candidateGateCount
satChecks
```

`equivalentPairCount` 是已證明 class 的 pair 總數；若 `truncated=true`，它可大於 `matches.size()`。

---

## 5. 各 Mode 使用範例

### 5.1 搜尋 NAND-equivalent signal pair

```cpp
Netlist::FunctionSearchQuery query;
query.type = Netlist::FunctionSearchQueryType::NandEquivalentInputPairs;
query.mode = Netlist::FunctionSearchMode::FindAny;
query.targetNetName = "n25";
query.internalSignalsOnly = true;
query.allowSameSignalPair = false;
query.timeLimitSeconds = 30.0;

const auto report = netlist.runFunctionSearchQuery(query);
```

```text
func_search nand_pair n25 --time-limit 30
func_search nand_pair n25 --all --max-results 256
```

成功 match 的 proof 應為 `SAT_UNSAT_MITER / UNSAT`。

### 5.2 搜尋 whole-design equivalent gate pairs

```cpp
Netlist::FunctionSearchQuery query;
query.type = Netlist::FunctionSearchQueryType::EquivalentGatePairs;
query.mode = Netlist::FunctionSearchMode::FindAll;
query.scope = Netlist::FunctionSearchScope::WholeDesign;
query.maxResults = 256;
query.timeLimitSeconds = 30.0;

const auto report = netlist.runFunctionSearchQuery(query);
```

```text
func_search equivalent_pairs whole --all --time-limit 30
```

讀取 `equivalenceClasses` 可直接取得分組；讀取 `matches` 可取得 pair records。

### 5.3 限制在 cone

```cpp
query.scope = Netlist::FunctionSearchScope::NetFanin;
query.scopeName = "n10";
```

```text
func_search equivalent_pairs net_fanin n10 --all
func_search equivalent_pairs gate_fanout U15 --all
```

### 5.4 限制 gate type

```cpp
query.gateTypeFilter = GateType::AND;
```

```text
func_search equivalent_pairs whole --all --gate-type AND
```

這代表 pair 的兩個成員都必須是 AND gate；不是要求它們的 fanin cone 只能含 AND。

---

## 6. CLI Grammar

```text
func_search nand_pair <target_net>
    [--find-any | --all]
    [--max-results N]
    [--patterns 1..4096]
    [--time-limit seconds]
    [--allow-same]
    [--include-boundary-signals]

func_search equivalent_pairs <scope> [scope_name]
    [--find-any | --all]
    [--gate-type type]
    [--max-results N]
    [--patterns 1..4096]
    [--time-limit seconds]

scope:
    whole
    net_fanin <net>
    net_fanout <net>
    gate_fanin <gate>
    gate_fanout <gate>
```

`--allow-same` 與 `--include-boundary-signals` 只屬於 `nand_pair`；`--gate-type` 只屬於 `equivalent_pairs`。錯用時 parser 直接回 error。

---

## 7. Prompt 對應

| Prompt 語意 | Command | 主要讀取 |
|---|---|---|
| 是否存在 signals 使 `NAND(a,b)==n25` | `func_search nand_pair n25` | `found`, 第一筆 match |
| 列出所有這類 NAND pairs | `func_search nand_pair n25 --all` | `complete`, `matches` |
| 找任意兩顆功能等價 gates | `func_search equivalent_pairs whole` | 第一筆 gate pair 與 proof |
| 列出所有 equivalent gate groups | `func_search equivalent_pairs whole --all` | `equivalenceClasses` |
| 找 n10 fanin cone 內的 equivalent gates | `func_search equivalent_pairs net_fanin n10 --all` | class/pair counts |
| 找所有 functionally equivalent AND pairs | `func_search equivalent_pairs whole --all --gate-type AND` | filtered classes |

---

## 8. 何時不要使用

| 問題 | 改用 |
|---|---|
| 已知兩條 net，只需判斷等價 | `func_query equivalence` |
| 已知 condition 下判斷等價 | `func_query conditional_equivalence` |
| 找結構 duplicate 並直接合併 | `edit_apply merge_structurally_equivalent_gates` |
| 搜尋並合併跨結構 functional duplicates | `edit_apply merge_functionally_equivalent_gates <scope>` |
| 修改後是否保持 whole-design function | `equiv_query previous_edit` 或 `equiv_query original` |
| critical depth 最佳化 | optimization flow |
| observability-aware redundant gate removal | 尚未由本 mode 支援 |

---

## 9. 實作與測試狀態

```text
NAND C++ API：mini test/test23
NAND CLI：mini test/test24
Equivalent gate-pair search + functional merge C++ API / CLI：mini test/test26
```

目前兩種 public modes 均只回 SAT-proven matches；simulation-only 結果不會出現在 `matches` 或 `equivalenceClasses`。
