# Build Troubleshooting Guide

Status: Active

Owner: Build and regression workflow

Source of truth:

- `Makefile`
- `TOOLS_SPEC/Makefile`
- `scripts/run_tools_regression.ps1`
- 實際 MSYS2 UCRT64 編譯結果

Update when:

- build entry、compiler environment、dependency library 或 executable 名稱改變
- 出現已確認且可重現的新編譯/連結問題
- focused test 的編譯方式或維護狀態改變

本文件記錄已確認的編譯問題與判讀方式。先查本文件，再決定是否修改原始碼；
大量 warning、測試程式過期與 production build failure 是三種不同問題，不可混為一談。

## 1. 正式編譯入口

### 1.1 LLM-facing `tools.exe`

在 repository root 的 MSYS2 UCRT64 shell 執行：

```bash
make -f TOOLS_SPEC/Makefile
```

成功標記：

```text
[SUCCESS] Built TOOLS_SPEC/../tools.exe
```

`TOOLS_SPEC/Makefile` 在需要重建時會用單一 compiler command 編譯 production sources 與
ABC/CaDiCaL 相關來源，因此輸出很多、耗時也比一般 incremental object build 長。只要程序仍在執行，
不可因暫時沒有新輸出就啟動第二份 build。

從 PowerShell 明確呼叫 MSYS2 時，可使用：

```powershell
& 'C:\msys64\usr\bin\bash.exe' -lc 'export MSYSTEM=UCRT64; export PATH=/ucrt64/bin:/usr/bin; cd /c/Users/user/Desktop/CAD/2026-CAD-contest-problem-A; make -f TOOLS_SPEC/Makefile'
```

外層 PowerShell 建議使用 single-quoted Bash script，避免 `$()`、wildcard 或反斜線先被
PowerShell 解讀。

### 1.2 Root `Makefile`

```bash
make
```

此入口使用 per-source object incremental build，產物是 `NetlistTool.exe`，不是交給 LLM 的
`tools.exe`。修改 `tools.cpp` 或 TOOLS_SPEC 後，交付前仍應使用 `TOOLS_SPEC/Makefile` 重建
`tools.exe`。

## 2. 本輪已確認的非致命警告

完整 `tools.exe` build 可能出現下列第三方 warning：

```text
ABC_ALLOC / ABC_CALLOC / ABC_FALLOC / ABC_REALLOC redefined
cast from void* ... loses precision
nauty setword conversion changes value / overflow
mockturtle resyn engine: no return statement
GHack clauses.size() value computed is not used
```

這些 warning 來自 bundled ABC、abcesop、bill、nauty、mockturtle 等第三方 headers。
本輪在這些 warning 存在時仍得到 exit code 0、成功產生新版 `tools.exe`，並通過 Basic regression。

判讀規則：

1. 以 process exit code 與 `[SUCCESS] Built .../tools.exe` 判斷 build 成敗。
2. warning 不可直接當成此次修改造成的 failure。
3. 若出現 `error:`、`undefined reference`、nonzero exit code 或沒有新 binary，才進一步定位。
4. 若 warning 的來源或內容改變，仍需重新核對，不能永久忽略所有 warning。

## 3. 看似卡住的完整編譯

### 現象

- 長時間持續輸出第三方 warning。
- 某段時間沒有新 terminal output。
- wrapper 回報仍有 session/process ID，而沒有 exit code。

### 原因

`TOOLS_SPEC/Makefile` 的 production build 是大型單次 compiler invocation。Mockturtle、ABC、SAT
相關 template/header 編譯會花較長時間，warning 也可能被 terminal 截斷。

### 處理

1. 先確認原 build process 是否仍在執行。
2. 持續 poll 同一個 process，直到取得 exit code。
3. 不要同時重開第二份完整 build，以免 CPU/RAM 競爭讓兩份都更慢。
4. 若確定超出合理時間且 process 無活動，再中止並保存最後一段輸出。
5. 中止後先查最後一個 source/error，不要立刻 clean rebuild。

Codex/sandbox 或 terminal wrapper 若出現 signal/pipe interruption，不等於 compiler error；應先確認
底層 process 是否仍存在。確定程序已終止後，再用單一 MSYS2 shell 重跑原命令。

## 4. PowerShell 與 Bash 的 `find` 衝突

### 已觀察訊息

```text
FIND: Parameter format not correct
```

### 原因

在 PowerShell 的 double-quoted `bash -lc "..."` 內容中使用 `$(find ...)` 時，`$()` 可能先被
PowerShell 展開，或解析到 Windows `find.exe`，導致 Bash 收到錯誤/空的 object list。

若後續 linker 同時回報：

```text
group ended before it began
```

通常是前面的 object-list command 已失敗，使連結參數順序被破壞，不代表 ABC/CaDiCaL library
本身損壞。

### 處理

- 優先使用既有 Makefile，不手寫 `find` 組裝 object list。
- 必須呼叫 Bash 時，讓完整 Bash script 位於 PowerShell single-quoted string。
- 先單獨印出 object list，再執行 linker；不可在 object discovery 失敗後繼續連結。

## 5. 舊 `mini test/test1/tester.cpp` API 漂移

本輪嘗試獨立編譯 `mini test/test1/tester.cpp` 時，確認存在與 Basic Query 修改無關的既有錯誤：

```text
runLocalSimplificationFixpointWithReport() now requires RequestDeadline*
trimDeadLogicWithReport() no longer exists
removeDanglingLogicWithReport() no longer exists
```

這代表舊 tester 的 Edit/Cleanup calls 尚未同步目前 `Netlist` API，不代表 production
`tools.exe` 編譯失敗，也不能拿這份 tester 的 compile failure 判定 Basic Query regression 失敗。

目前處理方式：

- Basic Query 使用可執行的 `mini test/test50/test50.ps1` 做 focused regression。
- 再使用受影響的官方 `NewTestCase` 做 CLI smoke。
- 若未來要恢復 test1，應建立獨立 maintenance task，逐一對照目前 Edit API contract；不要為了
  通過其他 feature 的測試而臨時猜測 replacement wrapper。

## 6. 建議驗證順序

```text
1. make -f TOOLS_SPEC/Makefile
2. 確認 exit code 0 與新版 tools.exe timestamp
3. 執行受影響的 focused mini test
4. 執行一個對應官方 NewTestCase smoke
5. git diff --check
```

Basic Query 範例：

```powershell
& '.\mini test\test50\test50.ps1'

$commands = "read NewTestCase/test91/test91.v`nstructure_query gate_info g0`nexit`n"
$commands | .\tools.exe
```

不要只驗證 executable 能啟動；至少要核對 report 的關鍵欄位、`complete`、大型結果 artifact
與此次修改相關的 edge case。

## 7. 快速判讀表

| 現象 | 類別 | 下一步 |
|---|---|---|
| 大量 ABC/nauty/mockturtle warning，最後 exit 0 | 已知第三方 warning | 跑 regression，不改 production code |
| process 尚有 session ID，沒有 exit code | build 未結束 | poll 原 process，不重開 build |
| `FIND: Parameter format not correct` | shell quoting/tool resolution | 改用 Makefile 或 single-quoted Bash script |
| `group ended before it began` 緊接在 find failure 後 | object list/連結命令損壞 | 先修 object discovery，不重建第三方庫 |
| `test1/tester.cpp` 缺 cleanup wrapper | tester API 漂移 | 不當成 feature build failure；另案同步 tester |
| `tools.exe` timestamp 早於修改 source | binary 過期 | 用 `TOOLS_SPEC/Makefile` 重建 |
| nonzero exit code 且有 source `error:` | 真正編譯失敗 | 從第一個 error 開始定位 |
