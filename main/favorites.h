/*
 * favorites.h -- the starred stations, per volume.
 *
 * A second `.m3u` beside `stations.m3u`, in the same format, read by the
 * same parser and written by the same `station_entry()`. Three reasons
 * it is a file and not a flag inside `stations.m3u`:
 *
 *  - `stationlist.h` and `stations_append()` are reused unchanged. A
 *    marker inside the existing file would need a format nothing else
 *    speaks and a rewrite of a file the listener hand-edits.
 *  - A station starred off the directory is not in `stations.m3u` at
 *    all. A flag can only mark something already in the list; a file
 *    can hold a station the card has never seen.
 *  - It is per volume, like everything else here, and a card carries
 *    its own favourites to another Tab5 without carrying the whole
 *    station list with it.
 *
 * WHICH VOLUME, WHICH IS THE SAME ANSWER AS EVERYWHERE ELSE
 *
 * SD first, then USB, first one that HAS the file. That is
 * stations_load()'s rule and this follows it so that a person with two
 * volumes mounted does not have to learn a second one. It has a cost
 * worth being honest about: star something with both mounted, pull the
 * SD card, and the star is gone -- it was the card's. The alternative,
 * a union across volumes, makes unstarring ambiguous (which file?) and
 * was rejected for that.
 *
 * WHAT MATCHING MEANS
 *
 * The URL, with the scheme and host case-folded and the path left
 * byte-exact -- see favorites_url_eq(). Anything cleverer starts
 * deciding that two URLs are "really" the same station, and the way
 * that fails is silent: a star that appears on a row nobody starred.
 * stationlist.h refuses what it cannot handle rather than repairing it,
 * and this does the same.
 *
 * THE COST OF MEMBERSHIP
 *
 * favorites_contains() is a linear scan of at most STATIONLIST_MAX
 * entries. The chooser resolves it once per row when it builds the
 * list, never while drawing: 64 rows against 64 favourites is 4096
 * comparisons on a list load, and zero per frame.
 *
 * THREADING
 *
 * Same as stations.c, because it is the same kind of thing: loaded and
 * written from the player task, read by anyone, the array swapped whole
 * under a short lock. NOT FROM ui_task -- every function here that is
 * not a read opens a file.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "stationlist.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Beside stations.m3u, at a volume's root. */
#define FAVORITES_FILENAME  "favorites.m3u"

/*
 * The temp file favorites_remove() writes before renaming over the
 * list. Hidden-dotted like settings.c's, so it does not show up in the
 * file chooser if a removal is interrupted.
 */
#define FAVORITES_TEMPNAME  ".favorites.tmp"

/*
 * Read the list. True if at least one favourite was parsed.
 *
 * False also means "no favourites", which is the ordinary state of a
 * card nobody has starred anything on -- it is not an error and is not
 * logged as one. The in-memory list is emptied either way, so a false
 * return leaves favorites_count() at 0 rather than leaving the previous
 * card's stars in place.
 *
 * Opens a file. Not from ui_task.
 */
bool favorites_load(void);

/*
 * Is this URL starred?
 *
 * Cheap and lock-held; safe from any task, including the one that
 * draws. This is the only function here that ui_task may call, and it
 * is why the set is kept in memory at all.
 */
bool favorites_contains(const char *url);

/*
 * Star one station: append and reload, exactly as stations_append()
 * does, down to the round trip -- the entry counts as added only if the
 * parser reads it back.
 *
 * `name` may be empty, in which case no `#EXTINF:` is written and the
 * parser labels the entry by its host.
 *
 * WHICH VOLUME. The one the favourites list came from when there is
 * one, else the volume the CURRENT STATION LIST came from, else the
 * first present volume. The middle case is the one that matters: the
 * first star on a card has no favourites file to belong to, and the
 * station being starred came from somewhere.
 *
 * False when the fields are refused, the list is full, there is no
 * volume, or the write or reload failed. The file is left alone in all
 * of them.
 *
 * Opens a file. Not from ui_task.
 */
bool favorites_add(const char *name, const char *url);

/*
 * Unstar: rewrite the file without this URL, then reload.
 *
 * THE FIRST NON-APPEND WRITE IN THIS PROGRAM'S STATION PATH, and it
 * follows settings.c's compaction because the risk is the same one: a
 * list truncated by a power cut is worse than a list with one extra
 * entry in it.
 *
 *   1. write .favorites.tmp in full, flush, close
 *   2. rename favorites.m3u -> .favorites.tmp's neighbour? no: see below
 *
 * There is no .bak here, deliberately. settings.c keeps one because its
 * file is the only record of a hundred settings; this file is a list of
 * stations that also exist in stations.m3u or in the directory, and a
 * second copy of it on the card buys less than the confusion of a
 * favorites.m3u.bak somebody finds and edits. The sequence is write
 * temp, rename over: power lost before the rename leaves the original
 * list intact and costs a temp file, which the next removal overwrites.
 *
 * True when the URL is gone from the file, INCLUDING when it was never
 * there -- the postcondition is "not starred", and a caller toggling a
 * star should not see a failure because it was already off.
 *
 * Opens a file. Not from ui_task.
 */
bool favorites_remove(const char *url);

/*
 * Star or unstar, whichever this URL is not. Returns the state it
 * ENDED in: true for starred.
 *
 * This is what the panel's button calls, and it is here rather than at
 * the call site so that the read and the write happen under one
 * decision. Two calls -- contains() then add() -- would be a race with
 * the portal, which can be starring the same station from a phone.
 *
 * On failure the state is unchanged and the return says what it still
 * is, so the button redraws to the truth rather than to what was asked
 * for.
 *
 * Opens a file. Not from ui_task.
 */
bool favorites_toggle(const char *name, const char *url);

/* How many are starred. 0 before the first load. */
int favorites_count(void);

/*
 * Copy favourite `i` out, oldest first -- the file's order, which is
 * the order they were starred in. False and a zeroed `out` when `i` is
 * out of range.
 *
 * The list shown by the radio menu is built from these, in this order.
 * Not sorted, for load_stations()'s reason: the order on screen is the
 * order `next` moves through.
 */
bool favorites_get(int i, station_t *out);

/* Which volume the loaded favourites came from, or STORAGE_COUNT. */
storage_id_t favorites_volume(void);

/*
 * URL equality, as this module means it: scheme and host case-folded,
 * everything from the path on byte-exact.
 *
 * Exposed because the chooser's gold row and the panel's star have to
 * agree with the file, and a second implementation of "the same
 * station" is the thing that would drift.
 */
bool favorites_url_eq(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
