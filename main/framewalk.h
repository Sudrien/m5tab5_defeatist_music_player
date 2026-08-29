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
    uint32_t frames;        /* frames seen */
    uint32_t rate;          /* Hz, from the first header */
    uint32_t sec;           /* 0 when the format's frame size is unknown */

    /*
     * Loudness per column, 0-255, already resampled from however many
     * frames were found.
     *
     * has_levels is false for the formats where the header alone does not
     * carry a loudness -- AAC and AMR state a frame length but nothing
     * about the content. Those still produce a duration; the caller has
     * to decide whether a waveform without them is worth a real decode.
     */
    bool has_levels;
    int columns;
    uint8_t level[FRAMEWALK_MAX_COLUMNS];
} framewalk_t;

#ifdef __cplusplus
}
#endif
