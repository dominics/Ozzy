PKGS     := libusb-1.0 sndfile
CFLAGS   += -std=c11 -O2 -g -Wall -Wextra -Werror -I../../common/devices/ploytec \
            $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS)) -lpthread -lm

PURE_SRC := feedback.c pacer.c ring.c source.c packetize.c xfer.c meter.c \
            ../../common/devices/ploytec/ploytec_codec.c
APP_SRC  := $(PURE_SRC) device.c init.c stream.c rt.c
TEST_SRC := tests/test_main.c $(PURE_SRC)

all: ploytec-play

ploytec-play: main.c $(APP_SRC) *.h
	$(CC) $(CFLAGS) -o $@ main.c $(APP_SRC) $(LDLIBS)

tests/run: $(TEST_SRC) *.h tests/*.h
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(LDLIBS)

tests/golden: tests/test_golden.c pacer.c pacer.h tests/fixtures/*.h
	$(CC) $(CFLAGS) -o $@ tests/test_golden.c pacer.c $(LDLIBS)

tests/midi: tests/test_midi.c ../../common/devices/ploytec/dynacord_midi.c ../../common/devices/ploytec/dynacord_midi.h tests/fixtures/midi_captures.h
	$(CC) $(CFLAGS) -o $@ tests/test_midi.c ../../common/devices/ploytec/dynacord_midi.c

test: tests/run tests/golden tests/midi
	./tests/run
	./tests/golden
	./tests/midi

clean:
	rm -rf ploytec-play tests/run tests/golden tests/midi *.dSYM tests/*.dSYM

.PHONY: all test clean
