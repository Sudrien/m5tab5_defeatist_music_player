/*
 * mediasync.h -- one reconcile: walk the card, merge it against the old
 * index, append what changed to the catalog, and write the new index.
 *
 * The engine only. It is handed the catalog, the tag reader and the
 * walk as functions, so texttest can run the whole merge against real
 * index files with a catalog in memory, and medialib.c binds it to
 * mediacat.c, covertag.c and mediawalk.c on the device.
 *
 * THE NEW INDEX IS WRITTEN AS THE MERGE GOES. The merge visits paths in
 * index order, and every step yields that path's new record -- a KEEP
 * copies the old one, anything else points at the line it just
 * appended -- so the new index streams to a temporary file with no sort
 * and no table in memory. When the merge completes, the old index is
 * removed and the new one renamed into its place.
 *
 * ANYTHING SHORT OF COMPLETION CHANGES NO INDEX. A stop, a failed walk,
 * disorder on either side, a catalog that will not take a line: the
 * temporary file is removed and the old index stands. Lines already
 * appended to the catalog are then pointed at by nothing. They are
 * harmless, and the next run appends its own.
 *
 * A DAMAGED OLD INDEX -- a record midx_rec_unpack() refuses, a size
 * that is not whole records, a long path the catalog does not confirm
 * -- fails the run AND removes the index, so the next run starts from
 * nothing: every track ADDed and its tags read again. That is the
 * rebuild MEDIA-INDEX.md settled on, and it is the only answer to an
 * index that cannot be believed.
 *
 * The one window: between removing the old index and renaming the new
 * one, a lost battery leaves no index, which is the same rebuild.
 *
 * NOT REENTRANT: the merge state is static. One run at a time.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mediacat.h"
#include "mediaindex.h"
#include "mediawalk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Append a record; *offset is where its line starts. */
    bool (*cat_append)(void *ctx, const mediacat_rec_t *r, uint32_t *offset);
    /* The record whose line starts at offset. */
    bool (*cat_read)(void *ctx, uint32_t offset, mediacat_rec_t *out);
    /* Fill title, artist and album for a track being ADDed or UPDATEd.
     * Leaving them empty is allowed: a track with no tags is still a
     * track. */
    void (*tags)(void *ctx, const char *path, mediacat_rec_t *r);
    /* The card, as mwalk_volume() offers it. */
    mwalk_result_t (*walk)(void *ctx, mwalk_fn fn, void *walk_ctx);
    void *ctx;

    const char *index_path;     /* the index, e.g. "/sd/.defeatist.ix1" */
    const char *temp_path;      /* where the new one is built */
    int64_t     now;            /* settings_now(), for written/deleted_at */
    char        clock;          /* MIDX_CLOCK_FLOOR or MIDX_CLOCK_SYNCED */
    const volatile bool *abort; /* polled per track; NULL for never */
} msync_ops_t;

typedef enum {
    MSYNC_DONE = 0,     /* the new index is in place */
    MSYNC_STOPPED,      /* *abort was set; the old index stands */
    MSYNC_FAILED,       /* something could not be read or written; the
                         * old index stands, or is gone if it was the
                         * thing that was damaged */
} msync_result_t;

typedef struct {
    int keep, add, update, revive, bury;
    int cat_reads;      /* catalog lines read: long paths, revives, buries */
    bool index_damaged; /* the old index was refused and removed */
} msync_stats_t;

msync_result_t msync_run(const msync_ops_t *ops, msync_stats_t *stats);

#ifdef __cplusplus
}
#endif
