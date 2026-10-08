#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "device.h"
#include "init.h"
#include "ring.h"
#include "source.h"
#include "stream.h"

static volatile sig_atomic_t interrupted;

static void on_signal(int sig)
{
	(void)sig;
	interrupted = 1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: ploytec-play [--rate HZ] [--tone HZ | --wav FILE] [--channel N|all]\n"
		"                    [--level DBFS] [--seconds N] [--verbose]\n"
		"  --rate     44100, 48000 (default) or 96000\n"
		"  --tone     sine frequency (default 1000 Hz when no --wav)\n"
		"  --wav      play a WAV at --rate; channels 1-4 map to outputs 1-4, mono goes to --channel\n"
		"  --channel  output 1-4 for tone or mono WAV, or 'all' (default all)\n"
		"  --level    tone level in dBFS (default -12)\n"
		"  --seconds  stop after N seconds (default: until Ctrl-C or end of WAV)\n");
}

static void print_stats(const struct stats *now, const struct stats *prev, double t)
{
	unsigned long long fbc = now->fb_count - prev->fb_count;
	double fmean = fbc ? (double)(now->fb_sum - prev->fb_sum) / (double)fbc : 0.0;
	fprintf(stderr, "t=%5.1fs frames=%llu F=%.3f pkts", t, now->frames_sent, fmean);
	for (int i = 0; i <= DYNACORD_OUT_MAX_FRAMES_PER_PKT; i++)
		if (now->pkt_hist[i] - prev->pkt_hist[i])
			fprintf(stderr, " %d:%llu", i, now->pkt_hist[i] - prev->pkt_hist[i]);
	fprintf(stderr, " underruns=%llu iso_err=%llu fb_bad=%llu in=%lluB\n",
		now->underruns, now->iso_errors, now->fb_invalid, now->in_bytes);
}

/* Top up the ring from the tone or WAV; sets *eof when the WAV ends. */
static void refill(struct ring *ring, struct tone *tone, struct wav *wav, uint8_t *buf, size_t chunk, int *eof)
{
	while (!*eof && ring_space(ring) >= chunk) {
		size_t n = chunk;
		if (wav)
			n = wav_render(wav, buf, chunk);
		else
			tone_render(tone, buf, chunk);
		*eof = n == 0;
		ring_write(ring, buf, n);
	}
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "rate", required_argument, NULL, 'r' }, { "tone", required_argument, NULL, 't' },
		{ "wav", required_argument, NULL, 'w' }, { "channel", required_argument, NULL, 'c' },
		{ "level", required_argument, NULL, 'l' }, { "seconds", required_argument, NULL, 's' },
		{ "verbose", no_argument, NULL, 'v' }, { "help", no_argument, NULL, 'h' }, { 0 },
	};
	unsigned rate = 48000;
	double hz = 1000.0, dbfs = -12.0, seconds = 0;
	const char *wav_path = NULL;
	int channel = 0, verbose = 0, opt, rc = 0, eof = 0;
	char err[512];

	while ((opt = getopt_long(argc, argv, "r:t:w:c:l:s:vh", opts, NULL)) != -1) {
		switch (opt) {
		case 'r': rate = (unsigned)atoi(optarg); break;
		case 't': hz = atof(optarg); break;
		case 'w': wav_path = optarg; break;
		case 'c': channel = strcmp(optarg, "all") == 0 ? 0 : atoi(optarg); break;
		case 'l': dbfs = atof(optarg); break;
		case 's': seconds = atof(optarg); break;
		case 'v': verbose = 1; break;
		default: usage(); return opt == 'h' ? 0 : 2;
		}
	}
	if (rate != 44100 && rate != 48000 && rate != 96000) {
		fprintf(stderr, "ploytec-play: unsupported rate %u\n", rate);
		return 2;
	}
	if (channel < 0 || channel > DYNACORD_CHANNELS) {
		fprintf(stderr, "ploytec-play: --channel must be 1-%d or all\n", DYNACORD_CHANNELS);
		return 2;
	}

	struct tone tone;
	struct wav *wav = NULL;
	if (wav_path) {
		if (!(wav = wav_open(wav_path, rate, channel ? channel : 1, err, sizeof(err)))) {
			fprintf(stderr, "ploytec-play: %s\n", err);
			return 1;
		}
	} else {
		tone_init(&tone, rate, hz, dbfs, channel);
	}

	struct ring ring;
	size_t chunk = rate / 100;
	uint8_t *buf = malloc(chunk * DYNACORD_OUT_FRAME_SIZE);
	ring_init(&ring, rate / 4);

	struct device dev;
	struct stream *st = NULL;
	struct stats cur, prev = { 0 };
	if (device_open(&dev, verbose, err, sizeof(err)) < 0 || init_rate(dev.h, rate, verbose, err, sizeof(err)) < 0) {
		fprintf(stderr, "ploytec-play: %s\n", err);
		rc = 1;
		goto out;
	}

	refill(&ring, &tone, wav, buf, chunk, &eof);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (!(st = stream_start(&dev, &ring, rate, err, sizeof(err)))) {
		fprintf(stderr, "ploytec-play: %s\n", err);
		rc = 1;
		goto out;
	}
	fprintf(stderr, "streaming at %u Hz, Ctrl-C to stop\n", rate);

	struct timespec t0, now;
	double next_print = 1.0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		refill(&ring, &tone, wav, buf, chunk, &eof);
		if (eof)
			stream_set_draining(st);
		clock_gettime(CLOCK_MONOTONIC, &now);
		double t = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
		if (t >= next_print) {
			stream_stats(st, &cur);
			print_stats(&cur, &prev, t);
			/* The firmware halts all streaming, feedback included, when its
			 * OUT buffer runs dry; the host sees no error for that. */
			if (t >= 2.0 && cur.fb_count == prev.fb_count) {
				fprintf(stderr, "ploytec-play: device stopped streaming (no feedback for 1 s)\n");
				rc = 1;
				break;
			}
			prev = cur;
			next_print += 1.0;
		}
		if (stream_failed(st)) {
			fprintf(stderr, "ploytec-play: device stopped responding (unplugged?)\n");
			rc = 1;
			break;
		}
		if (interrupted || (seconds > 0 && t >= seconds) || (eof && ring_fill(&ring) == 0))
			break;
		usleep(5000);
	}
	stream_stats(st, &cur);
	if (cur.underruns || cur.iso_errors)
		fprintf(stderr, "finished with %llu underruns, %llu iso errors\n", cur.underruns, cur.iso_errors);

out:
	if (st)
		stream_stop(st);
	device_close(&dev);
	ring_free(&ring);
	free(buf);
	if (wav)
		wav_close(wav);
	return rc;
}
