/* SPDX-License-Identifier: MIT */
/*
 * Dynacord CMS 600-3 constants for ploytec-play, on top of the shared Ploytec ones.
 */

#ifndef PLOYTEC_PLAY_DYNACORD_DEFS_H
#define PLOYTEC_PLAY_DYNACORD_DEFS_H

#include "ploytec_defs.h"

/*
 * Dynacord CMS 600-3 (old hardware revision with a Ploytec USB card).
 * Same vendor requests as the Xone devices, but PCM out is isochronous
 * S24_3LE with an explicit feedback endpoint. Endpoint values here are
 * full addresses including the direction bit.
 */
#define DYNACORD_VENDOR_ID              0x0562
#define DYNACORD_PID_CMS600_3           0x03EB
#define DYNACORD_EP_PCM_OUT             0x02    /* iso OUT, interface 0 alt 1 */
#define DYNACORD_EP_FEEDBACK            0x81    /* iso IN, interface 1 alt 1 */
#define DYNACORD_EP_PCM_IN              0x86    /* bulk IN, interface 1 alt 1 */
#define DYNACORD_EP_MIDI_OUT            0x04    /* bulk OUT, interface 0 alt 1 */
#define DYNACORD_EP_MIDI_IN             0x83    /* bulk IN, interface 0 alt 1 */
#define DYNACORD_EP_RATE_OUT            0x0002  /* SET_CUR wIndex for the PCM out rate */
#define DYNACORD_CHANNELS               4
#define DYNACORD_OUT_FRAME_SIZE         12      /* S24_3LE x 4 channels */
#define DYNACORD_OUT_MAX_FRAMES_PER_PKT 13      /* wMaxPacketSize 156 / 12 */
#define DYNACORD_PKTS_PER_MS            8       /* one iso packet per microframe */

#endif
