/* SPDX-License-Identifier: MIT */
/*
 * A device's timeline as OzzyHAL hands it to CoreAudio (GetZeroTimeStamp): the engine's clock
 * anchors (engine/shared/ozzy_shared_data.h, struct ozzy_timestamp) and a seed that changes
 * whenever the timeline starts over. Each OzzyHAL device has one, the main device and each of
 * its mono input devices (OzzyHALMono.h), so starting or stopping one doesn't restart another's.
 *
 * Only the device's IO thread (GetZeroTimeStamp) writes the anchor. Other threads (IO start and
 * stop, a configuration change, the monitor withdrawing the device) restart the timeline with
 * ozzy_timeline_restart: a new seed at once, and the IO thread drops its anchor at its next call.
 * While no fresh anchor comes (no driver, or one coming back), the last one is moved on at the
 * nominal rate; a driver that came back is a new clock, so the monitor marks the timeline with
 * ozzy_timeline_rebase and its first real anchor starts a new seed.
 * C and C++, header-only: OzzyHAL and the host tests.
 */

#ifndef OzzyTimeline_h
#define OzzyTimeline_h

#include <stdint.h>
#include <string.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

struct ozzy_timeline {
	uint64_t seed;            /* the seed CoreAudio gets (any thread, atomic) */
	uint32_t restarts;        /* ozzy_timeline_restart calls (any thread, atomic) */
	uint32_t restarts_seen;   /* as the IO thread last applied them */
	uint64_t anchor_sample;   /* the last real anchor handed out (the IO thread's; host 0: none) */
	uint64_t anchor_host;
	uint64_t served_sample;   /* the latest sample time handed out, real or moved on (the IO thread's) */
	uint32_t rebases;         /* ozzy_timeline_rebase calls (any thread, atomic) */
	uint32_t rebases_seen;    /* as the IO thread last applied them */
};

static inline void ozzy_timeline_init(struct ozzy_timeline *t)
{
	memset(t, 0, sizeof(*t));
	t->seed = 1;
}

/* Any thread: the timeline starts over (IO starts or stops, the device was reconfigured or went
 * away). Its anchor is dropped at the IO thread's next ozzy_timeline_anchor. */
static inline void ozzy_timeline_restart(struct ozzy_timeline *t)
{
	__atomic_fetch_add(&t->seed, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&t->restarts, 1, __ATOMIC_RELEASE);
}

/* Any thread: the clock behind the timeline is a new one (a driver came back), so the next real
 * anchor starts a new seed, wherever its sample time is. The anchor is kept until then. */
static inline void ozzy_timeline_rebase(struct ozzy_timeline *t)
{
	__atomic_fetch_add(&t->rebases, 1, __ATOMIC_RELEASE);
}

static inline uint64_t ozzy_timeline_seed(struct ozzy_timeline *t)
{
	return __atomic_load_n(&t->seed, __ATOMIC_RELAXED);
}

/*
 * IO thread: the anchor to hand out, from a read of the engine's clock (@got: the read
 * succeeded; *@sample, *@host: what it read), into *@sample and *@host, and the seed into *@seed.
 * Host time 0 is the engine's "no anchor yet" (a start or restart); a failed read, a writer
 * that kept the seqlock busy, or no driver at all. Either way the last real anchor is served,
 * never a (0, 0) one: false if there has been none since the timeline (re)started. The anchor
 * served is moved on by the whole periods of @period frames (@period_host host ticks each, 0: not
 * moved) that have passed by @now, so the clock keeps running at the nominal rate without a
 * driver; within the first period it is the last anchor itself. A real anchor behind the latest
 * sample time served means the engine restarted under running IO (watchdog, wake, a driver that
 * came back): a new seed. So does the first real anchor after ozzy_timeline_rebase.
 */
static inline bool ozzy_timeline_anchor(struct ozzy_timeline *t, bool got, uint64_t *sample, uint64_t *host,
					uint64_t *seed, uint64_t now, uint32_t period, uint64_t period_host)
{
	uint32_t restarts = __atomic_load_n(&t->restarts, __ATOMIC_ACQUIRE);
	uint32_t rebases = __atomic_load_n(&t->rebases, __ATOMIC_ACQUIRE);

	if (restarts != t->restarts_seen) {
		t->restarts_seen = restarts;
		t->anchor_sample = t->anchor_host = t->served_sample = 0;
		t->rebases_seen = rebases;   /* the restart's new seed covers it */
	}
	if (!got || *host == 0) {
		if (t->anchor_host == 0)
			return false;
		*sample = t->anchor_sample;
		*host = t->anchor_host;
		if (period && period_host && now > t->anchor_host) {
			uint64_t n = (now - t->anchor_host) / period_host;

			*sample += n * period;
			*host += n * period_host;
		}
		if (*sample > t->served_sample)
			t->served_sample = *sample;
	} else {
		if (*sample < t->served_sample || rebases != t->rebases_seen)
			__atomic_fetch_add(&t->seed, 1, __ATOMIC_RELAXED);
		t->rebases_seen = rebases;
		t->anchor_sample = t->served_sample = *sample;
		t->anchor_host = *host;
	}
	*seed = ozzy_timeline_seed(t);
	return true;
}

#endif
