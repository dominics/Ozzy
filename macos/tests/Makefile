# SPDX-License-Identifier: MIT
COMMON   := ../../common/devices/ploytec
CC       ?= clang
CXX      ?= clang++
CFLAGS   := -std=c11 -O2 -g -Wall -Wextra -Werror -MMD -MP
CXXFLAGS := -std=gnu++20 -O2 -g -Wall -Wextra -Werror -MMD -MP
LDLIBS   :=

C_SRC := \
	$(COMMON)/dynacord_pacer.c \
	$(COMMON)/dynacord_feedback.c \
	$(COMMON)/dynacord_midi.c \
	$(COMMON)/ploytec_codec.c

LIB_SRC := \
	../OzzyCore/OzzyEngine.cpp \
	../Devices/Dynacord/DynacordRing.cpp \
	../Devices/Dynacord/DynacordCodec.cpp \
	../Devices/Dynacord/DynacordEngine.cpp \
	../Devices/Ploytec/PloytecEngine.cpp \
	../OzzyMIDI/MidiParser.cpp

TEST_SRC := \
	test_main.cpp \
	test_golden.cpp \
	test_ring.cpp \
	test_codec.cpp \
	test_bus.cpp \
	test_engine_init.cpp \
	test_engine_stream.cpp \
	test_engine_poll.cpp \
	test_engine_midi.cpp \
	test_kext_status.cpp \
	test_ploytec_engine.cpp \
	test_midi_parser.cpp

OBJ := $(addprefix build/,$(notdir $(C_SRC:.c=.o) $(LIB_SRC:.cpp=.o) $(TEST_SRC:.cpp=.o)))
vpath %.c $(sort $(dir $(C_SRC)))
vpath %.cpp $(sort $(dir $(LIB_SRC))) .

all: run

build:
	mkdir -p build

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(OBJ)
	$(CXX) -o $@ $^ $(LDLIBS)

test: run
	./run

clean:
	rm -rf build run run.dSYM

# Upstream Xone engine, built only so OzzyFactory links outside the kernel.
build/PloytecEngine.o: CXXFLAGS += -Wno-unused-parameter -Wno-unused-variable -Wno-error

-include $(OBJ:.o=.d)

.PHONY: all test clean
