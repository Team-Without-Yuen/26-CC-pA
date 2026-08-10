# Build the LLM-facing tools executable from either the repository root:
#   make -f TOOLS_SPEC/Makefile
# or this directory:
#   cd TOOLS_SPEC
#   make

MAKEFILE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
ROOT_DIR := $(MAKEFILE_DIR)/..

CXX ?= g++

TARGET := $(ROOT_DIR)/tools.exe
CADICAL_LIB := $(ROOT_DIR)/include/lib/cadical/build/libcadical.a
ABC_LIB := $(ROOT_DIR)/include/lib/abc/libabc.a

ifeq ($(MSYSTEM),UCRT64)
  PLATFORM_DEF := -DWIN64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX
  ABC_DEFS := -DABC_USE_STDINT_H -DNUNLOCKED -DFMT_USE_WINDOWS_H=0
  SYS_LIBS := -lpthread -lm
else ifeq ($(MSYSTEM),MINGW64)
  PLATFORM_DEF := -DWIN64 -DWIN32_LEAN_AND_MEAN -DNOMINMAX
  ABC_DEFS := -DABC_USE_STDINT_H -DNUNLOCKED -DFMT_USE_WINDOWS_H=0
  SYS_LIBS := -lpthread -lm
else
  PLATFORM_DEF := -DLIN64
  ABC_DEFS :=
  SYS_LIBS := -lpthread -lm -ldl
endif

SOURCES := \
  $(ROOT_DIR)/tools.cpp \
  $(wildcard $(ROOT_DIR)/src/core/*.cpp) \
  $(wildcard $(ROOT_DIR)/src/io/*.cpp) \
  $(wildcard $(ROOT_DIR)/src/analysis/*.cpp) \
  $(wildcard $(ROOT_DIR)/src/optimization/*.cpp) \
  $(wildcard $(ROOT_DIR)/src/transformation/*.cpp) \
  $(wildcard $(ROOT_DIR)/src/SATEngine/*.cpp) \
  $(wildcard $(ROOT_DIR)/include/lib/abcsat/*.cpp) \
  $(wildcard $(ROOT_DIR)/include/lib/abcesop/*.cpp)

# tools.exe is compiled in one command, so track headers explicitly as inputs.
HEADERS := $(shell find $(ROOT_DIR)/include $(ROOT_DIR)/src -type f \
  \( -name '*.h' -o -name '*.hpp' \) 2>/dev/null)

CPPFLAGS := \
  -DFMT_HEADER_ONLY \
  $(PLATFORM_DEF) \
  $(ABC_DEFS) \
  -I$(ROOT_DIR) \
  -I$(ROOT_DIR)/include \
  -I$(ROOT_DIR)/include/lib \
  -I$(ROOT_DIR)/include/lib/nauty \
  -I$(ROOT_DIR)/include/lib/abcsat \
  -I$(ROOT_DIR)/include/lib/abcesop \
  -I$(ROOT_DIR)/include/lib/cadical/src \
  -I$(ROOT_DIR)/include/lib/abc/src

CXXFLAGS := \
  -std=c++20 \
  -fpermissive \
  -Wall \
  -Wno-unknown-pragmas \
  -Wno-narrowing \
  -Wno-sign-compare \
  -Wno-unused-parameter \
  -Wno-unused-variable \
  -Wno-unused-but-set-variable \
  -Wno-format \
  -Wno-misleading-indentation \
  -Wno-dangling-else \
  -Wno-class-memaccess \
  -Wno-int-to-pointer-cast \
  -O3

LDFLAGS := -Wl,--start-group $(ABC_LIB) $(CADICAL_LIB) -Wl,--end-group
LDLIBS := $(SYS_LIBS)

.PHONY: all libs rebuild clean check print-config

all: $(TARGET)

libs:
	$(MAKE) -C $(ROOT_DIR) libs

$(CADICAL_LIB) $(ABC_LIB):
	$(MAKE) -C $(ROOT_DIR) libs

$(TARGET): $(CADICAL_LIB) $(ABC_LIB) $(SOURCES) $(HEADERS)
	@echo [BUILD] Compiling tools.exe
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
	@echo [SUCCESS] Built $(TARGET)

rebuild:
	rm -f $(TARGET)
	$(MAKE) -f $(lastword $(MAKEFILE_LIST)) all

clean:
	rm -f $(TARGET)

check: all
	printf 'help\nexit\n' | $(TARGET)

print-config:
	@echo ROOT_DIR=$(ROOT_DIR)
	@echo TARGET=$(TARGET)
	@echo MSYSTEM=$(MSYSTEM)
	@echo CXX=$(CXX)
	@echo CADICAL_LIB=$(CADICAL_LIB)
	@echo ABC_LIB=$(ABC_LIB)
