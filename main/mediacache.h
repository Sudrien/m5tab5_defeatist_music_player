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
 *   framewalk_t          ~1 KB     stored
 *   cover, compressed    80-120 KB stored
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
 * Everything here is called from media_task and nowhere else. That is
 * not incidental -- it is why there is no lock. Entries hand out
 * borrowed pointers, and a borrowed pointer is only safe because the
 * one task that could evict it is the task holding it. If a second
 * caller ever appears, this needs a mutex and the borrow contract needs
 * to become a copy.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Borrowed, same contract as mediacache_art(). NULL when absent. */
const framewalk_t *mediacache_walk(const char *path);

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
