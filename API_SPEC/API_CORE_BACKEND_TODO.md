# API / Core Backend 待辦

這份文件記錄不屬於 `tools.cpp` parser/help/envelope printer 的事項。
包含 public C++ API、Netlist/engine、optimizer core、sequential pattern engine、
transaction validation、session state 與底層 regression。

對應的 tools 層同步項目請放在：

```text
API_SPEC/TOOLS待更新表.md
```

## 已完成：DFF.Q fanin scope 停在 boundary

官方 QA 要求 DFF.Q/register output 的 fanin cone 視為空的 combinational
boundary，不得自動改取同一 DFF 的 D-input cone。

已完成修改：

```text
- resolveRewriteScope() 的 NET_FANIN 已移除 DFF.Q -> D-pin 特例。
- cone_query net_fanin <DFF.Q> 回傳 empty cone、gate count 0、depth 0。
- scoped edit/optimization 對 empty cone 回 no-change / already optimal。
- gate-basis constraint 在 empty cone 上 vacuously satisfied。
- GATE_FANIN <DFF instance> 仍可解析到 D-pin cone，因為 target 是 DFF cell 本身。
```

驗證：

```text
NewTestCase/test26 n10:
- cone_query net_fanin n10 -> gates 0, nets 1
- edit_apply convert_basis net_fanin n10 -allow NOR NOT -> no_change
- opt_apply critical_path_depth --scope net_fanin n10 --objective cone --allowed NOR NOT -> already optimal, depth 0
```

## 已標記為 LLM 層策略：跨 prompt persistent constraints

同一 testcase 中，先前 prompt 建立的 structural constraints 在後續
transformation 後是否仍需成立，屬於 prompt / testcase 語意判斷。這件事先不在
底層自動保存 accumulated constraint state，避免 API 依自然語言做過度推論。

目前定位：

```text
- constraint memory 交給 LLM / testcase driver 維護。
- 若後續 prompt 需要保留前題 hard constraint，LLM 必須在 edit/opt/query 時重送對應條件或額外呼叫驗證工具。
- 底層 API 只保證單次 request 中明確帶入的 allowed/banned/fanout/equivalence requirement 會被檢查。
- 後續若要新增 LLM-facing policy 或 tool usage 說明，放在 tools 文件同步階段處理。
```

## 已完成：AND-only enable/hold candidate 改為 non-match diagnostic

官方 QA 已確認 `D = EN & DATA` 不算 enable/hold；只有含同一 DFF.Q feedback
的 MUX-hold function 才算 enable/hold。

已完成修改：

```text
- AND-only report kind 改為 DataGatingWithoutHoldFeedback。
- per-DFF status 改為 DATA_GATING_WITHOUT_HOLD_FEEDBACK。
- matched_dff_count / candidate_dff_count 都不計入 AND-only data gating。
- semanticsPending=false，因官方語意已明確，不再標成 pending candidate。
- functional fallback 仍可辨識 Q69 允許的 Q-free EN/DATA Boolean decomposition。
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

## Out of Scope：VerilogReader 靜默略過未知 primitive

`VerilogReader::read()` 目前只在第一個 token 可轉成既有 `GateType` 時呼叫
`parseGateInstance()`；MUX 或其他未知 primitive 會被直接略過，read 仍回成功，且沒有
unsupported diagnostic。

實證：

```text
Blup/function_search_runs/unsupported_probe/unknown_primitive_probe.v

mux g_mux (y, select, data0, data1);
read -> ok:true, gate_count:0
func_search equivalent_pairs whole --all
  -> complete:true, NO_MATCH, unsupported:false
func_search nand_pair y --all --include-boundary-signals
  -> complete:true, NO_MATCH, unsupported:false
```

若任意 Verilog primitive 可出現在輸入，這會讓下游 API 在失真的 named netlist 上產生看似
完整的答案。不過官方 testcase netlist 只會使用題目規定的 gate types；prompt 中的 MUX 等
語意會由合法 gates 組成的 Boolean function 表示，不會以 direct `mux` primitive 出現在
netlist。因此此項不列為競賽實作待辦，也不修改 Function Search。

一般 robustness 的可能修正方向：

```text
1. reader 遇到未知 primitive 時不得靜默忽略。
2. 若該 primitive 可保留，建立可辨識的 unsupported cell/net connectivity，讓 analysis 回 partial/unsupported。
3. 若無法安全保留，read 應回失敗並列出 instance/type/line diagnostic。
4. 若官方確認 direct MUX/cell 會出現，應正式擴充 GateType、reader、writer、graph、simulation 與 SAT encoding。
5. 新增 read -> structure -> write round-trip regression，禁止 gate instance 靜默遺失。
```

目前決議：`Out of Scope / No Action`。若官方日後變更 netlist gate-type contract，再重新開啟。

## 後期 AIG / Boolean 重構項目

目前決策：

```text
- Boolean-function 類能力先不做短期擴充。
- 後期先建立 current named netlist 對應的 AIG functional index，再統一處理 SAT / cofactor / equivalence / symmetry / enable-hold decomposition。
- current named netlist 仍是唯一正式 design state；AIG 只作為 functional analysis index，不取代 gate/net 名稱、depth、fanout 或 writer 結果。
- 現階段保留既有可回答能力，但不再新增臨時 heuristic 來補 hidden Boolean pattern。
```

後期重構範圍：

```text
- sequential_query enable_hold 的 Q-free EN/DATA Boolean decomposition。
- func_query boolean_expression 的 canonical / depth-limited / large-output policy。
- func_query symmetry 的完整 Boolean-level proof 與 cache。
- func_query equivalence / constant function 的共用 SAT/AIG cache。
- merge_functionally_equivalent_gates 這類依賴全域 Boolean equivalence 的 cleanup/edit。
```

## 已完成但歸屬 API / backend 的事項

### Sequential Pattern functional fallback

目前狀態與定位：

```text
C++ API 已完成
既有 sequential_query 行為維持 canonical/default mode
functional fallback 維持 opt-in
candidate ranking 為 engine 固定策略，不新增 CLI 參數
simulation-aware stable rerank 為 engine 固定策略，不新增 CLI 參數
simulation safe Reject 不占 max-functional-candidates quota
one-shot/reusable hybrid SAT session 為 engine 固定策略，不新增 CLI 參數
此功能目前不列為短期 default-policy 調整項；後期 AIG 重構後再統一決定 public/default behavior。
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
5. DFF.Q fanin rewrite scope 停在 sequential boundary，不解析到 D-pin data cone。
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
