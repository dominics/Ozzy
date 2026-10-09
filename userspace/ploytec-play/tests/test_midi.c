/*
 * CMS 600-3 MIDI packet layout against USB captures of the Windows driver.
 */
#include <string.h>

#include "dynacord_midi.h"
#include "test.h"
#include "fixtures/midi_captures.h"

int test_failures;

/* Concatenate one port's bytes from every in packet. Returns the count. */
static size_t unpack_all(const unsigned char *pkts, size_t len, enum dynacord_midi_port port,
			 unsigned char *out)
{
	size_t i, n = 0;

	for (i = 0; i + DYNACORD_MIDI_PKT_LEN <= len; i += DYNACORD_MIDI_PKT_LEN) {
		const uint8_t *data;
		unsigned int k = dynacord_midi_in_slot(pkts + i, DYNACORD_MIDI_PKT_LEN, port, &data);

		memcpy(out + n, data, k);
		n += k;
	}
	return n;
}

static int contains(const unsigned char *hay, size_t hay_len, const unsigned char *needle, size_t len)
{
	size_t i;

	for (i = 0; i + len <= hay_len; i++)
		if (!memcmp(hay + i, needle, len))
			return 1;
	return 0;
}

/*
 * Everything sent on either port comes back on the same port: USB-MIDI via
 * a loop cable on the DIN sockets, SystemCtrl from the mixer itself.
 */
static void test_echo(void)
{
	unsigned char got[midi_send_in_pkts_len];

	CHECK_EQ(unpack_all(midi_send_in_pkts, midi_send_in_pkts_len, DYNACORD_MIDI_PORT_USB, got),
		 midi_send_send_usb_len);
	CHECK(!memcmp(got, midi_send_send_usb, midi_send_send_usb_len));
	CHECK_EQ(unpack_all(midi_send_in_pkts, midi_send_in_pkts_len, DYNACORD_MIDI_PORT_SYSCTRL, got),
		 midi_send_send_sysctrl_len);
	CHECK(!memcmp(got, midi_send_send_sysctrl, midi_send_send_sysctrl_len));
}

/*
 * A keyboard on DIN MIDI IN: what the driver delivered appears, in order, in
 * the USB-MIDI slots (the capture runs a little longer than the app listened).
 */
static void test_keyboard(void)
{
	unsigned char got[midi_keyboard_in_pkts_len];
	size_t n = unpack_all(midi_keyboard_in_pkts, midi_keyboard_in_pkts_len, DYNACORD_MIDI_PORT_USB, got);

	CHECK(n >= midi_keyboard_recv_usb_len);
	CHECK(contains(got, n, midi_keyboard_recv_usb, midi_keyboard_recv_usb_len));
	CHECK_EQ(unpack_all(midi_keyboard_in_pkts, midi_keyboard_in_pkts_len, DYNACORD_MIDI_PORT_SYSCTRL, got), 0);
}

static void test_short_and_empty(void)
{
	static const uint8_t empty[9] = {0xfd, 0xfd, 0xfd, 0xfd, 0xfd, 0xfd, 0xfd, 0xfd, 0xfd};
	static const uint8_t full[9] = {0xf0, 1, 2, 3, 0xfd, 0x92, 0x30, 0x7f, 0xfd};
	const uint8_t *data;

	CHECK_EQ(dynacord_midi_in_slot(empty, 9, DYNACORD_MIDI_PORT_USB, &data), 0);
	CHECK_EQ(dynacord_midi_in_slot(empty, 9, DYNACORD_MIDI_PORT_SYSCTRL, &data), 0);
	CHECK_EQ(dynacord_midi_in_slot(full, 8, DYNACORD_MIDI_PORT_USB, &data), 0);
	CHECK_EQ(dynacord_midi_in_slot(full, 9, DYNACORD_MIDI_PORT_USB, &data), 4);
	CHECK(data == full);
	CHECK_EQ(dynacord_midi_in_slot(full, 9, DYNACORD_MIDI_PORT_SYSCTRL, &data), 3);
	CHECK(data == full + 5);
}

/* Our out packets have the same shape as every packet the Windows driver sent. */
static void test_out_shape(void)
{
	uint8_t pkt[DYNACORD_MIDI_PKT_LEN];
	size_t i, j;

	dynacord_midi_out_init(pkt);
	memcpy(dynacord_midi_slot(pkt, DYNACORD_MIDI_PORT_SYSCTRL), "\x92\x30\x7f", 3);
	CHECK(!memcmp(pkt, "\xfd\xfd\xfd\xfd\x92\x30\x7f\xfd\xe0", 9));

	for (i = 0; i < midi_send_out_pkts_len; i += DYNACORD_MIDI_PKT_LEN) {
		const unsigned char *p = midi_send_out_pkts + i;

		CHECK_EQ(p[DYNACORD_MIDI_TRAILER], DYNACORD_MIDI_OUT_TRAILER);
		/* left-aligned: no data byte after fill within a slot */
		for (j = 0; j < DYNACORD_MIDI_TRAILER; j++)
			if (j % DYNACORD_MIDI_SLOT_LEN && p[j - 1] == DYNACORD_MIDI_FILL)
				CHECK_EQ(p[j], DYNACORD_MIDI_FILL);
	}
}

int main(void)
{
	test_echo();
	test_keyboard();
	test_short_and_empty();
	test_out_shape();
	if (test_failures) {
		fprintf(stderr, "%d failure(s)\n", test_failures);
		return 1;
	}
	printf("midi: all tests passed\n");
	return 0;
}
