# 1. 編譯器與基本參數設定
CXX      = g++
CC       = gcc

# 針對 C++ 的參數
CXXFLAGS = -std=c++20 -DFMT_HEADER_ONLY -DWIN64 -DLIN64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
           -fpermissive -Wno-unknown-pragmas -Wno-narrowing -Wno-sign-compare -Wno-unused-parameter \
           -Wno-misleading-indentation -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format \
           -Wno-dangling-else -Wno-class-memaccess -Wno-int-to-pointer-cast -Wno-overflow \
           -Wall -O3 \
           -I. -Iinclude -Iinclude/lib -Iinclude/lib/nauty \
           -Iinclude/lib/abcsat -Iinclude/lib/abcesop

# 針對 C 語言的參數
CFLAGS   = -DWIN64 -DLIN64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -fpermissive \
           -Wno-narrowing -Wno-int-to-pointer-cast -Wno-overflow -Wall -O3 \
           -Iinclude/lib/nauty -Iinclude/lib/abcsat -Iinclude/lib/abcesop

# 2. 原始碼與目的檔 (.o) 設定 
# 尋找所有的 .cpp 檔案
SRCS_CPP = main.cpp \
           $(wildcard src/core/*.cpp) \
           $(wildcard src/io/*.cpp) \
           $(wildcard src/analysis/*.cpp) \
           $(wildcard src/optimization/*.cpp) \
           $(wildcard src/transformation/*.cpp) \
           $(wildcard include/lib/abcsat/*.cpp) \
           $(wildcard include/lib/abcesop/*.cpp)

# 尋找所有的 .c 檔案
SRCS_C   = 

# 將 .cpp 與 .c 轉換為對應的 .o 檔名
OBJS_CPP = $(SRCS_CPP:.cpp=.o)
OBJS_C   = $(SRCS_C:.c=.o)
OBJS     = $(OBJS_CPP) $(OBJS_C)

TARGET   = NetlistTool

# 3. 自動偵測作業系統與環境
ifeq ($(OS),Windows_NT)
    TARGET_BIN = $(TARGET).exe
    CLEAN_CMD  = rm -f $(OBJS) $(TARGET_BIN)

    # 透過 MSYSTEM 變數區分 UCRT64 與 MINGW64
    ifeq ($(MSYSTEM),UCRT64)
        $(info [INFO] Detected Environment: Windows UCRT64)
        LDFLAGS = -Linclude/lib/cadical/win/ucrt64 -lcadical
    else
        $(info [INFO] Detected Environment: Windows MinGW64)
        LDFLAGS = -Linclude/lib/cadical/win/mingw64 -lcadical
    endif
else
    $(info [INFO] Detected Environment: Linux)
    TARGET_BIN = $(TARGET)
    CLEAN_CMD  = rm -f $(OBJS) $(TARGET_BIN)
    LDFLAGS    = -Linclude/lib/cadical/linux -lcadical
endif

# 4. 編譯規則

# 避免有名為 all 或 clean 的檔案干擾 make 執行
.PHONY: all clean

# 預設目標 (輸入 make 就會執行這個)
all: $(TARGET_BIN)

# 連結所有的 .o 檔生成最終執行檔
$(TARGET_BIN): $(OBJS)
	@echo "[LINK] Generating executable $@"
	@$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) -o $@
	@echo "[SUCCESS] Build complete!"

# 定義 .cpp 如何編譯成 .o
%.o: %.cpp
	@echo "[CXX]  Compiling $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# 定義 .c 如何編譯成 .o
%.o: %.c
	@echo "[CC]   Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# 清除所有編譯出來的 .o 檔與執行檔
clean:
	@echo "[CLEAN] Removing object files and executable..."
	@$(CLEAN_CMD)
	@echo "[CLEAN] Done."