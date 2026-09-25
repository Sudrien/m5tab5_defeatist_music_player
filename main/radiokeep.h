/*
 * radiokeep.h -- two stations kept in flash, for a Tab5 with no media.
 *
 * The README's "zero storage internet radio scenario". A board with a
 * saved Wi-Fi network but no card and no drive had nothing to play: the
 * station list, the favourites and the directory's picks all live on a
 * volume. Now NVS holds two stations beside the saved networks:
 *
 *   LAST   the last station that reached first sound. Written whatever
 *          media is present -- it costs nothing to keep and is the one
 *          a card pulled mid-drive most likely wants back.
 *
 *   STAR   one starred station. ONLY LOOKED AT, AND ONLY CHANGED, WHEN
 *          NO VOLUME IS PRESENT. With media, favourites are the
 *          volume's favorites.m3u and nothing else, and starring writes
 *          there as it always has; this slot is neither read nor
 *          touched. Without media, the star button toggles this slot.
 *
 * With no volume, stations_load() installs these as the station list
 * (STAR first, then LAST if it is a different URL), labelled
 * RADIOKEEP_LABEL.
 *
 * WEAR. NVS appends entries into 4 KB pages and erases a page only when
 * it is full and compacted. A kept station is stored as "name\0url\0" --
 * about a hundred bytes, four or five 32-byte entries -- and LAST is
 * written only when the URL changed. The 20 KB partition then takes
 * millions of station changes before its sectors reach their rated
 * erase count.
 *
 * NVS writes stall the caches briefly. Called from the player task, at
 * a station's first sound, with seconds of audio in the ring.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "stationlist.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADIOKEEP_LABEL     "Kept on this Tab5"

/* A station reached first sound. Stores it as LAST unless it already is. */
void radiokeep_note_played(const char *name, const char *url);

/* STAR, for when no volume is present. */
bool radiokeep_star_is(const char *url);
/* Toggles STAR against `url` and returns the state it ended in: set to
 * this station, or cleared when it already was this one. */
bool radiokeep_star_toggle(const char *name, const char *url);

/* STAR then LAST (when different) into out[0..1]. Returns the count. */
int radiokeep_list(station_t out[2]);

#ifdef __cplusplus
}
#endif
