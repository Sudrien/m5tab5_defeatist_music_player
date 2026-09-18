/*
 * stations.h -- the station list, read off a volume.
 *
 * Phase 4's first half. `stationlist.h` is the parser and has been
 * host-tested since before the stream path existed; this is the part
 * that finds the file, reads it, and answers the two questions
 * play_stream() needs: which station is current, and what is next.
 *
 * WHERE THE FILE IS, AND WHY THERE
 *
 * `stations.m3u` at the root of a mounted volume. SD first, then USB --
 * the same order everything else in this program prefers, and if both
 * have one the SD card wins rather than the two being merged. Merging
 * would mean a station's position in the list depended on which drive
 * was plugged in, and "next" is a position.
 *
 * M3U because it needs no keyboard and can be edited anywhere:
 *
 *     #EXTM3U
 *     #EXTINF:-1,Michigan Radio
 *     https://26433.live.streamtheworld.com/WUOMFM.mp3
 *     #EXTINF:-1,WNZK
 *     https://stream.zeno.fm/erunhwj5lekvv
 *
 * It is also what radio-browser hands out directly -- its station
 * endpoints emit M3U as well as JSON -- so the portal's search, when it
 * lands, can append to this file without a JSON parser on the P4 and
 * without a second format to test. That is the whole reason for choosing
 * M3U over something of our own.
 *
 * A BARE URL PER LINE ALSO WORKS
 *
 * stationlist.h falls back to the URL's host for the name, which is what
 * pasting gives. A file of nothing but URLs is a valid station list with
 * ugly names, and that is a better failure than refusing it.
 *
 * WHAT IS NOT HERE
 *
 * No writing. The portal's search will add that, and it will go through
 * a temporary file and a rename exactly as settings.c does, because a
 * station list truncated by a power cut is worse than no station list.
 * No resolution of `.m3u` files a STATION serves -- that is a playlist
 * inside a stream URL and is still out of scope. No per-station codec or
 * bitrate: the stream tells us, and a file that claimed otherwise would
 * be believed over the stream.
 *
 * THREADING
 *
 * Loaded from the task that plays, read by anyone. The array is
 * module-scope and is replaced wholesale under a short lock rather than
 * edited in place, so a reader either sees the old list or the new one.
 * `stations_get()` copies, for the reason netstream_title() copies: a
 * pointer into a list that can be reloaded is a race the caller cannot
 * see.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "stationlist.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The file, at a volume's root. */
#define STATIONS_FILENAME   "stations.m3u"

/*
 * Longest station file read. 64 stations at a 512-byte URL and a
 * 64-byte name is about 37 KB; 64 KB is room for the comments and the
 * directives a file that has been through another player collects.
 *
 * A file longer than this is read up to the limit and parsed, rather
 * than refused: a truncated last entry fails the scheme check and is
 * counted, and the stations before it are perfectly good. Refusing the
 * lot because the end is ragged would lose 63 working stations to one
 * bad line.
 */
#define STATIONS_FILE_MAX   (64 * 1024)

/*
 * Read the list from the first volume that has one. True if at least one
 * station was parsed; false if no volume has the file, it could not be
 * read, or it contained nothing usable. Safe to call repeatedly -- a
 * card appearing is the reason to.
 *
 * Allocates in PSRAM and frees the previous list. Do not call this from
 * ui_task: it opens a file.
 */
bool stations_load(void);

/*
 * Replace the list with one that did not come off a volume.
 *
 * The radio directory's answer, parsed, installed as THE station list --
 * not a second list beside it. That is what makes the rest of this
 * header go on being true: next and previous move through what is on
 * screen, stations_index() counts in it, the chooser's marker means
 * what it meant, and play_stream() needs no idea where a station came
 * from.
 *
 * `label` is what the status line says instead of a mount path -- "Most
 * voted", a tag name. Copied, not kept by pointer.
 *
 * NOTHING IS WRITTEN TO THE CARD. A directory listing is a view, and
 * the card's own list comes back the moment stations_load() is called
 * again -- which is what the first row of the radio menu does. Keeping
 * a station is a separate act and is not this function.
 *
 * Allocates in PSRAM and frees the previous list, under the same lock
 * and with the same swap-whole discipline stations_load() uses.
 */
bool stations_set_remote(const station_t *list, int count, const char *label);

/*
 * Append one station to stations.m3u on the card, then reload.
 *
 * This is the write half of a file that was read-only until the portal
 * got a station form. `name` may be empty, in which case no `#EXTINF:`
 * line is written and the parser labels the entry by its host -- see
 * station_entry(), which owns the format and refuses anything that
 * would forge extra lines.
 *
 * WHICH VOLUME. The one the current list came from, when there is one:
 * a person adding a station means "add it to my list", and the list
 * they are looking at is on a particular card. When the list came from
 * the directory instead -- stations_volume() is STORAGE_COUNT -- the
 * first present volume gets it, because the alternative is refusing to
 * save a station while the directory is on screen, which is exactly
 * when somebody has found one worth keeping.
 *
 * Reloads through stations_load() on success, so the caller does not
 * have to and so the list is parsed by the same code that reads the
 * file at boot. That means the entry is only in the list if it can be
 * read back -- the round trip station_entry() is tested for.
 *
 * NOT FROM ui_task, for stations_load()'s reason and one more: this
 * writes to a card, and a write that lands behind a decode lease would
 * hold the frame the transport bar is drawing. The portal's HTTP task
 * is the intended caller.
 *
 * False when the fields are refused, the list is already
 * STATIONLIST_MAX long, there is no volume to write to, or the write
 * or the reload failed. The file is left alone in every one of those.
 */
bool stations_append(const char *name, const char *url);

/*
 * Where the current list came from, for the status line: a mount path
 * for the card's, the label above for the directory's. Never NULL.
 */
const char *stations_source(void);

/* How many stations are loaded. 0 before the first successful load. */
int stations_count(void);

/* Which volume the loaded list came from, or STORAGE_COUNT if none. */
storage_id_t stations_volume(void);

/*
 * Copy station `i` out. False if `i` is out of range, in which case
 * `out` is zeroed rather than left as it was -- a caller that ignores
 * the return value gets an empty station and not the previous one.
 */
bool stations_get(int i, station_t *out);

/*
 * The current station, and moving.
 *
 * The index is kept here rather than in player.c because "next station"
 * has to survive a station ending and being replaced, and because the
 * chooser will want to open on the current one.
 *
 * stations_next() and stations_prev() wrap. A station list is a ring:
 * there is no "end of the stations" the way there is an end of an album,
 * and stopping at the last one would make the button dead for a reason
 * nobody can see.
 */
int  stations_index(void);
void stations_set_index(int i);
int  stations_next(void);       /* advances and returns the new index */
int  stations_prev(void);

/*
 * Whether there is anywhere else to go -- i.e. more than one station.
 * This is what streamplan_transport() wants for `have_other_station`,
 * and it is a function rather than `count > 1` at the call site so that
 * the one-station case is decided in one place.
 */
bool stations_have_other(void);

/* The last load's parse statistics, for the log and for a screen that
 * wants to say why a file produced fewer stations than it has lines. */
void stations_stats(stationlist_stats_t *out);

#ifdef __cplusplus
}
#endif
