# OzzyDaemon: the fork's userspace backend. Drives a device through IOUSBLib with the portable
# engine and publishes its shared memory as a POSIX shm segment (see segment.h).
#   make           build/ozzy-daemon (this machine's architecture)
#   make UNIVERSAL=1   arm64 + x86_64
#   make check     host tests (no device needed)
#   make OZZY_VERSION=1.0.0   stamp a release version (default: git describe of the checkout)
CC       ?= cc
CXX      ?= c++
OZZY     := ../../..
BUILD    := build
OZZY_VERSION ?= $(shell git -C $(OZZY) describe --always --dirty 2>/dev/null || echo unknown)
VERSION  := $(OZZY_VERSION)
BUNDLE_VERSION := $(firstword $(subst -, ,$(subst +, ,$(VERSION))))
ARCHS    := $(if $(UNIVERSAL),-arch arm64 -arch x86_64,)
MINOS    := -mmacosx-version-min=11.0
CPPFLAGS += -DOZZY_USER_ENGINE -DOZZY_GIT_VERSION=\"$(VERSION)\" -I$(OZZY)
CFLAGS   ?= -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS   += $(ARCHS) $(MINOS)
LDLIBS   := -framework IOKit -framework CoreFoundation -framework SystemConfiguration
# The version for codesign and Finder: an Info.plist in the binary
LDFLAGS  += -Wl,-sectcreate,__TEXT,__info_plist,$(BUILD)/Info.plist

ENGINE   := $(OZZY)/engine/core/ozzy_engine.c $(OZZY)/engine/core/ozzy_isoc_output.c \
            $(OZZY)/engine/core/ozzy_isoc_input.c \
            $(OZZY)/engine/devices/ploytec/ploytec_engine.c $(OZZY)/engine/devices/ploytec/ploytec_io.c \
            $(OZZY)/engine/devices/ploytec/ploytec_isoc_engine.c $(OZZY)/engine/devices/ploytec/ploytec_us3xx_engine.c \
            $(OZZY)/engine/devices/usbmidi/usb_midi_engine.c \
            $(OZZY)/common/devices/ploytec/ploytec_handshake.c $(OZZY)/common/devices/ploytec/ploytec_codec.c \
            $(OZZY)/common/usb_midi.c $(OZZY)/common/midi_ump.c
DAEMON   := main.c device.c user_bus.c segment.c isoc_plan.c
HEADERS  := $(wildcard *.h $(OZZY)/engine/*/*.h $(OZZY)/engine/devices/*/*.h $(OZZY)/common/*.h \
            $(OZZY)/common/devices/*/*.h) $(OZZY)/macos/Shared/OzzyDaemonShared.h
TESTS    := test_isoc_plan test_mailbox test_segment

all: $(BUILD)/ozzy-daemon

$(BUILD):
	mkdir -p $@

# Rewritten only when the version changes, so the binary relinks when it does
$(BUILD)/Info.plist: Info.plist.in FORCE | $(BUILD)
	sed -e "s|@VERSION@|$(VERSION)|g" -e "s|@BUNDLE_VERSION@|$(BUNDLE_VERSION)|g" Info.plist.in > $@.tmp
	cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BUILD)/ozzy-daemon: $(DAEMON) $(ENGINE) $(HEADERS) $(BUILD)/Info.plist | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(DAEMON) $(ENGINE) $(LDLIBS)

$(BUILD)/test_isoc_plan: tests/test_isoc_plan.c isoc_plan.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_isoc_plan.c isoc_plan.c

$(BUILD)/test_mailbox: tests/test_mailbox.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_mailbox.c

$(BUILD)/test_segment: tests/test_segment.c segment.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_segment.c segment.c

# The layout as the daemon (C, engine types) and the plug-ins (C++, std::atomic) see it
$(BUILD)/layout_c: tests/layout_print.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/layout_print.c

$(BUILD)/layout_cxx: tests/layout_print.c $(HEADERS) | $(BUILD)
	$(CXX) -std=c++17 -I$(OZZY) $(MINOS) -x c++ -o $@ tests/layout_print.c

check: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/layout_c $(BUILD)/layout_cxx
	@fail=0; for t in $(TESTS); do $(BUILD)/$$t || fail=1; done; \
	tests/check-cli.sh || fail=1; \
	if $(BUILD)/layout_c | diff -u - $$($(BUILD)/layout_cxx > $(BUILD)/layout_cxx.txt; echo $(BUILD)/layout_cxx.txt); then \
		echo "layout: ok (C and C++ agree)"; else echo "layout: C and C++ DIFFER"; fail=1; fi; \
	[ $$fail = 0 ] && echo "all daemon tests passed" || { echo "DAEMON TESTS FAILED"; exit 1; }

clean:
	rm -rf $(BUILD)

.PHONY: all check clean FORCE
