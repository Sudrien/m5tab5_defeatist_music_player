/*
 * medialib.h -- the media index on a real volume: mediasync.c's engine
 * bound to the catalog (mediacat.c), the walk (mediawalk.c), the tag
 * readers (covertag.c, cuedir.c) and the clock.
 *
 * Two files per volume, both at the root beside .defeatist.dat, both
 * dotted and FAT-hidden: the catalog `.defeatist.cat` (mediacat.h) and
 * the index `.defeatist.ix1`, with `.defeatist.ixn` while a new one is
 * being written.
 *
 * THE VERSION IS IN THE NAME. MEDIA-INDEX.md settled that a format
 * change rebuilds the index rather than migrating it, and the cheapest
 * way to make an old index unreadable to a new build is for the new
 * build to look for a different file: `.ix2` next time. The old file is
 * then just a dotfile nobody reads; the build that bumps the name should
 * remove the old one, since this one cannot know what it will be.
 *
 * NOTHING CALLS THIS YET. When something does, it runs on a task with
 * room for it -- the engine's state is static, but covertag and cuedir
 * read on the caller's stack -- and only one volume at a time.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "mediasync.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIALIB_INDEX_NAME     ".defeatist.ix1"
#define MEDIALIB_TEMP_NAME      ".defeatist.ixn"

/*
 * Reconcile one mounted volume's index with its card. Blocking: a walk
 * of the whole volume, a tag read per new or changed track. `abort` is
 * polled per track and may be NULL. Stats may be NULL. Logs a summary.
 */
msync_result_t medialib_reconcile(storage_id_t vol, const volatile bool *abort,
                                  msync_stats_t *stats);

#ifdef __cplusplus
}
#endif
