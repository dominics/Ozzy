# SPDX-License-Identifier: MIT
COMMON   := ../../../common/devices/ploytec
CXX      ?= clang++
CC       ?= clang
CXXFLAGS := -std=gnu++20 -O2 -g -Wall -Wextra -Werror -MMD -MP
CFLAGS   := -std=c11 -O2 -g -Wall -Wextra -Werror -MMD -MP
FRAMEWORKS := -framework IOKit -framework CoreFoundation

build:
	mkdir -p build

build/%.o: %.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

FAKE_SRC := tools/fake-daemon.cpp SharedMemory.cpp ../../Devices/Dynacord/DynacordRing.cpp
FAKE_HDR := SharedMemory.h ../../Shared/OzzySharedData.h ../../Devices/Dynacord/DynacordRing.h \
	$(COMMON)/ploytec_defs.h
build/fake-daemon: $(FAKE_SRC) $(FAKE_HDR) | build
	$(CXX) $(CXXFLAGS) -o $@ $(FAKE_SRC)

all: build/fake-daemon

clean:
	rm -rf build

.PHONY: all clean
