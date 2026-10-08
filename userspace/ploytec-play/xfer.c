#include "xfer.h"

enum xfer_action xfer_action(enum libusb_transfer_status status, int stopping)
{
	if (stopping)
		return XFER_RETIRE;
	if (status == LIBUSB_TRANSFER_NO_DEVICE || status == LIBUSB_TRANSFER_CANCELLED)
		return XFER_FAIL;
	return XFER_RESUBMIT;
}
