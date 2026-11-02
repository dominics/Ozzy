/* SPDX-License-Identifier: MIT */
/* The daemon's shared memory segment (segment.h) */

#include "segment.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const size_t shm_bytes = sizeof(struct ozzy_shared_memory);

static void set_reason(struct segment *seg, const char *what, int err)
{
	if (err)
		snprintf(seg->reason, sizeof(seg->reason), "%s: %s", what, strerror(err));
	else
		snprintf(seg->reason, sizeof(seg->reason), "%s", what);
}

static uint32_t load_heartbeat(const volatile struct ozzy_shared_memory *shm)
{
	return __atomic_load_n((const volatile uint32_t *)&shm->heartbeat, __ATOMIC_RELAXED);
}

/*
 * Whether the segment behind @fd has a live writer: its heartbeat moves, or its magic appears
 * after it was missing, within @stale_after_ms. A segment too small to hold the shared memory
 * for the whole window is stale. A map failure counts as live, so nothing that couldn't be
 * inspected is unlinked.
 */
static bool is_live(int fd, unsigned int stale_after_ms)
{
	const volatile struct ozzy_shared_memory *shm = NULL;
	bool first_magic = false, live = false;
	uint32_t first_heartbeat = 0;
	unsigned int waited;

	for (waited = 0;; waited += 10) {
		struct stat sb;

		if (!shm && fstat(fd, &sb) == 0 && sb.st_size >= (off_t)shm_bytes) {
			void *p = mmap(NULL, shm_bytes, PROT_READ, MAP_SHARED, fd, 0);

			if (p == MAP_FAILED)
				return true;
			shm = p;
			first_magic = shm->magic == OZZY_MAGIC;
			first_heartbeat = load_heartbeat(shm);
		}
		if (shm) {
			if (!first_magic && shm->magic == OZZY_MAGIC)
				live = true;
			else if (load_heartbeat(shm) != first_heartbeat)
				live = true;
		}
		if (live || waited >= stale_after_ms)
			break;
		usleep(10000);
	}
	if (shm)
		munmap((void *)shm, shm_bytes);
	return live;
}

/* O_EXCL create and map; errno set on failure */
static bool map_new(struct segment *seg)
{
	/* shm_open applies the umask to the mode, and fchmod is refused on POSIX shm objects */
	mode_t old_mask = umask(0);
	int fd = shm_open(seg->name, O_RDWR | O_CREAT | O_EXCL, 0666);
	int open_errno = errno;
	void *p;

	umask(old_mask);
	if (fd < 0) {
		errno = open_errno;
		return false;
	}
	if (ftruncate(fd, (off_t)seg->size) != 0)
		goto fail;
	p = mmap(NULL, seg->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		goto fail;
	memset(p, 0, seg->size);
	seg->fd = fd;
	seg->base = p;
	seg->shm = p;
	seg->trailer = ozzy_daemon_trailer_at(p, shm_bytes);
	seg->trailer->version = OZZY_DAEMON_VERSION;
	seg->trailer->size = sizeof(struct ozzy_daemon_trailer);
	seg->trailer->daemon_pid = (uint32_t)getpid();
	__atomic_store_n(&seg->trailer->magic, OZZY_DAEMON_MAGIC, __ATOMIC_RELEASE);
	/* The shared memory's header (magic, version, IDs) comes with ozzy_engine_attach */
	seg->shm->session_id = arc4random();
	return true;

fail:
	open_errno = errno;
	close(fd);
	shm_unlink(seg->name);
	errno = open_errno;
	return false;
}

enum segment_result segment_create(struct segment *seg, const char *name, unsigned int stale_after_ms)
{
	struct stat sb;
	bool live;
	int fd;

	memset(seg, 0, sizeof(*seg));
	seg->fd = -1;
	snprintf(seg->name, sizeof(seg->name), "%s", name);
	seg->size = (size_t)ozzy_daemon_segment_size(shm_bytes);

	if (map_new(seg))
		return SEGMENT_CREATED;
	if (errno != EEXIST) {
		set_reason(seg, "shm_open", errno);
		return SEGMENT_ERROR;
	}

	fd = shm_open(seg->name, O_RDONLY, 0);
	if (fd < 0 && errno == ENOENT) {
		/* Removed since the failed create */
		if (map_new(seg))
			return SEGMENT_CREATED;
		set_reason(seg, errno == EEXIST ? "in use by a live daemon" : "shm_open", errno == EEXIST ? 0 : errno);
		return errno == EEXIST ? SEGMENT_IN_USE : SEGMENT_ERROR;
	}
	if (fd < 0) {
		int err = errno;

		set_reason(seg, "shm_open", err);
		return err == EACCES ? SEGMENT_IN_USE : SEGMENT_ERROR;
	}
	if (fstat(fd, &sb) != 0) {
		set_reason(seg, "fstat", errno);
		close(fd);
		return SEGMENT_IN_USE;
	}
	if (sb.st_uid != getuid()) {
		close(fd);
		snprintf(seg->reason, sizeof(seg->reason), "owned by uid %u", (unsigned int)sb.st_uid);
		return SEGMENT_IN_USE;
	}
	live = is_live(fd, stale_after_ms);
	close(fd);
	if (live) {
		set_reason(seg, "in use by a live daemon", 0);
		return SEGMENT_IN_USE;
	}

	shm_unlink(seg->name);
	if (map_new(seg)) {
		set_reason(seg, "replaced a stale segment", 0);
		return SEGMENT_REPLACED_STALE;
	}
	if (errno == EEXIST) {
		set_reason(seg, "in use by a live daemon", 0);
		return SEGMENT_IN_USE;
	}
	set_reason(seg, "shm_open", errno);
	return SEGMENT_ERROR;
}

void segment_tick(struct segment *seg)
{
	if (seg->shm)
		__atomic_fetch_add((volatile uint32_t *)&seg->shm->heartbeat, 1u, __ATOMIC_RELAXED);
}

void segment_destroy(struct segment *seg)
{
	if (seg->base) {
		/* A mark to tell whether the name still refers to this segment */
		uint32_t mark = arc4random() | 1;
		bool ours = false;
		int fd;

		__atomic_store_n(&seg->trailer->magic, 0u, __ATOMIC_RELEASE);
		seg->shm->audio.driver_ready = false;
		seg->shm->audio.hardware_present = false;
		seg->shm->magic = 0;
		__atomic_store_n((volatile uint32_t *)&seg->shm->heartbeat, mark, __ATOMIC_RELEASE);
		munmap(seg->base, seg->size);
		seg->base = NULL;
		seg->shm = NULL;
		seg->trailer = NULL;

		fd = shm_open(seg->name, O_RDONLY, 0);
		if (fd >= 0) {
			void *p = mmap(NULL, shm_bytes, PROT_READ, MAP_SHARED, fd, 0);

			close(fd);
			if (p != MAP_FAILED) {
				const volatile struct ozzy_shared_memory *s = p;

				ours = s->magic == 0 && load_heartbeat(s) == mark;
				munmap(p, shm_bytes);
			}
		}
		if (ours)
			shm_unlink(seg->name);
	}
	if (seg->fd >= 0) {
		close(seg->fd);
		seg->fd = -1;
	}
}
