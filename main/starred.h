/*
 * starred.h -- stars on local files and folders, per volume.
 *
 * The chooser's star on the SD and USB tabs. A starred row is a mark
 * and nothing else yet: the list is not played, not shown as a list,
 * and not a playlist. It is written so that it can become one --
 * `starred.m3u` at the volume's root, one path per line, relative to
 * that root, so the card carries its stars to another Tab5 or a
 * computer and a later patch can play the file as it stands.
 *
 * Folders end in '/'. That is how a line says which of the two it is,
 * and it keeps "Album" the folder and "Album" the file (a cue sheet's
 * image with no extension, say) from being the same star.
 *
 * Cue tracks are stored as the chooser names them, "Album.cue#03". No
 * other player reads that form; nothing else reads this file yet.
 *
 * MATCHING is byte-exact on the relative path, favorites.h's rule for
 * the same reason: anything cleverer is a star that appears on a row
 * nobody starred. A renamed file loses its star.
 *
 * WHICH VOLUME is the one the path is on. There is no "first volume
 * that has the file" rule as there is for stations, because a file's
 * star can only mean the file on that volume.
 *
 * THREADING. starred_contains() may open a file -- the first question
 * about a volume, and the first after it is remounted, loads that
 * volume's list -- so it is for the tasks that already do I/O to build
 * a listing, which is where the chooser asks. starred_toggle() writes;
 * the player task calls it on the chooser's behalf, as it does for
 * radio stars. The in-memory list is under a mutex.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STARRED_FILENAME  "starred.m3u"
#define STARRED_TEMPNAME  ".starred.tmp"

/* Per volume. A star past this is refused, and said so. */
#define STARRED_MAX       (512)

/* Is this absolute path starred? `is_dir` picks the folder form. */
bool starred_contains(const char *path, bool is_dir);

/*
 * Star or unstar, whichever it is not, and rewrite the file. Returns
 * the state it ENDED in, read back from the file: on a failed write
 * that is the state it still has, so a row redraws to the truth.
 */
bool starred_toggle(const char *path, bool is_dir);

#ifdef __cplusplus
}
#endif
