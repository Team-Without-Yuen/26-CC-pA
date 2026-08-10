# AIG / SAT Engine 合併前盤查

## 1. 來源與目前狀態

| 項目 | 內容 |
|---|---|
| 實作 commit | `5ac9b5fe9645248f38b49287efef4096610b8ac6` |
| commit message | `complete SAT engine of phase A` |
| 遠端狀態 | 已由 PR #69 合併至 `origin/main` (`becf97e`) |
| 本地 branch | `blup`，尚未含 PR #69 |
| 主要規模 | 25 files，約 3,532 insertions / 317 deletions |

這不是單一演算法檔，而是一個新的 Boolean engine 層。合併時應以 `origin/main` 為整體來源，
不能只挑 `src/SATEngine`，否則會漏掉 Netlist 版本管理、include relocation 與 build wiring。

## 2. 已提供的核心能力

| 元件 | 責任 |
|---|---|
| `AigModel` / `AigBuilder` | 由 current named netlist 建立衍生 AIG；DFF Q 作 pseudo-PI；undriven 可作 free PI |
| `NameMap` | named net 與 AIG signal 的雙向映射；允許多個具名 net 對應同一功能 signal |
| `SatEngine` | lazy cone encoding、incremental CaDiCaL、equivalence/constant/assumption query |
| `Fraig` | 同一顆 AIG 內的功能等價代表與學習 |
| `Primitives` | 高階 facade；resolve、cofactor、support、symmetry、cut/NPN、snapshot CEC |
| `Netlist dirty/revision` | mutation 後使 AIG cache 失效；下一次 Boolean query lazy rebuild |

架構方向符合「named netlist 是唯一正式設計狀態，AIG 是衍生 functional index」。AIG 不取代
gate/net 名稱、結構、depth 或 fanout query。

## 3. Function Search 串接程度

目前尚未串接。

`src/analysis/FunctionAnalysis.cpp` 在此 commit 只有下列變更：

```text
include/core/SatTime.h -> include/SATEngine/SatTime.h
```

因此現有 NAND pair search 與 equivalent gate-pair classification 仍走原本的 simulation +
per-query SAT 流程。`Primitives` 的實際使用入口目前位於 `main.cpp` 測試程式，不在
`tools.cpp` 或 Function Search public facade。

## 4. 合併風險

| ID | 風險 | 驗證方式 |
|---|---|---|
| AIG-MERGE-001 | 本地 `Netlist.h`、optimization、transformation、Makefile 已有其他修改，直接 merge 可能衝突或漏接 dirty marking | 逐檔解 conflict，禁止整檔採單邊版本 |
| AIG-MERGE-002 | `SatTime.h` 從 `include/core` 移至 `include/SATEngine`，所有 include 必須同步 | 全專案 `rg SatTime.h` + clean build |
| AIG-MERGE-003 | engine 正確性依賴每個 mutation 呼叫 `markDirty()`；任何直接寫 gates/nets 的路徑都可能留下 stale AIG | audit mutation sites + mutation/requery regression |
| AIG-MERGE-004 | commit Makefile 建置 `main.cpp`/`NetlistTool`，但交付 LLM 使用的是 `tools.cpp`/`tools.exe` | backend build 通過後再同步 `TOOLS_SPEC/Makefile` |
| AIG-MERGE-005 | `Primitives` 預設 Phase A；Phase B incremental SAT 需明確啟用與驗證 | 分別記錄 Phase A/Phase B 結果，不把 demo 成功視為已接高階 API |
| AIG-MERGE-006 | AIG build 對 unresolved/topo-dropped cone 的處理會影響 soundness | 已加入 `ModelHealth` 與 proof gate；test36 驗證 Invalid 只能 Unknown/拒絕 |
| AIG-MERGE-007 | working tree 目前有大量未提交 API、文件及 tools 修改 | merge 前建立明確 checkpoint，避免 stash/resolve 時遺失使用者變更 |

## 5. 建議合併順序

1. 在隔離 worktree 對 `origin/main` 做 clean build 與 AIG 自帶測試，先確認上游本身可編譯。
2. 保存目前 `blup` 的完整 checkpoint，記錄 tracked 與 untracked 範圍。
3. 將 `origin/main` merge 進 `blup`，逐檔處理 conflict。
4. 先驗證 named-netlist query/edit regressions，再驗證 AIG rebuild、stale signal rejection 與 CEC。
5. 重跑 `Blup/FUNCTION_SEARCH_BASELINE_BEFORE_AIG.md` 的四個固定 command。
6. 比較 match set、完整性與時間；先確認 merge 沒有改變既有 Function Search 語意。
7. 最後才設計 Function Search 使用 AIG 的 adapter；保持現有 query/report/CLI 參數不變。
8. backend 穩定後，才更新 `tools.cpp`、`TOOLS_SPEC/Makefile` 與工具文件。

### Backend ownership contract

每一份 current named `Netlist` 只允許由後端 analysis context 持有一個共用 `Primitives`。Function
Search、Function Analysis、Sequential Pattern、symmetry 與 CEC 等 Boolean 高階 API 必須
接收這個共用 instance，不得各自長期保存另一份 AIG context。工具呼叫維持單執行緒依序執行；
本規則是 cache ownership 與一致性要求，不是多執行緒同步機制。

`tools.cpp` 不直接擁有或操作 AIG；它仍呼叫原有高階 API。後續由各 API 的 backend 實作逐步
改用共用 `Primitives`，只有 public schema 或 tool envelope 改變時才需要同步 tools 層。

`restoreFrom()` 還原的是電路內容，但 rollback 本身仍是新的設計狀態，因此 restore 後必須
保持 revision 單調遞增並設為 dirty，強制下一個 Boolean query 重建 functional index。

## 6. Function Search AIG 化的初步邊界

第一階段只替換 Boolean proof/index backend：

- 候選 scope、gate type、active/tombstone 過濾仍由 named netlist 決定。
- net/gate 名稱與 report ID 一律由 named netlist/NameMap 回填。
- FindAny/FindAll、timeout、完整 artifact 與 report contract 不變。
- netlist mutation 只修改 named netlist；AIG 由 dirty/revision 失效後重建。
- unresolved、floating 或 unsupported cone 不得被默認成 constant 0 後宣告等價。

這樣可先取得 shared AIG/CNF 帶來的效能收益，又不會同時改 public semantics 與 LLM routing。

## 7. 隔離驗證結果

驗證 worktree：`C:/Users/user/Desktop/CAD/aig-origin-main-review`，detached at `becf97e`。

### 原始碼編譯

新增的五個實際 source 全部在 MSYS2 UCRT64 / g++ 下成功產生 object：

```text
src/SATEngine/AigBuilder.cpp
src/SATEngine/EngineStubs.cpp
src/SATEngine/NameMap.cpp
src/SATEngine/Primitives.cpp
src/SATEngine/Session.cpp
```

完整首次 build 在 10 分鐘外部上限內尚未完成 ABC，因此被測試 runner 終止；CaDiCaL library
已完成。ABC build 曾輸出 `arch_flags: cannot execute binary file: Exec format error`，但後續仍繼續
編譯。這是 external dependency clean-build 環境問題，不是上述 AIG source 的 compiler error，
但正式交付前仍需完成一次完整 clean link。

### 預編譯程式 smoke test

使用 commit 內的 `NetlistTool.exe` 與 `mini test/test12/tools_equiv_circuit.v`：

| 驗證 | 結果 |
|---|---|
| AIG / NameMap build | 5 gates、8 nets 全部 bound；soundness OK |
| structural equivalence classes | 回報 `{n0, n1, y}` |
| snapshot 後未修改 CEC | equivalent，2/2 comparison points |
| stale SigRef | mutation 後 generation 1 -> 2，正確丟出 `StaleSignal` |
| 等價修改 | 插入 NOT-NOT 並 reroute，rebuild 後 CEC equal |
| 非等價修改 | `g1` input `b -> a`，CEC not_equal，定位 `z` 並回 counterexample |

這些結果證明 engine 的基本生命週期與 Phase A golden CEC 可運作；尚不能證明所有既有 mutation
path 都有正確呼叫 `markDirty()`，也不能代表 Function Search 已完成 AIG 串接。

## 8. Model-health 安全化結果

本地合併後已補上 `Sound / Conservative / Invalid` 狀態，並修正 PO-only floating net 原本會
被綁成 constant 0 的問題。floating/undriven signal 以 free PI 保守建模；missing gate input、
DFF D、無法解析的必要 control、invalid output 與 topo-dropped gate 會使模型成為 Invalid。

Invalid model 的 checked proof API 固定回 `Unknown`；沒有三態回傳的 Boolean 操作會丟
`UnsoundModel`。`UnknownPolicy::AsEqual` 不得繞過此門檻。專用 regression：

```text
mini test/test36/test36.cpp
19 passed, 0 failed
```

合併後驗證：

| 驗證 | 結果 |
|---|---|
| MSYS2 UCRT64 全專案增量 compile/link | `NetlistTool.exe` 成功 |
| NetlistTool Phase A smoke | equivalence、snapshot、CEC 成功 |
| `mini test/test2` | 40 passed, 0 failed |
| Function Search test29 | 7 classes / 7 pairs，complete |
| Function Search test30 | 1 class / 1 pair，complete |
| Function Search test35 FindAny | witness `n26080`, `n6359`，complete |
| Function Search test35 FindAll | 512 pairs，complete，無 timeout/Unknown |

Windows 建置必須使用 MSYS2 UCRT64。直接以舊 `C:/MinGW/bin/g++` 連結 UCRT64 static library
會出現 `stat64i32` / `fstat64i32` ABI 錯誤，不能誤判為 AIG source failure。

這一層不改 tools grammar、report schema、Phase 切換或既有高階 API。
