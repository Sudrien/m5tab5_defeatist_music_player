/*
 * mediaindex.h -- the two rules the media index cannot get wrong: the
 * order paths are kept in, and what a reconcile does with each pair.
 *
 * MEDIA-INDEX.md is the plan. This is the first code of it, and it is
 * deliberately the part with no I/O: every decision a reconcile makes
 * is taken here, from values, so texttest can check all of them. The
 * walk that feeds it and the catalog it writes are later patches, and
 * they only have to do what this file says.
 *
 * PATHS are relative to the volume root, with no leading slash --
 * "Artist/Album/01 Song.flac", "Artist/Album/Album.cue#03" -- the form
 * starred.m3u already uses. Relative because SD and USB are shown to
 * MPD as one library (SD preferred), and the only way `Artist/x.flac`
 * on one volume can be recognised as `Artist/x.flac` on the other is by
 * the part below the mount point. Cue tracks are the "<sheet>.cue#NN"
 * names cuesheet.h gives them everywhere else; the image a sheet covers
 * is not a track of its own, because cuedir.h already hides it.
 *
 * MATCHING IS BYTE-EXACT, favorites.h's and starred.h's rule, for their
 * reason: anything cleverer is two paths that are the same file to one
 * part of the program and different files to another. The cost is that
 * "ABBA/" on the SD and "Abba/" on the USB drive are two folders in the
 * merged library. That is the truth about the two volumes.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the order ------------------------------------------------------ */

/*
 * The index is sorted by path, and a reconcile is a merge-join of that
 * index against a walk of the card. A merge-join is only correct if
 * both sides arrive in THE SAME total order, and the obvious choice for
 * each side is not the same order.
 *
 * The walk is depth-first, one directory at a time, each directory's
 * names sorted. strcmp() on whole paths is not that order, because '/'
 * is not the smallest byte: ' ' '!' '#' '-' '.' and every other byte
 * below 0x2F sort ahead of it. So with a folder "a" and a folder "a b",
 *
 *     walk order:    a/x.flac, a b/y.flac      ("a" < "a b" as names)
 *     strcmp order:  a b/y.flac, a/x.flac      (' ' < '/')
 *
 * and a merge-join fed one of each would see a/x.flac on the card side
 * while the index side was still on "a b/", call the whole of "a b/"
 * missing, and tombstone it. Then "a b/" arrives from the card and is
 * added back as new, every tag re-read. Every walk would do it again.
 *
 * The fix is to make '/' sort below every other byte (NUL aside, which
 * ends the string). Then comparing whole paths is exactly comparing
 * component by component, which is exactly what a depth-first walk
 * with sorted directories produces. A name never contains '/', so
 * within one directory this is plain strcmp() order, which is what the
 * walk must sort names by.
 *
 * NOT THE CHOOSER'S ORDER. browser.c sorts folders before files and
 * case-insensitively, which is right for a screen and wrong here: a
 * walk sorted that way disagrees with this function about "b/" versus
 * "a.flac", and about "B" versus "a". The walk sorts with
 * midx_name_cmp(), and display order is somebody else's business.
 *
 * Bytes compare as unsigned, so UTF-8 sorts by codepoint.
 */
static inline int midx_path_cmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (*x && *x == *y) {
        x++;
        y++;
    }
    if (*x == *y) return 0;
    /* NUL lowest, then '/', then every other byte in its own order. */
    const unsigned kx = (*x == 0) ? 0u : (*x == '/') ? 1u : (unsigned)*x + 1u;
    const unsigned ky = (*y == 0) ? 0u : (*y == '/') ? 1u : (unsigned)*y + 1u;
    return (kx < ky) ? -1 : 1;
}

/* Names within one directory. The same function, and named separately
 * so the walk's qsort says what it is sorting by. */
static inline int midx_name_cmp(const char *a, const char *b)
{
    return midx_path_cmp(a, b);
}

/*
 * Whether `next` may follow `prev` on one side of a merge. Strictly
 * increasing: the card cannot hold one path twice, and the index side
 * is the LATEST record per path, not every record the append-only
 * catalog holds.
 *
 * A reconcile checks every element it takes from either side and
 * STOPS at the first that fails, writing nothing further. What it has
 * already written may include wrong tombstones -- an element arriving
 * late means the other side's copy was passed and buried before the
 * disorder was visible -- and that is survivable only because of what
 * a tombstone is: the next correct walk finds the file present and
 * revives it without reading a tag. A merge that carried on past
 * disorder would bury and re-add everything after it, every time.
 */
static inline bool midx_in_order(const char *prev, const char *next)
{
    return prev == 0 || midx_path_cmp(prev, next) < 0;
}

/* ---- the step ------------------------------------------------------- */

/*
 * What says a file has changed. Both, because either alone misses
 * something ordinary: a re-tag in place often keeps the size (padding
 * absorbs it) and a copy tool that preserves timestamps keeps the
 * mtime. mtime is whatever stat() returns -- FAT local time read as
 * though UTC, cardtime.h says why -- which is fine for equality: it is
 * the same wrong number every time it is read.
 *
 * For a cue track, what goes in here is the walk's decision and not
 * made yet: the tracks depend on the sheet AND on the audio's length
 * (cuedir.h), so a stamp from the sheet alone would miss a re-rip.
 */
typedef struct {
    int64_t  mtime;
    uint64_t size;
} midx_stamp_t;

static inline bool midx_stamp_eq(midx_stamp_t a, midx_stamp_t b)
{
    return a.mtime == b.mtime && a.size == b.size;
}

/* One entry from the walk. */
typedef struct {
    const char  *path;
    midx_stamp_t stamp;
} midx_card_t;

/* The latest catalog record for one path. deleted_at 0 is live. */
typedef struct {
    const char  *path;
    midx_stamp_t stamp;
    int64_t      deleted_at;
} midx_cat_t;

typedef enum {
    MIDX_DONE = 0,  /* both sides exhausted */
    MIDX_KEEP,      /* nothing to write */
    MIDX_ADD,       /* on the card, never catalogued: read tags, append */
    MIDX_UPDATE,    /* changed: read tags again, append a live record */
    MIDX_REVIVE,    /* tombstoned, back unchanged: append it live, same
                     * tags -- the whole reason tombstones are kept */
    MIDX_BURY,      /* catalogued, gone from the card: append a
                     * tombstone, deleted_at = now */
} midx_action_t;

typedef struct {
    midx_action_t action;
    bool          take_card;    /* advance the walk */
    bool          take_cat;     /* advance the index */
} midx_step_t;

/*
 * One step of the merge-join. NULL for a side that is exhausted.
 *
 * The five cases are MEDIA-INDEX.md's list, plus the one it leaves
 * implicit and that matters most over time: a path already tombstoned
 * and still absent is KEEP. Burying it again would move its time of
 * death to every walk's "now", so a tombstone could never age out, and
 * each walk would append one line per dead file for ever.
 *
 * A tombstoned path that is back but CHANGED is UPDATE rather than
 * REVIVE: its old tags describe a different file, and the record
 * UPDATE appends is live, which clears the tombstone in the same line.
 *
 * WHAT THE CALLER OWES THIS. A BURY is a claim that the walk looked
 * and the file was not there. A directory whose readdir failed partway
 * has not been looked at, and everything under it would come back
 * BURY. So a walk that cannot finish a directory stops, and the
 * reconcile stops with it: never feed this a partial listing as though
 * it were a complete one.
 */
static inline midx_step_t midx_step(const midx_card_t *card,
                                    const midx_cat_t *cat)
{
    midx_step_t s = { MIDX_DONE, false, false };
    if (!card && !cat) return s;

    int c;
    if (!card)      c = 1;      /* only the index left */
    else if (!cat)  c = -1;     /* only the card left */
    else            c = midx_path_cmp(card->path, cat->path);

    if (c < 0) {
        s.action = MIDX_ADD;
        s.take_card = true;
    } else if (c > 0) {
        s.action = cat->deleted_at ? MIDX_KEEP : MIDX_BURY;
        s.take_cat = true;
    } else {
        s.take_card = true;
        s.take_cat = true;
        if (!midx_stamp_eq(card->stamp, cat->stamp)) {
            s.action = MIDX_UPDATE;
        } else {
            s.action = cat->deleted_at ? MIDX_REVIVE : MIDX_KEEP;
        }
    }
    return s;
}

/* ---- whose clock ---------------------------------------------------- */

/*
 * Every record says which clock wrote its times: the floor (settings.c,
 * raised by cardtime.c) before NTP, or a synced clock after. A floor
 * time is never later than the truth and may be months earlier, so two
 * times are only compared when both carry the same mark -- which is the
 * whole of what this is for. One character in the JSON.
 */
#define MIDX_CLOCK_FLOOR    'f'
#define MIDX_CLOCK_SYNCED   's'

#ifdef __cplusplus
}
#endif
