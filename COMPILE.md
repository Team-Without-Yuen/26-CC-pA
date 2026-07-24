# NetlistTool

網表處理工具,整合 [Berkeley ABC](https://github.com/berkeley-abc/abc) 與 [CaDiCaL](https://github.com/arminbiere/cadical) 兩套外部函式庫。

支援環境:**Linux** / **MSYS2 UCRT64** / **MSYS2 MINGW64**,各環境產生對應的原生執行檔。

---

## 目錄結構

```
.
├── Makefile
├── scripts/
│   └── build_abc.sh          # ABC 的編譯邏輯 (由 Makefile 呼叫)
├── main.cpp
├── src/                      # 主程式原始碼
│   ├── core/  io/  analysis/  optimization/  transformation/
└── include/
    └── lib/
        ├── abc/              # Berkeley ABC 原始碼 (含已修正的相容性補丁)
        ├── cadical/          # CaDiCaL 原始碼
        ├── abcsat/  abcesop/  nauty/ ...
```

> **注意**:`include/lib/abc` 內含針對 Windows/MinGW 的相容性修正 (見文末「維護筆記」),請勿直接用上游原版覆蓋。

---

## 建置方式

編好的 `.a` / `.o` **不進版本控制**,每個人在自己的環境第一次建置時會自動編出外部函式庫。

目前有兩個不同入口：

| 入口 | 用途 | 建置檔 |
|------|------|--------|
| `main.cpp` → `NetlistTool.exe` | critical-path optimizer 的整合測試程式 | 根目錄 `Makefile` |
| `tools.cpp` → `tools.exe` | 給 LLM 使用的 public command CLI | `TOOLS_SPEC/Makefile` |

兩者共用相同的 `src/`、ABC 與 CaDiCaL library，但不可把
`NetlistTool.exe` 的實驗性 optimizer 參數當成正式 tools schema。

### 一般流程

在 **對應環境的終端機** (Linux shell、或 MSYS2 的 **UCRT64** / **MINGW64** 視窗) 進到專案根目錄:

```bash
make          # 首次會自動編 CaDiCaL + ABC,再編主程式
```

若要建置 LLM-facing tools CLI：

```bash
make -f TOOLS_SPEC/Makefile
```

這個 target 會共用相同的 ABC/CaDiCaL build，輸出 `tools.exe`。

首次建置會較久 (ABC 有一千多個檔案)。編完得到:

- Linux → `NetlistTool`
- Windows → `NetlistTool.exe`

### 常用指令

| 指令 | 作用 |
|------|------|
| `make` | 建置主程式 (需要時自動編外部庫) |
| `make libs` | 只 (重) 建 `libcadical.a` 與 `libabc.a` |
| `make clean` | 清掉主程式的 `.o` / `.d` 與執行檔 |
| `make clean_all` | 連同 ABC、CaDiCaL 的編譯結果一起清除 |

### Tools Regression

在 PowerShell 7 從專案根目錄執行：

```powershell
.\scripts\run_tools_regression.ps1 -Profile Quick
.\scripts\run_tools_regression.ps1 -Profile Tools
.\scripts\run_tools_regression.ps1 -Profile Full
```

| Profile | 內容 |
|---|---|
| `Quick` | 使用現有 `tools.exe` 跑 test32、diff 與文件檢查 |
| `Tools` | 重新建置 `tools.exe`，跑 test9、test21、test27、test32 |
| `Full` | 跑全部 PowerShell CLI regressions，並重新編譯/執行 C++ test31 |

完整 stdout/stderr 存在 `Testing/tools-regression/<timestamp>/`；該目錄已由
`.gitignore` 排除。終端只顯示 PASS/FAIL、耗時與失敗步驟最後 40 行。

官方 bounded optimization smoke 是可選項，不會預設加入 `Full`：

```powershell
.\scripts\run_tools_regression.ps1 -Profile Quick `
    -OfficialOptimizationSmoke -OfficialCases 40 `
    -OptimizationTimeLimitSeconds 30 -OfficialCaseTimeoutSeconds 120
```

`OptimizationTimeLimitSeconds` 是傳給 high-level API 的預算；
`OfficialCaseTimeoutSeconds` 是 runner 的 process-level hard timeout，可在
mockturtle 尚未支援 cooperative cancellation 時避免整個測試無限卡住。

### 環境偵測

Makefile 以 `MSYSTEM` 變數判斷環境 (UCRT64 / MINGW64 會設定它,Linux 不會),並自動套用對應的:

- 平台巨集:Windows 用 `-DWIN64`,Linux 用 `-DLIN64` (兩者互斥)
- 系統庫:Linux 額外連 `-ldl`
- ABC 相容性旗標:僅在 Windows 套用 (見下)

執行 `make` 時開頭會印出 `[INFO] Detected Environment: <ENV>`,可據此確認判斷正確。

---

## 疑難排解

| 症狀 | 可能原因與處理 |
|------|----------------|
| `make` 開頭印出的環境不對 (例如在 MSYS2 卻顯示 LINUX) | 開錯終端機。Windows 請用 **MSYS2 UCRT64** 或 **MINGW64** 視窗,不要用藍色的 **MSYS** 視窗。可先 `echo $MSYSTEM` 確認為 `UCRT64` 或 `MINGW64`。 |
| CaDiCaL 卡在 `sys/mman.h: No such file` | 那是在編 `mobical` (測試工具,非函式庫)。本專案只編 `libcadical.a` 已跳過它;若手動編請 `cd build && make libcadical.a`,不要裸 `make`。 |
| ABC 出現 `loses precision` / `getc_unlocked` / `sys/wait.h` / `LONG` 等錯誤 | 這些是 Windows 相容性問題,已由 `scripts/build_abc.sh` 的旗標 + 原始碼補丁處理。若重現,代表 `include/lib/abc` 被上游原版覆蓋了,請還原補丁 (見「維護筆記」)。 |
| ABC 連結時 `ar: Argument list too long` | Windows 命令列長度限制。`build_abc.sh` 已改用 `xargs ar` 分批打包繞過;若你手動編到此步,執行 `find . -name '*.o' \| xargs ar rcs libabc.a`。 |
| 連結主程式出現 ABC/CaDiCaL 的 `undefined reference` | (1) 確認 ABC 標頭有用 `extern "C"` 包住;(2) 若仍失敗,試著把 Makefile `LDFLAGS` 裡 `$(ABC_LIB)` 與 `$(CADICAL_LIB)` 的順序對調。 |

---

## 維護筆記 (給之後更新 ABC/CaDiCaL 版本的人)

`include/lib/abc` 內含以下針對 Windows/MinGW 的原始碼修正。**若日後更新 ABC 上游版本,需重新套用**:

1. **內建 CaDiCaL 的平台判斷** — `src/sat/cadical/` 下多個檔案 (如 `file.hpp`、`cadical_file.cpp`、`cadical_resources.cpp`) 原本用 `#ifdef WIN32` (無底線),MinGW 不會定義它。已改為 `#ifdef _WIN32` (MinGW 內建),使其正確走 Windows 分支。

2. **`utilPth.c` 的 atomic 守衛** — `src/misc/util/utilPth.c` 原本的 `#ifndef _WIN32` 會讓 MinGW 誤走 MSVC 的 `Interlocked`/`LONG` 分支 (但又因 `__MINGW32__` 而未 include `windows.h`)。已改為 `#if !defined(_WIN32) || defined(__MINGW32__)`,讓 MinGW 走標準 `<stdatomic.h>`。

ABC 的 Windows 編譯旗標 (`-DABC_USE_STDINT_H -DNUNLOCKED`、`ABC_USE_NO_READLINE=1`、`xargs ar` 打包) 統一放在 `scripts/build_abc.sh`,且僅在非 Linux 環境套用。Linux 直接 `make libabc.a` 即可。
