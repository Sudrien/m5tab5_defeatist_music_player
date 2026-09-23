/*
 * mediacat.h -- the media catalog: one JSON line per track record, per
 * volume, appended.
 *
 * MEDIA-INDEX.md is the plan and mediaindex.h the rules; this is the
 * file the index points into. `.defeatist.cat` at the volume root,
 * beside `.defeatist.dat`, dotted and FAT-hidden like it.
 *
 * APPEND-ONLY, settings.h's pattern and for its reasons: a record is one
 * write at the end of the file, with no moment where the file is half
 * rewritten, and a card pulled mid-append loses the line being written
 * and nothing before it. A path's LATEST line is its record; the index
 * says which line that is, so nothing ever scans for it.
 *
 * NOT replaygain.c's pattern, which gave appending up -- and the reason
 * is worth having here, because it looks like the same file. A sidecar
 * holds one record, so every appended line repeated the whole of the
 * last one and the old copies were pure waste. The catalog holds
 * thousands of records and a line is one of them; appending writes the
 * new thing and nothing else.
 *
 * A LINE, as written:
 *
 *   {"format_version":1,"path":"Artist/Album/01 Song.flac",
 *    "mtime":1735500000,"size":8760320,
 *    "title":"Song","artist":"Artist","album":"Album",
 *    "written":1789084800,"clock":"s"}
 *
 * with "deleted_at" added on a tombstone. `written` and `deleted_at`
 * are times from settings_now(), and `clock` says whether that was the
 * floor ("f") or a synced clock ("s"); mediaindex.h says why the two
 * are never compared. `mtime` is not one of those times -- it is the
 * card's own, from stat(), used only for equality.
 *
 * Readable words rather than one-letter keys, because this is a file on
 * a card people plug into a computer, and settings.h's argument holds:
 * a line somebody can read is a line somebody can fix. A key this build
 * does not know is skipped, so a later field costs an old build
 * nothing. A different format_version is not read at all.
 *
 * cJSON on both sides, as settings.c and replaygain.c already use it.
 * Tags can hold quotes, backslashes and newlines; a hand-rolled escaper
 * is how one of them splits a line in two.
 *
 * THREADING. One caller: the index task, when there is one. The line
 * buffer is a file-scope static -- MEDIACAT_LINE_MAX is well past what
 * belongs on a stack -- so nothing here is reentrant.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "mediaindex.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIACAT_FILENAME        ".defeatist.cat"
#define MEDIACAT_FORMAT_VERSION  (1)

/*
 * The longest line read or written. A path is at most MIDX_PATH_MAX,
 * three tags at 63 bytes each; JSON escaping can grow a control byte to
 * six ("\u001f"), so the worst honest line is about 2.2 KB. A line
 * longer than this is refused on write and skipped on read.
 */
#define MEDIACAT_LINE_MAX        (3072)

/* Sized like id3_tags_t and replaygain_tags_t: what the rest of the
 * player can show. */
#define MEDIACAT_TAG_LEN         (64)

/*
 * One record. About 720 bytes -- a static or the heap, never a local.
 */
typedef struct {
    char         path[MIDX_PATH_MAX + 1];   /* relative to the volume */
    midx_stamp_t stamp;
    char         title[MEDIACAT_TAG_LEN];
    char         artist[MEDIACAT_TAG_LEN];
    char         album[MEDIACAT_TAG_LEN];
    int64_t      written;       /* settings_now() when the line was made */
    int64_t      deleted_at;    /* 0: live */
    char         clock;         /* MIDX_CLOCK_FLOOR or MIDX_CLOCK_SYNCED */
} mediacat_rec_t;

/*
 * The line for a record, '\n' included, into out. Returns its length,
 * or -1 if the record is not one the catalog can hold (a path
 * mediaindex.h would refuse, an unknown clock) or the line would not
 * fit.
 */
int mediacat_encode(const mediacat_rec_t *r, char *out, size_t out_size);

/*
 * A line back into a record. False for anything that is not a record
 * this build wrote: not JSON, another format_version, no path, a path
 * the index could not hold, a stamp that is not a whole number of at
 * most fifteen digits (mediacat.c says why fifteen). Strings
 * longer than the record's fields are refused rather than cut, since
 * a cut path is a different path.
 */
bool mediacat_decode(const char *line, mediacat_rec_t *out);

/*
 * "/sd/.defeatist.cat" and so on. False if it would not fit.
 */
bool mediacat_path(storage_id_t vol, char *out, size_t out_size);

/*
 * Append one record to the volume's catalog. On success *offset is the
 * byte offset the line starts at -- what the index stores.
 *
 * If the file does not end in '\n' -- the last append was cut off by a
 * pulled card or a lost battery -- a '\n' is written first. Without it
 * this line would be glued to the torn one and both would fail to
 * parse, so one lost write would silently cost the next as well.
 *
 * Opens and closes the file per call, under the BACKGROUND class. The
 * caller batches nothing and holds no lease.
 */
bool mediacat_append(storage_id_t vol, const mediacat_rec_t *r,
                     uint32_t *offset);

/*
 * Read the record whose line starts at `offset`, from a file the caller
 * holds open for reading. False for an offset past the end, a line with
 * no '\n' within MEDIACAT_LINE_MAX (torn, or not a line start), or a
 * line mediacat_decode() refuses.
 */
bool mediacat_read_at(FILE *f, uint32_t offset, mediacat_rec_t *out);

#ifdef __cplusplus
}
#endif
