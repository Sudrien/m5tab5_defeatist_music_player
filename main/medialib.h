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
 * medialib_request() is how a reindex starts: it makes a task for the
 * run and returns. Two things call it: the REINDEX button on the
 * panel's SD and USB tabs, and medialib_poll(), which starts one on
 * each volume a little after it is mounted. One run at a time, on
 * either volume -- the engine's state is static.
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

/*
 * The task a reindex runs on. Made per run and deleted after, so its
 * stack is only held while there is work. 8 KB: the engine, the walk
 * and the catalog keep their buffers static; what is on the stack is
 * the tag path -- cuedir_track() and sheet_load() hold about 2.5 KB of
 * paths, covertag about 1 KB -- and printf. The high-water mark is
 * logged after every run, so the first run on hardware says whether
 * that estimate holds.
 */
#define MEDIALIB_STACK      (8192)

typedef enum {
    MEDIALIB_NONE = 0,  /* not run this session */
    MEDIALIB_RUNNING,
    MEDIALIB_DONE,
    MEDIALIB_STOPPED,   /* the volume went away mid-run */
    MEDIALIB_FAILED,
} medialib_state_t;

typedef struct {
    medialib_state_t state;
    msync_stats_t    stats;     /* live while RUNNING */
    int              ms;        /* how long the last run took */
    bool             pending;   /* mounted; an automatic run is due */
} medialib_status_t;

/*
 * Start a reindex of one volume on its own task, and return. False,
 * with nothing started, if the volume is not mounted, a reindex is
 * already running (on either volume), or the task could not be made.
 *
 * While it runs the volume is held with storage_hold_background(): a
 * card pulled mid-run is marked absent and unmounted only once the run
 * has closed its files, and the walk stops at the next track when it
 * sees the volume gone.
 */
bool medialib_request(storage_id_t vol);

/* Whether a reindex is running, on either volume. A value. */
bool medialib_busy(void);

/*
 * How long after a mount the automatic reindex waits. Boot is the
 * busiest the card ever is -- settings, the resume track, the chooser,
 * the playlist, cue sheets parsed for all of them -- and USB mounts a
 * second or two behind the SD; the index can wait for all of that. It
 * also means a card pushed in and pulled straight out again never
 * starts a run only to have it stopped.
 */
#define MEDIALIB_SETTLE_MS  (10000)

/*
 * The automatic reindex. Call often, from one task -- ui_task, which
 * runs whether or not anything is playing; the player's idle loop does
 * not, and a drive plugged in mid-album would wait for the album.
 *
 * Cheap when nothing has changed: one compare of storage_generation().
 * A volume that has newly appeared is due MEDIALIB_SETTLE_MS later, once
 * per mount; if both are due, the SD goes first and the USB when it is
 * done. A run that fails is not retried until the next mount or a
 * press of REINDEX, so a card that cannot be indexed is not walked
 * over and over. A press of REINDEX during the wait takes the place of
 * the automatic run.
 */
void medialib_poll(void);

/* A copy of one volume's status, for drawing. The counts in a RUNNING
 * status are read while the run writes them; each is a plain int, and
 * a count one behind on screen is not worth a lock. */
void medialib_status(storage_id_t vol, medialib_status_t *out);

#ifdef __cplusplus
}
#endif
