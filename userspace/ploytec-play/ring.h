#ifndef PLOYTEC_PLAY_RING_H
#define PLOYTEC_PLAY_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Single-producer, single-consumer ring of DYNACORD_OUT_FRAME_SIZE-byte frames. */
struct ring {
	uint8_t *buf;
	size_t cap;                  /* capacity in frames */
	_Atomic size_t head;         /* frames written (producer) */
	_Atomic size_t tail;         /* frames read (consumer) */
};

int ring_init(struct ring *r, size_t cap_frames);   /* 0 or -1 */
void ring_free(struct ring *r);
size_t ring_space(struct ring *r);
size_t ring_fill(struct ring *r);

/* Copy up to n frames in or out; returns the number of frames copied. */
size_t ring_write(struct ring *r, const uint8_t *frames, size_t n);
size_t ring_read(struct ring *r, uint8_t *out, size_t n);

#endif
