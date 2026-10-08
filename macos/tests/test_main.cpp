/* SPDX-License-Identifier: MIT */
#include <cstdio>
#include "test.h"
#include "tests.h"

int test_failures;

int main()
{
	test_golden_pacer();
	if (test_failures) {
		std::fprintf(stderr, "%d failure(s)\n", test_failures);
		return 1;
	}
	std::printf("macos tests passed\n");
	return 0;
}
