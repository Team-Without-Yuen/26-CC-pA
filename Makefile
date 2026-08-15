# =============================================================
#  NetlistTool - Main Makefile
#  Supported environments: Linux / MSYS2 UCRT64 / MSYS2 MINGW64
# =============================================================

CXX = g++
CC  = gcc
AR  = ar

# ---- 外部原始碼路徑 ----
CADICAL_DIR = include/lib/cadical
ABC_DIR     = include/lib/abc

TARGET = tools

# =============================================================
# 1. 環境偵測
#    產生三種身分之一: UCRT64 / MINGW64 / LINUX
# =============================================================
ifeq ($(MSYSTEM),UCRT64)
	ENV        := UCRT64
	IS_WINDOWS := 1
else ifeq ($(MSYSTEM),MINGW64)
	ENV        := MINGW64
	IS_WINDOWS := 1
else
	ENV        := LINUX
	IS_WINDOWS := 0
endif

$(info [INFO] Detected Environment: $(ENV))

# ---- 依環境決定平台巨集、執行檔名、系統庫 ----
ifeq ($(IS_WINDOWS),1)
	PLATFORM_DEF = -DWIN64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX
	TARGET_BIN   = $(TARGET).exe
	SYS_LIBS     = -lpthread -lm
else
	PLATFORM_DEF = -DLIN64
	TARGET_BIN   = $(TARGET)
	SYS_LIBS     = -lpthread -lm -ldl
endif

# =============================================================
# 2. 編譯參數
# =============================================================
COMMON_WARN = -Wno-unknown-pragmas -Wno-narrowing -Wno-sign-compare \
			  -Wno-unused-parameter -Wno-misleading-indentation \
			  -Wno-unused-variable -Wno-unused-but-set-variable \
			  -Wno-format -Wno-dangling-else -Wno-class-memaccess \
			  -Wno-int-to-pointer-cast -Wno-overflow

# 在這裡新增了 -Iinclude/SATEngine
COMMON_INC  = -I. -Iinclude -Iinclude/SATEngine -Iinclude/lib -Iinclude/lib/nauty \
			  -Iinclude/lib/abcsat -Iinclude/lib/abcesop \
			  -I$(CADICAL_DIR)/src -I$(ABC_DIR)/src

# ABC 標頭需要的巨集,必須與編 libabc.a 時 (scripts/build_abc.sh) 一致
# 另:FMT_USE_WINDOWS_H=0 阻止 fmt 引入 <windows.h>,避免其 rpcndr.h 的
#    'typedef unsigned char boolean' 與 nauty 的 'typedef int boolean' 衝突。
ifeq ($(IS_WINDOWS),1)
	ABC_DEFS = -DABC_USE_STDINT_H -DNUNLOCKED -DFMT_USE_WINDOWS_H=0
else
	ABC_DEFS =
endif

CXXFLAGS = -std=c++20 -O3 -Wall $(PLATFORM_DEF) $(ABC_DEFS) -DFMT_HEADER_ONLY \
		   -fpermissive $(COMMON_WARN) $(COMMON_INC)

CFLAGS   = -O3 -Wall $(PLATFORM_DEF) $(ABC_DEFS) \
		   -Wno-narrowing -Wno-int-to-pointer-cast -Wno-overflow \
		   $(COMMON_INC)

# =============================================================
# 3. 原始碼與目的檔
# =============================================================
# 在這裡新增了 $(wildcard src/SATEngine/*.cpp)
SRCS_CPP = tools.cpp \
		   $(wildcard src/core/*.cpp) \
		   $(wildcard src/io/*.cpp) \
		   $(wildcard src/analysis/*.cpp) \
		   $(wildcard src/optimization/*.cpp) \
		   $(wildcard src/transformation/*.cpp) \
		   $(wildcard src/SATEngine/*.cpp) \
		   $(wildcard include/lib/abcsat/*.cpp) \
		   $(wildcard include/lib/abcesop/*.cpp)

SRCS_C   =

OBJS = $(SRCS_CPP:.cpp=.o) $(SRCS_C:.c=.o)

# 標頭依賴追蹤 (改了 .h 會自動重編)
DEPS = $(OBJS:.o=.d)

# =============================================================
# 4. 外部靜態庫路徑
# =============================================================
CADICAL_LIB = $(CADICAL_DIR)/build/libcadical.a
ABC_LIB     = $(ABC_DIR)/libabc.a
LDFLAGS = -Wl,--start-group $(ABC_LIB) $(CADICAL_LIB) -Wl,--end-group $(SYS_LIBS)

# =============================================================
# 5. 規則
# =============================================================
.PHONY: all clean clean_all libs

all: $(TARGET_BIN)

# 只想 (重) 建外部庫時:make libs
libs: $(CADICAL_LIB) $(ABC_LIB)

# ---- CaDiCaL:configure 後只編 libcadical.a (跳過會用到 mmap 的 mobical) ----
$(CADICAL_LIB):
	@echo "[BUILD] Compiling CaDiCaL ($(ENV))..."
	@cd $(CADICAL_DIR) && ./configure
	@cd $(CADICAL_DIR)/build && $(MAKE) -j4 libcadical.a
	@echo "[BUILD] CaDiCaL done -> $(CADICAL_LIB)"

# ---- ABC:委派給腳本,依環境帶入正確旗標並用 xargs ar 打包 ----
$(ABC_LIB):
	@echo "[BUILD] Compiling Berkeley ABC ($(ENV))..."
	@ENV=$(ENV) bash scripts/build_abc.sh "$(ABC_DIR)"
	@echo "[BUILD] ABC done -> $(ABC_LIB)"

# ---- 主程式連結 ----
$(TARGET_BIN): $(CADICAL_LIB) $(ABC_LIB) $(OBJS)
	@echo "[LINK] $@"
	@$(CXX) $(OBJS) $(LDFLAGS) -o $@
	@echo "[SUCCESS] Build complete -> $@"

# ---- 編譯樣式規則 (含 -MMD -MP 產生 .d 依賴檔) ----
%.o: %.cpp
	@echo "[CXX] $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

%.o: %.c
	@echo "[CC]  $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ---- 清理 ----
clean:
	@echo "[CLEAN] Removing objects & executable..."
	@rm -f $(OBJS) $(DEPS) $(TARGET_BIN) $(TARGET).exe
	@echo "[CLEAN] Done."

clean_all: clean
	@echo "[CLEAN] Cleaning CaDiCaL & ABC builds..."
	-@cd $(CADICAL_DIR) && $(MAKE) clean 2>/dev/null || true
	-@cd $(ABC_DIR) && $(MAKE) clean 2>/dev/null || true
	@rm -f $(CADICAL_LIB) $(ABC_LIB)
	@echo "[CLEAN_ALL] Done."

# 引入自動產生的標頭依賴
-include $(DEPS)