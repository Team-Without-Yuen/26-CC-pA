# ICCAD 2026 Problem A

本 repository 實作 gate-level Verilog netlist 的查詢、分析、修改、等價驗證與最佳化工具，
並透過 `tools.cpp` 提供 LLM-facing command interface。

## Testcases

```text
testcase/testNN/
  testNN.v
  prompt.txt

NewTestCase/testNN/
  testNN.v
  prompt.txt
```

- `testcase/test01` 到 `test40`：早期公開測資快照。
- `NewTestCase/test01` 到 `test91`：目前 repository 內的擴充測資集合。
- `prompt.txt` 每一行是一個獨立的自然語言 request。

## Repository Layout

| Path | Responsibility |
|---|---|
| `include/`, `src/` | Netlist、分析、transformation、SAT/AIG 與 optimizer 實作 |
| `API_SPEC/` | C++ 高階 API contract、usage 與 backend policy |
| `TOOLS_SPEC/` | `tools.cpp/tools.exe` 的 LLM-facing command contract |
| `PROJECT_SPEC/` | 跨模組架構、ownership 與官方 Q&A 設計影響 |
| `mini test/`, `Testing/` | focused regression 與執行證據 |

## Build Tools

在已安裝專案依賴的 MSYS2 UCRT64/MINGW64 環境中執行：

```bash
make -f TOOLS_SPEC/Makefile
```

產物為 repository root 的 `tools.exe`。command routing、參數與輸出契約以
[`TOOLS_SPEC/README.md`](TOOLS_SPEC/README.md) 為入口；C++ API 使用方式則從
[`API_SPEC/ANALYSIS_API_INVENTORY.md`](API_SPEC/ANALYSIS_API_INVENTORY.md) 開始。
編譯耗時、第三方 warning、shell quoting 與 tester API 漂移的判讀方式見
[`PROJECT_SPEC/BUILD_TROUBLESHOOTING.md`](PROJECT_SPEC/BUILD_TROUBLESHOOTING.md)。

## Evaluation Interface

Prompt 由 contestant agent 逐題讀取；agent 透過工具取得結果，再依題目要求輸出自然語言
答案或修改後的 Verilog。read/write 與分析/修改 request 採不同時間預算，現行 policy 見
[`API_SPEC/REQUEST_TIME_BUDGET_POLICY.md`](API_SPEC/REQUEST_TIME_BUDGET_POLICY.md)。
