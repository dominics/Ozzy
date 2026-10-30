/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/*
 * CoreAudio-side conversion for the CMS 600-3 over the frame-indexed rings
 * (see DynacordRing.h). Output: float x 4 -> S24_3LE x 4. Input: raw 64-byte
 * Ploytec capture frames -> float x 4, channels 1-4 of ploytec_decode_frame.
 */

/* Encode one frame of 4 float samples as S24_3LE, clamping to [-1, 1). */
void DynacordEncodeFrame(uint8_t dst[12], const float src[4]);

/* Write frameCount interleaved 4-channel frames at sampleTime. */
void DynacordWriteOutput(uint8_t* ring, const float* src, uint64_t sampleTime, uint32_t frameCount,
			 uint32_t ringFrames, uint32_t bytesPerFrame);

/* Read frameCount interleaved 4-channel frames from sampleTime. */
void DynacordReadInput(float* dst, const uint8_t* ring, uint64_t sampleTime, uint32_t frameCount,
		       uint32_t ringFrames, uint32_t bytesPerFrame);

/* Read frameCount mono frames of one 1-based channel (1-4) from sampleTime;
 * silence for any other channel. */
void DynacordReadInputChannel(float* dst, const uint8_t* ring, uint64_t sampleTime, uint32_t frameCount,
			      uint32_t ringFrames, uint32_t bytesPerFrame, uint32_t channel);

/* Silence the whole output ring. */
void DynacordClearOutput(uint8_t* ring, uint32_t bufferSize);
