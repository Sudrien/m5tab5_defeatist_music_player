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
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
 * For a cue track it is the sheet's and the audio's together -- the
 * later mtime, the summed size -- because the tracks depend on the
 * sheet AND on the audio's length (cuedir.h), and a stamp from the
 * sheet alone would miss a re-rip. mediawalk.c makes it.
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

/* ---- the index file ------------------------------------------------- */

/*
 * One record per path, fixed width, in midx_path_cmp() order, so a
 * lookup is a binary search by seeking: log2(n) reads of one record.
 * The index holds the LATEST catalog record for each path, tombstones
 * included -- reconcile needs to see the dead to revive them -- and is
 * never the truth: it is rebuilt from what the catalog says.
 *
 * 128 bytes, little-endian, packed and unpacked byte by byte rather
 * than as a struct, so there is no padding or host byte order to agree
 * on:
 *
 *     0   key       104  the first 104 bytes of the path, NUL-padded
 *   104   path_len    2  the whole path's length, 1..MIDX_PATH_MAX
 *   106   flags       1  MIDX_F_DEAD
 *   107   reserved    1  zero
 *   108   cat_off     4  byte offset of the record's line in the catalog
 *   112   mtime       8  the stamp, so reconcile runs off the index
 *   120   size        8    alone and never reads the catalog to compare
 *
 * A PREFIX, NOT A HASH. The merged SD+USB listing walks both indexes in
 * path order, which a hash cannot give. A path longer than the key is
 * still found exactly: when the query and a record agree on all 104
 * bytes, the record's full path is read out of the catalog at cat_off.
 * Only paths that long pay that read -- "Artist/Album/NN Title.flac" is
 * usually half of it.
 *
 * THE STAMP IS HERE so reconcile is a merge of two sequential files, the
 * walk and this, with the catalog touched only to append. 20 000 tracks
 * is 2.5 MB of index.
 *
 * 2^32 bytes of catalog is the limit cat_off sets. At a few hundred
 * bytes a line that is millions of lines, and FAT32 stops a file there
 * anyway.
 */
#define MIDX_REC_SIZE   (128)
#define MIDX_KEY_LEN    (104)

/* The rest of the player holds a path in 512 bytes (player.c, cuedir.c,
 * mediacache.c); this is that, less the NUL, less "/usb/". */
#define MIDX_PATH_MAX   (506)

#define MIDX_F_DEAD     (0x01)
#define MIDX_F_KNOWN    (MIDX_F_DEAD)

typedef struct {
    char         key[MIDX_KEY_LEN + 1];  /* NUL-terminated here */
    uint16_t     path_len;
    uint8_t      flags;
    uint32_t     cat_off;
    midx_stamp_t stamp;
} midx_rec_t;

static inline void midx_put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static inline uint64_t midx_get_le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/*
 * Fill one on-disk record. False, with nothing written, for a path this
 * format cannot hold: empty, too long, or with a leading '/' -- index
 * paths are relative, and an absolute one is a caller that forgot to
 * strip the mount.
 */
static inline bool midx_rec_pack(uint8_t out[MIDX_REC_SIZE], const char *path,
                                 uint32_t cat_off, uint8_t flags,
                                 midx_stamp_t stamp)
{
    if (!path || !*path || *path == '/') return false;
    if (flags & ~MIDX_F_KNOWN) return false;
    const size_t len = strlen(path);
    if (len > MIDX_PATH_MAX) return false;

    memset(out, 0, MIDX_REC_SIZE);
    memcpy(out, path, len < MIDX_KEY_LEN ? len : MIDX_KEY_LEN);
    midx_put_le(out + 104, len, 2);
    out[106] = flags;
    midx_put_le(out + 108, cat_off, 4);
    midx_put_le(out + 112, (uint64_t)stamp.mtime, 8);
    midx_put_le(out + 120, stamp.size, 8);
    return true;
}

/*
 * Read one on-disk record. False for anything the packer could not have
 * written -- which is what a torn write, a file from another version or
 * a sector of something else looks like. The caller's answer to false
 * is the same in every case: this index is not to be trusted, rebuild.
 */
static inline bool midx_rec_unpack(const uint8_t in[MIDX_REC_SIZE],
                                   midx_rec_t *r)
{
    const size_t len = (size_t)midx_get_le(in + 104, 2);
    if (len == 0 || len > MIDX_PATH_MAX) return false;
    if (in[106] & ~MIDX_F_KNOWN) return false;
    if (in[107] != 0) return false;
    if (in[0] == '/') return false;

    /* Exactly min(len, KEY) bytes of path and then NUL padding: a NUL
     * inside the path or a byte in the padding is not a record. */
    const size_t k = len < MIDX_KEY_LEN ? len : MIDX_KEY_LEN;
    for (size_t i = 0; i < MIDX_KEY_LEN; i++) {
        if ((i < k) != (in[i] != 0)) return false;
    }

    memcpy(r->key, in, MIDX_KEY_LEN);
    r->key[MIDX_KEY_LEN] = '\0';
    r->path_len = (uint16_t)len;
    r->flags = in[106];
    r->cat_off = (uint32_t)midx_get_le(in + 108, 4);
    r->stamp.mtime = (int64_t)midx_get_le(in + 112, 8);
    r->stamp.size = midx_get_le(in + 120, 8);
    return true;
}

/* ---- looking things up ---------------------------------------------- */

/*
 * Where the records come from. The functions below never touch a file:
 * `read` fetches record i, and `fullpath` fetches the whole path of the
 * catalog line at cat_off, for the one case the key cannot settle. On
 * the device they are an fseek and an fread; on the host, an array.
 *
 * `scratch` holds a full path during a comparison. It is the CALLER'S,
 * and must not be a local on a task stack -- MIDX_PATH_MAX + 1 bytes is
 * over the line CLAUDE.md draws. A static, or the heap.
 *
 * `err` is set, and stays set, when a callback fails or the catalog
 * disagrees with the index about a path: the index points at a line
 * that is not the path it claims. A search that sets it has returned
 * something meaningless, and the index should be rebuilt.
 *
 * What can be caught is a line whose length or first KEY bytes differ
 * from the record. A line that agrees on both and differs further on --
 * a sibling with the same long prefix and the same length -- cannot be
 * told from the right one, and the search answers as though it were
 * right. The rule that keeps that from happening is upstream: cat_off
 * is only ever good for the catalog the index was built against, so
 * anything that rewrites the catalog (compaction) rebuilds the index
 * in the same step, and an index never outlives its catalog.
 */
typedef struct {
    bool   (*read)(void *ctx, uint32_t i, midx_rec_t *out);
    bool   (*fullpath)(void *ctx, uint32_t cat_off, char *buf, size_t n);
    void    *ctx;
    uint32_t n;
    char    *scratch;
    size_t   scratch_n;
    bool     err;
} midx_src_t;

/* The record's whole path, into src->scratch, checked against the key
 * and length the record claims. NULL, with err set, on any mismatch. */
static inline const char *midx_rec_fullpath(midx_src_t *src,
                                            const midx_rec_t *r)
{
    if (r->path_len <= MIDX_KEY_LEN) return r->key;   /* the key is it */
    if (!src->fullpath || !src->scratch ||
        src->scratch_n < (size_t)r->path_len + 1 ||
        !src->fullpath(src->ctx, r->cat_off, src->scratch, src->scratch_n)) {
        src->err = true;
        return NULL;
    }
    if (strlen(src->scratch) != r->path_len ||
        memcmp(src->scratch, r->key, MIDX_KEY_LEN) != 0) {
        src->err = true;
        return NULL;
    }
    return src->scratch;
}

/*
 * midx_path_cmp(q, the record's path), reading the catalog only when
 * the key cannot decide. 0 with err set if it could not decide at all.
 */
static inline int midx_rec_cmp(midx_src_t *src, const char *q,
                               const midx_rec_t *r)
{
    if (r->path_len <= MIDX_KEY_LEN) return midx_path_cmp(q, r->key);

    /* The key is the first KEY bytes of a longer path. If q differs
     * within them, or ends within them, the key alone decides. */
    for (size_t i = 0; i < MIDX_KEY_LEN; i++) {
        if (q[i] != r->key[i]) {
            const char a[2] = { q[i], 0 }, b[2] = { r->key[i], 0 };
            return midx_path_cmp(a, b);
        }
    }
    /* q is exactly the key: a proper prefix of the record, so before it
     * -- without the catalog read, which is for a q that runs on. */
    if (q[MIDX_KEY_LEN] == '\0') return -1;
    const char *full = midx_rec_fullpath(src, r);
    return full ? midx_path_cmp(q, full) : 0;
}

/* Does the record's path begin with `prefix`? */
static inline bool midx_rec_has_prefix(midx_src_t *src, const char *prefix,
                                       const midx_rec_t *r)
{
    const size_t p = strlen(prefix);
    if (p > r->path_len) return false;
    if (p <= MIDX_KEY_LEN) return memcmp(r->key, prefix, p) == 0;
    const char *full = midx_rec_fullpath(src, r);
    return full && memcmp(full, prefix, p) == 0;
}

typedef enum {
    MIDX_AT,            /* first record >= q */
    MIDX_PAST_PREFIX,   /* first record past every path beginning with q */
} midx_seek_t;

/*
 * Binary search. Returns the index of the first record the mode asks
 * for, or src->n when there is none; check src->err afterwards.
 *
 * MIDX_PAST_PREFIX is what makes a folder listing cheap. Every path
 * beginning with "a/b/" is one contiguous run in this order -- true of
 * any byte-by-byte order -- so having listed the subfolder "a/b/" the
 * listing jumps past all of it in one search rather than reading
 * through thousands of tracks beneath it. Listing a folder costs a
 * search per child, not a read per descendant.
 */
static inline uint32_t midx_seek(midx_src_t *src, const char *q,
                                 midx_seek_t mode)
{
    uint32_t lo = 0, hi = src->n;
    while (lo < hi && !src->err) {
        const uint32_t mid = lo + (hi - lo) / 2;
        midx_rec_t r;
        if (!src->read(src->ctx, mid, &r)) {
            src->err = true;
            break;
        }
        const int c = midx_rec_cmp(src, q, &r);    /* q vs record */
        const bool left = (mode == MIDX_AT)
            ? (c > 0)
            : (c > 0 || midx_rec_has_prefix(src, q, &r));
        if (left) lo = mid + 1;
        else      hi = mid;
    }
    return lo;
}

/*
 * The index of `path`, or -1. Dead records are found like live ones;
 * whether a tombstone counts as "there" is the caller's question.
 */
static inline int64_t midx_find(midx_src_t *src, const char *path,
                                midx_rec_t *out)
{
    const uint32_t i = midx_seek(src, path, MIDX_AT);
    if (src->err || i >= src->n) return -1;
    if (!src->read(src->ctx, i, out)) {
        src->err = true;
        return -1;
    }
    if (midx_rec_cmp(src, path, out) != 0 || src->err) return -1;
    return i;
}

/*
 * The child of folder `dir` that `path` lies under, for a listing.
 * `dir` is "" for the root or ends in '/'. Copies the child's name into
 * `name` and returns true if it is a subfolder, false if `path` is a
 * file directly in `dir`. `name` must hold MIDX_PATH_MAX + 1.
 */
static inline bool midx_child(const char *dir, const char *path, char *name)
{
    const size_t d = strlen(dir);
    const char *rest = path + d;
    const char *slash = strchr(rest, '/');
    const size_t n = slash ? (size_t)(slash - rest) : strlen(rest);
    memcpy(name, rest, n);
    name[n] = '\0';
    return slash != NULL;
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
