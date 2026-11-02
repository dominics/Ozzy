/* SPDX-License-Identifier: MIT */
/* The daemon mailbox (macos/Shared/OzzyDaemonShared.h): a client and a daemon thread, the
 * sequence numbers, timeouts, the MIDI flag and the segment's layout */

#include "../../../../engine/shared/ozzy_shared_data.h"
#include "../../../Shared/OzzyDaemonShared.h"
#include "../../../../tests/check.h"
#include <pthread.h>
#include <stdlib.h>

static struct ozzy_daemon_trailer *trailer;
static volatile bool stop_daemon;
static unsigned int served;

/* Answers each request with method * 1000 + args[0] after a few ms, like a restart */
static void *daemon_thread(void *arg)
{
	(void)arg;
	while (!__atomic_load_n(&stop_daemon, __ATOMIC_ACQUIRE)) {
		uint32_t seq, method, args[2];

		if (ozzy_daemon_take_request(trailer, &seq, &method, args)) {
			usleep(3000);
			ozzy_daemon_ack(trailer, seq, (int32_t)(method * 1000 + args[0]));
			served++;
		}
		usleep(500);
	}
	return NULL;
}

static void setup(void *mem)
{
	trailer = mem;
	memset(trailer, 0, sizeof(*trailer));
	trailer->version = OZZY_DAEMON_VERSION;
	trailer->size = sizeof(*trailer);
	__atomic_store_n(&trailer->magic, OZZY_DAEMON_MAGIC, __ATOMIC_RELEASE);
}

static void test_calls(void)
{
	static struct ozzy_daemon_trailer mem;
	pthread_t thread;
	uint32_t args[2] = { 3, 1 };

	setup(&mem);
	pthread_create(&thread, NULL, daemon_thread, NULL);
	CHECK(ozzy_daemon_call(trailer, OZZY_METHOD_APPLY_SETTING, args, 2, 1000) == 1003, "first call answered");
	args[0] = 48000;
	CHECK(ozzy_daemon_call(trailer, OZZY_METHOD_SET_RATE, args, 1, 1000) == 2 * 1000 + 48000, "second call answered");
	for (uint32_t i = 0; i < 20; i++) {
		args[0] = i;
		if (ozzy_daemon_call(trailer, OZZY_METHOD_APPLY_SETTING, args, 2, 1000) != (int32_t)(1000 + i)) {
			CHECK(false, "call %u got another call's answer", i);
			break;
		}
	}
	CHECK(served == 22, "each call served once (%u)", served);
	CHECK(trailer->mailbox.args[1] == 1, "two arguments passed");
	__atomic_store_n(&stop_daemon, true, __ATOMIC_RELEASE);
	pthread_join(thread, NULL);

	/* No daemon answering: a timeout, and the next request still pairs with its own answer */
	args[0] = 7;
	CHECK(ozzy_daemon_call(trailer, OZZY_METHOD_APPLY_SETTING, args, 2, 20) == kIOReturnTimeout, "timeout");
	uint32_t seq, method, got[2];
	CHECK(ozzy_daemon_take_request(trailer, &seq, &method, got) && got[0] == 7, "the timed-out request is pending");
	ozzy_daemon_ack(trailer, seq, 0);
	CHECK(!ozzy_daemon_take_request(trailer, &seq, &method, got), "acked: nothing pending");
}

static void test_invalid(void)
{
	static struct ozzy_daemon_trailer mem;
	uint32_t args[1] = { 44100 };

	memset(&mem, 0, sizeof(mem));
	CHECK(ozzy_daemon_call(&mem, OZZY_METHOD_SET_RATE, args, 1, 10) == kIOReturnNotAttached, "no magic: not attached");
	setup(&mem);
	mem.version = OZZY_DAEMON_VERSION + 1;
	CHECK(ozzy_daemon_call(&mem, OZZY_METHOD_SET_RATE, args, 1, 10) == kIOReturnNotAttached, "newer version refused");
	mem.version = OZZY_DAEMON_VERSION;
	CHECK(ozzy_daemon_call(&mem, OZZY_METHOD_SET_RATE, args, 3, 10) == kIOReturnNotAttached, "too many arguments");
}

static void test_midi_flag(void)
{
	static struct ozzy_daemon_trailer mem;

	setup(&mem);
	CHECK(!ozzy_daemon_take_midi_out_pending(&mem), "no MIDI pending at first");
	ozzy_daemon_midi_out_pending(&mem);
	ozzy_daemon_midi_out_pending(&mem);
	CHECK(ozzy_daemon_take_midi_out_pending(&mem), "MIDI pending seen");
	CHECK(!ozzy_daemon_take_midi_out_pending(&mem), "and cleared");
}

static void test_layout(void)
{
	char name[OZZY_DAEMON_SEGMENT_NAME_MAX];
	uint64_t shm = sizeof(struct ozzy_shared_memory);
	uint64_t off = OZZY_DAEMON_TRAILER_OFFSET(shm);

	CHECK(off >= shm && off % OZZY_DAEMON_PAGE == 0, "trailer after the shared memory, page aligned");
	CHECK(ozzy_daemon_segment_size(shm) >= off + sizeof(struct ozzy_daemon_trailer), "segment holds the trailer");
	CHECK(ozzy_daemon_segment_size(shm) % OZZY_DAEMON_PAGE == 0, "segment is whole pages");
	CHECK(offsetof(struct ozzy_daemon_trailer, mailbox) % 64 == 0, "mailbox on its own cache line");
	ozzy_daemon_segment_name(name, 0x14200000);
	CHECK(strcmp(name, "/ozzy.14200000") == 0 && strlen(name) < 31, "segment name %s", name);
}

int main(void)
{
	test_calls();
	test_invalid();
	test_midi_flag();
	test_layout();
	return CHECK_DONE("mailbox");
}
