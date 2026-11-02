#ifndef PLOYTEC_PLAY_XFER_H
#define PLOYTEC_PLAY_XFER_H

#include <libusb.h>

enum xfer_action {
	XFER_RESUBMIT,   /* keep the transfer going */
	XFER_RETIRE,     /* we are stopping: let it end */
	XFER_FAIL,       /* the device or pipe is gone: stop the stream */
};

/*
 * What to do with a transfer that has just completed. A transfer cancelled
 * while we are not stopping means the pipe was aborted (for example by an
 * unplug), so it counts as a failure rather than a quiet exit.
 */
enum xfer_action xfer_action(enum libusb_transfer_status status, int stopping);

#endif
