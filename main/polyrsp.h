/*
 * polyrsp -- a small fixed-point polyphase resampler for the USB path.
 *
 * Stereo interleaved int16 in, the same out, at in_rate * L / M where
 * L/M is the reduced ratio of the two rates. One Kaiser-windowed sinc,
 * cut at 0.45 of the lower rate, split into L phases of POLYRSP_TAPS
 * taps (more when converting down). Q15 coefficients, int32 sums: each
 * output sample is TAPS multiply-adds and nothing else, so 44.1 -> 48
 * kHz stereo is about 3 M multiply-adds a second.
 *
 * polyrsp_open() designs the filter (floating point, a few ms) and is
 * meant for the decode task; polyrsp_process() is the per-block half.
 * See 5039 in ARCHITECTURE.md for why this exists.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct polyrsp polyrsp_t;

/* NULL if the ratio needs more coefficients than it is worth keeping
 * (POLYRSP_MAX_COEFS) or memory runs out. max_in_frames is the most
 * polyrsp_process() will ever be handed at once. */
polyrsp_t *polyrsp_open(uint32_t in_rate, uint32_t out_rate, uint32_t max_in_frames);
void       polyrsp_close(polyrsp_t *r);

/* Clears the filter history to silence, as after a seek. */
void       polyrsp_reset(polyrsp_t *r);

/* The most frames one call can return for max_in_frames of input. */
uint32_t   polyrsp_max_out(const polyrsp_t *r);

/* Converts in_frames (<= max_in_frames) and returns how many frames it
 * wrote to out. The filter's history carries across calls. */
uint32_t   polyrsp_process(polyrsp_t *r, const int16_t *in, uint32_t in_frames,
                           int16_t *out);
