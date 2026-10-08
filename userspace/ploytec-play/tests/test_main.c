#include <stdint.h>
#include "test.h"
#include "../feedback.h"
#include "../pacer.h"

int test_failures;

static void test_feedback(void)
{
	const uint8_t steady[] = { 0x60, 0x60, 0x60 };
	const uint8_t newest_45[] = { 0x2d, 0x2c, 0x2c };
	const uint8_t older_45[] = { 0x2c, 0x2d, 0x2c };

	CHECK_EQ(feedback_parse(steady, 3), 96);
	CHECK_EQ(feedback_parse(newest_45, 3), 45);
	CHECK_EQ(feedback_parse(older_45, 3), 44);
	CHECK_EQ(feedback_parse(steady, 0), -1);
	CHECK_EQ(feedback_parse(steady, 1), 96);
}

static void test_pacer_nominal(void)
{
	static const unsigned rates[] = { 44100, 48000, 96000 };
	for (unsigned r = 0; r < 3; r++) {
		struct pacer p;
		pacer_init(&p, rates[r]);
		long total = 0;
		int lo = 99, hi = 0;
		for (int i = 0; i < 8000; i++) {
			int n = pacer_next(&p);
			total += n;
			if (n < lo) lo = n;
			if (n > hi) hi = n;
		}
		CHECK_EQ(total, rates[r]);
		CHECK(hi - lo <= 1);
	}
}

static void test_pacer_spread(void)
{
	for (int total = 0; total <= 8 * 13; total++) {
		int sum = 0;
		for (int slot = 0; slot < 8; slot++) {
			int n = pacer_spread(total, slot);
			sum += n;
			CHECK(n >= total / 8);
			CHECK(n <= (total + 7) / 8);
		}
		CHECK_EQ(sum, total);
	}
}

static void test_pacer_feedback(void)
{
	struct pacer p;
	pacer_init(&p, 96000);

	/* A short millisecond gives exactly one 11-frame packet in the next group. */
	pacer_feedback(&p, 95);
	int sum = 0, elevens = 0;
	for (int i = 0; i < 8; i++) {
		int n = pacer_next(&p);
		sum += n;
		elevens += n == 11;
	}
	CHECK_EQ(sum, 95);
	CHECK_EQ(elevens, 1);

	/* Out-of-range and negative readings are ignored and counted. */
	pacer_feedback(&p, 96);
	pacer_feedback(&p, 200);
	pacer_feedback(&p, -1);
	pacer_feedback(&p, 0);
	CHECK_EQ(p.invalid, 3);
	sum = 0;
	for (int i = 0; i < 8; i++)
		sum += pacer_next(&p);
	CHECK_EQ(sum, 96);

	/* Feedback that arrives mid-group takes effect at the next group. */
	pacer_init(&p, 48000);
	for (int i = 0; i < 3; i++)
		CHECK_EQ(pacer_next(&p), 6);
	pacer_feedback(&p, 47);
	sum = 0;
	for (int i = 0; i < 5; i++)
		sum += pacer_next(&p);
	CHECK_EQ(sum, 30);
	sum = 0;
	for (int i = 0; i < 8; i++)
		sum += pacer_next(&p);
	CHECK_EQ(sum, 47);
}

int main(void)
{
	test_feedback();
	test_pacer_nominal();
	test_pacer_spread();
	test_pacer_feedback();
	if (test_failures) {
		fprintf(stderr, "%d failure(s)\n", test_failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
