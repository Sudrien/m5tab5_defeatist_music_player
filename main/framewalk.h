/*
 * framewalk.h -- one sequential pass over a compressed file, reading only
 * frame headers.
 *
 * Two things come out of it and they share every byte of the work:
 *
 *   1. A frame count, which is the duration for the formats that do not
 *      state one -- raw ADTS, AMR, and a CBR MP3 with no Xing header.
 *      duration.c handles everything that does state one, in a couple of
 *      fread()s, and is always tried first; this is the backstop, not a
 *      replacement.
 *   2. A loudness envelope for the waveform.
 *
 * The point is that neither costs a decode. Every frame header states its
 * own length, so the walk is header -> skip -> header, and the audio data
 * is never touched. On MP3 the loudness comes free too: global_gain sits
 * in the side info at a fixed bit offset, which is a genuine per-granule
 * loudness value read without a Huffman table in sight.
 *
 * That makes the walk I/O bound rather than CPU bound, which is the whole
 * reason it is fast enough to run while a track plays.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Columns the caller wants. The framed layout is about 548 and the fill
 * layout 720, so this covers both with room to spare. */
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

/*
 * Walk f from its current position to EOF.
 *
 * Blocking and sequential -- expects to be called from a low-priority
 * task after playback has started, not before it. The file position is
 * left at EOF; callers sharing a handle with a decoder must open their
 * own.
 *
 * `abort_flag`, when non-NULL, is polled every few hundred KB. Setting it
 * stops the walk and returns ESP_ERR_INVALID_STATE, which is how a track
 * change cancels a scan that is no longer wanted.
 */
esp_err_t framewalk_scan(FILE *f, int columns, volatile bool *abort_flag,
                         framewalk_t *out);

/*
 * Is there a walker for this file at all?
 *
 * Cheap -- reads the first few bytes. Exists so the caller can decline to
 * read a whole file for nothing: an Ogg has no frame walker here, so the
 * scan read 30 MB off the card, reported zero frames and drew nothing.
 * That is a second of contention with the decoder in exchange for a log
 * line saying it failed.
 */
bool framewalk_supports(FILE *f);

#ifdef __cplusplus
}
#endif
