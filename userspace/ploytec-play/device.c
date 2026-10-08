#include <stdio.h>
#include "device.h"
#include "ploytec_defs.h"

int device_open(struct device *d, int verbose, char *err, size_t errlen)
{
	int r;

	d->ctx = NULL;
	d->h = NULL;
	d->claimed = 0;
	if ((r = libusb_init_context(&d->ctx, NULL, 0)) < 0) {
		snprintf(err, errlen, "libusb init: %s", libusb_strerror(r));
		return -1;
	}
	if (verbose)
		libusb_set_option(d->ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);

	d->h = libusb_open_device_with_vid_pid(d->ctx, DYNACORD_VENDOR_ID, DYNACORD_PID_CMS600_3);
	if (!d->h) {
		snprintf(err, errlen,
			 "no Dynacord CMS 600-3 (%04x:%04x) found. If it is plugged in, power-cycle "
			 "the mixer with the USB cable connected: the USB card only enumerates then.",
			 DYNACORD_VENDOR_ID, DYNACORD_PID_CMS600_3);
		return -1;
	}
	libusb_set_auto_detach_kernel_driver(d->h, 1);

	for (int iface = 0; iface < PLOYTEC_NUM_INTERFACES; iface++) {
		if ((r = libusb_claim_interface(d->h, iface)) < 0) {
			snprintf(err, errlen, "claim interface %d: %s", iface, libusb_strerror(r));
			return -1;
		}
		d->claimed |= 1 << iface;
		if ((r = libusb_set_interface_alt_setting(d->h, iface, PLOYTEC_ALT_SETTING)) < 0) {
			snprintf(err, errlen, "interface %d alt %d: %s", iface, PLOYTEC_ALT_SETTING,
				 libusb_strerror(r));
			return -1;
		}
	}
	return 0;
}

void device_close(struct device *d)
{
	for (int iface = 0; iface < PLOYTEC_NUM_INTERFACES; iface++)
		if (d->claimed & (1 << iface))
			libusb_release_interface(d->h, iface);
	if (d->h)
		libusb_close(d->h);
	if (d->ctx)
		libusb_exit(d->ctx);
	d->h = NULL;
	d->ctx = NULL;
	d->claimed = 0;
}
