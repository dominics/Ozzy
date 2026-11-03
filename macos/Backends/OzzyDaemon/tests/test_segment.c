/* SPDX-License-Identifier: MIT */
/* The daemon's POSIX shm segment: create, mode, trailer, live and stale segments, destroy, abandon */

#include "../segment.h"
#include "../../../../tests/check.h"
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static char name[OZZY_DAEMON_SEGMENT_NAME_MAX];
static volatile bool ticking;

static void *ticker(void *arg)
{
	struct segment *seg = arg;

	while (__atomic_load_n(&ticking, __ATOMIC_ACQUIRE)) {
		segment_tick(seg);
		usleep(5000);
	}
	return NULL;
}

static bool exists(void)
{
	int fd = shm_open(name, O_RDONLY, 0);

	if (fd < 0)
		return false;
	close(fd);
	return true;
}

int main(void)
{
	struct segment a, b;
	struct stat sb;
	pthread_t thread;
	int fd;

	/* A name of this run's own (the test never touches a device's segment) */
	snprintf(name, sizeof(name), "/ozzy.t%x", (unsigned int)getpid());
	shm_unlink(name);

	CHECK(segment_create(&a, name, 50) == SEGMENT_CREATED, "create: %s", a.reason);
	CHECK(a.trailer && ozzy_daemon_trailer_valid(a.trailer), "trailer set up");
	CHECK(a.trailer->daemon_pid == (uint32_t)getpid(), "trailer has our pid");
	CHECK((uint8_t *)a.trailer - (uint8_t *)a.base == (ptrdiff_t)OZZY_DAEMON_TRAILER_OFFSET(sizeof(struct ozzy_shared_memory)),
	      "trailer at its offset");
	fd = shm_open(name, O_RDONLY, 0);
	CHECK(fd >= 0 && fstat(fd, &sb) == 0 && (sb.st_mode & 0777) == 0666, "mode 0666 (%o)", sb.st_mode & 0777);
	if (fd >= 0)
		close(fd);

	/* Live (heartbeat moving): the second create leaves it alone */
	a.shm->magic = OZZY_MAGIC;
	__atomic_store_n(&ticking, true, __ATOMIC_RELEASE);
	pthread_create(&thread, NULL, ticker, &a);
	CHECK(segment_create(&b, name, 100) == SEGMENT_IN_USE, "live segment in use: %s", b.reason);
	__atomic_store_n(&ticking, false, __ATOMIC_RELEASE);
	pthread_join(thread, NULL);

	/* Stale (a crashed daemon: still mapped by nobody, heartbeat still): replaced */
	munmap(a.base, a.size);
	close(a.fd);
	CHECK(segment_create(&b, name, 50) == SEGMENT_REPLACED_STALE, "stale segment replaced: %s", b.reason);
	CHECK(b.shm->magic == 0 && ozzy_daemon_trailer_valid(b.trailer), "the replacement is fresh");

	/* A stale instance's destroy must not remove the live replacement: fake one by mapping the
	 * old name again and destroying it after b took over */
	segment_destroy(&b);
	CHECK(!exists(), "destroy unlinks its own segment");

	CHECK(segment_create(&a, name, 50) == SEGMENT_CREATED, "create again");
	struct segment old = a;   /* shares the mapping: destroy marks it, then sees its own mark */
	segment_destroy(&old);
	CHECK(!exists(), "a destroy that still owns the name unlinks it");

	CHECK(segment_create(&a, name, 50) == SEGMENT_CREATED, "create a third time");
	struct segment stale = a;
	stale.base = mmap(NULL, a.size, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0);
	stale.shm = stale.base;
	stale.trailer = ozzy_daemon_trailer_at(stale.base, sizeof(struct ozzy_shared_memory));
	stale.fd = -1;
	shm_unlink(name);   /* the stale one's name now goes to a new segment */
	CHECK(segment_create(&b, name, 50) == SEGMENT_CREATED, "replacement created");
	segment_destroy(&stale);
	CHECK(exists(), "the stale instance leaves the replacement's name alone");
	segment_destroy(&b);
	segment_destroy(&a);
	CHECK(!exists(), "all gone");

	/* Clearing a crashed daemon's segment: nothing there, live, then stale */
	CHECK(!segment_clear_stale(name, 50), "nothing to clear");
	CHECK(segment_create(&a, name, 50) == SEGMENT_CREATED, "create a fourth time");
	__atomic_store_n(&ticking, true, __ATOMIC_RELEASE);
	pthread_create(&thread, NULL, ticker, &a);
	CHECK(!segment_clear_stale(name, 100) && exists(), "a live segment stays");
	__atomic_store_n(&ticking, false, __ATOMIC_RELEASE);
	pthread_join(thread, NULL);
	munmap(a.base, a.size);   /* a crash: mapped by nobody, heartbeat still */
	close(a.fd);
	CHECK(segment_clear_stale(name, 50) && !exists(), "a stale segment is removed");

	/* Abandoned after a stuck abort: unlinked and marked gone, but still mapped, so a transfer
	 * IOKit still holds writes into memory nothing else has */
	CHECK(segment_create(&a, name, 50) == SEGMENT_CREATED, "create a fifth time");
	a.shm->magic = OZZY_MAGIC;
	{
		volatile struct ozzy_shared_memory *kept = a.shm;
		void *base = a.base;
		size_t size = a.size;

		segment_abandon(&a);
		CHECK(!exists() && !a.base && a.fd < 0, "abandon unlinks and forgets the segment");
		CHECK(kept->magic == 0 && !kept->audio.hardware_present, "the plug-ins see it gone");
		kept->audio.input_buffer[0] = 1;   /* still mapped: no fault */
		munmap(base, size);
	}

	return CHECK_DONE("segment");
}
