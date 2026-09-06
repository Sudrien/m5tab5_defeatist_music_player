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
 *   cover, compressed    up to 4 MB    stored, SHARED between entries
 *   "this file has no cover"           stored, as one bool
 *   cover, decoded       ~980 KB       NOT stored
 *
 * The cover figure used to read "80-120 KB", which was an assumption
 * about album art and not a bound on anything. The bound is
 * COVERTAG_MAX_IMAGE, 4 MB, and 1006 measured three slots holding
 * 10935 KB. See "the covers are shared" below.
 *
 * The decoded 700x700 RGB565 frame is a megabyte and the hardware JPEG
 * codec turns the compressed bytes back into one in single-digit
 * milliseconds. Caching the decode would cost forty times the memory to
 * save a delay nobody can perceive. (The 550 ms figure in player.c is a
 * 3000x3000 cover, which is a different animal and still not worth a
 * megabyte of cache.)
 *
 * AND THE COVERS ARE SHARED (1102)
 *
 * An album has one picture and its tracks are consecutive, so previous,
 * current and next are usually three paths whose cover is the same
 * image. 1006 caught the cost of not knowing that: three slots, three
 * copies of one 3.7 MB PNG, 10935 KB of PSRAM next to the shadow buffer
 * and a decode ring with audio running through it.
 *
 * So the image is a refcounted blob and the entries point at it. On the
 * store path the incoming bytes are hashed (albumart_cover_hash, the
 * same function albumart_draw() uses) and compared against what is
 * already held; on a match of hash AND length AND a full memcmp the
 * duplicate is freed and the refcount goes up. An album that shares a
 * cover costs one copy instead of three.
 *
 * WHAT THIS DELIBERATELY IS NOT. 1006 listed three fixes and declined to
 * choose between them, because each is a behaviour change: capping the
 * cached size, downscaling before caching, or not prefetching past some
 * size. This is none of them. Every caller still gets back exactly the
 * bytes the file contained, covers of every size are still cached, and
 * prefetch still runs. That decision is still open and this patch does
 * not pre-empt it -- it only stops the cache paying three times for one
 * picture.
 *
 * It follows that the worst case is unchanged. Three tracks with three
 * different 4 MB covers still cost 12 MB, because they are three
 * different pictures and the cache is not permitted to have an opinion
 * about that. This helps the common case and does nothing for the bad
 * one, which is the honest description of it.
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
 *
 * Ownership is honoured on every path, including the two that do not
 * keep the pointer: when every slot is pinned, and when the same image
 * is already held under another path. In the second case img is freed
 * and the entry is pointed at the copy that already exists, so the
 * caller cannot tell the difference and must not look.
 */
void mediacache_put_art(const char *path, uint8_t *img, size_t len);

/*
 * The identity hash of the stored cover, or 0 when this path has none.
 *
 * This is the thing 1011 could not do. cover_hash() ran only inside
 * albumart_draw(), so the drawn cover had a hash and the prefetched one
 * did not, and a comparison at a track change had nothing to compare
 * against. Storing now hashes, so it does -- which is the precondition
 * for skipping a decode when the next track's cover is the picture
 * already on screen (1011b). Nothing compares them yet; 1011b still
 * needs somewhere to re-blit from, and that is unchanged by this.
 *
 * 0 is not a reachable hash value in practice and is used as "absent".
 * Safe from any task: it copies a word out.
 */
uint32_t mediacache_art_hash(const char *path);

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

/*
 * For logging.
 *
 * `bytes` counts a shared cover ONCE, because the number is there to say
 * what the cache is costing the heap and a blob held by three entries
 * costs its size once. `saved` is what the old accounting would have
 * reported over and above that -- the sharing, made visible, so a log
 * line can say whether it fired rather than leaving it to be inferred
 * from a total that got smaller. Either may be NULL.
 */
void mediacache_stats(int *entries, size_t *bytes, size_t *saved);

#ifdef __cplusplus
}
#endif
