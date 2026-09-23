/*
 * mediawalk.h -- one volume, every playable track on it, in the media
 * index's order.
 *
 * The card side of a reconcile. mediaindex.h has the merge; this is
 * what the merge is fed, and it owes the merge two things that a
 * listing does not:
 *
 * ORDER. Depth-first, each folder's entries sorted by midx_name_cmp(),
 * which makes the whole walk strictly increasing under midx_path_cmp()
 * -- the index's order. Not the chooser's folders-first,
 * case-insensitive order; mediaindex.h has the example of why.
 *
 * COMPLETENESS, OR NOTHING. A reconcile reads a path missing from the
 * walk as a file deleted from the card, and buries it. So a folder
 * this cannot read to the end is not a shorter folder: the walk stops
 * and says MWALK_FAILED, and the reconcile writes nothing further.
 * That is why there is no truncating cap here of the kind the chooser
 * (512 entries) and the playlist (1024) have. The one cap,
 * MWALK_DIR_MAX, fails the walk rather than cutting the folder.
 *
 * What IS left out is left out every time, so it can never flip
 * between walks: dotfiles and "._" sidecars (storage_is_hidden()),
 * anything decoder_supports() says no to, audio a cue sheet covers
 * (cuedir.h), folders deeper than MWALK_DEPTH_MAX, and paths longer
 * than MIDX_PATH_MAX, which the rest of the player could not open
 * either. Each is logged when met.
 *
 * CUE TRACKS are the "<sheet>.cue#NN" names cuedir gives them, and
 * their stamp is the sheet's and the audio's together: mtime the later
 * of the two, size the sum. A track's existence and extent depend on
 * both -- which tracks a sheet keeps depends on the audio's length --
 * so a re-ripped image under an untouched sheet has to count as a
 * change, and a stamp from the sheet alone would miss it.
 *
 * I/O goes under STORAGE_IO_BACKGROUND, one lease per readdir() and per
 * stat(), never across the walk.
 *
 * NOT REENTRANT: the folder stack and the path buffer are statics, as
 * nothing this size belongs on a task stack. One walk at a time.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "mediaindex.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Entries in one folder -- files, subfolders and cue tracks. Past this
 * the walk FAILS; it does not truncate. An album is tens, a folder of
 * loose singles a few thousand, and a folder bigger than this is one
 * nobody is browsing on this screen. */
#define MWALK_DIR_MAX       (8192)

/* Folders below the volume root. Deeper ones are skipped, every time. */
#define MWALK_DEPTH_MAX     (24)

typedef enum {
    MWALK_DONE = 0,     /* every entry was offered */
    MWALK_STOPPED,      /* the callback returned false */
    MWALK_FAILED,       /* a folder or file could not be read: the walk
                         * is incomplete and must not be reconciled past
                         * the last entry offered */
} mwalk_result_t;

/*
 * One track. `path` is relative to the volume root, with no leading
 * slash, and good until the callback returns. Return false to stop.
 */
typedef bool (*mwalk_fn)(void *ctx, const char *path, midx_stamp_t stamp);

/* Walk the volume mounted at `mount` ("/sd", "/usb"). */
mwalk_result_t mwalk_volume(const char *mount, mwalk_fn fn, void *ctx);

/*
 * A cue track's tags from the sheet the walk has loaded, for the track
 * being offered right now. Only from inside the callback, and only for
 * the path it was handed: anything else is false, and the caller reads
 * the tags the long way (cuedir_tags()) -- a sheet parse and an audio
 * probe per track, which on a folder of images was most of a first
 * index's time on the SD card. Avoiding that is the point of this.
 */
bool mwalk_cue_tags(const char *path, char *title, char *artist,
                    char *album, size_t each);

#ifdef __cplusplus
}
#endif
