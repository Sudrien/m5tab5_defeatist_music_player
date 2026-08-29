/*
 * replaygain.h -- one sidecar file per track, holding whatever has been
 * measured about it so far.
 *
 * The file is JSON Lines: one JSON object per line, and the last line
 * that parses and matches the track wins. It is still named
 * "." + filename + ".rgcache" -- a dotfile, which storage_is_hidden()
 * already excludes from every listing and playlist scan, so nothing had
 * to learn to hide it.
 *
 * WHY JSONL RATHER THAN A PACKED STRUCT
 *
 * The first version of this file was a packed C struct, which is
 * smaller and faster to parse and was the wrong shape for what this
 * actually is. Two things pushed it over:
 *
 *   1. The sections arrive at different times. The waveform comes from
 *      a frame walk, which can happen during a prefetch of a track
 *      nobody has played yet. The loudness comes from a full,
 *      uninterrupted play. Neither waits for the other, so a write of
 *      one must not clobber the other -- which is a read-modify-write,
 *      and a struct that grows a field is a version bump that discards
 *      every sidecar on the card.
 *   2. More metadata is expected. A format where adding a field costs
 *      nothing and old readers ignore what they do not know is worth
 *      more here than the bytes it costs.
 *
 * An append is also its own atomicity story: a line is written whole or
 * it is not, and a torn final line -- power lost mid-write -- is
 * detected by the reader, which falls back to the previous line. That
 * replaced the temp-file-and-rename the packed version used. Every line
 * therefore repeats filesize and mtime rather than relying on the first
 * one, so any line is self-describing and the fallback is a real
 * fallback rather than a line with no key on it.
 *
 * The file is compacted -- rewritten as a single line -- once it grows
 * past REPLAYGAIN_COMPACT_BYTES, so re-measuring a track repeatedly
 * does not grow its sidecar without bound.
 *
 * TWO VERSIONS, DELIBERATELY
 *
 *   REPLAYGAIN_FORMAT_VERSION   the shape of the record
 *   LOUDNESS_VERSION            what the loudness numbers MEAN
 *
 * They change for different reasons and must not invalidate each other.
 * Adding a field is a format bump and should not throw away a perfectly
 * good loudness measurement; changing the gate, the weighting, or
 * sample peak to true peak is a loudness bump and should not throw away
 * a waveform that took a whole-file read to produce. A stale loudness
 * version is reported as has_loudness = false, so the next full play
 * recomputes just that section.
 *
 * WHAT IS IN A LINE
 *
 *   {"format_version":1,
 *    "filesize":8760320,"mtime":1735500000,
 *    "waveform":{"has_levels":true,"frames":10475,"sec":273,
 *                "columns":720,"level":"<base64>"},
 *    "loudness":{"version":1,"integrated_lufs":-18.4,
 *                "sample_peak_dbfs":-1.2,"blocks":2711}}
 *
 * Either section may be absent. `columns` is what was actually filled,
 * NOT always REPLAYGAIN_COLUMNS: a track too short to produce a full
 * envelope stores the columns it has and the UI stretches them across
 * the bar, which is better than padding with silence that never
 * happened.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#include "framewalk.h"
#include "loudness.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The most columns a waveform is stored with. Pinned rather than
 * aliased to LCD_H_RES: this is a file format, and the panel changing
 * should not silently change what is on the card. A walk that produced
 * more than this is resampled down; one that produced fewer is stored
 * at its own width, see `columns` in replaygain_waveform_t.
 */
#define REPLAYGAIN_COLUMNS          (720)

/* The shape of the record. See "TWO VERSIONS" above. */
#define REPLAYGAIN_FORMAT_VERSION   (1)

/* Rewrite the file as one line once it passes this. Four or five lines
 * of a 720-column waveform, so an ordinary sidecar never reaches it and
 * one that has been re-measured repeatedly gets tidied. */
#define REPLAYGAIN_COMPACT_BYTES    (8192)

typedef struct {
    bool     present;       /* is any of the rest of this meaningful */
    bool     has_levels;    /* false for AAC/AMR -- a frame count but no
                             * per-frame loudness, so `level` is flat */
    uint32_t frames;
    uint32_t sec;
    int      columns;       /* what was actually filled, <= REPLAYGAIN_COLUMNS */
    uint8_t  level[REPLAYGAIN_COLUMNS];
} replaygain_waveform_t;

typedef struct {
    bool     present;       /* false when absent, or written by a
                             * different LOUDNESS_VERSION */
    float    integrated_lufs;
    float    sample_peak_dbfs;
    uint32_t blocks;        /* gated blocks; 0 means the answer came
                             * from the ungated fallback and is thin --
                             * see "SHORT TRACKS" in loudness.h */
} replaygain_loudness_t;

typedef struct {
    uint32_t filesize;
    int64_t  mtime;
    replaygain_waveform_t waveform;
    replaygain_loudness_t loudness;
} replaygain_t;

/*
 * Read the sidecar for `path` (the track, not the sidecar).
 *
 * Fills *out with whatever is present and current. Returns false only
 * when there is nothing usable at all -- no file, no parsable line, or
 * a size/mtime that no longer matches the track. A file with a waveform
 * and no loudness returns true with `out->loudness.present` false, and
 * that is the ordinary case for a track that has been walked but never
 * played to the end.
 *
 * Opens its own handle; safe from any task.
 */
bool replaygain_load(const char *path, replaygain_t *out);

/*
 * Store the waveform section, leaving any loudness section alone.
 *
 * Resamples the walk down to REPLAYGAIN_COLUMNS if it produced more,
 * using the max-per-bucket rule framewalk.c uses -- max rather than
 * mean, so a transient survives the second downsample instead of being
 * averaged into the floor twice. A walk with fewer columns is stored at
 * its own width.
 */
esp_err_t replaygain_save_waveform(const char *path, const framewalk_t *w);

/*
 * Store the loudness section, leaving any waveform section alone.
 *
 * Called once, after a track has played start to finish without a seek.
 * `blocks` is loudness_finish()'s gated block count and is stored as
 * the confidence figure rather than being turned into a boolean here.
 */
esp_err_t replaygain_save_loudness(const char *path, float integrated_lufs,
                                   float sample_peak_dbfs, uint32_t blocks);

#ifdef __cplusplus
}
#endif
