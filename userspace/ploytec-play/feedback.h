#ifndef PLOYTEC_PLAY_FEEDBACK_H
#define PLOYTEC_PLAY_FEEDBACK_H

#include <stdint.h>

/*
 * Parse one iso packet from the feedback endpoint. Each packet is a sliding
 * window of the frames the device consumed in each of the last 3 ms, newest
 * first. Returns the newest count, or -1 if the packet is empty.
 */
int feedback_parse(const uint8_t *pkt, int len);

#endif
