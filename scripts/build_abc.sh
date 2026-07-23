#!/usr/bin/env bash
# =============================================================
#  build_abc.sh  -  依環境編譯 Berkeley ABC 成 libabc.a
#  由主 Makefile 呼叫: ENV=<UCRT64|MINGW64|LINUX> bash scripts/build_abc.sh <ABC_DIR>
# =============================================================
set -euo pipefail

ABC_DIR="${1:?usage: build_abc.sh <ABC_DIR>}"
ENV="${ENV:-LINUX}"          # 由 Makefile 傳入,預設 LINUX

cd "$ABC_DIR"

echo "[ABC] Environment = $ENV"

if [ "$ENV" = "LINUX" ]; then
    # ---- Linux:ABC 原生支援,直接編 ----
    #   ABC_USE_NO_READLINE=1 : 不依賴 readline,與 Windows 端保持一致,
    #   也避免未裝 libreadline-dev 的機器編譯失敗。
    #   (若團隊需要 ABC 的互動式 shell,改裝 libreadline-dev 並移除此旗標)
    make -j4 libabc.a ABC_USE_NO_READLINE=1
else
    # ---- Windows (UCRT64 / MINGW64):套用相容性旗標 ----
    #   -DABC_USE_STDINT_H : 讓 ABC_PTRINT_T 用 intptr_t (修 LLP64 下 long=4 的 precision 錯誤)
    #   -DNUNLOCKED        : 內建 cadical 改用普通 putc/getc (Windows 無 *_unlocked)
    #   ABC_USE_NO_READLINE=1 : 不連 readline
    ABC_OPTFLAGS="-DABC_USE_STDINT_H -DNUNLOCKED -O3 -fpermissive -Wno-int-to-pointer-cast"

    # 先編所有 .o (最後的 ar 打包步驟預期會因 arg 太長而失敗,故容許此步非零結束)
    make -j4 libabc.a \
        ABC_USE_NO_READLINE=1 \
        OPTFLAGS="$ABC_OPTFLAGS" || true

    # ---- 用 xargs ar 分批打包,繞過 Windows 命令列長度限制 ----
    if [ ! -f libabc.a ] || [ -n "$(find src -name '*.o' -newer libabc.a -print -quit 2>/dev/null)" ]; then
        echo "[ABC] Packing libabc.a via xargs ar (Windows arg-length workaround)..."
        rm -f libabc.a
        find . -name '*.o' | xargs ar rcs libabc.a
    fi
fi

# ---- 驗證產出 ----
if [ -f libabc.a ]; then
    echo "[ABC] OK: $(ls -la libabc.a | awk '{print $5, $NF}')"
else
    echo "[ABC] ERROR: libabc.a not produced" >&2
    exit 1
fi
