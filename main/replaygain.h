/*
 * replaygain.h -- a per-file sidecar so the frame walk is a once-per-file
 * cost rather than a once-per-play one.
 *
 * framewalk.c already produces everything a ReplayGain-style pass needs
 * -- global_gain is a real per-granule loudness read at a fixed bit
 * offset, no decode involved -- but it is a whole-file read, and paying
 * that on every play of every track is the thing CLAUDE.md names as the
 * reason WAVEFORM_SCAN is off. A sidecar next to the file is what turns
 * "read the file" into "read a few hundred bytes", for every play after
 * the first.
 *
 * WHAT LIVES IN THE SIDECAR
 *
 *   filesize     4 bytes, from fstat()
 *   mtime        8 bytes, from fstat(), so an edited file invalidates
 *   waveform     the walk's envelope, resampled to REPLAYGAIN_COLUMNS
 *                (720, matching LCD_H_RES -- see below) and stored as
 *                one byte per column, "raw" bytes in the sense that
 *                framewalk_t's level[] already is: 0-255, no further
 *                encoding
 *
 * Deliberately not the ReplayGain dB/peak pair (yet -- see "What this
 * does not do yet"). The waveform is the part that has to travel with
 * the record either way, since a UI redraw needs it and a gain
 * computation does not, so it goes in first and the record is shaped to
 * hold the rest without a version bump.
 *
 * WHY 720, NOT FRAMEWALK_MAX_COLUMNS
 *
 * framewalk_t can carry up to FRAMEWALK_MAX_COLUMNS (1024) so the same
 * scan buffer would fit a wider panel without a rescan. The panel this
 * firmware actually draws to is fixed at LCD_H_RES = 720, and the
 * sidecar is a file format, not an in-memory buffer -- there is no
 * reason to spend flash-card bytes and a resample step on 304 columns
 * nothing here will ever read. REPLAYGAIN_COLUMNS is pinned at 720
 * rather than aliased to LCD_H_RES so the on-disk format has a value
 * that does not silently change if the panel does; bumping it is a
 * format version bump (see below), not a #define edit.
 *
 * KEYING: SIZE AND MTIME, NOT A HASH
 *
 * A content hash needs to read the file to invalidate the cache of not
 * having to read the file, which defeats the point. Size and mtime are
 * exactly what the browser's fstat() already has to hand back, and they
 * catch the case that matters -- the file on the card changed under
 * this sidecar -- without adding a second whole-file pass to the design
 * this exists to remove one from.
 *
 * WHERE THE SIDECAR LIVES
 *
 * Next to the track, named "." + the track's filename + ".rgcache" --
 * a dotfile, which storage_is_hidden() already excludes from directory
 * listings for exactly this reason (it also covers the AppleDouble
 * sidecars macOS leaves). No new hiding logic is needed; the browser
 * and playlist scanners already skip anything starting with '.'.
 *
 * ATOMICITY
 *
 * Written to a ".tmp" suffix and renamed over the final name.  A crash
 * or a card pulled mid-write leaves either the old sidecar (rename never
 * happened) or a stray ".tmp" (ignored by every reader, and overwritten
 * by the next write) -- never a half-written file masquerading as a
 * valid cache entry that replaygain_load() would otherwise trust.
 *
 * WHAT THIS DOES NOT DO YET
 *
 *  - No integrated/peak gain values. computing them from framewalk_t's
 *    per-granule levels is a separate patch from wiring the sidecar
 *    itself; REPLAYGAIN_VERSION exists so adding them later is a clean
 *    version bump rather than a reinterpretation of old sidecars.
 *  - No seek index. CLAUDE.md notes the sidecar "can carry the seek
 *    index too, which would let MP3D_DO_NOT_SCAN remove what is left of
 *    the open" -- also future, also why the header is versioned rather
 *    than assumed frozen.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#include "framewalk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* On-disk column count. Not LCD_H_RES -- see "WHY 720" above. Bumping
 * this is a REPLAYGAIN_VERSION bump; old sidecars are simply rewritten,
 * they are never partially reinterpreted. */
#define REPLAYGAIN_COLUMNS      (720)

/* Bump whenever the on-disk layout changes shape. replaygain_load()
 * refuses anything else and the caller re-walks and rewrites, the same
 * path taken for a missing or stale sidecar -- a version mismatch is not
 * a corrupt file, it is a file written by different code. */
#define REPLAYGAIN_VERSION      (2)

/* In memory, this is what a completed lookup or a completed walk hands
 * around. On disk it is this same shape, minus the pointer indirection
 * -- see replaygain.c for the packed record.
 */
typedef struct {
    uint32_t filesize;
    int64_t  mtime;                          /* seconds, from st_mtime */
    bool     has_levels;                     /* mirrors framewalk_t; false
                                               * for AAC/AMR, where waveform
                                               * is written flat-zero and
                                               * must not be drawn as real */
    uint32_t frames;                         /* mirrors framewalk_t.frames */
    uint32_t sec;                            /* mirrors framewalk_t.sec --
                                               * the duration for formats
                                               * with no other source (raw
                                               * ADTS, AMR); 0 elsewhere */
    uint8_t  waveform[REPLAYGAIN_COLUMNS];    /* 0-255, raw levels */
} replaygain_t;

/*
 * Look up the sidecar for `path` (the track's own path, not the
 * sidecar's) and fill *out if it exists, matches the file's current
 * size and mtime, and was written by this version of the format.
 *
 * Blocking, and a handful of small reads -- no full-file pass. Safe to
 * call from any task; it opens its own handle via storage_io_open() and
 * does not touch the decoder's.
 *
 * Returns false on a cache miss for any reason: no sidecar, size or
 * mtime mismatch, version mismatch, truncated or unreadable file. Every
 * one of those is answered the same way by the caller -- walk the track
 * and call replaygain_save() -- so the reason is logged here rather than
 * distinguished in the return value.
 */
bool replaygain_load(const char *path, replaygain_t *out);

/*
 * Write the sidecar for `path` from a completed framewalk_t.
 *
 * Resamples w->level[] (up to FRAMEWALK_MAX_COLUMNS, however many the
 * walk actually filled) down to REPLAYGAIN_COLUMNS the same way
 * waveform.c resamples for the bar -- max per bucket, not mean, so a
 * transient that made the track's shape recognisable does not get
 * averaged back into the noise floor a second time.
 *
 * Written via a temp file and renamed into place; see "ATOMICITY" above.
 * Failure is logged and otherwise ignored by the caller -- a sidecar
 * that fails to write costs the next play a re-walk, which is the state
 * every play was in before this file existed.
 */
esp_err_t replaygain_save(const char *path, const framewalk_t *w);

#ifdef __cplusplus
}
#endif
