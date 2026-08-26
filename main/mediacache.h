/*
 * mediacache.h -- a tiny MRU cache of the two slow per-track artefacts.
 *
 * Both the cover and the envelope are expensive for the same reason:
 * they come off the same slow device the decoder is reading, and both
 * are thrown away the moment the track changes. That is fine going
 * forward once -- but it makes going back cost exactly as much as
 * arriving somewhere new, and it means the next track's cover cannot
 * start arriving until after it is already playing.
 *
 * Three entries is the whole design: previous, current, next. That is
 * what a back button and a prefetch between them can want at once, and
 * a fourth would only be a track nobody is about to look at.
 *
 * WHAT IS STORED, AND WHAT DELIBERATELY IS NOT
 *
 *   id3_tags_t           192 B     stored
 *   framewalk_t          ~1 KB     stored
 *   cover, compressed    80-120 KB stored
 *   "this file has no cover"       stored, as one bool
 *   cover, decoded       ~980 KB   NOT stored
 *
 * The decoded 700x700 RGB565 frame is a megabyte and the hardware JPEG
 * codec turns the compressed bytes back into one in single-digit
 * milliseconds. Caching the decode would cost forty times the memory to
 * save a delay nobody can perceive. (The 550 ms figure in player.c is a
 * 3000x3000 cover, which is a different animal and still not worth a
 * megabyte of cache.)
 *
 * THREADING
 *
 * There are two callers now, and the contract is split down the middle
 * because of it.
 *
 * This file used to be media_task's alone, which is why it had no lock:
 * borrowed pointers were safe because the only task that could evict one
 * was the task holding it. The decode loop broke that when it started
 * asking for tags and the envelope at the instant of a track change --
 * which it has to, since the entire value of prefetching them is that
 * they are on screen before anything slow has run.
 *
 * So there is a mutex, and:
 *
 *   Safe from any task    mediacache_tags(), mediacache_walk_copy(),
 *                         mediacache_pin(), mediacache_unpin_all()
 *                         -- they copy out, or touch only flags.
 *
 *   media_task only       mediacache_art(), mediacache_walk(), every
 *                         mediacache_put_*(), mediacache_clear()
 *                         -- they borrow past the lock, or they evict.
 *
 * The rule behind the split is unchanged: a borrowed pointer is bounded
 * by the next eviction, and only the borrower is allowed to evict. The
 * decode loop therefore never stores anything here. media_task caches
 * what the decode loop read, which costs one small read per track and
 * keeps eviction in one place.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "albumart.h"        /* id3_tags_t */
#include "framewalk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Previous, current, next. See above. */
#define MEDIACACHE_ENTRIES  (3)

void mediacache_init(void);

/*
 * Look up the cover for a path.
 *
 * Returns a BORROWED pointer owned by the cache, valid until the next
 * mediacache_put_*() that evicts this entry. Do not free it. Marks the
 * entry as most recently used.
 *
 * NULL when the path is not cached, which is the ordinary case and not
 * an error.
 */
const uint8_t *mediacache_art(const char *path, size_t *len);

/*
 * Store a cover. The cache TAKES OWNERSHIP of img and will free() it.
 *
 * img must come from the ordinary heap, because that is what
 * covertag_extract_art() returns and re-homing it into PSRAM would mean
 * a copy of the thing being cached to avoid a copy of the thing being
 * cached.
 */
void mediacache_put_art(const char *path, uint8_t *img, size_t len);

/* Borrowed, same contract as mediacache_art(). NULL when absent.
 * media_task only. */
const framewalk_t *mediacache_walk(const char *path);

/* The same lookup, copied into the caller's storage. Safe from any task,
 * and the only form the decode loop may use -- a track change installs a
 * prefetched envelope through this, on the decode loop, before the
 * decoder has opened the file. */
bool mediacache_walk_copy(const char *path, framewalk_t *out);

/* Copied in -- a framewalk_t is a kilobyte and the caller's copy is
 * about to be overwritten by the next scan. */
void mediacache_put_walk(const char *path, const framewalk_t *w);

/*
 * Protect a path from eviction, and release the protection.
 *
 * The playing track and the one behind it are both pinned, so a prefetch
 * of the next track cannot evict the thing a back button is about to
 * want. With three entries and two pins there is exactly one slot for
 * prefetch to use, which is the intended shape rather than a shortage.
 */
/*
 * Title, artist and album.
 *
 * Copied out rather than borrowed -- 192 bytes is cheaper to copy than
 * to reason about, and unlike the cover this one is read by a caller
 * (the decode loop, through load_tags()) that is not media_task. The
 * borrow contract in this file is only safe within media_task; a copy is
 * what lets the tags leave it.
 *
 * out may be NULL, which makes this an existence test.
 */
bool mediacache_tags(const char *path, id3_tags_t *out);
void mediacache_put_tags(const char *path, const id3_tags_t *t);

/*
 * "There is no picture in this file", remembered.
 *
 * Without it, a file with no cover is indistinguishable from one that
 * has simply not been read yet, so every return to it re-reads the tag
 * to find nothing again -- and prefetch has no way to report the useful
 * half of what it learned. The negative is as much of an answer as the
 * positive and costs one bool.
 */
bool mediacache_no_art(const char *path);
void mediacache_put_no_art(const char *path);

void mediacache_pin(const char *path);
void mediacache_unpin_all(void);

/* Everything goes, pins included. For a volume disappearing: the paths
 * in here point at files that are no longer reachable. */
void mediacache_clear(void);

/* For logging. */
void mediacache_stats(int *entries, size_t *bytes);

#ifdef __cplusplus
}
#endif
