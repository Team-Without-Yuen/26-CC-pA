# API / Core Backend 待辦

這份文件記錄不屬於 `tools.cpp` parser/help/envelope printer 的事項。
包含 public C++ API、Netlist/engine、optimizer core、sequential pattern engine、
transaction validation、session state 與底層 regression。

對應的 tools 層同步項目請放在：

```text
API_SPEC/TOOLS待更新表.md
```

## P0-1：DFF.Q fanin scope 必須停在 boundary

官方 QA 要求 DFF.Q/register output 的 fanin cone 視為空的 combinational
boundary，不得自動改取同一 DFF 的 D-input cone。

底層待修改：

```text
- resolveRewriteScope() 移除 DFF.Q -> D-pin 特例。
- net_fanin <DFF.Q> 回傳 empty cone、gate count 0、depth 0。
- scoped edit/optimization 對 empty cone 回 verified no-op。
- gate-basis constraint 在 empty cone 上 vacuously satisfied。
- 移除或 deprecated resolved_through_dff_data_pin 的 API 語意。
- 更新 test26/test33 coverage。
- 更新 mini test/test30/test31/test32。
```

這項修改後，如果 testcase 中的 n8 確認是 DFF.Q，應直接以 boundary depth 0
回 original/already optimal，不再進入大型 D-pin XAG optimization。

## P0-2：跨 prompt persistent constraints

同一 testcase 中，先前 prompt 建立的 structural constraints 在後續
transformation 後仍需成立。不能只依賴 LLM 在後續 prompt 記得重送 constraint；
final writer 前必須有 deterministic validation。

底層待設計：

```text
- session 保存 accumulated hard constraints。
- 成功的 fanout limit、whole/scope gate basis 等 request 註冊 constraint。
- 每次後續 edit/opt candidate commit 前重驗全部 accumulated constraints。
- write 前執行 final constraint validation。
- 違反舊 constraint 時 rollback/reject，並在 report 說明是哪一條失敗。
- read 新 design 時清空 constraint state。
```

## P1-1：AND-only enable/hold candidate

官方 QA 已確認 `D = EN & DATA` 不算 enable/hold；只有含同一 DFF.Q feedback
的 MUX-hold function 才算 enable/hold。

底層待修改：

```text
- public sequential_query 預設不輸出 AndGatedDataCandidate 作為 match。
- matched_dff_count / candidate_dff_count 不得把 AND-only 算成 enable/hold。
- semanticsPending 說明改成明確 non-match diagnostic，或移除 public record。
- 更新 SEQUENTIAL_PATTERN_API.md。
- 更新 SEQUENTIAL_PATTERN_USAGE.md。
- 更新 test19/test21 coverage。
```

## P1-2：Critical Path Optimization core runtime blocker

目前不是 tools parser 待更新，而是 optimizer core 待補：

```text
- 單次 mockturtle primitive cooperative cancellation。
- ScopedFaninCone 不應先做全設計 XAG；需要 cone-isolated resynthesis。
```

背景驗證紀錄：

```text
mini test/test31: 11 passed
test22: global depth 41 -> 20
test26: n10 -> D-pin n1113，global depth 58 -> 30，NOR/NOT compliance PASS
mini test/test32: tools、timeout、depth-0 / NAND-NOT depth-2 lower bound PASS
NewTestCase/test40: official bounded smoke PASS，約 3.6 秒
NewTestCase/test33: 120 秒 process timeout；large D-pin cone core runtime blocker
```

## 已完成但歸屬 API / backend 的事項

### Sequential Pattern functional fallback

目前狀態：

```text
C++ API 已完成
既有 sequential_query 行為維持 canonical/default mode
functional fallback 維持 opt-in
candidate ranking 為 engine 固定策略，不新增 CLI 參數
simulation-aware stable rerank 為 engine 固定策略，不新增 CLI 參數
simulation safe Reject 不占 max-functional-candidates quota
one-shot/reusable hybrid SAT session 為 engine 固定策略，不新增 CLI 參數
```

公開 API / engine counter 已包含：

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

### Critical Path Optimization V1 facade

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
5. DFF.Q fanin rewrite scope 解析到 D-pin data cone。（待依 P0-1 修改）
6. XAG candidate 使用 Problem A 實際 depth；NOT/BUF 都算一層。
7. OptimizationResult.changed / equivalenceChecked / equivalent 使用真實狀態。
```

正式 C++ 用法見：

```text
API_SPEC/OPT_APPLY_API.md
API_SPEC/OPT_APPLY_USAGE.md
```

### PathQuery all-DFF max-depth DP fast path

目前狀態：

```text
`path_query max_depth all_dff_q all_dff_d` 已改為 multi-source / multi-endpoint
DP-style longest-path engine。
```

語意：

```text
- 適用於無 required/avoid constraints 的 MaxDepth query。
- 所有 DFF.Q startpoints 視為 depth-0 sources。
- 所有 DFF.D endpoints 中取最大 reachable depth。
- 回傳 depth 與一條 witness combinational path。
- 若偵測到 combinational cycle，保留 fallback 到原本 pair-wise DFS。
```

背景驗證紀錄：

```text
修正前：NewTestCase/test40 path_query max_depth all_dff_q all_dff_d 70 秒外層 timeout
修正後：同一 command 約 0.5 秒完成，Depth=104
對照：NewTestCase/test40 depth_query all_dff_d 約 0.17 秒完成，worst Depth=104
mini test/test33: 10 passed
Tools regression profile: 9 passed
```

### Technology mapping internal helpers

以下屬於 optimizer internal helper，不新增 tools.cpp command，也不擴張 public
gate type：

```text
convertToBasisOnGateSet
absorbInvertersOnGateSet
```

strict containment、no-op success 與 OptApply regression 已完成。

## 已符合但需同步文件的 QA 結論

```text
- Q61: 名稱只查 current transformed netlist，不維護 original aliases。
- Q62/Q66: verified no-op / zero replacement 可接受。
- Q64: cost 必須讀 final constrained candidate 的 metrics。
- Q67: intermediate helper gate 不受 final basis 限制。
- Q68: transformation log 可簡短；generated Verilog 與 final validation 才是評分依據。
```
