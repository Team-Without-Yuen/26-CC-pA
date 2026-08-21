# Public API Consolidation

這份文件定義提供給 LLM 與 `tools.cpp` help/schema 的公開查詢邊界。合併只發生在公開 facade；底層 C++ API 與演算法模組仍保留各自責任。

## 1. 公開 Query Families

| 公開 command | 負責問題 | 內部 dispatch |
|---|---|---|
| `structure_query` | design inventory、gate/net/port 資訊、structural issue、一層 driver/load/fanout/pin connectivity | `runBasicQuery()` 或 `runDirectConnectivityQuery()` |
| `cone_query` | transitive fanin/fanout、reachable cone、shared/largest cone | `runConeQuery()` |
| `path_query` | endpoint path、register-to-register path、mandatory nodes、separator、PI-to-PO cut | `runPathQuery()`；必要時呼叫 Graph dominator engine |
| `depth_query` | arrival depth、critical path、endpoint threshold | `runDepthQuery()` |
| `func_query` | 指定 net/bus 的 equivalence、constant、dependence、symmetry、expression | `runFunctionQuery()` |
| `func_search` | 未知 internal signal(s) 的 Boolean candidate search；支援基本 unary/binary pattern | `runFunctionSearchQuery()` |
| `sequential_query` | DFF enable/hold/feedback-MUX pattern；目前 CLI 為 canonical fast path | `runSequentialPatternQuery()`；C++ 層另有受限 functional fallback |

Edit、whole-design equivalence、report 與 optimization 維持各自 command family，不與 read-only query 合併。

Optimization 的 C++ public facade 為 `runOptQuery()` / `runOptApply()`；
`CriticalPathDepth` 已完成 transactional apply、basis/depth validation 與
whole-design SAT。`tools.cpp` 已公開 `opt_query/opt_apply critical_path_depth`，
並只呼叫高階 facade，不直接暴露內部 `DepthOptimizer`。LLM-facing 契約見
`TOOLS_SPEC/OPTIMIZATION_TOOL.md`。

## 2. 合併後的責任

`structure_query` 合併的是 LLM-facing namespace，不合併內部 report 型別。mode 屬於基本物件/統計時建立 `BasicQuery`；屬於 immediate edge/fanout 時建立 `DirectConnectivityQuery`。兩者 mode 名稱目前不衝突，無法辨識的 mode 必須回錯誤。

`path_query` 是所有 endpoint connectivity 的公開 owner。一般 path 使用 start/end endpoint；register path 使用 `all_dff_q`、`all_dff_d` 或指定 `dff_q:<ff>`、`dff_d:<ff>`；mandatory/separator/cut 使用 dominator-based Graph engine，不列舉所有 paths。

## 3. Legacy Compatibility

以下命令仍可執行既有腳本，但不得出現在新的 help、LLM tool schema 或 prompt routing：

```text
basic_query
conn_query
reg_path_query
graph_query
```

對應 C++ `runRegisterPathQuery()` 與 `runGraphQuery()` 也保留為 legacy wrapper/internal engine。保留它們可避免破壞內部模組與舊測試，不代表它們仍是公開 API family。

## 4. Routing Examples

```text
How many NOT gates are there?
  -> structure_query count_by_type NOT

List all flip-flops driven by clock n0.
  -> structure_query fanout_load n0

List all register-to-register paths.
  -> path_query enumerate all_dff_q all_dff_d

Find all articulation points between n2 and n14.
  -> path_query mandatory_nodes n2 n14

Is n55104 a cut between any PI and PO?
  -> path_query pi_po_cut n55104
```

## 5. 不合併的邊界

`ConeQuery` 不併入 PathQuery，因為 cone 問的是 reachable set，而 path 問的是指定 endpoint 關係。`DepthQuery` 不併入 PathQuery，因為它負責全設計 timing endpoint 與 global critical path。`FunctionQuery` 與 `SequentialPatternQuery` 也維持獨立，分別負責已指定訊號的 Boolean 證明與 register-control semantics。內部 `FunctionalPatternEngine` 不新增公開 command；未來 matcher 仍由 `sequential_query` facade 轉接。

`DepthQuery` 與 `OptApply` 也不合併：前者只量測 current design，後者會搜尋
候選並可能修改 netlist。固定指定 basis conversion 仍由 `EditApply` 負責。
