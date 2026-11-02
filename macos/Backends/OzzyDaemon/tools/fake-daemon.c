/* SPDX-License-Identifier: MIT */
/*
 * fake-daemon: serves an OzzyDaemon segment with no USB device, for testing OzzyHAL, OzzyMIDI
 * and the tools without hardware. The real engine runs on the host tests' fake bus
 * (tests/fake_bus.c), so the shared memory is filled exactly as ozzy-daemon (and the kext) fill
 * it for the chosen model: header, ring layout, flags, settings, clock anchors. Transfers
 * complete at the sample rate in real time; MIDI IN is idle, or a loop of MIDI OUT
 * (--midi-loop, like a cable from the mixer's MIDI out to its MIDI in).
 *
 * Like the daemon: the heartbeat ticks each 250 ms, the mailbox is polled each millisecond and
 * APPLY_SETTING / SET_RATE restart the engine before the ack (--restart-delay: as slowly as the
 * real handshake, with the heartbeat stopped meanwhile, or with --hold-beat kept moving by
 * segment_beat_start as ozzy-daemon does). The CMS 600-3's second MIDI port is published in the
 * trailer as ozzy-daemon publishes it. Failure modes for the plug-ins' health checks:
 *   SIGUSR1           hang: the heartbeat, the clock and the mailbox stop; the process stays
 *   --crash-after S   crash: exit after S seconds without removing the segment, as a killed
 *                     daemon leaves it (magic, session_id, hardware_present intact)
 */

#include <errno.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "tests/fake_bus.h"
#include "common/devices/ploytec/ploytec_midi.h"
#include "engine/devices/ploytec/ploytec_midi2.h"
#include "common/ozzy_version.h"
#include "macos/Backends/OzzyDaemon/segment.h"

#define HEARTBEAT_MS   250
#define STALE_MS       1000     /* a segment of the same name left by a crashed daemon (as ozzy-daemon) */
#define POPS_PER_TICK  256      /* transfers completed per millisecond at most */
#define LOOP_BYTES     4096

enum fake_mode {
	FAKE_RUNNING,
	FAKE_HUNG,
};

struct fake_daemon {
	struct fake_bus fb;
	struct segment seg;
	struct ozzy_engine *engine;
	const struct ozzy_device *model;
	enum fake_mode mode;
	bool midi_loop;
	bool verbose;
	bool hold_beat;            /* --hold-beat */
	uint32_t restart_delay_ms;  /* --restart-delay */

	uint64_t t0_ns;           /* the clock's start: frames due since then */
	uint64_t out_frames;      /* PCM OUT frames completed since t0 */
	uint32_t fb_acc;          /* feedback: frames x 8000 not yet reported */

	uint8_t loop[LOOP_BYTES]; /* --midi-loop: bytes sent, not yet received */
	uint32_t loop_head, loop_tail;
};

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t hang_requested;

static void on_stop(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void on_hang(int sig)
{
	(void)sig;
	hang_requested = 1;
}

static uint64_t now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* Endpoints as the family's devices have them at their streaming setting (tests/test_endpoints.c) */
static bool bind_endpoints(struct ozzy_engine *e)
{
	/* Xone:DB4 at alt 1 */
	static const struct ozzy_usb_endpoint xone[] = {
		{ 0, 0x02, 0x01, 1, 4, NULL },
		{ 0, 0x83, 0x03, 1, 64, NULL },
		{ 0, 0x04, 0x02, 0, 512, NULL },
		{ 0, 0x05, 0x03, 1, 964, NULL },
		{ 1, 0x81, 0x01, 1, 4, NULL },
		{ 1, 0x86, 0x03, 1, 1024, NULL },
	};
	/* Akai EIE Pro at alt 1; the CMS 600-3 has its layout */
	static const struct ozzy_usb_endpoint isoc[] = {
		{ 0, 0x02, 0x05, 1, 0x0800 | 312, NULL },
		{ 0, 0x83, 0x02, 0, 512, NULL },
		{ 0, 0x04, 0x02, 0, 512, NULL },
		{ 1, 0x81, 0x05, 1, 3, NULL },
		{ 1, 0x86, 0x02, 0, 512, NULL },
	};
	const struct ozzy_usb_endpoint *bound[OZZY_PIPE_ROLES];

	switch (e->model->family) {
	case OZZY_FAMILY_PLOYTEC_XONE:
		ozzy_engine_bind_endpoints(e, xone, sizeof(xone) / sizeof(xone[0]), bound);
		return true;
	case OZZY_FAMILY_PLOYTEC_ISOC:
		ozzy_engine_bind_endpoints(e, isoc, sizeof(isoc) / sizeof(isoc[0]), bound);
		return true;
	default:
		return false;
	}
}

static uint8_t pipe_of(const struct fake_daemon *fd, enum ozzy_pipe_role role)
{
	return fd->engine->pipe[role].address;
}

/* --midi-loop: what an OUT transfer on the MIDI pipe carried (0xFD is padding) */
static void loop_take(struct fake_daemon *fd, const struct fake_xfer *x)
{
	uint32_t i;

	for (i = 0; i + 1 < x->size; i++) {
		if (x->buf[i] == PLOYTEC_MIDI_IDLE_BYTE)
			continue;
		if (fd->loop_tail - fd->loop_head < LOOP_BYTES)
			fd->loop[fd->loop_tail++ % LOOP_BYTES] = x->buf[i];
	}
}

/* An IN transfer's contents: silence, valid feedback, idle MIDI (or the loop's next bytes) */
static void fill_in(struct fake_daemon *fd, struct fake_xfer *x)
{
	uint32_t k, i;

	if (x->isoc && x->pipe == pipe_of(fd, OZZY_PIPE_FEEDBACK)) {
		/* ISOC_FB_COUNT: byte 0 = frames played in the packet's interval */
		uint8_t interval = fd->engine->pipe[OZZY_PIPE_FEEDBACK].interval;
		uint32_t uframes = 1u << (interval ? interval - 1 : 0);
		uint32_t rate = fd->seg.shm->audio.sample_rate;

		for (k = 0; k < x->count; k++) {
			uint8_t *b = x->buf + x->packets[k].offset;

			fd->fb_acc += rate * uframes;
			memset(b, 0, x->packets[k].length);
			b[0] = (uint8_t)(fd->fb_acc / ISOC_FB_UFRAMES_PER_SEC);
			fd->fb_acc %= ISOC_FB_UFRAMES_PER_SEC;
		}
		return;
	}
	if (x->pipe == pipe_of(fd, OZZY_PIPE_MIDI_IN)) {
		memset(x->buf, PLOYTEC_MIDI_IDLE_BYTE, x->size);
		/* M4_C1: 4 slots, then the control byte */
		for (i = 0; i < 4 && i + 1 < x->size && fd->loop_head != fd->loop_tail; i++)
			x->buf[i] = fd->loop[fd->loop_head++ % LOOP_BYTES];
		return;
	}
	if (x->isoc) {
		for (k = 0; k < x->count; k++)
			memset(x->buf + x->packets[k].offset, 0, x->packets[k].length);
	} else {
		memset(x->buf, 0, x->size);
	}
}

/* Complete @x now; returns the PCM OUT frames it moved */
static uint64_t complete(struct fake_daemon *fd, struct fake_xfer *x)
{
	struct ozzy_engine *e = fd->engine;
	uint64_t frames = 0;
	uint32_t k;

	fd->fb.engine = e;
	if (x->pipe & 0x80)
		fill_in(fd, x);
	else if (fd->midi_loop && x->pipe == pipe_of(fd, OZZY_PIPE_MIDI_OUT))
		loop_take(fd, x);

	if (x->isoc) {
		for (k = 0; k < x->count; k++) {
			x->packets[k].actual = x->packets[k].length;
			x->packets[k].status = 0;
			x->packets[k].time = 0;   /* the bus clock (now) */
		}
		if (x->pipe == pipe_of(fd, OZZY_PIPE_PCM_OUT)) {
			uint32_t bpf = fd->seg.shm->audio.output_bytes_per_frame;

			for (k = 0; k < x->count && bpf; k++)
				frames += x->packets[k].length / bpf;
		}
		ozzy_engine_isoc_complete(e, x->pipe, x->xfer, 0, x->packets, x->count);
	} else {
		if (x->pipe == pipe_of(fd, OZZY_PIPE_PCM_OUT))
			frames = ozzy_engine_transfer_frames(e, OZZY_PCM_OUT);
		ozzy_engine_complete(e, x->pipe, x->xfer, 0, x->size);
	}
	return frames;
}

static void reset_clock(struct fake_daemon *fd)
{
	fd->t0_ns = now_ns();
	fd->out_frames = 0;
	fd->fb_acc = 0;
}

/* Complete the transfers due by now: PCM OUT at the sample rate, the rest as they come up */
static void pump(struct fake_daemon *fd)
{
	uint64_t now = now_ns(), due;
	uint32_t rate = fd->seg.shm->audio.sample_rate;
	unsigned int pops = 0, queued = fd->fb.tail - fd->fb.head;
	struct fake_xfer x;

	fd->fb.now = mach_absolute_time();
	if (fd->fb.timer_on)
		ozzy_engine_timer(fd->engine);
	if (!pipe_of(fd, OZZY_PIPE_PCM_OUT) || !rate) {
		/* No clock to pace by: what was queued at the start of this tick */
		while (pops++ < queued && fake_bus_pop(&fd->fb, &x))
			complete(fd, &x);
		return;
	}
	due = (now - fd->t0_ns) * rate / 1000000000ull;
	while (fd->out_frames < due && pops++ < POPS_PER_TICK && fake_bus_pop(&fd->fb, &x))
		fd->out_frames += complete(fd, &x);
}

/* --- The mailbox, as device.c --- */

static IOReturn restart(struct fake_daemon *fd)
{
	bool ok;

	if (fd->hold_beat)
		segment_hold(&fd->seg);
	ok = ozzy_engine_restart(fd->engine);
	/* The real handshake's time: the main loop's heartbeat stands still meanwhile */
	if (fd->restart_delay_ms)
		usleep(fd->restart_delay_ms * 1000u);
	if (fd->hold_beat)
		segment_release(&fd->seg);

	reset_clock(fd);
	return ok ? kIOReturnSuccess : kIOReturnIOError;
}

static IOReturn apply_setting(struct fake_daemon *fd, uint32_t id, uint32_t value)
{
	if (id > 0xFFFF || value > 0xFF)
		return kIOReturnBadArgument;
	if (id >= OZZY_NUM_SETTINGS || ozzy_setting_descs[id].kind != OZZY_SETTING_DEVICE ||
	    ozzy_engine_change_setting(fd->engine, id, value) == OZZY_SETTING_INVALID)
		return kIOReturnBadArgument;
	return restart(fd);
}

static IOReturn set_rate(struct fake_daemon *fd, uint32_t rate)
{
	enum ozzy_setting_change change = ozzy_engine_change_rate(fd->engine, rate);

	if (change == OZZY_SETTING_INVALID)
		return kIOReturnBadArgument;
	return change == OZZY_SETTING_RESTART ? restart(fd) : kIOReturnSuccess;
}

static void poll_mailbox(struct fake_daemon *fd)
{
	uint32_t seq, method, args[2];
	IOReturn status;

	if (ozzy_daemon_take_midi_out_pending(fd->seg.trailer))
		ozzy_engine_midi_out_pending(fd->engine);
	if (!ozzy_daemon_take_request(fd->seg.trailer, &seq, &method, args))
		return;
	switch (method) {
	case OZZY_METHOD_APPLY_SETTING:
		status = apply_setting(fd, args[0], args[1]);
		break;
	case OZZY_METHOD_SET_RATE:
		status = set_rate(fd, args[0]);
		break;
	default:
		status = kIOReturnUnsupported;
		break;
	}
	printf("fake-daemon: request %u (%u, %u): 0x%x, now %u Hz\n", method, args[0], args[1], (unsigned int)status,
	       (unsigned int)fd->seg.shm->audio.sample_rate);
	fflush(stdout);
	ozzy_daemon_ack(fd->seg.trailer, seq, status);
}

static const struct ozzy_device *model_for_key(const char *key)
{
	unsigned int i;

	for (i = 0; i < OZZY_NUM_DEVICES; i++)
		if (strcmp(ozzy_devices[i].key, key) == 0)
			return &ozzy_devices[i];
	return NULL;
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage: fake-daemon [options]\n"
		"  --location HEX      the USB locationID the segment is named by (default 0f000000)\n"
		"  --model KEY         a device-table key (default DynacordCMS600; Xone and isoc families)\n"
		"  --rate HZ           the rate to start at (default: the model's start rate)\n"
		"  --midi-loop         MIDI OUT comes back on MIDI IN (isoc family)\n"
		"  --restart-delay MS  a restart (SET_RATE, APPLY_SETTING) takes this long, heartbeat stopped\n"
		"  --hold-beat         keep the heartbeat moving through a restart, as ozzy-daemon does\n"
		"  --crash-after S     exit after S seconds, leaving the segment behind\n"
		"  --seconds S         run for S seconds, then remove the segment (default: until a signal)\n"
		"  --list              the models it can fake\n"
		"SIGUSR1 hangs it (heartbeat, clock and mailbox stop); SIGINT / SIGTERM remove the segment.\n");
}

static bool parse_u32(const char *s, int base, uint32_t *out)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, base);
	if (errno || end == s || *end || v > 0xFFFFFFFFul)
		return false;
	*out = (uint32_t)v;
	return true;
}

int main(int argc, char **argv)
{
	static struct fake_daemon fd;
	uint32_t location = 0x0f000000, rate = 0, crash_after = 0, seconds = 0;
	const char *key = "DynacordCMS600";
	char name[OZZY_DAEMON_SEGMENT_NAME_MAX];
	enum segment_result made;
	uint64_t start, last_beat;
	void *mem;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "--midi-loop")) {
			fd.midi_loop = true;
		} else if (!strcmp(a, "--hold-beat")) {
			fd.hold_beat = true;
		} else if (!strcmp(a, "--list")) {
			unsigned int k;

			for (k = 0; k < OZZY_NUM_DEVICES; k++)
				if (ozzy_devices[k].family == OZZY_FAMILY_PLOYTEC_XONE ||
				    ozzy_devices[k].family == OZZY_FAMILY_PLOYTEC_ISOC)
					printf("%-20s %04x:%04x %s\n", ozzy_devices[k].key, ozzy_devices[k].vid,
					       ozzy_devices[k].pid, ozzy_devices[k].name);
			return 0;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(stdout);
			return 0;
		} else if (v && !strcmp(a, "--location") && parse_u32(v, 16, &location)) {
			i++;
		} else if (v && !strcmp(a, "--model")) {
			key = v;
			i++;
		} else if (v && !strcmp(a, "--rate") && parse_u32(v, 10, &rate)) {
			i++;
		} else if (v && !strcmp(a, "--restart-delay") && parse_u32(v, 10, &fd.restart_delay_ms)) {
			i++;
		} else if (v && !strcmp(a, "--crash-after") && parse_u32(v, 10, &crash_after)) {
			i++;
		} else if (v && !strcmp(a, "--seconds") && parse_u32(v, 10, &seconds)) {
			i++;
		} else {
			fprintf(stderr, "fake-daemon: bad argument '%s'\n", a);
			usage(stderr);
			return 2;
		}
	}
	fd.model = model_for_key(key);
	if (!fd.model) {
		fprintf(stderr, "fake-daemon: no model '%s' (--list)\n", key);
		return 2;
	}

	ozzy_daemon_segment_name(name, location);
	made = segment_create(&fd.seg, name, STALE_MS);
	if (made != SEGMENT_CREATED && made != SEGMENT_REPLACED_STALE) {
		fprintf(stderr, "fake-daemon: segment %s: %s\n", name, fd.seg.reason);
		return 1;
	}
	if (made == SEGMENT_REPLACED_STALE)
		printf("fake-daemon: segment %s: %s\n", name, fd.seg.reason);
	if (fd.hold_beat)
		segment_beat_start(&fd.seg, HEARTBEAT_MS);
	snprintf((char *)fd.seg.shm->product_name, sizeof(fd.seg.shm->product_name), "%s (fake)", fd.model->name);

	mem = calloc(1, ozzy_engine_size(fd.model));
	fd.engine = mem ? ozzy_engine_init(mem, fd.model) : NULL;
	if (!fd.engine || !bind_endpoints(fd.engine)) {
		fprintf(stderr, "fake-daemon: can't fake %s (only the Xone and isoc families)\n", fd.model->name);
		segment_destroy(&fd.seg);
		return 1;
	}
	/* The kext's and the daemon's bus: no PCM rings (the HAL's ring in shm), clock anchors */
	fake_bus_init(&fd.fb, (struct ozzy_shared_memory *)fd.seg.shm, false, NULL);
	{
		/* The bus clock is mach_absolute_time, as the HAL reads the anchors' host times */
		mach_timebase_info_data_t tb;

		mach_timebase_info(&tb);
		fd.fb.hz = 1000000000ull * tb.denom / tb.numer;
	}
	fd.fb.now = mach_absolute_time();
	ozzy_engine_attach(fd.engine, &fd.fb.bus, fd.seg.shm);
	if (ozzy_engine_set_midi2(fd.engine, &fd.seg.trailer->midi2.in, &fd.seg.trailer->midi2.out))
		ozzy_daemon_midi2_publish(fd.seg.trailer, DYNACORD_MIDI_PORT_SYSCTRL_NAME);
	ozzy_engine_set_usb_speed(fd.engine, OZZY_USB_SPEED_HIGH);
	ozzy_engine_set_driver_version(fd.engine, OZZY_GIT_VERSION " (fake)");
	ozzy_engine_set_anchors(fd.engine, true);
	if (rate)
		ozzy_engine_set_rate(fd.engine, rate);
	fd.fb.engine = fd.engine;   /* what an abort completes to */
	if (!ozzy_engine_start(fd.engine)) {
		fprintf(stderr, "fake-daemon: engine start failed\n");
		segment_destroy(&fd.seg);
		return 1;
	}
	reset_clock(&fd);

	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	signal(SIGUSR1, on_hang);
	printf("fake-daemon: %s at %u Hz in %s (pid %d)\n", fd.model->name, (unsigned int)fd.seg.shm->audio.sample_rate,
	       name, (int)getpid());
	fflush(stdout);

	start = last_beat = now_ns();
	while (!stop_requested) {
		uint64_t now = now_ns();

		if (hang_requested && fd.mode != FAKE_HUNG) {
			fd.mode = FAKE_HUNG;
			printf("fake-daemon: hung (SIGUSR1)\n");
			fflush(stdout);
		}
		if (crash_after && now - start >= crash_after * 1000000000ull) {
			printf("fake-daemon: crashing, segment left behind\n");
			fflush(stdout);
			_exit(3);
		}
		if (seconds && now - start >= seconds * 1000000000ull)
			break;
		if (fd.mode == FAKE_RUNNING) {
			if (now - last_beat >= HEARTBEAT_MS * 1000000ull) {
				segment_tick(&fd.seg);
				last_beat = now;
			}
			poll_mailbox(&fd);
			pump(&fd);
		}
		usleep(1000);
	}

	ozzy_engine_stop(fd.engine);
	segment_destroy(&fd.seg);
	free(mem);
	printf("fake-daemon: stopped\n");
	return 0;
}
