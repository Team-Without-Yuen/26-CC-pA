# Function Search 使用說明

這份文件說明外部程式如何呼叫 `FunctionSearchQuery`。目前公開支援 `NAND(a,b) == target` 的 internal signal pair 搜尋。

設計背景、證明流程與限制請看：

```text
API_SPEC/FUNCTION_SEARCH_API.md
```

---

## 1. FindAny

對應 prompt：

```text
Does there exist any pair of internal signals (a, b) already in the netlist
such that NAND(a, b) is equivalent to n25?
```

呼叫方式：

```cpp
Netlist::FunctionSearchQuery query;
query.type = Netlist::FunctionSearchQueryType::NandEquivalentInputPairs;
query.mode = Netlist::FunctionSearchMode::FindAny;
query.targetNetName = "n25";
query.internalSignalsOnly = true;
query.allowSameSignalPair = false;
query.simulationPatternCount = 256;
query.timeLimitSeconds = 30.0;

const Netlist::FunctionSearchReport report =
    netlist.runFunctionSearchQuery(query);
```

判讀方式：

```cpp
if (report.ok && report.complete && report.found) {
    const auto& match = report.matches.front();
    // NAND(match.netNameA, match.netNameB) 已由 SAT 證明等價於 n25。
} else if (report.ok && report.complete && !report.found) {
    // 完整搜尋後確定不存在。
} else {
    // timeout / unknown / unsupported，不能回答不存在。
}
```

`FindAny` 找到第一組已證明的 pair 就停止，因此 `allCandidatesExamined` 不一定為 true，但存在性問題已完整回答。

---

## 2. FindAll

```cpp
Netlist::FunctionSearchQuery query;
query.type = Netlist::FunctionSearchQueryType::NandEquivalentInputPairs;
query.mode = Netlist::FunctionSearchMode::FindAll;
query.targetNetName = "target";
query.maxResults = 256;
query.timeLimitSeconds = 30.0;

const auto report = netlist.runFunctionSearchQuery(query);
```

只有以下條件成立時，`matches` 才是完整集合：

```cpp
report.ok && report.complete && report.allCandidatesExamined &&
!report.timedOut && !report.truncated
```

若 `status == "RESULT_LIMIT_REACHED"`，代表已證明至少還有一組結果超過上限，目前只得到前 `maxResults` 組。

---

## 3. Match 欄位

```cpp
for (const auto& match : report.matches) {
    std::cout << match.netNameA << " " << match.netNameB << "\n";
    std::cout << match.proofMethod << " " << match.solverStatus << "\n";
}
```

公開結果只會列出 SAT-proven matches：

```text
provenEquivalent: true
proofMethod: SAT_UNSAT_MITER
solverStatus: UNSAT
```

simulation signature 不會單獨成為 match proof。

---

## 4. 錯誤與不完整結果

| 狀態 | 外部行為 |
|---|---|
| `TARGET_NOT_FOUND` | 修正 target 名稱 |
| `SCALAR_TARGET_REQUIRED` | 指定單一 net 或 bus bit |
| `TIMEOUT` | 回報 unknown/partial，可調整時間上限後重試 |
| `SOLVER_UNKNOWN` | 不得當成 no match |
| `UNSUPPORTED_OR_PARTIAL` | 回報目前無法完整分析的 logic |
| `RESULT_LIMIT_REACHED` | 明確註明只回傳部分 matches |

---

## 5. API 責任邊界

```text
Are n1 and n2 equivalent?
  -> FunctionQuery::Equivalence

Does any pair (a,b) satisfy NAND(a,b)==n25?
  -> FunctionSearchQuery::NandEquivalentInputPairs

Find and merge all equivalent gates.
  -> FunctionSearchQuery 負責找候選與證明
  -> EditApply 負責實際修改及修改後驗證
```

---

## 6. tools CLI 使用

格式：

```text
func_search nand_pair <target_net>
    [--all]
    [--max-results N]
    [--patterns N]
    [--time-limit seconds]
    [--allow-same]
    [--include-boundary-signals]
```

| Option | 預設 | 語意 |
|---|---:|---|
| `--all` | 關閉 | 使用 `FindAll`；未指定時使用 `FindAny` |
| `--max-results N` | `256` | `FindAll` 最多保留幾組結果；達上限回 `partial` |
| `--patterns N` | `256` | simulation patterns，合法範圍 `1..4096` |
| `--time-limit seconds` | `30` | simulation、enumeration 與 SAT 共用時間 |
| `--allow-same` | 關閉 | 允許 `(a,a)` |
| `--include-boundary-signals` | 關閉 | 允許 PI 等 boundary signal；題目要求 internal signals 時不要使用 |

範例：

```text
func_search nand_pair n25
func_search nand_pair n25 --all --max-results 256 --time-limit 30
```

主要回傳欄位：

```text
report_status
found
complete
all_candidates_examined
timed_out
truncated
candidate_signal_count
candidate_pairs_considered
sat_checks
match_count
matches[].net_a / net_b / proof_method / solver_status
```

只有 `status: ok` 且 `complete: true` 時，LLM 才能把 found/no-match 當成完整答案。`status: partial`、`timeout` 或 `unsupported` 時，不得回答「不存在」。

---

## 7. Prompt 對應

| Prompt | Command | 主要讀取 |
|---|---|---|
| Does there exist any internal pair `(a,b)` such that `NAND(a,b)==n25`? | `func_search nand_pair n25` | `found`、第一筆 SAT-proven match |
| Find all such pairs. | `func_search nand_pair n25 --all` | `complete`、`all_candidates_examined`、`matches` |

---

## 8. 實作與測試狀態

```text
C++ API regression：mini test/test23
CLI regression：mini test/test24
Official prompt target：NewTestCase/test35 n25
```
