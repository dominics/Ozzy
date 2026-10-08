#include <stdio.h>
#include <unistd.h>
#include "init.h"
#include "ploytec_protocol.h"

#define TIMEOUT_MS 1000

static int ctrl(libusb_device_handle *h, uint8_t type, uint8_t req, uint16_t val, uint16_t idx,
		uint8_t *data, uint16_t len, int verbose, const char *what, char *err, size_t errlen)
{
	int r = libusb_control_transfer(h, type, req, val, idx, data, len, TIMEOUT_MS);
	if (r < 0) {
		snprintf(err, errlen, "%s (%02x %02x %04x %04x): %s", what, type, req, val, idx,
			 libusb_strerror(r));
		return -1;
	}
	if (verbose) {
		fprintf(stderr, "ctrl %-12s %02x %02x %04x %04x len %u ->", what, type, req, val, idx, len);
		for (int i = 0; i < r && (type & 0x80); i++)
			fprintf(stderr, " %02x", data[i]);
		fprintf(stderr, "\n");
	}
	return r;
}

int init_rate(libusb_device_handle *h, unsigned rate, int verbose, char *err, size_t errlen)
{
	uint8_t buf[16], rbuf[3], status;
	int r;

	if ((r = ctrl(h, 0xC0, PLOYTEC_CMD_FIRMWARE, 0, 0, buf, 15, verbose, "firmware", err, errlen)) < 0)
		return -1;
	if (r >= 3) {
		struct ploytec_firmware_version fw = ploytec_parse_firmware(buf);
		fprintf(stderr, "firmware: chip %02x, version %u.%u\n", fw.chip_id, fw.major, fw.minor);
	}

	if (ctrl(h, 0xC0, PLOYTEC_CMD_STATUS, 0, 0, &status, 1, verbose, "status", err, errlen) < 0)
		return -1;
	if (ctrl(h, PLOYTEC_CMD_GET_RATE_TYPE, PLOYTEC_CMD_GET_RATE_REQ, 0x0100, 0, rbuf, 3, verbose,
		 "get rate", err, errlen) < 0)
		return -1;
	fprintf(stderr, "current rate: %u Hz\n", ploytec_decode_rate(rbuf));

	ploytec_encode_rate(rate, rbuf);
	for (int i = 0; i < 5; i++) {
		uint16_t ep = i % 2 == 0 ? PLOYTEC_EP_RATE_IN : DYNACORD_EP_RATE_OUT;
		if (ctrl(h, PLOYTEC_CMD_SET_RATE_TYPE, PLOYTEC_CMD_SET_RATE_REQ, 0x0100, ep, rbuf, 3,
			 verbose, "set rate", err, errlen) < 0)
			return -1;
		usleep(10000);
	}

	if (ctrl(h, PLOYTEC_CMD_GET_RATE_TYPE, PLOYTEC_CMD_GET_RATE_REQ, 0x0100, PLOYTEC_EP_RATE_IN,
		 rbuf, 3, verbose, "verify rate", err, errlen) < 0)
		return -1;
	if (ploytec_decode_rate(rbuf) != rate) {
		snprintf(err, errlen, "device reports %u Hz after setting %u Hz; is the rate supported?",
			 ploytec_decode_rate(rbuf), rate);
		return -1;
	}

	if (ctrl(h, 0xC0, PLOYTEC_CMD_STATUS, 0, 0, &status, 1, verbose, "status", err, errlen) < 0)
		return -1;
	if (ctrl(h, 0x40, PLOYTEC_CMD_STATUS, ploytec_confirm_wvalue(status), 0, NULL, 0, verbose,
		 "confirm", err, errlen) < 0)
		return -1;
	return 0;
}
