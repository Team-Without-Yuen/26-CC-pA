# Build the LLM-facing tools executable from either the repository root:
#   mingw32-make -f PROJECT_SPEC/Makefile
# or this directory:
#   cd PROJECT_SPEC
#   mingw32-make

MAKEFILE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
ROOT_DIR := $(MAKEFILE_DIR)/..

CXX ?= g++
CADICAL_VARIANT ?= ucrt64

TARGET := $(ROOT_DIR)/tools.exe

SOURCES := \
  $(ROOT_DIR)/tools.cpp \
  $(wildcard $(ROOT_DIR)/src/core/*.cpp) \
  $(wildcard $(ROOT_DIR)/src/io/*.cpp) \
  $(wildcard $(ROOT_DIR)/src/analysis/*.cpp) \
  $(wildcard $(ROOT_DIR)/src/optimization/*.cpp) \
  $(wildcard $(ROOT_DIR)/src/transformation/*.cpp) \
  $(wildcard $(ROOT_DIR)/include/lib/abcsat/*.cpp) \
  $(wildcard $(ROOT_DIR)/include/lib/abcesop/*.cpp)

CPPFLAGS := \
  -DFMT_HEADER_ONLY \
  -DWIN64 \
  -DLIN64 \
  -DWIN32_LEAN_AND_MEAN \
  -DNOMINMAX \
  -I$(ROOT_DIR) \
  -I$(ROOT_DIR)/include \
  -I$(ROOT_DIR)/include/lib \
  -I$(ROOT_DIR)/include/lib/nauty \
  -I$(ROOT_DIR)/include/lib/abcsat \
  -I$(ROOT_DIR)/include/lib/abcesop

CXXFLAGS := \
  -std=c++20 \
  -fpermissive \
  -w \
  -Wa,-mbig-obj \
  -O2

LDFLAGS := \
  -s \
  -L$(ROOT_DIR)/include/lib/cadical/win/$(CADICAL_VARIANT)

LDLIBS := -lcadical

.PHONY: all rebuild print-config

all: $(TARGET)

$(TARGET): $(SOURCES)
	@echo [BUILD] Compiling tools.exe
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
	@echo [SUCCESS] Built $(TARGET)

rebuild:
	$(MAKE) -B -f $(lastword $(MAKEFILE_LIST)) all

print-config:
	@echo ROOT_DIR=$(ROOT_DIR)
	@echo TARGET=$(TARGET)
	@echo CADICAL_VARIANT=$(CADICAL_VARIANT)
