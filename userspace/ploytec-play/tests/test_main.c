#include <stdint.h>
#include "test.h"
#include "../feedback.h"

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

int main(void)
{
	test_feedback();
	if (test_failures) {
		fprintf(stderr, "%d failure(s)\n", test_failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
