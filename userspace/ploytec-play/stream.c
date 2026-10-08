#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "stream.h"
#include "feedback.h"
#include "pacer.h"
#include "packetize.h"
#include "xfer.h"

#define OUT_XFERS 32
#define OUT_PKTS 24
#define FB_XFERS 8
#define FB_PKTS 5
#define FB_PKT_SIZE 64          /* wMaxPacketSize of the feedback endpoint */
#define IN_XFERS 8
#define IN_FRAME_SIZE 64

struct stream {
	struct device *d;
	struct ring *ring;
	struct pacer pacer;
	pthread_t thread;
	pthread_mutex_t lock;          /* guards stats */
	struct stats stats;
	atomic_int stop_events;
	atomic_int silence;
	atomic_int draining;
	atomic_int stopping;
	atomic_int active;
	atomic_int failed;
	struct libusb_transfer *out[OUT_XFERS], *fb[FB_XFERS], *in[IN_XFERS];
};

static void *event_thread(void *arg)
{
	struct stream *s = arg;
	struct timeval tv = { 0, 100000 };
	while (!atomic_load(&s->stop_events))
		libusb_handle_events_timeout_completed(s->d->ctx, &tv, NULL);
	return NULL;
}

static void fill_out(struct stream *s, struct libusb_transfer *t)
{
	unsigned lengths[OUT_PKTS];
	struct fill_result res = { 0 };
	enum fill_mode mode = atomic_load(&s->silence) ? FILL_SILENCE :
			      atomic_load(&s->draining) ? FILL_DRAINING : FILL_NORMAL;

	t->length = fill_packets(&s->pacer, s->ring, t->buffer, lengths, t->num_iso_packets, mode, &res);
	for (int i = 0; i < t->num_iso_packets; i++)
		t->iso_packet_desc[i].length = lengths[i];

	pthread_mutex_lock(&s->lock);
	s->stats.frames_sent += res.frames;
	s->stats.underruns += res.underruns;
	for (int i = 0; i <= DYNACORD_OUT_MAX_FRAMES_PER_PKT; i++)
		s->stats.pkt_hist[i] += res.hist[i];
	pthread_mutex_unlock(&s->lock);
}

static void resubmit(struct stream *s, struct libusb_transfer *t)
{
	switch (xfer_action(t->status, atomic_load(&s->stopping))) {
	case XFER_RESUBMIT:
		if (!atomic_load(&s->failed) && libusb_submit_transfer(t) == 0)
			return;
		atomic_store(&s->failed, 1);
		break;
	case XFER_FAIL:
		atomic_store(&s->failed, 1);
		break;
	case XFER_RETIRE:
		break;
	}
	atomic_fetch_sub(&s->active, 1);
}

static void out_cb(struct libusb_transfer *t)
{
	struct stream *s = t->user_data;
	unsigned long long errs = 0;
	for (int i = 0; i < t->num_iso_packets; i++)
		if (t->iso_packet_desc[i].status != LIBUSB_TRANSFER_COMPLETED)
			errs++;
	if (errs) {
		pthread_mutex_lock(&s->lock);
		s->stats.iso_errors += errs;
		pthread_mutex_unlock(&s->lock);
	}
	if (t->status == LIBUSB_TRANSFER_COMPLETED && !atomic_load(&s->stopping))
		fill_out(s, t);
	resubmit(s, t);
}

static void fb_cb(struct libusb_transfer *t)
{
	struct stream *s = t->user_data;
	unsigned long long sum = 0, count = 0;
	for (int i = 0; i < t->num_iso_packets; i++) {
		struct libusb_iso_packet_descriptor *d = &t->iso_packet_desc[i];
		if (d->status != LIBUSB_TRANSFER_COMPLETED)
			continue;
		int v = feedback_parse(libusb_get_iso_packet_buffer_simple(t, (unsigned)i), (int)d->actual_length);
		if (v < 0)
			continue;
		pacer_feedback(&s->pacer, v);
		sum += (unsigned long long)v;
		count++;
	}
	pthread_mutex_lock(&s->lock);
	s->stats.fb_sum += sum;
	s->stats.fb_count += count;
	s->stats.fb_invalid = s->pacer.invalid;
	pthread_mutex_unlock(&s->lock);
	resubmit(s, t);
}

static void in_cb(struct libusb_transfer *t)
{
	struct stream *s = t->user_data;
	pthread_mutex_lock(&s->lock);
	s->stats.in_bytes += (unsigned long long)t->actual_length;
	if (t->status == LIBUSB_TRANSFER_COMPLETED)
		meter_feed(&s->stats.in, t->buffer, (size_t)t->actual_length);
	pthread_mutex_unlock(&s->lock);
	resubmit(s, t);
}

static struct libusb_transfer *make_iso(struct stream *s, unsigned char ep, int pkts, int pkt_size,
					libusb_transfer_cb_fn cb)
{
	struct libusb_transfer *t = libusb_alloc_transfer(pkts);
	unsigned char *buf = calloc((size_t)pkts, (size_t)pkt_size);
	libusb_fill_iso_transfer(t, s->d->h, ep, buf, pkts * pkt_size, pkts, cb, s, 0);
	libusb_set_iso_packet_lengths(t, (unsigned)pkt_size);
	return t;
}

struct stream *stream_start(struct device *d, struct ring *ring, unsigned rate, char *err, size_t errlen)
{
	struct stream *s = calloc(1, sizeof(*s));
	int in_size = (int)((rate * 3 / 1000) & ~7u) * IN_FRAME_SIZE;

	s->d = d;
	s->ring = ring;
	pacer_init(&s->pacer, rate);
	pthread_mutex_init(&s->lock, NULL);

	for (int i = 0; i < OUT_XFERS; i++) {
		s->out[i] = make_iso(s, DYNACORD_EP_PCM_OUT, OUT_PKTS,
				     DYNACORD_OUT_MAX_FRAMES_PER_PKT * DYNACORD_OUT_FRAME_SIZE, out_cb);
		fill_out(s, s->out[i]);
	}
	for (int i = 0; i < FB_XFERS; i++)
		s->fb[i] = make_iso(s, DYNACORD_EP_FEEDBACK, FB_PKTS, FB_PKT_SIZE, fb_cb);
	for (int i = 0; i < IN_XFERS; i++) {
		s->in[i] = libusb_alloc_transfer(0);
		libusb_fill_bulk_transfer(s->in[i], d->h, DYNACORD_EP_PCM_IN, malloc((size_t)in_size),
					  in_size, in_cb, s, 0);
	}

	struct libusb_transfer **all[] = { s->fb, s->in, s->out };
	int counts[] = { FB_XFERS, IN_XFERS, OUT_XFERS };
	for (int g = 0; g < 3 && !atomic_load(&s->failed); g++) {
		for (int i = 0; i < counts[g]; i++) {
			int r = libusb_submit_transfer(all[g][i]);
			if (r < 0) {
				snprintf(err, errlen, "submit transfer on ep %02x: %s", all[g][i]->endpoint,
					 libusb_strerror(r));
				atomic_store(&s->failed, 1);
				break;
			}
			atomic_fetch_add(&s->active, 1);
		}
	}
	pthread_create(&s->thread, NULL, event_thread, s);
	if (atomic_load(&s->failed)) {
		stream_stop(s);
		return NULL;
	}
	return s;
}

void stream_stop(struct stream *s)
{
	struct libusb_transfer **all[] = { s->fb, s->in, s->out };
	int counts[] = { FB_XFERS, IN_XFERS, OUT_XFERS };

	if (!atomic_load(&s->failed)) {
		atomic_store(&s->silence, 1);
		usleep(OUT_XFERS * OUT_PKTS * 125 + 10000);
	}
	atomic_store(&s->stopping, 1);
	/* Cancel repeatedly: a callback already running when stopping was set
	 * may resubmit its transfer once more. */
	for (int waited = 0; atomic_load(&s->active) > 0 && waited < 2000; waited += 10) {
		for (int g = 0; g < 3; g++)
			for (int i = 0; i < counts[g]; i++)
				libusb_cancel_transfer(all[g][i]);
		usleep(10000);
	}
	int stuck = atomic_load(&s->active);
	atomic_store(&s->stop_events, 1);
	pthread_join(s->thread, NULL);
	if (stuck > 0) {
		/* Transfers still owned by libusb can't be freed safely. */
		fprintf(stderr, "ploytec-play: %d transfer(s) did not finish cancelling\n", stuck);
		return;
	}
	for (int g = 0; g < 3; g++) {
		for (int i = 0; i < counts[g]; i++) {
			free(all[g][i]->buffer);
			libusb_free_transfer(all[g][i]);
		}
	}
	pthread_mutex_destroy(&s->lock);
	free(s);
}

void stream_set_draining(struct stream *s)
{
	atomic_store(&s->draining, 1);
}

void stream_stats(struct stream *s, struct stats *out)
{
	pthread_mutex_lock(&s->lock);
	*out = s->stats;
	memset(s->stats.in.peak, 0, sizeof(s->stats.in.peak));
	pthread_mutex_unlock(&s->lock);
}

int stream_failed(struct stream *s)
{
	return atomic_load(&s->failed);
}
