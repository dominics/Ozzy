#ifndef PLOYTEC_PLAY_INIT_H
#define PLOYTEC_PLAY_INIT_H

#include <stddef.h>
#include <libusb.h>

/* Run the Ploytec vendor-request sequence that sets and verifies the sample rate.
 * Returns 0, or -1 with a message in err. */
int init_rate(libusb_device_handle *h, unsigned rate, int verbose, char *err, size_t errlen);

#endif
