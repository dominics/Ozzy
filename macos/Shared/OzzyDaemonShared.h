/* SPDX-License-Identifier: MIT */
/*
 * The fork's part of the OzzyDaemon segment: the daemon publishes struct ozzy_shared_memory in a
 * POSIX shm segment (one per device, named by its USB locationID), and after it, at a fixed
 * offset, this trailer. The trailer carries what the kext's user client does for OzzyHAL and
 * OzzyMIDI (the OZZY_METHOD_* calls) as a mailbox in shared memory. C and C++, header-only:
 * the daemon (C) and the plug-ins (C++) both include it.
 *
 * Mailbox protocol (one caller at a time: OzzyHAL, which calls with its mapping lock held):
 * - client: writes method and args, then req_seq = ack_seq + 1 (release)
 * - daemon: sees req_seq != ack_seq (acquire), runs the request, writes status, then
 *   ack_seq = req_seq (release), only after the shared memory reflects the result (a restart
 *   has rewritten audio.*), which is when the HAL reads it
 * - client: waits for ack_seq == req_seq (acquire), then reads status
 * OZZY_METHOD_MIDI_OUT_PENDING doesn't use the mailbox: OzzyMIDI sets midi_out_pending and the
 * daemon's engine thread exchanges it back to 0.
 */

#ifndef OzzyDaemonShared_h
#define OzzyDaemonShared_h

#include <IOKit/IOReturn.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#define OZZY_DAEMON_MAGIC          0x545A5A4Fu   /* "OZZT" */
#define OZZY_DAEMON_VERSION        1
#define OZZY_DAEMON_SEGMENT_PREFIX "/ozzy."      /* + the locationID as 8 hex digits */
#define OZZY_DAEMON_SEGMENT_NAME_MAX 32          /* PSHMNAMLEN is 31 */
#define OZZY_DAEMON_PAGE           16384u        /* the trailer starts on a 16 KiB boundary */
#define OZZY_DAEMON_CALL_TIMEOUT_MS 3000u        /* a restart is about a second (the handshake) */

/* Where the trailer starts: struct ozzy_shared_memory rounded up to OZZY_DAEMON_PAGE. Both sides
 * compute it from their own sizeof (std::atomic and volatile fields have the same layout), and
 * the daemon's tests check that C and C++ agree. */
#define OZZY_DAEMON_TRAILER_OFFSET(shm_size) \
	(((uint64_t)(shm_size) + OZZY_DAEMON_PAGE - 1) & ~(uint64_t)(OZZY_DAEMON_PAGE - 1))

struct ozzy_daemon_mailbox {
	uint32_t req_seq;             /* client: one past ack_seq for a new request */
	uint32_t method;              /* OZZY_METHOD_APPLY_SETTING or OZZY_METHOD_SET_RATE */
	uint32_t args[2];             /* APPLY_SETTING: id, value; SET_RATE: rate */
	uint32_t ack_seq;             /* daemon: req_seq of the request it finished */
	int32_t status;               /* daemon: an IOReturn, as the kext's method returned it */
	uint32_t midi_out_pending;    /* OzzyMIDI: bytes queued in midi_out (the daemon clears it) */
	uint32_t _reserved;
};

struct ozzy_daemon_trailer {
	uint32_t magic;               /* OZZY_DAEMON_MAGIC once the daemon has set the segment up */
	uint32_t version;             /* OZZY_DAEMON_VERSION */
	uint32_t size;                /* sizeof(struct ozzy_daemon_trailer) as the daemon built it */
	uint32_t daemon_pid;
	uint8_t _pad[48];
	struct ozzy_daemon_mailbox mailbox __attribute__((aligned(64)));
};

/* The segment's name for the device at @location_id */
static inline void ozzy_daemon_segment_name(char name[OZZY_DAEMON_SEGMENT_NAME_MAX], uint32_t location_id)
{
	snprintf(name, OZZY_DAEMON_SEGMENT_NAME_MAX, OZZY_DAEMON_SEGMENT_PREFIX "%08x", location_id);
}

/* Bytes the segment has: the shared memory, then the trailer, rounded up to a page */
static inline uint64_t ozzy_daemon_segment_size(uint64_t shm_size)
{
	uint64_t end = OZZY_DAEMON_TRAILER_OFFSET(shm_size) + sizeof(struct ozzy_daemon_trailer);
	return (end + OZZY_DAEMON_PAGE - 1) & ~(uint64_t)(OZZY_DAEMON_PAGE - 1);
}

static inline struct ozzy_daemon_trailer *ozzy_daemon_trailer_at(void *segment, uint64_t shm_size)
{
	return (struct ozzy_daemon_trailer *)((uint8_t *)segment + OZZY_DAEMON_TRAILER_OFFSET(shm_size));
}

/* Whether @t was set up by a daemon this header understands */
static inline bool ozzy_daemon_trailer_valid(const struct ozzy_daemon_trailer *t)
{
	return __atomic_load_n(&t->magic, __ATOMIC_ACQUIRE) == OZZY_DAEMON_MAGIC &&
	       t->version == OZZY_DAEMON_VERSION && t->size >= sizeof(struct ozzy_daemon_trailer);
}

/* --- Client side (OzzyHAL, OzzyMIDI) --- */

/*
 * ozzy_daemon_call - Run @method with @args (@count of them) in the daemon and wait up to
 * @timeout_ms for it. Returns the daemon's status, or kIOReturnNotAttached without a daemon,
 * kIOReturnTimeout if it didn't answer (the request may still run).
 */
static inline int32_t ozzy_daemon_call(struct ozzy_daemon_trailer *t, uint32_t method, const uint32_t *args,
				       unsigned int count, unsigned int timeout_ms)
{
	struct ozzy_daemon_mailbox *m = &t->mailbox;
	uint32_t seq;
	uint64_t deadline;
	unsigned int i;

	if (!ozzy_daemon_trailer_valid(t) || count > 2)
		return kIOReturnNotAttached;
	m->method = method;
	for (i = 0; i < 2; i++)
		m->args[i] = i < count ? args[i] : 0;
	seq = __atomic_load_n(&m->ack_seq, __ATOMIC_ACQUIRE) + 1;
	__atomic_store_n(&m->req_seq, seq, __ATOMIC_RELEASE);

	deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (uint64_t)timeout_ms * 1000000u;
	while (__atomic_load_n(&m->ack_seq, __ATOMIC_ACQUIRE) != seq) {
		if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline || !ozzy_daemon_trailer_valid(t))
			return kIOReturnTimeout;
		usleep(1000);
	}
	return __atomic_load_n(&m->status, __ATOMIC_RELAXED);
}

/* OZZY_METHOD_MIDI_OUT_PENDING: tell the daemon midi_out has bytes */
static inline void ozzy_daemon_midi_out_pending(struct ozzy_daemon_trailer *t)
{
	__atomic_store_n(&t->mailbox.midi_out_pending, 1u, __ATOMIC_RELEASE);
}

/* --- Daemon side --- */

/* A request waiting: its sequence number, method and arguments. False if none. */
static inline bool ozzy_daemon_take_request(struct ozzy_daemon_trailer *t, uint32_t *seq, uint32_t *method,
					    uint32_t args[2])
{
	struct ozzy_daemon_mailbox *m = &t->mailbox;
	uint32_t req = __atomic_load_n(&m->req_seq, __ATOMIC_ACQUIRE);

	if (req == __atomic_load_n(&m->ack_seq, __ATOMIC_RELAXED))
		return false;
	*seq = req;
	*method = m->method;
	args[0] = m->args[0];
	args[1] = m->args[1];
	return true;
}

/* Finish request @seq with @status (after the shared memory shows its result) */
static inline void ozzy_daemon_ack(struct ozzy_daemon_trailer *t, uint32_t seq, int32_t status)
{
	__atomic_store_n(&t->mailbox.status, status, __ATOMIC_RELAXED);
	__atomic_store_n(&t->mailbox.ack_seq, seq, __ATOMIC_RELEASE);
}

/* Whether OzzyMIDI queued MIDI out since the last call (clears it) */
static inline bool ozzy_daemon_take_midi_out_pending(struct ozzy_daemon_trailer *t)
{
	return __atomic_exchange_n(&t->mailbox.midi_out_pending, 0u, __ATOMIC_ACQ_REL) != 0;
}

#endif
