# tools 同步狀態與核心待辦

這份文件記錄 C++ 高階 API 到 `tools.cpp` / `TOOLS_SPEC` 的同步狀態，以及已經超出 tools parser 範圍、仍需底層核心處理的事項。公開 CLI schema 已同步的功能不得再列為「tools 尚未修改」。

## Sequential Pattern functional fallback

目前狀態：

```text
C++ API 已完成
tools.cpp parser / help / envelope 已完成
TOOLS_SPEC/SEQUENTIAL_QUERY_TOOL.md 已同步
既有 sequential_query 行為維持 canonical/default mode
functional fallback 維持 opt-in
candidate ranking 為 engine 固定策略，不新增 CLI 參數
simulation-aware stable rerank 為 engine 固定策略，不新增 CLI 參數
simulation safe Reject 不占 max-functional-candidates quota
one-shot/reusable hybrid SAT session 為 engine 固定策略，不新增 CLI 參數
```

`sequential_query enable_hold` 已新增：

| CLI 參數 | 對應 C++ 欄位 | 語意 |
| --- | --- | --- |
| `--functional-fallback` | `enableFunctionalFallback` | canonical 無 confirmed match 時啟用 SAT cofactor search |
| `--max-functional-candidates N` | `maxFunctionalCandidates` | 每顆 DFF 最多檢查的未被 simulation 安全排除 control candidates；safe Reject 不占 quota |
| `--max-functional-matches N` | `maxFunctionalMatchesPerDff` | 每顆 DFF 最多回傳的 functional patterns |
| `--functional-find-any` | `findAllFunctionalMatches=false` | count/existence 題找到第一個 proof 即停止 |
| `--resolve-functional-data` | `resolveFunctionalDataNets` | 額外尋找 active cofactor 對應的具名 data net |
| `--no-resolve-functional-data` | `resolveFunctionalDataNets=false` | 計數/存在性題關閉具名 data net 搜尋 |
| `--max-functional-data-candidates N` | `maxFunctionalDataCandidatesPerMatch` | 每筆 pattern 的 named-data 搜尋上限 |
| `--no-functional-simulation-filter` | `enableFunctionalSimulationFilter=false` | 除錯或 A/B 驗證時停用安全 simulation prefilter |
| `--functional-simulation-patterns N` | `functionalSimulationPatternCount` | bit-parallel patterns，限制 1..4096 |
| `--functional-per-dff-time-limit SEC` | `functionalPerDffTimeLimitSeconds` | 單一 DFF 最多使用的時間切片 |
| `--functional-time-limit SEC` | `functionalTimeLimitSeconds` | 整次 sequential query 共用時間預算 |

公開輸出已包含：

```text
complete
timed_out
functional_candidate_count
functional_searchable_candidate_count
functional_candidates_examined
functional_unexamined_candidate_count
functional_inconclusive_candidate_count
functional_simulation_pattern_count
functional_simulation_candidate_count
functional_simulation_rejected_candidate_count
functional_simulation_seconds
functional_sat_check_count
functional_match_count
elapsed_seconds
```

每顆 DFF detail 已包含：

```text
functional_fallback_attempted
functional_fallback_complete
functional_fallback_timed_out
functional_candidate_limit_reached
functional_candidate_count
functional_searchable_candidate_count
functional_candidates_examined
functional_unexamined_candidate_count
functional_inconclusive_candidate_count
functional_simulation_candidate_count
functional_simulation_rejected_candidate_count
functional_sat_check_count
```

每筆 pattern 已包含：

```text
detection_method: structural_canonical | structural_canonical_with_sat | functional_cofactor_sat
active_level
hold_level
data_function_resolved
data_search_attempted
data_search_complete
data_search_timed_out
data_candidate_count
data_candidates_examined
hold_functionally_proven
load_functionally_proven
```

## Envelope 與回答規則

```text
- complete=false 時 envelope 必須標示 partial。
- partial 狀態下，已回傳的 confirmed matches 可回答；未找到不得回答成「確定不存在」。
- functional_candidate_count 包含 safe Reject；functional_candidates_examined 只計實際進入 bounded SAT/reachability loop 的非 Reject candidates。
- functional_searchable_candidate_count = functional_candidate_count - functional_simulation_rejected_candidate_count。
- functional_unexamined_candidate_count 使用 guarded searchable - examined；functional_inconclusive_candidate_count 每個已進入但 timeout/unknown/unsupported 的 control 最多計一次。
- functional_simulation_rejected_candidate_count 的 candidates 不占 max-functional-candidates，也不得單獨造成 functional_candidate_limit_reached。
- 所有非 Reject candidates 都已完整檢查時，允許 complete=true；只有 timeout、unsupported、unknown、match limit 或仍有未檢查的非 Reject candidates 才維持 partial。
- FindAny 找到第一個 SAT-proven match 後，該 DFF existence 可 complete=true；較早 inconclusive candidate 只保留在診斷 counter。
- query-wide timeout 若發生在某顆 DFF 進入 engine 前，不虛構 candidate counters；LLM 必須先讀 top-level timed_out/complete。
- 數量題使用 matched_dff_count，不使用 functional_match_count，避免 nested controls 重複計算 DFF。
- 數量題使用 FindAny 並關閉 named-data resolution；詢問 control/data 細節時才使用 FindAll 或 data resolution。
- data_function_resolved=false 時不得輸出不存在的 data net。
- data_search_complete=false 時只能回答尚未完整解析。
- functional fallback 應為 opt-in；all-DFF summary 不可無條件啟用 SAT search。
```

## 文件與測試同步

已更新：

```text
TOOLS_SPEC/README.md
TOOLS_SPEC/SEQUENTIAL_QUERY_TOOL.md
tools.cpp help / parser / enum printer / JSON-like envelope
mini test/test21 CLI regression 18/18（canonical 相容、functional proof、partial、timeout/error、參數錯誤）
```

CLI regression 驗證：預設相容、指定 DFF functional match、candidate limit、timeout、partial envelope、分頁與錯誤參數。C++ focused reference 為 `mini test/test28`。

## Critical Path Optimization V1

目前 C++ public facade 已完成：

```text
OptPassKind::CriticalPathDepth
Netlist::runOptQuery()
Netlist::runOptApply()
NetlistEditReport.depthChange
NetlistEditReport.depthOptimization
```

已完成安全條件：

```text
1. working copy 產生候選，通過後才 commit。
2. structure / Problem A / gate-basis / depth target validation。
3. mandatory whole-design SAT：同名 PO + DFF.D。
4. no improvement、target 未達、basis violation、timeout、不等價時保留 original。
5. DFF.Q fanin rewrite scope 解析到 D-pin data cone。
6. XAG candidate 使用 Problem A 實際 depth；NOT/BUF 都算一層。
7. OptimizationResult.changed / equivalenceChecked / equivalent 使用真實狀態。
```

驗證：

```text
mini test/test31: 11 passed
test22: global depth 41 -> 20
test26: n10 -> D-pin n1113，global depth 58 -> 30，NOR/NOT compliance PASS
mini test/test32: tools、timeout、depth-0 / NAND-NOT depth-2 lower bound PASS
NewTestCase/test40: official bounded smoke PASS，約 3.6 秒
NewTestCase/test33: 120 秒 process timeout；large D-pin cone core runtime blocker
```

Technology mapping 更新：

```text
convertToBasisOnGateSet / absorbInvertersOnGateSet 屬於 optimizer internal helper。
不新增 tools.cpp command，也不擴張 public gate type。
strict containment、no-op success 與 OptApply regression 已完成。
```

### tools.cpp 已串接

已新增公開 commands：

```text
opt_query critical_path_depth
opt_apply critical_path_depth [options]
```

參數對應：

| CLI 參數 | C++ 欄位 | 語意 |
|---|---|---|
| `--scope whole` | `scope=WHOLE_NETLIST` | 全設計 |
| `--scope net_fanin --name n10` | `scope/scopeName` | 指定 net fanin rewrite scope |
| `--scope gate_fanin --name g10` | `scope/scopeName` | 指定 gate fanin rewrite scope |
| `--objective global` | `GlobalMaximum` | maximum design depth |
| `--objective cone` | `ScopedFaninCone` | resolved fanin root depth |
| `--allowed nor,not` | `allowedTypes` | basis 白名單 |
| `--banned xor,xnor` | `bannedTypes` | basis 黑名單 |
| `--target-depth 5` | `targetDepth` | 未達成不提交 |
| `--time-limit 240` | `timeLimitSeconds` | optimization + SAT 預算 |
| `--allow-no-improvement` | `requireDepthImprovement=false` | 允許同 depth 的合規候選 |
| `--verbose` | `verbose=true` | optimizer log |

公開輸出至少包含：

```text
success / changed / rolled_back / message
before_depth / after_depth / target_depth / improved / meets_target
objective / scope / requested_scope_name / resolved_root_net_name
resolved_through_dff_data_pin
baseline_constraints_satisfied / final_constraints_satisfied
candidate_generated / candidate_accepted
equivalence_checked / equivalent / equivalence_timed_out
compared_output_count / compared_dff_d_count
elapsed_seconds / warnings
```

正式 C++ 用法見：

```text
API_SPEC/OPT_APPLY_API.md
API_SPEC/OPT_APPLY_USAGE.md
```

公開 tool 契約見：

```text
TOOLS_SPEC/OPTIMIZATION_TOOL.md
```

已完成 parser、help、JSON-like envelope、`NetlistEditReport.depthOptimization`
輸出，以及與 `report_query last_edit` / `equiv_query previous_edit` 共用的
session cache。CLI regression：`mini test/test32`。

目前不是 tools parser 待更新，而是 optimizer core 待補：

```text
1. 單次 mockturtle primitive cooperative cancellation。
2. ScopedFaninCone 不應先做全設計 XAG；需要 cone-isolated resynthesis。
```
