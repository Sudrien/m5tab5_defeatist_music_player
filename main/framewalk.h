/*
 * framewalk.h -- the envelope carrier type.
 *
 * This file used to declare a scanner that read global_gain out of
 * every MP3 frame header to build a seek-bar envelope. That number is
 * the encoder's quantisation-step choice -- how many bits a granule was
 * worth -- which correlates with loudness because a busy passage gets
 * more bits, and is not loudness. It was a whole-file read producing a
 * proxy.
 *
 * The envelope now comes from loudness.c, off the decoded PCM of a
 * normal play: real peak magnitude, no extra read, and it arrives as a
 * by-product of listening rather than as a scan scheduled around the
 * ring. The scanner is gone; what is left is the struct the cache, the
 * sidecar and waveform.c all pass around, kept under its old name
 * because renaming it would touch far more than it would explain.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAMEWALK_MAX_COLUMNS   (1024)

typedef struct {
    uint32_t sec;           /* 0 when the length is not known yet */

    /*
     * Amplitude per column, 0-255, measured off the decoded PCM by
     * loudness.c and resampled to whatever the doubling landed on.
     *
     * has_levels is vestigial and deliberately kept. It dates from the
     * frame walk, where it was false for the formats whose headers state
     * a frame length but nothing about the content -- AAC and AMR --
     * which produced a duration and no envelope. The walk is gone (0206)
     * and both remaining writers set it true unconditionally, so the test
     * in waveform.c can no longer fail on it. It stays because it is
     * still read there and removing it is a behaviour change rather than
     * a struct trim: the guard would have to go with it, and a NULL or
     * zero-column envelope would then be one test away from being drawn.
     *
     * `frames` and `rate` were the other two survivors and are gone as of
     * 1001 -- nothing wrote them after the walk was deleted, so every
     * reader saw the zeros memset() left.
     */
    bool has_levels;
    int columns;
    uint8_t level[FRAMEWALK_MAX_COLUMNS];
} framewalk_t;

#ifdef __cplusplus
}
#endif
