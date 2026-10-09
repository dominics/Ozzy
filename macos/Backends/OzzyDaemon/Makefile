# SPDX-License-Identifier: MIT
COMMON   := ../../../common/devices/ploytec
CXX      ?= clang++
CC       ?= clang
CXXFLAGS := -std=gnu++20 -O2 -g -Wall -Wextra -Werror -MMD -MP
CFLAGS   := -std=c11 -O2 -g -Wall -Wextra -Werror -MMD -MP
FRAMEWORKS := -framework IOKit -framework CoreFoundation

.DEFAULT_GOAL := all

build:
	mkdir -p build

build/%.o: %.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

FAKE_SRC := tools/fake-daemon.cpp SharedMemory.cpp ../../Devices/Dynacord/DynacordRing.cpp
FAKE_HDR := SharedMemory.h ../../Shared/OzzySharedData.h ../../Devices/Dynacord/DynacordRing.h \
	$(COMMON)/ploytec_defs.h
build/fake-daemon: $(FAKE_SRC) $(FAKE_HDR) | build
	$(CXX) $(CXXFLAGS) -o $@ $(FAKE_SRC)

CLIENT_SRC := tools/shm-client.cpp RealTime.cpp ../../Devices/Dynacord/DynacordCodec.cpp
CLIENT_HDR := $(FAKE_HDR) RealTime.h ../../Devices/Dynacord/DynacordCodec.h
build/ploytec_codec.o: $(COMMON)/ploytec_codec.c $(COMMON)/ploytec_codec.h $(COMMON)/ploytec_defs.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/shm-client: $(CLIENT_SRC) $(CLIENT_HDR) build/ploytec_codec.o | build
	$(CXX) $(CXXFLAGS) -o $@ $(CLIENT_SRC) build/ploytec_codec.o

all: build/fake-daemon build/shm-client build/OzzyDaemon

DAEMON_CXX := main.cpp Session.cpp UserBus.cpp RealTime.cpp SharedMemory.cpp \
	../../Devices/Dynacord/DynacordEngine.cpp ../../Devices/Dynacord/DynacordRing.cpp \
	../../OzzyCore/OzzyEngine.cpp ../../Devices/Ploytec/PloytecEngine.cpp
DAEMON_C := $(COMMON)/dynacord_pacer.c $(COMMON)/dynacord_feedback.c $(COMMON)/dynacord_midi.c \
	$(COMMON)/ploytec_codec.c
DAEMON_OBJ := $(addprefix build/,$(notdir $(DAEMON_CXX:.cpp=.o) $(DAEMON_C:.c=.o)))
DAEMON_HDR := $(FAKE_HDR) ../../OzzyCore/IOzzyBus.h ../../OzzyCore/OzzyEngine.h ../../OzzyCore/OzzyFactory.h \
	../../Devices/Dynacord/DynacordEngine.h ../../Devices/Ploytec/PloytecEngine.h ../../Shared/OzzyLog.h \
	Session.h UserBus.h RealTime.h ../OzzyKext/KextStatus.h

vpath %.c $(sort $(dir $(DAEMON_C)))
vpath %.cpp $(sort $(dir $(DAEMON_CXX)))

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c $< -o $@

$(DAEMON_OBJ): $(DAEMON_HDR)

# Upstream Xone engine, built only so OzzyFactory links outside the kernel.
build/PloytecEngine.o: CXXFLAGS += -Wno-unused-parameter -Wno-unused-variable -Wno-error

build/OzzyDaemon: $(DAEMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(FRAMEWORKS)

-include $(DAEMON_OBJ:.o=.d)

clean:
	rm -rf build

.PHONY: all clean
