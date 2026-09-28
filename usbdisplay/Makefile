# SPDX-License-Identifier: GPL-2.0-only
#
# Builds and runs the test suite with any ordinary C++ compiler.
#
# The driver itself cannot be built this way and never will be: it needs the
# Windows driver kit, the indirect display stub library and a shader
# compiler, none of which exist outside Windows. What can be built anywhere
# is everything that decides *what to send* and *what the bytes should be* -
# the protocol, the damage planner, the cost model and the colour
# conversion - which is the part where a mistake is silent and expensive.
#
# That matters for two reasons beyond convenience. It gives quick feedback
# without a Windows machine, and it compiles the same code with a second
# compiler, which catches the undefined behaviour that one compiler happens
# to forgive.
#
#   make            build and run
#   make build      build only
#   make clean

CXX      ?= c++
# -Wpedantic is deliberately absent. It objects to constructs that are
# legal, harmless and used throughout, and the noise would drown the
# warnings that matter.
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror
LDFLAGS  ?=

# The conversion kernels are written against SSE2 directly, which is part of
# the 64-bit x86 baseline. Somewhere else they would need a second
# implementation rather than a compiler flag, so say so plainly instead of
# failing later with a missing header.
ARCH := $(shell uname -m 2>/dev/null || echo unknown)
ifeq ($(filter x86_64 amd64,$(ARCH)),)
  $(error These tests need a 64-bit x86 machine: the conversion kernels use SSE2 directly. Detected $(ARCH).)
endif

ROOT    := $(dir $(lastword $(MAKEFILE_LIST)))
BUILD   := $(ROOT)build/tests
TARGET  := $(BUILD)/usbdisplay-tests

SOURCES := \
	$(ROOT)src/core/proto.cpp \
	$(ROOT)src/core/macrosilicon.cpp \
	$(ROOT)src/render/rect.cpp \
	$(ROOT)src/render/damage.cpp \
	$(ROOT)src/render/convert.cpp \
	$(ROOT)tests/testing.cpp \
	$(ROOT)tests/test_rect.cpp \
	$(ROOT)tests/test_damage.cpp \
	$(ROOT)tests/test_convert.cpp \
	$(ROOT)tests/test_chip.cpp \
	$(ROOT)tests/test_portable.cpp

OBJECTS := $(patsubst $(ROOT)%.cpp,$(BUILD)/%.o,$(SOURCES))

.PHONY: all build test clean
all: test

build: $(TARGET)

test: $(TARGET)
	@$(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lpthread

$(BUILD)/%.o: $(ROOT)%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	@rm -rf $(BUILD)
