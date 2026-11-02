#include "feedback.h"

int feedback_parse(const uint8_t *pkt, int len)
{
	if (len < 1)
		return -1;
	return pkt[0];
}
