/* SPDX-License-Identifier: MIT */
/*
 * The Dynacord CMS 600-3's second MIDI port (OZZY_QUIRK_MIDI_PORT2, ploytec_midi2.h) against
 * USB captures of the Windows driver (fixtures/dynacord_midi_captures.h): both ports decode
 * from the captured MIDI IN transfers, both encode into MIDI OUT transfers of the Windows
 * driver's shape, and a device without the quirk (or a CMS without rings) keeps port 2 idle.
 */

#include "fake_bus.h"
#include "check.h"
#include "engine/devices/ploytec/ploytec_midi2.h"
#include "fixtures/dynacord_midi_captures.h"
#include <stdlib.h>
#include <string.h>

#define EP_MIDI_OUT 0x04
#define EP_MIDI_IN  0x83

/* The captured MIDI IN transfers, handed out one per MIDI IN completion */
struct feed {
	const unsigned char *pkts;
	size_t len, pos;
};

/* What came out of the engine: port bytes from the rings, MIDI OUT transfers off the bus */
struct sink {
	uint8_t port[DYNACORD_MIDI_PORTS][20000];
	size_t port_len[DYNACORD_MIDI_PORTS];
	uint8_t out[4096 * DYNACORD_MIDI_XFER_LEN];
	size_t out_xfers;
};

static struct ozzy_shared_memory shm;
static struct ozzy_midi_buffer midi2_in, midi2_out;
static struct feed feed;
static struct sink sink;

static void fill(struct fake_xfer *x, void *arg)
{
	uint32_t k;

	if (x->isoc) {
		for (k = 0; k < x->count; k++) {   /* count feedback: 12 frames a millisecond */
			uint8_t *b = x->buf + x->packets[k].offset;

			b[0] = 0x00;
			b[1] = 0x00;
			b[2] = 0x0C;
		}
		return;
	}
	if (x->pipe != EP_MIDI_IN)
		return;
	if (feed.pos + DYNACORD_MIDI_XFER_LEN <= feed.len) {
		memcpy(x->buf, feed.pkts + feed.pos, DYNACORD_MIDI_XFER_LEN);
		feed.pos += DYNACORD_MIDI_XFER_LEN;
	} else {
		memset(x->buf, PLOYTEC_MIDI_IDLE_BYTE, x->size);
	}
}

/* Both in rings into the sink (port 1's is the shared memory's) */
static void drain(void)
{
	sink.port_len[0] += ozzy_midi_ring_pop(&shm.midi_in, sink.port[0] + sink.port_len[0],
					       (uint32_t)(sizeof(sink.port[0]) - sink.port_len[0]));
	sink.port_len[1] += ozzy_midi_ring_pop(&midi2_in, sink.port[1] + sink.port_len[1],
					       (uint32_t)(sizeof(sink.port[1]) - sink.port_len[1]));
}

/* One millisecond: the MIDI timer, then the transfers queued so far complete */
static void run_ms(struct fake_bus *fb, struct ozzy_engine *e)
{
	unsigned int n = fb->tail - fb->head;
	struct fake_xfer x;

	if (fb->timer_on)
		ozzy_engine_timer(e);
	n = fb->tail - fb->head;
	while (n-- && fake_bus_pop(fb, &x)) {
		if (x.pipe == EP_MIDI_OUT && sink.out_xfers < sizeof(sink.out) / DYNACORD_MIDI_XFER_LEN) {
			CHECK(x.size == DYNACORD_MIDI_XFER_LEN, "MIDI OUT transfer of %u bytes", x.size);
			memcpy(sink.out + sink.out_xfers++ * DYNACORD_MIDI_XFER_LEN, x.buf, DYNACORD_MIDI_XFER_LEN);
		}
		fake_bus_complete(fb, e, &x, fill, NULL);
		drain();
	}
}

/* An engine for @vid:@pid on the EIE / CMS endpoints, port 2 on the test's rings if @midi2 */
static struct ozzy_engine *start(struct fake_bus *fb, uint16_t vid, uint16_t pid, bool midi2, bool *midi2_set)
{
	const struct ozzy_device *model = ozzy_device_lookup(vid, pid);
	struct ozzy_engine *e;

	memset(&shm, 0, sizeof(shm));
	memset(&midi2_in, 0, sizeof(midi2_in));
	memset(&midi2_out, 0, sizeof(midi2_out));
	memset(&sink, 0, sizeof(sink));
	memset(&feed, 0, sizeof(feed));
	fake_bus_init(fb, &shm, false, NULL);
	e = ozzy_engine_init(malloc(ozzy_engine_size(model)), model);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_OUT, 0x02, 1, 1024, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_PCM_IN, 0x86, 2, 512, 1);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_FEEDBACK, 0x81, 1, 3, 4);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_OUT, EP_MIDI_OUT, 2, 512, 0);
	ozzy_engine_set_endpoint(e, OZZY_PIPE_MIDI_IN, EP_MIDI_IN, 2, 512, 0);
	*midi2_set = midi2 && ozzy_engine_set_midi2(e, &midi2_in, &midi2_out);
	ozzy_engine_attach(e, &fb->bus, &shm);   /* keeps the second port's rings */
	CHECK(ozzy_engine_start(e), "%s: start", model->name);
	return e;
}

static void stop(struct ozzy_engine *e)
{
	ozzy_engine_stop(e);
	free(e);
}

/* Feed @len bytes of captured MIDI IN transfers through the engine */
static void run_in(struct fake_bus *fb, struct ozzy_engine *e, const unsigned char *pkts, size_t len)
{
	unsigned int ms;

	feed.pkts = pkts;
	feed.len = len;
	feed.pos = 0;
	for (ms = 0; ms < 100000 && feed.pos < feed.len; ms++)
		run_ms(fb, e);
	for (ms = 0; ms < 10; ms++)   /* the transfers already filled */
		run_ms(fb, e);
	CHECK(feed.pos == feed.len, "fed %zu of %zu captured bytes", feed.pos, feed.len);
}

static bool contains(const uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t len)
{
	size_t i;

	for (i = 0; i + len <= hay_len; i++)
		if (!memcmp(hay + i, needle, len))
			return true;
	return false;
}

/* Only the CMS 600-3 has a second port */
static void test_table(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < OZZY_NUM_DEVICES; i++)
		n += ozzy_device_has_midi2(&ozzy_devices[i]);
	CHECK(n == 1 && ozzy_device_has_midi2(ozzy_device_lookup(0x0562, 0x03EB)), "%u devices with a second port", n);
}

/* Messages sent on both ports came back on the same port (USB-MIDI over a DIN loop cable,
 * SystemCtrl from the mixer); a keyboard on DIN MIDI IN arrived on USB-MIDI only */
static void test_in(void)
{
	struct fake_bus fb;
	struct ozzy_engine *e;
	bool set;

	e = start(&fb, 0x0562, 0x03EB, true, &set);
	CHECK(set, "CMS: second port refused");
	run_in(&fb, e, midi_send_in_pkts, midi_send_in_pkts_len);
	CHECK(sink.port_len[0] == midi_send_send_usb_len && !memcmp(sink.port[0], midi_send_send_usb, sink.port_len[0]),
	      "echo: port 1 got %zu bytes, sent %u", sink.port_len[0], midi_send_send_usb_len);
	CHECK(sink.port_len[1] == midi_send_send_sysctrl_len &&
	      !memcmp(sink.port[1], midi_send_send_sysctrl, sink.port_len[1]),
	      "echo: port 2 got %zu bytes, sent %u", sink.port_len[1], midi_send_send_sysctrl_len);
	stop(e);

	e = start(&fb, 0x0562, 0x03EB, true, &set);
	run_in(&fb, e, midi_keyboard_in_pkts, midi_keyboard_in_pkts_len);
	CHECK(sink.port_len[0] >= midi_keyboard_recv_usb_len &&
	      contains(sink.port[0], sink.port_len[0], midi_keyboard_recv_usb, midi_keyboard_recv_usb_len),
	      "keyboard: port 1 got %zu bytes, the driver delivered %u", sink.port_len[0], midi_keyboard_recv_usb_len);
	CHECK(sink.port_len[1] == 0, "keyboard: %zu bytes on port 2", sink.port_len[1]);
	stop(e);
}

/* The bytes of @port in the MIDI OUT transfers sent, and whether they all have the Windows
 * driver's shape: the out control byte, each slot left-aligned */
static size_t out_port(enum dynacord_midi_port port, uint8_t *dst, unsigned int *misshapen)
{
	size_t t, n = 0;
	uint32_t j, at = dynacord_midi_slot(port);

	for (t = 0; t < sink.out_xfers; t++) {
		const uint8_t *p = sink.out + t * DYNACORD_MIDI_XFER_LEN;
		bool idle = false;

		*misshapen += p[DYNACORD_MIDI_CONTROL] != DYNACORD_MIDI_OUT_CONTROL;
		for (j = at; j < at + DYNACORD_MIDI_SLOT_LEN; j++) {
			if (p[j] == PLOYTEC_MIDI_IDLE_BYTE) {
				idle = true;
				continue;
			}
			*misshapen += idle;
			dst[n++] = p[j];
		}
	}
	return n;
}

/* The Windows driver's own MIDI OUT transfers have the shape out_port() checks */
static void test_capture_shape(void)
{
	unsigned int misshapen = 0;
	uint8_t got[2][midi_send_out_pkts_len];

	memcpy(sink.out, midi_send_out_pkts, midi_send_out_pkts_len);
	sink.out_xfers = midi_send_out_pkts_len / DYNACORD_MIDI_XFER_LEN;
	CHECK(out_port(DYNACORD_MIDI_PORT_USB, got[0], &misshapen) == midi_send_send_usb_len &&
	      !memcmp(got[0], midi_send_send_usb, midi_send_send_usb_len), "capture: port 1");
	CHECK(out_port(DYNACORD_MIDI_PORT_SYSCTRL, got[1], &misshapen) == midi_send_send_sysctrl_len &&
	      !memcmp(got[1], midi_send_send_sysctrl, midi_send_send_sysctrl_len), "capture: port 2");
	CHECK(!misshapen, "capture: %u misshapen bytes", misshapen);
}

/* What the Windows driver sent on each port goes out on the same port, in its shape */
static void test_out(void)
{
	struct fake_bus fb;
	struct ozzy_engine *e;
	unsigned int ms, misshapen = 0;
	uint8_t got[2][4096 * DYNACORD_MIDI_SLOT_LEN];
	size_t n1, n2;
	bool set;

	e = start(&fb, 0x0562, 0x03EB, true, &set);
	ozzy_midi_ring_push(&shm.midi_out, midi_send_send_usb, midi_send_send_usb_len);
	ozzy_midi_ring_push(&midi2_out, midi_send_send_sysctrl, midi_send_send_sysctrl_len);
	for (ms = 0; ms < 100; ms++)
		run_ms(&fb, e);
	n1 = out_port(DYNACORD_MIDI_PORT_USB, got[0], &misshapen);
	n2 = out_port(DYNACORD_MIDI_PORT_SYSCTRL, got[1], &misshapen);
	CHECK(n1 == midi_send_send_usb_len && !memcmp(got[0], midi_send_send_usb, n1), "out: port 1 sent %zu bytes", n1);
	CHECK(n2 == midi_send_send_sysctrl_len && !memcmp(got[1], midi_send_send_sysctrl, n2), "out: port 2 sent %zu bytes", n2);
	CHECK(!misshapen, "out: %u misshapen bytes", misshapen);
	/* Port 1's pace (4,3,3,3,3,3,3,3 a millisecond) holds for port 2 too */
	CHECK(sink.out_xfers >= 20 && n1 <= (sink.out_xfers / 8 + 1) * 25, "out: %zu bytes in %zu transfers", n1,
	      sink.out_xfers);
	stop(e);
}

/* Port 2 stays idle on a CMS without rings and on a device without the quirk; port 1 works */
static void test_without(void)
{
	static const struct { uint16_t vid, pid; bool midi2; const char *what; } cases[] = {
		{ 0x0562, 0x03EB, false, "CMS without rings" },
		{ 0x09E8, 0x0010, true, "Akai EIE Pro" },
	};
	struct fake_bus fb;
	struct ozzy_engine *e;
	unsigned int c, ms, misshapen;
	uint8_t got[4096 * DYNACORD_MIDI_SLOT_LEN];
	bool set;

	for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		e = start(&fb, cases[c].vid, cases[c].pid, cases[c].midi2, &set);
		CHECK(!set, "%s: second port taken", cases[c].what);
		ozzy_midi_ring_push(&shm.midi_out, midi_send_send_usb, midi_send_send_usb_len);
		ozzy_midi_ring_push(&midi2_out, midi_send_send_sysctrl, midi_send_send_sysctrl_len);
		for (ms = 0; ms < 100; ms++)
			run_ms(&fb, e);
		misshapen = 0;
		CHECK(out_port(DYNACORD_MIDI_PORT_SYSCTRL, got, &misshapen) == 0, "%s: port 2 sent", cases[c].what);
		CHECK(out_port(DYNACORD_MIDI_PORT_USB, got, &misshapen) == midi_send_send_usb_len, "%s: port 1", cases[c].what);
		stop(e);

		e = start(&fb, cases[c].vid, cases[c].pid, cases[c].midi2, &set);
		run_in(&fb, e, midi_send_in_pkts, midi_send_in_pkts_len);
		CHECK(sink.port_len[0] == midi_send_send_usb_len, "%s: port 1 got %zu", cases[c].what, sink.port_len[0]);
		CHECK(sink.port_len[1] == 0, "%s: port 2 got %zu", cases[c].what, sink.port_len[1]);
		stop(e);
	}
}

int main(void)
{
	test_table();
	test_in();
	test_capture_shape();
	test_out();
	test_without();
	return CHECK_DONE("midi port 2");
}
