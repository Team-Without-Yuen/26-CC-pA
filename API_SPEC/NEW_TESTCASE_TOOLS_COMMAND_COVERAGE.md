# NewTestCase Tools Command Coverage

本文件逐條核對 `NewTestCase/test31` 到 `test40` 的 prompt、目前 `tools.cpp` command、必要結果欄位與尚未完成的缺口。

狀態定義：

| Status | 意思 |
|---|---|
| `Ready` | 單一既有 command 可直接取得答案或完成 edit |
| `Composite` | 現有 commands 可完整回答，但需要兩次以上查詢 |
| `Conditional` | API 已有，但必須檢查 timeout / partial / output size 才能宣稱完整 |
| `Partial` | 只能近似處理，scope、功能語意或 report 尚不完整 |
| `Missing` | 缺正式高階演算法或 command |
| `Future` | 屬真正 optimization，需候選生成、驗證、比較與 rollback flow |

所有 testcase 的共通 prompt：

| Prompt | Command | 必要結果 | Status |
|---|---|---|---|
| load `testXX.v` | `read NewTestCase/testXX/testXX.v` | `status: ok`, original snapshot | `Ready` |
| total gate count and type breakdown | `structure_query summary` | `gate_count`, all gate type counts | `Ready` |
| write current design | `write testXX_out.v` | `status: ok`, current revision | `Ready` |

## test31

| Line / prompt | Command | 必要結果 | Status |
|---|---|---|---|
| 4 reachable gates from `n2` | `cone_query net_fanout n2` | cone gate names/count | `Ready` |
| 5 collapse back-to-back inverters | `edit_apply collapse_double_inverter` | edit report + certificate | `Ready` |
| 6 prove pre/post transformation equivalence | `equiv_query previous_edit` | complete + equivalent | `Ready` |
| 7 direct fanout of PI `n0` | `structure_query fanout_load n0` | total loads and classified gate/DFF loads | `Ready` |
| 8 output `n16` always zero | `func_query always_zero n16` | complete solver status + answer | `Ready` |
| 9 shared fanin gates of `n16/n17` | `cone_query shared_fanin n16 n17` | shared gate names/count | `Ready` |
| 10 Boolean equation of `n16` | `func_query boolean_expression n16` | complete expression and support PIs | `Conditional` |
| 11 gates connected to output of `g0` | `structure_query gate_fanout g0` | immediate load gate names | `Ready` |
| 12 rename `n1289` | `edit_apply rename_net n1289 renamed_sig` | changed names + certificate | `Ready` |
| 13 all gates connected to renamed signal | `structure_query net_driver renamed_sig` + `structure_query net_loads renamed_sig` | driver and load gate names | `Composite` |
| 14 dedicated buffer per load of `n2` | `edit_apply insert_buffers_on_each_load n2` | successful edit report | `Ready` |
| 15 number of BUFs added | `report_query last_edit` | `gate_type_count_delta.BUF` / mapping delta | `Ready` |
| 16 max PI-to-DFF.D depth | `depth_query all_dff_d` | worst DFF.D depth/path | `Ready` |
| 17 output count with depth > 4 | `depth_query po_exceeding 4` | `Report count` | `Ready` |
| 18 `g0` type and pins | `structure_query gate_info g0` | type + formatted pin connections | `Ready` |
| 19 equivalence of `n1287/n2404` | `func_query equivalence n1287 n2404` | complete solver status + equivalent | `Ready` |

## test32

| Line / prompt | Command | 必要結果 | Status |
|---|---|---|---|
| 4 `g0` on any maximum-depth path | `depth_query gate_on_critical g0` | explicit yes/no | `Ready` |
| 5 number of PIs and POs | `structure_query summary` | PI/PO counts | `Ready` |
| 6 all register-to-register paths | `path_query enumerate all_dff_q all_dff_d` | automatic artifact + path count/complete flag | `Conditional` |
| 7 NAND with constant-1 to NOT | query candidates, then typed simplification | only selected NAND gates rewritten | `Partial` |
| 8 current NOT count | `structure_query count_by_type NOT` | active NOT count | `Ready` |
| 9 every `n2` to `n12` path through `g0` | `path_query every_through pi:n2 po:n12 -req gate:g0` | explicit yes/no | `Ready` |
| 10 rename `n1214` | `edit_apply rename_net n1214 renamed_wire` | changed names + certificate | `Ready` |
| 11 current vs original equivalence | `equiv_query original` | complete + equivalent | `Ready` |
| 12 transitive fanout of `n0` | `cone_query net_fanout n0` | reachable gate names | `Ready` |
| 13 gate count in cone of `n12` | `cone_query net_fanin n12` | cone gate count | `Ready` |
| 14 remove dangling/dead gates | `edit_apply trim_dead_logic` | removed active gates + certificate | `Ready` |
| 15 dangling gates removed | `report_query last_edit` | removed/active gate delta | `Ready` |
| 16 NAND gates with constant inputs | `structure_query const_input_gates NAND` | matching gate names/count | `Ready` |
| 17 simplify only reported NAND gates | `edit_apply simplify_constants NAND any` | selected NAND candidates/rewrites | `Ready` |
| 18 NAND gates eliminated | `report_query last_edit` | `eliminated_target_gate_count` | `Ready` |
| 19 all `n2` to `n12` paths | `path_query enumerate pi:n2 po:n12` | automatic artifact + path count/complete flag | `Conditional` |

## test33

| Line / prompt | Command | 必要結果 | Status |
|---|---|---|---|
| 4 all depth-0 PI-to-PO paths | `path_query direct_pi_po` | count + depth-0 witnesses | `Ready` |
| 5 XNOR to NOR-only | `edit_apply replace_type whole XNOR -allow NOR` | mapping report + certificate | `Ready` |
| 6 current NOR count | `structure_query count_by_type NOR` | NOR count | `Ready` |
| 7 remove dangling/dead gates | `edit_apply trim_dead_logic` | removed gate delta | `Ready` |
| 8 dangling gates removed | `report_query last_edit` | removed active gates | `Ready` |
| 9 cone `n8` to NAND/NOT basis | `edit_apply convert_basis net_fanin n8 -allow NAND NOT` | no disallowed cone gates + certificate | `Ready` |
| 10 NAND count in cone `n8` | `cone_query net_fanin n8 --gate-types NAND` | `filtered gates` | `Ready` |
| 11 equivalence `n55146/n55104` | `func_query equivalence n55146 n55104` | complete + equivalent | `Ready` |
| 12 every `n3` to `n9` path | `path_query enumerate pi:n3 po:n9` | automatic artifact + path count/complete flag | `Conditional` |
| 13 whether `n8` functionally depends on `n1` | `func_query depends_on n8 n1` | complete exact SAT/cofactor yes/no | `Ready` |
| 14 current vs loaded equivalence | `equiv_query original` | complete + equivalent | `Ready` |
| 15 whether `n55104` is a PI-to-PO cut | `path_query pi_po_cut n55104` | complete cut yes/no + witness pair | `Ready` |
| 16 list PIs with widths | `structure_query list_pi` | ordered name/width/range summaries | `Ready` |
| 17 merge structural duplicates | `edit_apply merge_structurally_equivalent_gates` | edit report + certificate | `Ready` |
| 18 number merged | `report_query last_edit` | removed active gate count | `Ready` |
| 19 optimize `n8` depth under NAND/NOT | `opt_apply critical_path_depth --scope net_fanin n8 --objective cone --allowed NAND NOT` | n8 resolves to a large D-pin cone；120 s outer timeout still expires inside mockturtle | `Blocked (Core Runtime)` |

## test34

| Line / prompt | Command | 必要結果 | Status |
|---|---|---|---|
| 4 whole design AND/NOT mapping | `edit_apply convert_basis whole -allow AND NOT` | final basis + certificate | `Ready` |
| 5 current AND count | `structure_query count_by_type AND` | AND count | `Ready` |
| 6 current vs loaded equivalence | `equiv_query original` | complete + equivalent | `Ready` |
| 7 expression for `n30` | `func_query boolean_expression n30` | expression + support PIs | `Conditional` |
| 8 path `n2` to `n30` avoiding `n4552` | `path_query exists net:n2 net:n30 -avoid net:n4552` | explicit yes/no | `Ready` |
| 9 max PI-to-PO depth | `depth_query all_po` | worst PO depth/path | `Ready` |
| 10 deepest output bit | `depth_query deepest_output` | output name + depth | `Ready` |
| 11 direct fanout of PI `n0` | `structure_query fanout_load n0` | classified direct loads | `Ready` |

## test35

| Line / prompt | Command | 必要結果 | Status |
|---|---|---|---|
| 4 XNOR to NOR-only | `edit_apply replace_type whole XNOR -allow NOR` | mapping report + certificate | `Ready` |
| 5 NOR gates added | `report_query last_edit` | added NOR count | `Ready` |
| 6 path PI `n2` to PO `n25` | `path_query exists pi:n2 po:n25` | explicit yes/no | `Ready` |
| 7 gates connected to output of `g0` | `structure_query gate_fanout g0` | load gate names | `Ready` |
| 8 deepest output bit | `depth_query deepest_output` | output name + depth | `Ready` |
| 9 all NAND gates with pins | `structure_query gates_by_type NAND --with-pins` | 19,682 structured gate/input/output records；完整 artifact | `Ready`；test70 實測 19,682/19,682 complete |
| 10 rename `n7431` | `edit_apply rename_net n7431 renamed_wire` | changed names + certificate | `Ready` |
| 11 pre/post transformation equivalence | `equiv_query previous_edit` | complete + equivalent | `Ready` |
| 12 equivalence `n29498/n29471` | `func_query equivalence n29498 n29471` | complete + equivalent | `Ready` |
| 13 find `(a,b)` where `NAND(a,b) == n25` | `func_search nand_pair n25` | witness pair + UNSAT proof, or complete none | `Ready` |
| 14 collapse double inverters | `edit_apply collapse_double_inverter` | edit report + certificate | `Ready` |
| 15 current maximum combinational depth | `depth_query global_critical` | worst depth/path | `Ready` |
| 16 Boolean function of `n25` | `func_query boolean_expression n25` | expression + support PIs | `Conditional` |
| 17 DFFs clocked by `n0` | `structure_query fanout_load n0` | DFF clock-pin load names | `Ready` |
| 18 XOR to 4-NAND implementation | `edit_apply replace_type whole XOR -allow NAND` | standard 4-NAND mapping + certificate | `Ready` |
| 19 NAND gates added | `report_query last_edit` | added NAND count | `Ready` |

## test36

| Line / prompt | Command | 必要結果 | Status |
|---|---|---|---|
| 4 whether `n10239` is PI-to-PO cut | `path_query pi_po_cut n10239` | complete cut yes/no + witness pair | `Ready` |
| 5 current vs original equivalence | `equiv_query original` | complete + equivalent | `Ready` |
| 6 AND gates with constant 0 | `structure_query const_input_gates AND 0` | gate names/count | `Ready` |
| 7 simplify only reported AND gates | `edit_apply simplify_constants AND 0` | selected AND candidates/rewrites | `Ready` |
| 8 AND gates eliminated | `report_query last_edit` | `eliminated_target_gate_count` | `Ready` |
| 9 `g0` type and pins | `structure_query gate_info g0` | type + pin connections | `Ready` |
| 10 max PI-to-DFF.D depth | `depth_query all_dff_d` | worst DFF.D depth | `Ready` |
| 11 symmetry of `n11` w.r.t. two inputs | `func_query symmetry n11 n3 n9[0]` | `SYMMETRIC`、support flags、solver status | `Ready` |
| 12 DFFs clocked by `n0` | `structure_query fanout_load n0` | DFF clock-pin loads | `Ready` |
| 13 PI with highest fanout | `structure_query pi_fanout` | max fanout + max-fanout PI names | `Ready` |
| 14 enforce fanout <= 16 | `edit_apply insert_buffers_for_fanout 16` | meets constraint + gate delta | `Ready` |
| 15 current fanout of `n1` | `structure_query fanout_load n1` | total fanout | `Ready` |
| 16 direct fanout of `n0` | `structure_query fanout_load n0` | classified direct loads | `Ready` |

## test37

| Line / prompt | Command | 必要結果 | Status |
|---|---|---|---|
| 4 gate type count in cone `n8` | `cone_query net_fanin n8` | all cone gate type counts | `Ready` |
| 5 cone `n8` to NAND/NOT | `edit_apply convert_basis net_fanin n8 -allow NAND NOT` | final cone basis + certificate | `Ready` |
| 6 current NAND count in cone | `cone_query net_fanin n8 --gate-types NAND` | `filtered gates` | `Ready` |
| 7 prune unused gates | `edit_apply trim_dead_logic` | removed active gates + certificate | `Ready` |
| 8 current vs loaded equivalence | `equiv_query original` | complete + equivalent | `Ready` |
| 9 all register paths | `path_query enumerate all_dff_q all_dff_d` | automatic artifact + path count/complete flag | `Conditional` |
| 10 PI/PO counts | `structure_query summary` | PI/PO counts | `Ready` |
| 11 list POs with widths | `structure_query list_po` | ordered name/width/range summaries | `Ready` |
| 12 gates with input tied to `1'b1` | `structure_query const_input_gates all 1` | gate names/count | `Ready` |
| 13 Boolean function of `n8` | `func_query boolean_expression n8` | expression + support PIs | `Conditional` |
| 14 floating inputs/unconnected outputs | `structure_query structural_issues` | floating/undriven/no-load names | `Ready` |
| 15 floating signal count | same structural report | `floatingNets.size()` | `Ready` |
| 16 gates connected to output `g0` | `structure_query gate_fanout g0` | load gate names | `Ready` |
| 17 symmetry of `n8` w.r.t. two inputs | `func_query symmetry n8 n3 n4[0]` | `SYMMETRIC`、support flags、solver status | `Ready` |

## test38

| Line / prompt | Command | 必要結果 | Status |
|---|---|---|---|
| 4 reset `n1` fanout <= 4 | `edit_apply insert_buffers_for_net n1 4` | meets constraint + certificate | `Ready` |
| 5 current max fanout of `n1` | `structure_query fanout_load n1` | total fanout | `Ready` |
| 6 PI with highest fanout | `structure_query pi_fanout` | max-fanout PI names | `Ready` |
| 7 collapse double inverters | `edit_apply collapse_double_inverter` | edit report + certificate | `Ready` |
| 8 current maximum depth | `depth_query global_critical` | worst depth/path | `Ready` |
| 9 remove redundant gates | `edit_apply merge_structurally_equivalent_gates` | structural-identity certificate + active gate delta | `Ready`；official test38 實測為 14 個 structural duplicates |
| 10 redundant gates removed | `report_query last_edit` | `-diff.active_gate_count_delta` | `Ready`；official test38 為 14 |
| 11 depth-0 PI-to-PO paths | `path_query direct_pi_po` | count + witnesses | `Ready` |
| 12 articulation points between `n2/n14` | `path_query mandatory_nodes n2 n14` | path status + ordered articulation net list | `Ready` |
| 13 rename `n440` | `edit_apply rename_net n440 renamed_wire` | changed names + certificate | `Ready` |
| 14 current vs original equivalence | `equiv_query original` | complete + equivalent | `Ready` |
| 15 NAND gates with constants | `structure_query const_input_gates NAND` | gate names/count | `Ready` |
| 16 simplify only reported NAND gates | `edit_apply simplify_constants NAND any` | selected NAND candidates/rewrites | `Ready` |
| 17 NAND gates eliminated | `report_query last_edit` | `eliminated_target_gate_count` | `Ready` |
| 18 type counts in cone `n14` | `cone_query net_fanin n14` | cone gate type counts | `Ready` |
| 19 all gates in fanin cone `n14` | `cone_query net_fanin n14` | cone gate names | `Ready` |

## test39

| Line / prompt | Command | 必要結果 | Status |
|---|---|---|---|
| 4 OR gates with constant 1 | `structure_query const_input_gates OR 1` | gate names/count | `Ready` |
| 5 simplify only reported OR gates | `edit_apply simplify_constants OR 1` | selected OR candidates/rewrites | `Ready` |
| 6 OR gates eliminated | `report_query last_edit` | `eliminated_target_gate_count` | `Ready` |
| 7 outputs with depth > 4 | `depth_query po_exceeding 4` | report count | `Ready` |
| 8 collapse double inverters | `edit_apply collapse_double_inverter` | edit report + certificate | `Ready` |
| 9 current maximum depth | `depth_query global_critical` | worst depth/path | `Ready` |
| 10 list XOR gates | `structure_query gates_by_type XOR` | gate names/count | `Ready` |
| 11 Boolean equation of `n12` | `func_query boolean_expression n12` | expression + support PIs | `Conditional` |
| 12 dedicated buffer per load of `n2` | `edit_apply insert_buffers_on_each_load n2` | edit report | `Ready` |
| 13 BUFs added | `report_query last_edit` | added BUF count | `Ready` |
| 14 NOR gates with constants | `structure_query const_input_gates NOR` | gate names/count | `Ready` |
| 15 simplify only reported NOR gates | `edit_apply simplify_constants NOR any` | selected NOR candidates/rewrites | `Ready` |
| 16 NOR gates eliminated | `report_query last_edit` | `eliminated_target_gate_count` | `Ready` |
| 17 list PIs with widths | `structure_query list_pi` | ordered name/width/range summaries | `Ready` |
| 18 XOR to 4-NAND | `edit_apply replace_type whole XOR -allow NAND` | mapping report + certificate | `Ready` |
| 19 current NAND count | `structure_query count_by_type NAND` | NAND count | `Ready` |

## test40

| Line / prompt | Command | 必要結果 | Status |
|---|---|---|---|
| 4 whole design NAND/NOT mapping | `edit_apply convert_basis whole -allow NAND NOT` | final basis + certificate | `Ready` |
| 5 NAND with constant-1 to NOT | `edit_apply simplify_constants NAND 1 --inputs 2` | selected NAND-to-NOT rewrites | `Ready` |
| 6 current NOT count | `structure_query count_by_type NOT` | NOT count | `Ready` |
| 7 collapse double inverters | `edit_apply collapse_double_inverter` | edit report + certificate | `Ready` |
| 8 current vs original equivalence | `equiv_query original` | complete + equivalent | `Ready` |
| 9 POs with widths | `structure_query list_po` | ordered name/width/range summaries | `Ready` |
| 10 list XOR gates | `structure_query gates_by_type XOR` | names/count | `Ready` |
| 11 max register-to-register depth | `path_query max_depth all_dff_q all_dff_d` | representative DFF pair + depth/path | `Ready` |
| 12 report DFF enable/hold structures | `sequential_query enable_hold all --confirmed-only` 自動完整寫入 artifact；需要非 canonical functional search 時才 opt-in `--functional-fallback` | DFF names、pattern、D-input logic、artifact path、functional completeness | `Ready (Canonical + Bounded Functional)` |
| 13 count DFFs with enable/hold | canonical：`sequential_query enable_hold all --summary-only`；functional lower bound：再加 `--functional-fallback --functional-find-any --no-resolve-functional-data` 與明確 budget | `matched_dff_count` unique confirmed DFF count；只有 `complete:true` 才是完整數量 | `Ready (Canonical + Bounded Functional)` |
| 14 `g0` type and pins | `structure_query gate_info g0` | type + pin connections | `Ready` |
| 15 output with largest fanin cone | `cone_query largest_output` | selected output + gate count | `Ready` |
| 16 optimize `n14` depth under NAND/NOT | `opt_apply critical_path_depth --scope net_fanin n14 --objective cone --allowed NAND NOT` | `NOT(NAND(n514,n412))` is proven optimal at depth 2；original retained | `Ready` |
| 17 current depth of cone `n14` | `depth_query net n14` | depth/path | `Ready` |
| 18 current maximum depth | `depth_query global_critical` | worst depth/path | `Ready` |

## Confirmed Gaps

| Priority | Gap | Blocking prompts |
|---|---|---|
| Completed | symmetry analysis | test36、test37 與 mini test22 已驗證 |
| Completed | NAND witness-pair function search | test35；API + CLI + mini test23/test24 |
| Completed | test38 structural redundancy removal | official flow 實測移除 14 gates，且 whole-design SAT 通過 |
| Partial | constrained depth optimization flow | test40 official flow PASS；test33 large D-pin cone blocked by non-preemptible global XAG core |
| P1 | cooperative timeout / cone-isolated resynthesis | `--time-limit` cannot interrupt one mockturtle primitive；blocks test33 bounded completion |
| P2 | general observability-aware redundancy removal | 尚無已確認 NewTestCase prompt；保留給 hidden-case hardening |

## Response Rules

1. SAT、equivalence、path enumeration 只有在 `complete: true` 時才能當最終答案。
2. `report_query last_edit` 只能回答最近一次 edit/optimization apply；新的 apply 會覆蓋 cached report。
3. `exceeding` 包含 PO 與 DFF.D；output-only 題固定使用 `po_exceeding`。
4. `gate_fanout` 回答 gate output 的直接 load gates；transitive reachable 必須使用 `cone_query`。
5. `PrimaryInputsOfNet` 是 structural support，不足以單獨證明 Boolean functional dependence；exact yes/no 使用 `func_query depends_on`。
6. `simplify_constants` 的 query/edit filter 應使用相同 gate type、constant value 與 input count；回答 eliminated count 時讀 `constant_simplification.eliminated_target_gate_count`，不可直接使用 `simplified_count`。
7. `structure_query` 的 total/list/type/structural 結果只包含 current active objects，不計 UNKNOWN/removed tombstones。

## Official Case Smoke Audit

代表性非最佳化 commands 已直接在 test31 到 test40 的官方 Verilog 上執行。這一輪刻意不執行無界 path enumeration 與巨大完整 Boolean expression；optimization 的 C++/CLI regression 分別見 mini test/test31 與 test32。

| Case | Session time | Envelope result |
|---|---:|---|
| test31 | 0.661 s | 5/5 complete，無 non-ok status |
| test32 | 0.122 s | 4/4 complete，無 non-ok status |
| test33 | 0.350 s | 4/4 complete，無 non-ok status |
| test34 | 0.144 s | 4/4 complete，無 non-ok status |
| test35 | 0.196 s | 4/4 complete，無 non-ok status |
| test36 | 0.075 s | 3/3 complete，無 non-ok status |
| test37 | 0.260 s | 3/3 complete，無 non-ok status |
| test38 | 0.034 s | 3/3 complete，無 non-ok status |
| test39 | 0.630 s | 3/3 complete，無 non-ok status |
| test40 | 0.138 s | 4/4 complete，無 non-ok status |

此處的 complete 只代表本 smoke session 中選取的 commands；`Conditional`、`Partial`、`Missing`、`Future` 項目仍以逐題表格為準。

### Official Optimization Smoke

使用：

```powershell
.\scripts\run_tools_regression.ps1 -Profile Quick `
    -OfficialOptimizationSmoke -OptimizationTimeLimitSeconds 30 `
    -OfficialCaseTimeoutSeconds 120
```

| Case | Result | 說明 |
|---|---|---|
| test40 | PASS，約 3.6 秒 | 完整前置 NAND/NOT mapping/cleanup 後，n14 depth 2 命中安全 lower-bound proof，回 original |
| test33 | 待重測 | 若 n8 為 DFF.Q，應以 boundary depth 0 回 original；hidden 中非 DFF.Q 大型 cone 仍可能卡在不可搶占的 global XAG primitive |

### test35 Function Search 實測

```text
command: func_search nand_pair n25 --time-limit 45
status: ok
complete: true
elapsed: 6.51 s
match: (n26080, n6359)
proof: SAT_UNSAT_MITER / UNSAT
```
