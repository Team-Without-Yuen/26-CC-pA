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

`FindAny` 找到一組 SAT-proven witness 即停止。`FindAll` 預設不設數量上限；公開 CLI 會把完整 records 自動寫檔，只在 terminal 回 summary 與路徑。只有題目明確要求前 N 筆時才傳 `--max-results N`。

---

## 3. 輸入欄位

共同欄位：

| 欄位 | 預設 | 條件 |
|---|---:|---|
| `mode` | `FindAny` | `FindAny` 或 `FindAll` |
| `maxResults` | 0 | 0 表示不限量；正值表示明確 result limit |
| `maxStoredMatches` | 256 | C++ report sample 上限；0 表示不保留 sample |
| `simulationPatternCount` | 256 | 1..4096 |
| `timeLimitSeconds` | 30 | 必須大於 0 |
| `expandEquivalentPairs` | true | 一般 query 保持 true；批次 merge 可設 false，只保留 classes 與總 pair count |
| `writeMatchesToFile` | false | C++ caller 需要 streaming artifact 時設 true |
| `outputFilePath` | 空字串 | C++ caller 可指定路徑；CLI 會自動指定唯一檔名 |

`EquivalentGatePairs` scope：

| Scope | `scopeName` |
|---|---|
| `WholeDesign` | 不使用 |
| `NetFanin` | existing net |
| `NetFanout` | existing net |
| `GateFanin` | existing gate |
| `GateFanout` | existing gate |

`gateTypeFilter = GateType::UNKNOWN` 表示不限制 type；可設定 AND、OR、NAND、NOR、NOT、BUF、XOR 或 XNOR。

NAND candidate 規則：

| 設定 | 候選集合 |
|---|---|
| `internalSignalsOnly=true` | 有有效 driver 的 internal scalar nets；排除 PI、PO、constant、undriven 與 target，DFF.Q 可作為 internal boundary candidate |
| `internalSignalsOnly=false` | 另納入 PI 與有有效 driver 的 PO；仍排除 constant、target 與非 PI undriven nets |
| `allowSameSignalPair=false` | unordered distinct pairs，不重複 `(a,b)` / `(b,a)` |
| `allowSameSignalPair=true` | 在上述 pairs 之外加入 `(a,a)` |

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

FindAll 優先讀：

```text
matchCount
wroteMatchesToFile
outputFilePath
```

`matchCount` 是結果數，`matches.size()` 只是記憶體 sample 數。公開 CLI 另輸出 `stored_match_count`、`wrote_matches_to_file` 與 `output_file`；當 `wrote_matches_to_file:true` 時，完整 records 在檔案中，terminal 不重複列出 matches/classes。

`EquivalentGatePairs` 另讀：

```text
equivalenceClassCount
equivalentPairCount
equivalenceClasses
matches
candidateGateCount
satChecks
```

`equivalentPairCount` 是已證明 class 的 pair 總數。若 caller 明確設定 `maxResults`，它可能大於 artifact records 或 `matches.size()`；此時 `truncated=true`，不可宣稱列出了全部 pairs。

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
func_search nand_pair n25 --all
```

成功 match 的 proof 應為 `SAT_UNSAT_MITER / UNSAT`。

### 5.2 搜尋 whole-design equivalent gate pairs

```cpp
Netlist::FunctionSearchQuery query;
query.type = Netlist::FunctionSearchQueryType::EquivalentGatePairs;
query.mode = Netlist::FunctionSearchMode::FindAll;
query.scope = Netlist::FunctionSearchScope::WholeDesign;
query.maxResults = 0;
query.maxStoredMatches = 0;
query.writeMatchesToFile = true;
query.outputFilePath = "equivalent_pairs.txt";
query.timeLimitSeconds = 30.0;

const auto report = netlist.runFunctionSearchQuery(query);
```

```text
func_search equivalent_pairs whole --all --time-limit 30
```

公開 CLI 讀取 `match_count` 與 `output_file`；完整 classes 與 pair records 在 artifact。C++ caller 若不寫檔，可從 `equivalenceClasses` 取得緊湊分組，但 `matches` 最多只含 `maxStoredMatches` 筆 sample。

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

`--all` 會自動建立 `<design>_function_search_<sequence>.txt`，且不覆寫既有檔案。不要為了避免大輸出自行加 `--max-results`；完整大型結果本來就應留在 artifact，回答只需給總數與檔案路徑。

---

## 7. Prompt 對應

| Prompt 語意 | Command | 主要讀取 |
|---|---|---|
| 是否存在 signals 使 `NAND(a,b)==n25` | `func_search nand_pair n25` | `found`, 第一筆 match |
| 列出所有這類 NAND pairs | `func_search nand_pair n25 --all` | `complete`, `match_count`, `output_file` |
| 找任意兩顆功能等價 gates | `func_search equivalent_pairs whole` | 第一筆 gate pair 與 proof |
| 列出所有 equivalent gate groups | `func_search equivalent_pairs whole --all` | `equivalence_class_count`, `match_count`, `output_file` |
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

目前兩種 public modes 均只回 SAT-proven matches；simulation-only 結果不會出現在 `matches`、artifact 或 `equivalenceClasses`。沒有有效 driver 的 net 目前視為 unconstrained Boolean leaf。官方 netlist 只包含題目規定的 gate types；prompt 中的 MUX 等語意屬合法 gates 所形成的 Boolean pattern，不以 direct MUX primitive 處理。

`test24` 為 9/9 通過，`test26` 為 18/18 通過；NewTestCase test29/test30 的 FindAll artifact 分別為 7/7 與 1/1 records，test35 FindAny 回完整 SAT witness。

`Blup/function_search_runs/candidate_scope_probe` 另驗證 PI、PO、internal、DFF.Q、floating、
target exclusion、unordered/self pair、whole/net_fanin scope 與 BUF gate-type filter。
