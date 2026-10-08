#include <stdio.h>
#include <stdlib.h>
#include "device.h"
#include "init.h"

int main(int argc, char **argv)
{
	char err[512];
	struct device d;
	unsigned rate = argc > 1 ? (unsigned)atoi(argv[1]) : 48000;

	if (device_open(&d, 0, err, sizeof(err)) < 0 || init_rate(d.h, rate, 1, err, sizeof(err)) < 0) {
		fprintf(stderr, "ploytec-play: %s\n", err);
		device_close(&d);
		return 1;
	}
	fprintf(stderr, "init ok at %u Hz\n", rate);
	device_close(&d);
	return 0;
}
