/* SPDX-License-Identifier: MIT */
/* Which user's daemon drives a device (handover.h) */

#include "handover.h"

void handover_init(struct handover *h, uid_t me)
{
	*h = (struct handover){ .me = me };
}

static enum handover_action start_if_wanted(const struct handover *h)
{
	return h->front && h->present && !h->running && !h->retry_pending ? HANDOVER_START : HANDOVER_NONE;
}

/* A new round: forget the retries, and stop the device if it was running and is no longer wanted */
static enum handover_action changed(struct handover *h, bool wanted)
{
	h->retries = 0;
	h->failures = 0;
	h->retry_pending = false;
	h->lost = false;
	if (!wanted) {
		if (h->running) {
			h->running = false;
			return HANDOVER_STOP;
		}
		return HANDOVER_NONE;
	}
	return start_if_wanted(h);
}

enum handover_action handover_console(struct handover *h, uid_t console_uid)
{
	bool front = console_uid != HANDOVER_NO_CONSOLE && console_uid == h->me;

	if (front == h->front)
		return HANDOVER_NONE;
	h->front = front;
	return changed(h, front);
}

enum handover_action handover_device(struct handover *h, bool present)
{
	if (present == h->present)
		return HANDOVER_NONE;
	h->present = present;
	return changed(h, present);
}

enum handover_action handover_started(struct handover *h, enum handover_start result)
{
	switch (result) {
	case HANDOVER_STARTED:
		h->running = true;
		h->retries = 0;
		h->failures = 0;
		h->lost = false;
		return HANDOVER_NONE;
	case HANDOVER_BUSY:
		if (!h->front || !h->present)
			return HANDOVER_NONE;
		h->retry_pending = true;
		if (h->retries >= HANDOVER_FAST_RETRIES)
			return HANDOVER_RETRY_SLOWLY;
		h->retries++;
		return HANDOVER_RETRY_SOON;
	case HANDOVER_FAILED:
		if (!h->front || !h->present)
			return HANDOVER_NONE;
		h->retry_pending = true;
		if (h->lost || h->failures >= HANDOVER_FAILED_RETRIES)
			return HANDOVER_RETRY_SLOWLY;
		h->failures++;
		return HANDOVER_RETRY_FAILED;
	case HANDOVER_UNSUPPORTED:
		return HANDOVER_GIVE_UP;
	}
	return HANDOVER_NONE;
}

enum handover_action handover_lost(struct handover *h)
{
	h->running = false;
	h->lost = true;
	h->retries = HANDOVER_FAST_RETRIES;   /* busy from here on: slowly as well */
	if (!h->front || !h->present)
		return HANDOVER_NONE;
	h->retry_pending = true;
	return HANDOVER_RETRY_SLOWLY;
}

enum handover_action handover_retry(struct handover *h)
{
	if (!h->retry_pending)
		return HANDOVER_NONE;
	h->retry_pending = false;
	return start_if_wanted(h);
}
