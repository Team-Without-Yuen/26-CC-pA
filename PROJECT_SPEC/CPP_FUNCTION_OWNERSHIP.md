# C++ Function Ownership

這份文件記錄目前 `.cpp` 檔案的職責分工。  
目前已經完成第一階段 source-level 分檔：原本混在 `NetlistAnalysis.cpp` 的分析 API 已拆成 Basic / Connectivity / Function / Cone / Path / Depth 等檔案。

## 1. 分類基準

目前 `Netlist.h` 的主要 API 分類如下：

```text
0. Construction / Writer Access
1. Low-Level Analysis
   1.1 Basic Netlist / ID / Type Query
   1.2 Direct Connectivity
   1.3 Boolean / Function Analysis
   1.4 Cone Analysis
   1.5 Combinational Path Analysis
   1.6 Depth Analysis
2. Low-Level Mutation / Cleanup
3. Optimization Candidate / Strategy
4. Unified High-Level Query APIs
```

## 2. Current Source Ownership

| 檔案 | 主要職責 | 內容 |
| --- | --- | --- |
| `src/core/Netlist.cpp` | Core construction / basic accessors | 建立 net/gate/port、維護 name-to-id map、`getGateId()`、`getNetId()`、`expandNetToBits()`、`getGateInfo()` |
| `src/analysis/BasicAnalysis.cpp` | Basic Netlist / ID / Type Query | valid ID 判斷、PI/PO/DFF/combinational 類型判斷、名稱列表、port width、gate type count、structural issue summary、`runBasicQuery()` |
| `src/analysis/ConnectivityAnalysis.cpp` | Direct Connectivity | net driver、net loads、gate input/output nets、gate fanin/fanout、directly connected、`runDirectConnectivityQuery()` |
| `src/analysis/FunctionAnalysis.cpp` | Boolean Function & Search Analysis | CaDiCaL SAT helper、equivalence、dependence、symmetry/cofactor miter、constant/truth analysis、simulation-filtered NAND pair search、`runFunctionQuery()`、`runFunctionSearchQuery()` |
| `src/analysis/SequentialPatternAnalysis.cpp` | Sequential Pattern Analysis | DFF D-input canonical feedback-MUX detection、NOT/NAND-self/NOR-self polarity normalization、enable polarity/data/feedback extraction、`runSequentialPatternQuery()` |
| `src/analysis/WholeDesignEquivalence.cpp` | Cross-netlist Equivalence | batched SAT miter、PI/DFF.Q boundary alignment、PO 與 DFF.D next-state equivalence、mismatch localization |
| `src/analysis/GraphAnalysis.cpp` | Internal Directed Graph Engine | PI-to-PO cut、source-target dominator/mandatory nodes、cycle/unsupported report、`runGraphQuery()`；由 public `PathQuery` mode 呼叫，舊 GraphQuery 入口僅保留相容性 |
| `src/analysis/ConeAnalysis.cpp` | Cone Analysis | transitive fanin/fanout cone、cone gate/net report、cone-local longest/shortest path wrapper、`runConeQuery()` |
| `src/analysis/PathAnalysis.cpp` | Public Endpoint Connectivity & Path Analysis | endpoint resolver、exist/find/enumerate/min/max/every-through/every-avoid、DFF.Q/DFF.D boundary path、mandatory nodes/separator dispatch、`runPathQuery()`；`runRegisterPathQuery()` 為 legacy wrapper |
| `src/analysis/DepthAnalysis.cpp` | Depth / Critical Path Analysis | net/gate level、endpoint depth report、PI-to-PO / PI-to-DFF-D depth、global critical path、`runDepthQuery()` |
| `src/analysis/OptimizationAnalysis.cpp` | Optimization Planning | depth endpoint 轉 optimization candidate、critical path buffer candidate、removable buffer candidate |
| `src/transformation/NetlistTransformation.cpp` | Graph Mutation / Buffer Insertion | rename、disconnect/connect、remove gate、insert buffer family |
| `src/transformation/EditFlow.cpp` | Public Edit Transaction Dispatcher | public/internal command validation、operation dispatch、report normalization、rollback policy |
| `src/transformation/TechMapper.cpp` | Technology Mapping | basis conversion、gate-type replacement、mapping delta 與 local equivalence certificate |
| `src/optimization/NetlistOptimization.cpp` | Cleanup / Simplification / Validation | dead logic、buffer cleanup、double inverter、constant propagation、same-input simplification、dangling logic、structural hashing、validation / rollback helpers |
| `src/io/VerilogReader.cpp` | Restricted Verilog parser frontend | 讀 flattened gate-level Verilog，呼叫 construction API 建圖 |
| `src/io/VerilogWriter.cpp` | Restricted Verilog writer | 將目前 netlist graph 輸出回 restricted Verilog |

## 3. 已完成的拆分

| 原本混在一起的功能 | 現在位置 | 狀態 |
| --- | --- | --- |
| Basic query/report | `src/analysis/BasicAnalysis.cpp` | 已完成 |
| Direct connectivity query/report | `src/analysis/ConnectivityAnalysis.cpp` | 已完成 |
| Function/SAT analysis 與 NAND pair search | `src/analysis/FunctionAnalysis.cpp` | 已完成；其他 arbitrary functional pair mode 尚未實作 |
| Sequential pattern query/report | `src/analysis/SequentialPatternAnalysis.cpp` | 第一版完成；AND-only semantics 待官方確認 |
| Transitive cone analysis | `src/analysis/ConeAnalysis.cpp` | 已完成 |
| `isEndpoint()` path helper | `src/analysis/PathAnalysis.cpp` | 已完成 |

`src/analysis/NetlistAnalysis.cpp` 已移除，避免再變成新的雜物檔。

## 4. 目前刻意保留不拆的部分

| 區塊 | 原因 |
| --- | --- |
| `src/core/Netlist.cpp` 的 `getLogicalWireCount()` / `getGateInfo()` | 雖然偏 report，但依賴 core structure，短期留在 core 較穩 |
| `src/optimization/NetlistOptimization.cpp` | cleanup pass 彼此互相呼叫，最佳化功能仍在成長，太早拆會增加維護成本 |
| `src/transformation/NetlistTransformation.cpp` | mutation API 目前還可控，等 buffer insertion / rewrite 變多再拆 |

## 5. 後續整理建議

下一階段不是再拆 analysis，而是先穩定 API 邊界：

```text
1. 確認 Netlist.h 宣告順序和這份 ownership 一致。
2. 補 mini test 覆蓋 Basic / Connectivity / Function / Cone / Path / Depth 高階 API。
3. 等 cleanup / transformation API 成熟後，再考慮拆 optimization source。
4. 若要外接 mockturtle / ABC，新增獨立 adapter，不要塞進 core Netlist。
```
