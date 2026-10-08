#include <stdlib.h>
#include <string.h>
#include "ring.h"
#include "ploytec_defs.h"

#define FS DYNACORD_OUT_FRAME_SIZE

int ring_init(struct ring *r, size_t cap_frames)
{
	r->buf = malloc(cap_frames * FS);
	if (!r->buf)
		return -1;
	r->cap = cap_frames;
	atomic_init(&r->head, 0);
	atomic_init(&r->tail, 0);
	return 0;
}

void ring_free(struct ring *r)
{
	free(r->buf);
	r->buf = NULL;
}

size_t ring_fill(struct ring *r)
{
	return atomic_load_explicit(&r->head, memory_order_acquire) -
	       atomic_load_explicit(&r->tail, memory_order_acquire);
}

size_t ring_space(struct ring *r)
{
	return r->cap - ring_fill(r);
}

static void copy_in(struct ring *r, size_t pos, const uint8_t *src, size_t n)
{
	size_t at = pos % r->cap, first = r->cap - at < n ? r->cap - at : n;
	memcpy(r->buf + at * FS, src, first * FS);
	memcpy(r->buf, src + first * FS, (n - first) * FS);
}

static void copy_out(struct ring *r, size_t pos, uint8_t *dst, size_t n)
{
	size_t at = pos % r->cap, first = r->cap - at < n ? r->cap - at : n;
	memcpy(dst, r->buf + at * FS, first * FS);
	memcpy(dst + first * FS, r->buf, (n - first) * FS);
}

size_t ring_write(struct ring *r, const uint8_t *frames, size_t n)
{
	size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
	size_t space = ring_space(r);
	if (n > space)
		n = space;
	copy_in(r, head, frames, n);
	atomic_store_explicit(&r->head, head + n, memory_order_release);
	return n;
}

size_t ring_read(struct ring *r, uint8_t *out, size_t n)
{
	size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
	size_t fill = atomic_load_explicit(&r->head, memory_order_acquire) - tail;
	if (n > fill)
		n = fill;
	copy_out(r, tail, out, n);
	atomic_store_explicit(&r->tail, tail + n, memory_order_release);
	return n;
}
