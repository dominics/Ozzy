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

CLIENT_SRC := tools/shm-client.cpp ../../Devices/Dynacord/DynacordCodec.cpp
CLIENT_HDR := $(FAKE_HDR) ../../Devices/Dynacord/DynacordCodec.h
build/ploytec_codec.o: $(COMMON)/ploytec_codec.c $(COMMON)/ploytec_codec.h $(COMMON)/ploytec_defs.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/shm-client: $(CLIENT_SRC) $(CLIENT_HDR) build/ploytec_codec.o | build
	$(CXX) $(CXXFLAGS) -o $@ $(CLIENT_SRC) build/ploytec_codec.o

all: build/fake-daemon build/shm-client

clean:
	rm -rf build

.PHONY: all clean
