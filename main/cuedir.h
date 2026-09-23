/*
 * cuedir.h -- cue sheets on the card, turned into tracks.
 *
 * cuesheet.h parses bytes; this is the part that touches the card. It
 * reads a folder's sheets, finds the audio each FILE line means, asks
 * each audio file its length so cue_finish() can drop what is past the
 * end, and hands back two things:
 *
 *   for a listing (the chooser, the playlist): the tracks as virtual
 *   names, "Album.cue#03", with a label to draw, and the audio files the
 *   sheets cover, which are hidden -- a disc image listed beside its own
 *   tracks is the same music twice;
 *
 *   for playing (the decoder, the tags): one track by its virtual path,
 *   with the real audio path and where in it the track starts and ends.
 *
 * BOTH GO THROUGH ONE FUNCTION, sheet_load() in cuedir.c, so the track
 * the chooser calls #03 is the track the decoder opens as #03. The names
 * are positions among the kept tracks (cuesheet.h says why), and two
 * separate derivations of "kept" is how #03 would come to mean two
 * different songs.
 *
 * A SHEET WITH NOTHING PLAYABLE HIDES NOTHING. If none of its FILE lines
 * resolve, or every track is dropped, the audio beside it lists as the
 * plain file it is: a broken sheet should cost the sheet, not the music.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cuesheet.h"
#include "storage_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cuedir cuedir_t;

/*
 * Read every sheet in dir. NULL when there are none (the normal case,
 * and cheap: one readdir) or on no memory. `cls` is the arbiter class
 * the reads go under -- a listing is BACKGROUND.
 */
cuedir_t *cuedir_load(const char *dir, storage_io_class_t cls);
void      cuedir_free(cuedir_t *cd);

/* Whether a plain file in this folder is audio a sheet covers. */
bool cuedir_hides(const cuedir_t *cd, const char *name);

int         cuedir_count(const cuedir_t *cd);
/* "Album.cue#03": a name in the folder, joined to it like any other. */
const char *cuedir_name(const cuedir_t *cd, int i);
/* "03  Title", for drawing. */
const char *cuedir_label(const cuedir_t *cd, int i);
/* The name, in this folder, of the audio file track i plays from. For
 * the media index, which has to notice when that file changes as well
 * as when the sheet does. */
const char *cuedir_audio(const cuedir_t *cd, int i);
/* Track i's title, performer and album from the loaded sheet -- what
 * cuedir_tags() gives for its virtual path, cut and defaulted the same
 * way, without reading the sheet again. */
bool cuedir_row_tags(const cuedir_t *cd, int i, char *title, char *artist,
                     char *album, size_t each);

/*
 * One track, by virtual path. Everything in it is a copy.
 *
 * About 1 KB: static or heap, never a local (CLAUDE.md).
 */
typedef struct {
    char     audio[512];            /* the real file */
    uint32_t start;                 /* CD frames into it */
    uint32_t end;                   /* CD frames; 0 = the end of the file */
    int      number;                /* as the sheet numbered it */
    char     title[CUE_TEXT_MAX];
    char     performer[CUE_TEXT_MAX];
    char     album[CUE_TEXT_MAX];
} cuetrack_t;

bool cuedir_track(const char *vpath, storage_io_class_t cls, cuetrack_t *out);

/*
 * The real file behind a path: the audio for a cue track, the path
 * itself otherwise. For the code that opens a track's file to read
 * something other than its audio -- the cover, the size. Writes into out
 * and returns it, or returns path unchanged when path is not a cue track
 * (so the common case costs one strrchr and no copy).
 */
const char *cuedir_file_of(const char *path, char *out, size_t out_size);

/*
 * A cue track's title, performer and album from its sheet, each cut on a
 * character boundary to `each` bytes. False for anything else. The
 * sheet is the only honest source: an image's own tags describe the
 * whole disc, and would put the album's name on every track.
 */
bool cuedir_tags(const char *vpath, char *title, char *artist, char *album,
                 size_t each);

#ifdef __cplusplus
}
#endif
