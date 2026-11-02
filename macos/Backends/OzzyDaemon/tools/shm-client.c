/* SPDX-License-Identifier: MIT */
/*
 * shm-client: a client of an OzzyDaemon segment (ozzy-daemon or fake-daemon) without CoreAudio
 * or CoreMIDI: what the segment publishes, the mailbox calls OzzyHAL makes, MIDI through the
 * rings as OzzyMIDI moves it, and whether the daemon is alive. Exit status 0 when each asked
 * step succeeded, so scripts can check it (make check).
 *
 * The segment is world-writable: a field that sizes or indexes anything is read once and
 * checked before use.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "engine/shared/ozzy_shared_data.h"
#include "macos/Shared/OzzyDaemonShared.h"

struct client {
	char name[OZZY_DAEMON_SEGMENT_NAME_MAX];
	void *base;
	size_t size;
	volatile struct ozzy_shared_memory *shm;
	struct ozzy_daemon_trailer *trailer;
};

static uint64_t now_ms(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000u;
}

static bool open_segment(struct client *c, uint32_t location)
{
	uint64_t need = ozzy_daemon_segment_size(sizeof(struct ozzy_shared_memory));
	struct stat sb;
	int fd;

	ozzy_daemon_segment_name(c->name, location);
	fd = shm_open(c->name, O_RDWR, 0);
	if (fd < 0) {
		fprintf(stderr, "shm-client: %s: %s (is a daemon running?)\n", c->name, strerror(errno));
		return false;
	}
	if (fstat(fd, &sb) != 0 || (uint64_t)sb.st_size < need) {
		fprintf(stderr, "shm-client: %s: %lld bytes, need %llu\n", c->name, (long long)sb.st_size,
			(unsigned long long)need);
		close(fd);
		return false;
	}
	c->size = (size_t)need;
	c->base = mmap(NULL, c->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (c->base == MAP_FAILED) {
		perror("shm-client: mmap");
		return false;
	}
	c->shm = c->base;
	c->trailer = ozzy_daemon_trailer_at(c->base, sizeof(struct ozzy_shared_memory));
	if (c->shm->magic != OZZY_MAGIC || c->shm->version != OZZY_SHARED_VERSION) {
		fprintf(stderr, "shm-client: %s: magic 0x%08x version %u (expected 0x%08x, %u)\n", c->name,
			c->shm->magic, c->shm->version, OZZY_MAGIC, OZZY_SHARED_VERSION);
		return false;
	}
	if (!ozzy_daemon_trailer_valid(c->trailer)) {
		fprintf(stderr, "shm-client: %s: no valid trailer (magic 0x%08x)\n", c->name, c->trailer->magic);
		return false;
	}
	return true;
}

static void print_info(const struct client *c)
{
	volatile struct ozzy_shared_memory *s = c->shm;
	volatile struct ozzy_audio_buffer *a = &s->audio;

	printf("segment     %s (daemon pid %u)\n", c->name, c->trailer->daemon_pid);
	printf("device      %04x:%04x '%s'\n", s->vendor_id, s->product_id, (const char *)s->product_name);
	printf("session     0x%08x  heartbeat %u\n", s->session_id, (unsigned int)s->heartbeat);
	printf("state       hardware_present %d  driver_ready %d\n", (int)a->hardware_present, (int)a->driver_ready);
	printf("rate        %u Hz  flags 0x%x\n", (unsigned int)a->sample_rate, (unsigned int)a->device_flags);
	printf("channels    %u in / %u out  (%u / %u bytes per frame)\n", a->input_channels, a->output_channels,
	       a->input_bytes_per_frame, a->output_bytes_per_frame);
	printf("rings       %u in / %u out frames  anchors each %u  safety %u out / %u in\n", a->input_ring_frames,
	       a->output_ring_frames, a->timestamp_frames, a->safety_offset_frames, a->input_safety_offset_frames);
	printf("anchor      sample %llu  host %llu\n", (unsigned long long)a->timestamp.sample_time,
	       (unsigned long long)a->timestamp.host_time);
}

/*
 * Whether the daemon is alive: the heartbeat moves within @window_ms (the daemon ticks it each
 * 250 ms) and the trailer stays valid. OzzyHAL's check, by hand.
 */
static bool alive(const struct client *c, unsigned int window_ms)
{
	uint32_t first = __atomic_load_n((const volatile uint32_t *)&c->shm->heartbeat, __ATOMIC_RELAXED);
	uint64_t end = now_ms() + window_ms;

	while (now_ms() < end) {
		if (!ozzy_daemon_trailer_valid(c->trailer))
			return false;
		if (__atomic_load_n((const volatile uint32_t *)&c->shm->heartbeat, __ATOMIC_RELAXED) != first)
			return true;
		usleep(10000);
	}
	return false;
}

static bool call(const struct client *c, uint32_t method, const uint32_t *args, unsigned int count)
{
	uint64_t t = now_ms();
	int32_t status = ozzy_daemon_call(c->trailer, method, args, count, OZZY_DAEMON_CALL_TIMEOUT_MS);

	printf("call %u: status 0x%x after %llu ms, rate now %u Hz\n", method, (unsigned int)status,
	       (unsigned long long)(now_ms() - t), (unsigned int)c->shm->audio.sample_rate);
	return status == kIOReturnSuccess;
}

/* Send @hex (bytes as hex digits) to MIDI OUT; with @expect, wait up to a second for them to
 * come back on MIDI IN (a loop) */
static bool midi(const struct client *c, const char *hex, bool expect)
{
	uint8_t out[256], in[256];
	uint32_t n = 0, got = 0;
	uint64_t end;

	while (hex[0] && hex[1] && n < sizeof(out)) {
		char pair[3] = { hex[0], hex[1], 0 };

		out[n++] = (uint8_t)strtoul(pair, NULL, 16);
		hex += 2;
		while (*hex == ' ' || *hex == ',')
			hex++;
	}
	if (ozzy_midi_ring_push(&c->shm->midi_out, out, n) != n) {
		fprintf(stderr, "shm-client: MIDI out ring full\n");
		return false;
	}
	ozzy_daemon_midi_out_pending(c->trailer);
	printf("midi out    %u bytes\n", n);
	if (!expect)
		return true;
	for (end = now_ms() + 1000; got < n && now_ms() < end; usleep(1000))
		got += ozzy_midi_ring_pop(&c->shm->midi_in, in + got, sizeof(in) - got);
	printf("midi in     %u bytes:", got);
	for (uint32_t i = 0; i < got; i++)
		printf(" %02x", in[i]);
	printf("\n");
	return got == n && memcmp(in, out, n) == 0;
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage: shm-client [--location HEX] [steps]\n"
		"  --location HEX        the segment /ozzy.<HEX> (default 0f000000, fake-daemon's)\n"
		"steps, in order (default: --info):\n"
		"  --info                what the segment publishes\n"
		"  --wait-ready MS       until driver_ready, at most MS ms\n"
		"  --alive MS            the heartbeat moves within MS ms\n"
		"  --dead MS             it doesn't (a hung or crashed daemon)\n"
		"  --set-rate HZ         OZZY_METHOD_SET_RATE through the mailbox\n"
		"  --setting ID VALUE    OZZY_METHOD_APPLY_SETTING\n"
		"  --expect-rate HZ      audio.sample_rate is HZ\n"
		"  --midi-send HEX       bytes to MIDI OUT (e.g. 903c40)\n"
		"  --midi-loop HEX       ... and expect them back on MIDI IN within a second\n");
}

int main(int argc, char **argv)
{
	struct client c = { 0 };
	uint32_t location = 0x0f000000, v, w;
	bool ok = true, did = false;
	int i;

	for (i = 1; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "--location")) {
			location = (uint32_t)strtoul(argv[i + 1], NULL, 16);
			memmove(&argv[i], &argv[i + 2], (size_t)(argc - i - 2) * sizeof(char *));
			argc -= 2;
			break;
		}
	}
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		}
	}
	if (!open_segment(&c, location))
		return 1;

	for (i = 1; i < argc && ok; i++) {
		const char *a = argv[i], *p = i + 1 < argc ? argv[i + 1] : NULL;

		did = true;
		if (!strcmp(a, "--info")) {
			print_info(&c);
		} else if (p && !strcmp(a, "--wait-ready")) {
			uint64_t end = now_ms() + strtoul(p, NULL, 10);

			while (!c.shm->audio.driver_ready && now_ms() < end)
				usleep(10000);
			ok = c.shm->audio.driver_ready;
			printf("ready       %s\n", ok ? "yes" : "no");
			i++;
		} else if (p && (!strcmp(a, "--alive") || !strcmp(a, "--dead"))) {
			bool is_alive = alive(&c, (unsigned int)strtoul(p, NULL, 10));

			printf("daemon      %s\n", is_alive ? "alive" : "gone");
			ok = is_alive == !strcmp(a, "--alive");
			i++;
		} else if (p && !strcmp(a, "--set-rate")) {
			v = (uint32_t)strtoul(p, NULL, 10);
			ok = call(&c, OZZY_METHOD_SET_RATE, &v, 1);
			i++;
		} else if (p && i + 2 < argc && !strcmp(a, "--setting")) {
			uint32_t args[2] = { (uint32_t)strtoul(p, NULL, 10), (uint32_t)strtoul(argv[i + 2], NULL, 10) };

			ok = call(&c, OZZY_METHOD_APPLY_SETTING, args, 2);
			i += 2;
		} else if (p && !strcmp(a, "--expect-rate")) {
			w = (uint32_t)strtoul(p, NULL, 10);
			ok = c.shm->audio.sample_rate == w;
			printf("rate        %u Hz (expected %u)\n", (unsigned int)c.shm->audio.sample_rate, w);
			i++;
		} else if (p && (!strcmp(a, "--midi-send") || !strcmp(a, "--midi-loop"))) {
			ok = midi(&c, p, !strcmp(a, "--midi-loop"));
			i++;
		} else {
			fprintf(stderr, "shm-client: bad step '%s'\n", a);
			usage(stderr);
			return 2;
		}
	}
	if (!did)
		print_info(&c);
	munmap(c.base, c.size);
	return ok ? 0 : 1;
}
