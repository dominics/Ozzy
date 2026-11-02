#ifndef PLOYTEC_PLAY_DEVICE_H
#define PLOYTEC_PLAY_DEVICE_H

#include <stddef.h>
#include <libusb.h>

struct device {
	libusb_context *ctx;
	libusb_device_handle *h;
	int claimed;            /* bitmask of claimed interfaces */
};

/* Open the CMS 600-3, claim both interfaces and select alt setting 1 on each.
 * Returns 0, or -1 with a message in err. device_close is safe either way. */
int device_open(struct device *d, int verbose, char *err, size_t errlen);
void device_close(struct device *d);

#endif
