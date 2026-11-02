#ifndef PLOYTEC_PLAY_TEST_H
#define PLOYTEC_PLAY_TEST_H

#include <stdio.h>

extern int test_failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		test_failures++; \
	} \
} while (0)

#define CHECK_EQ(a, b) do { \
	long long _a = (long long)(a), _b = (long long)(b); \
	if (_a != _b) { \
		fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %lld, expected %s == %lld\n", \
			__FILE__, __LINE__, #a, _a, #b, _b); \
		test_failures++; \
	} \
} while (0)

#endif
