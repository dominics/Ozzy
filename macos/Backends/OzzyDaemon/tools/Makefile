# Tools for OzzyDaemon's segment, no device needed:
#   fake-daemon   serves a segment with the real engine on a fake bus (a chosen model)
#   shm-client    reads it, calls its mailbox, sends MIDI, checks the heartbeat
#   backend-client   the plug-ins' daemon backend (macos/Shared/OzzyBackend.h) against it
#   make check    runs the two together (check.sh)
CC       ?= cc
CXX      ?= c++
OZZY     := ../../../..
BUILD    := build
VERSION  := $(shell git -C $(OZZY) describe --always --dirty 2>/dev/null || echo unknown)
MINOS    := -mmacosx-version-min=11.0
CPPFLAGS += -DOZZY_USER_ENGINE -DOZZY_GIT_VERSION=\"$(VERSION)\" -I$(OZZY)
CFLAGS   ?= -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS   += $(MINOS)

ENGINE   := $(OZZY)/engine/core/ozzy_engine.c $(OZZY)/engine/core/ozzy_isoc_output.c \
            $(OZZY)/engine/core/ozzy_isoc_input.c \
            $(OZZY)/engine/devices/ploytec/ploytec_engine.c $(OZZY)/engine/devices/ploytec/ploytec_io.c \
            $(OZZY)/engine/devices/ploytec/ploytec_isoc_engine.c $(OZZY)/engine/devices/ploytec/ploytec_us3xx_engine.c \
            $(OZZY)/engine/devices/usbmidi/usb_midi_engine.c \
            $(OZZY)/common/devices/ploytec/ploytec_handshake.c $(OZZY)/common/devices/ploytec/ploytec_codec.c \
            $(OZZY)/common/usb_midi.c $(OZZY)/common/midi_ump.c
HEADERS  := $(wildcard ../*.h $(OZZY)/engine/*/*.h $(OZZY)/engine/devices/*/*.h $(OZZY)/common/*.h \
            $(OZZY)/common/devices/*/*.h $(OZZY)/tests/fake_bus.h) $(OZZY)/macos/Shared/OzzyDaemonShared.h

all: $(BUILD)/fake-daemon $(BUILD)/shm-client $(BUILD)/backend-client

$(BUILD):
	mkdir -p $@

$(BUILD)/fake-daemon: fake-daemon.c ../segment.c $(OZZY)/tests/fake_bus.c $(ENGINE) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ fake-daemon.c ../segment.c $(OZZY)/tests/fake_bus.c $(ENGINE)

$(BUILD)/shm-client: shm-client.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ shm-client.c

# As the plug-ins build it: C++, the client side of the shared memory (no OZZY_USER_ENGINE)
$(BUILD)/backend-client: backend-client.cpp $(OZZY)/macos/Shared/OzzyBackend.h $(HEADERS) | $(BUILD)
	$(CXX) -std=gnu++11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers $(MINOS) \
		-I$(OZZY) -o $@ backend-client.cpp -framework IOKit -framework CoreFoundation

check: all
	./check.sh $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
