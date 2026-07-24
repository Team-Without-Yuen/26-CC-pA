# ICCAD Problem A QA 注意事項整理

來源文件：

```text
2026-CAD-contest-problem-A/A_QA_20260615.pdf
```

這份文件整理新版 Q&A 中對目前 backend / parser / analysis / transformation / evaluation environment 會造成影響的重點。

## 1. Verilog 輸入規則

官方再次確認：

```text
- Verilog syntax 會遵守 Problem Statement Section 3.2。
- final evaluation 的 Verilog 語法不會超出題目定義。
- test benchmarks 的 gate instances 都會有 explicit instance names。
- 只會使用 built-in primitive keywords：
  and, or, nand, nor, not, buf, xor, xnor, dff
- 不會出現 NAND2X1 / AOI22X1 這類 technology library cell name。
```

目前 parser 可以繼續以 restricted flattened gate-level Verilog 為核心，不需要支援一般 RTL。

可繼續不支援：

```text
assign
always
initial
case
if / else
for / generate
behavioral reg semantics
complex expression assignment
hierarchical module instantiation
technology library cell
unnamed primitive instance
```

## 2. DFF 規則

新版 QA 最重要的一點是：testcase 裡的 DFF 形式才是正確的。

正確 testcase DFF 形式：

```verilog
dff g10537(.RN(n1), .SN(1'b1), .CK(n0), .D(n49[2]), .Q(n194[2]));
```

注意：

```text
- dff 會作為 primitive gate 出現在 input Verilog。
- 不會另外提供 RTL module definition。
- .SN 是 active-low set signal。
- .RN 是 active-low reset signal。
- .CK 是 clock。
- .D 是 data input。
- .Q 是 output。
```

對 parser 的影響：

```text
- 必須支援 DFF named port connection。
- DFF inputPinNames 與 inputNetIds 的順序必須保持對齊。
- disconnect / reconnect 時不能 erase DFF input vector 中間元素，應保留 -1，避免 pin 對應錯位。
```

對 analysis 的影響：

```text
- DFF 是 sequential boundary。
- combinational path / cone / depth 分析不穿越 DFF。
- DFF.Q output 應視為 pseudo primary input。
- DFF.D input 可以作為 timing endpoint。
- DFF clock/reset/set pin 也可能出現在 fanout 類問題。
```

## 3. Constant 的定義

QA 明確說明：

```text
A signal is functionally constant if it provably remains 0 or 1 for all possible inputs.
```

因此 constant 有兩種層級：

| 類型 | 意義 | 目前對應功能 |
| --- | --- | --- |
| Structural constant | netlist 中直接接 `1'b0` / `1'b1` | parser / direct structural query |
| Functional constant | 透過邏輯推導或 SAT 證明永遠為 0/1 | function analysis / SAT query |

對 prompt 解讀的影響：

```text
- "input tied to constant 1" 通常偏 structural constant。
- "output always 0 regardless of all inputs" 是 functional constant。
- "constant propagation" 類 transformation 可能先從 structural constant 做起。
- 若 prompt 沒明確說 structural，analysis 回答時要注意是否需要 functional proof。
```

DFF 相關補充：

```text
- DFF initial state is 0。
- X values are ignored。
```

## 4. Fanout Load 的定義

fanout load 要包含所有 sink pin / output connection。

官方確認應列入 fanout load 的項目：

```text
- primitive gate input pins
- DFF D pins
- DFF clock pins
- DFF reset/set pins，例如 RN / SN
- primary output connections
```

對 fanout query / fanout optimization 的影響：

```text
- 計算 max fanout 時不能只算 combinational gate input。
- clock / reset / set net 如果驅動大量 DFF pin，也算 high fanout。
- 如果 prompt 只要求 optimize reset 或 clock，才只針對特定 signal 處理。
- buffer insertion report 應明確列出新增 BUF 與被重新驅動的 loads。
```

## 5. Path Enumeration 規則

官方明確說：

```text
Complete enumeration means list every path literally.
```

因此：

```text
- 如果 prompt 要 complete enumeration，就不能只回 path count 或 sample。
- 若結果非常大，應完整寫入檔案，回答中提供 output file path。
- QA 特別提到 testcase14 可能有 289,366 paths，literal listing 會非常大。
```

對 implementation 的建議：

```text
- path_query enumerate 需要支援寫檔模式。
- 工具回覆可包含：
  1. total path count
  2. output file path
  3. 若檔案很大，不要把全部內容塞進 LLM response
- 內部 traversal 要注意 memory blow-up，最好串流寫出。
```

## 6. Depth / Fanin Cone 跨 DFF 規則

官方確認：

```text
Combinational depth only; treat DFF.Q outputs as primary inputs.
```

因此：

```text
- fanin cone depth 不穿越 DFF。
- register-to-register path 應視為 DFF.Q -> combinational logic -> DFF.D。
- PI-to-DFF.D、DFF.Q-to-PO、DFF.Q-to-DFF.D 都屬於 combinational boundary 之間的分析。
- 不需要 multi-cycle sequential traversal。
- 不需要處理 feedback loop 的 sequential depth。
```

## 7. Functional Equivalence 規則

官方確認：

```text
We only consider combinational equivalence.
```

對有 DFF 的 design：

```text
- 不需要 sequential equivalence checking。
- DFF.Q 可視為 pseudo primary input。
- equivalence 可以比較：
  - PI-to-PO combinational behavior
  - PI-to-DFF.D
  - DFF.Q-to-PO
  - DFF.Q-to-DFF.D
```

對 transformation 的影響：

```text
- rewrite / buffer insertion / tech mapping 後，主要需要保證 combinational equivalence。
- 若 prompt 沒有額外 scoring metric，functional equivalence 是 primary judging metric。
- gate count / depth / fanout 等 secondary metrics 只在 prompt 明確指定時作為 cost。
```

## 8. Transformation / Optimization Judging

官方確認：

```text
- Functional equivalence is the primary judging metric.
- Secondary metrics will be explicitly specified in the prompt.
- Cost will be defined inside the prompt.
- 若多種 legal rewrite 都可行，prompt 會說明優先 criteria。
```

新版 testcase 已經把 cost function 寫進 prompt，例如：

```text
The cost function is the maximum logic depth of the final design; smaller is better.
The cost function is the total gate count of the final design; smaller is better.
The cost function is the depth of the cone of n8; smaller is better.
```

對系統設計的影響：

```text
- transformation flow 應保留 original snapshot。
- 修改後應做 validation / equivalence check。
- 需要統一 edit / optimization report，記錄：
  - changed objects
  - added gates/nets
  - removed gates/nets
  - final gate count
  - final max depth / cone depth / fanout
  - whether equivalence passed
- 若無法優化或已最佳，可照 prompt 回報 original / unchanged。
```

## 9. Hidden Prompt 範圍

官方說 hidden prompts 不會只限於 document example API，但仍在同一大類 EDA task 內。

明確不會出現：

```text
- timing analysis with annotated delays
- placement / routing constraints
```

可能仍會出現的大類：

```text
- basic design loading / writing
- structural reporting
- path / fanin / fanout / cone analysis
- Boolean / equivalence analysis
- netlist transformation
- optimization under explicitly stated constraints
- DFF / sequential-boundary structural reporting
```

## 10. Output / Judging 方式

官方確認：

```text
- Evaluation uses LLM-as-a-judge.
- Based on semantic equivalence, not exact string match.
- Answers do not need exact wording.
- If prompt does not request examples, 不需要額外提供 example case。
```

Verilog output：

```text
- output Verilog 不需要 byte-for-byte identical。
- textually different but functionally equivalent output is acceptable。
- gate / wire declaration order、formatting、indentation 可不同。
```

大型輸出：

```text
- full enumerated lists are expected。
- large result sets should be written to file, then response provides file path。
```

## 11. Execution Environment

重要限制：

```text
- Docker image submissions are not accepted。
- Evaluation team 會直接執行 submitted program。
- 參賽者不用自己在 evaluation 端執行。
- General internet access 不提供。
- Internet access only for model API endpoints。
- 可以 bundle third-party binaries，沒有 size limit。
- open-source / permissive license 沒有額外限制。
```

API host：

```text
OpenAI host: https://api.openai.com
Anthropic host: https://api.anthropic.com
```

API key / config：

```text
- development 使用自己的 API key。
- evaluation 時官方會把 config file 裡的 api_key 換成官方 key。
- API key 是寫在 configuration file 中，而不是只能依賴 environment variable。
```

Working directory：

```text
- 更新後 QA 說 Docker 不支援。
- 仍需依 prompt 給定 path 讀 input Verilog。
- output files 應寫到同一 testcase directory。
```

## 12. Concurrency / Runtime 限制

官方確認：

```text
- Only single-threaded execution is permitted。
- Only one LLM request at a time。
- 不允許 concurrent / asynchronous LLM calls。
- 不允許 multi-threaded / multi-process worker pool。
```

Runtime：

```text
- basic operation request: 60 seconds。
- all other requests: 300 seconds。
- no token limit。
- rate limits follow GPT-4o-mini / Claude Haiku policies。
```

對 implementation 的影響：

```text
- 不要設計 parallel SAT worker。
- 不要平行呼叫 LLM。
- 大型 path enumeration / SAT / equivalence 要有 timeout-aware fallback。
- 需要用 rule-based routing 減少 LLM calls。
```

## 13. Model / API

官方提供 / 使用模型：

```text
- GPT-4o mini
- Claude 4.5 Haiku
```

評分方式：

```text
- each LLM is considered as a separate case。
- 若兩者都 deprecated，才會換模型。
- development 階段用自己的 API key。
- evaluation 階段官方替換 API key。
```

MCP：

```text
- MCP is supported。
- MCP server must reside on local machine。
- Environment is offline, so package installation cannot rely on internet。
- IPC via stdio / local pipes is acceptable。
```

## 14. 對目前專案的直接 TODO

根據新版 QA / testcase，建議優先補：

```text
1. main.cpp 更新成 TechMapReport 版本，避免主程式編譯失敗。
2. complete path enumeration 寫檔模式。
3. transformation / optimization 統一 report。
4. original loaded netlist snapshot + current-vs-original equivalence check。
5. fanout count API 明確包含 DFF D/CK/RN/SN 與 PO load。
6. DFF clock/reset driven flip-flop query。
7. register-to-register path listing。
8. Boolean equation / expression export。
9. symmetry check API。
10. cut / articulation point API。
11. enable / hold structure detection。
12. tech mapping 後 final count / added / removed 統計文件化。
```

## 15. 目前 parser / testcase 快速確認

新版 40 個 testcase 的 Verilog 快速掃描結果：

```text
- 40 個 testcase 都能被目前 tools.exe read 成功。
- 沒有看到 assign / always / initial / generate / case / for。
- test21-test40 大量出現 DFF named-port instances。
- 最大 testcase 約超過 100k gates，需注意記憶體與時間。
```

特別大的 testcase：

```text
test39: 約 112k gates / 16k DFF
test12: 約 91k gates
test33: 約 64k gates
test37: 約 46k gates
test35: 約 34k gates
```

## 16. 最新官方回覆：Depth、Hidden Structure 與修改等價性

官方確認：

```text
1. NOT 與 BUF 和其他 gates 一樣，各計一個 logic level。
2. final evaluation 會使用未公開的 hidden netlist 與 prompts；實作需要處理公開測資未出現的結構。
3. transformation 與 optimization prompt 都必須維持 functional equivalence。
```

對目前設計的影響：

```text
- DepthQuery 必須把 NOT/BUF 都算一層，不能把它們視為零延遲 wrapper。
- 不能只靠 released testcase 中出現的 canonical MUX 外觀；Sequential Pattern C++ API 因此加入受限的 functional cofactor fallback。
- DFF primitive 與 named pins 維持題目固定格式；可變部分是 D-input 前的組合實作。
- low-level rewiring 不應直接暴露成預設 edit 行為；公開 transformation 必須保留 equivalence certificate 或 whole-design validation/rollback。
```

仍需注意：官方回答 hidden case 可能有 MUX-based structure，不等於已明確允許新的 native `mux` primitive syntax。parser 是否需要接受 native MUX gate，仍應和「只允許既定 primitive」的舊規格分開確認；目前 functional matcher 針對既有 primitive 組成的任意等價結構。
