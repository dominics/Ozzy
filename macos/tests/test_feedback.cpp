/* SPDX-License-Identifier: MIT */
#include "../../common/devices/ploytec/dynacord_feedback.h"
#include "../../common/devices/ploytec/dynacord_pacer.h"
#include "test.h"
#include "tests.h"

void test_feedback_parse()
{
	const uint8_t win48[3] = { 48, 48, 49 };   /* newest first */
	const uint8_t win44[3] = { 44, 45, 44 };
	CHECK_EQ(feedback_parse(win48, 3), 48);
	CHECK_EQ(feedback_parse(win44, 3), 44);
	CHECK_EQ(feedback_parse(win48, 1), 48);
	CHECK_EQ(feedback_parse(win48, 0), -1);
}

void test_feedback_out_of_range_rejected_by_pacer()
{
	struct pacer p;
	pacer_init(&p, 48000);
	pacer_feedback(&p, 200);
	pacer_feedback(&p, 0);
	pacer_feedback(&p, 45);
	CHECK_EQ(p.invalid, 3);
	pacer_feedback(&p, 50);
	CHECK_EQ(p.invalid, 3);
}
