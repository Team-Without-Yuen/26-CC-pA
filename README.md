# ICCAD 2026 Problem A

本 repository 實作 gate-level Verilog netlist 的查詢、分析、修改、等價驗證與最佳化工具，
並透過 `tools.cpp` 提供 LLM-facing command interface。

## Testcases

```text
testcase/testNN/
  testNN.v
  prompt.txt
```

- `prompt.txt` 每一行是一個獨立的自然語言 request。

## Repository Layout

| Path | Responsibility |
|---|---|
| `include/`, `src/` | Netlist、分析、transformation、SAT/AIG 與 optimizer 實作 |
| `API_SPEC/` | C++ 高階 API contract、usage 與 backend policy |
| `TOOLS_SPEC/` | `tools.cpp/tools.exe` 的 LLM-facing command contract |
| `mini test/`, `Testing/` | focused regression 與執行證據 |

## Evaluation Interface

Prompt 由 contestant agent 逐題讀取；agent 透過工具取得結果，再依題目要求輸出自然語言
答案或修改後的 Verilog。read/write 與分析/修改 request 採不同時間預算，現行 policy 見
[`API_SPEC/REQUEST_TIME_BUDGET_POLICY.md`](API_SPEC/REQUEST_TIME_BUDGET_POLICY.md)。
