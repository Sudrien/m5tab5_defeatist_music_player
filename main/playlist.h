/*
 * playlist.h -- one directory's worth of playable files, in order.
 *
 * A folder is the unit rather than a saved list of arbitrary tracks,
 * because a folder is what an album is on disk. Nothing here is
 * persisted: the list is rebuilt from the directory whenever one is
 * chosen, so a file added on a desktop appears the next time that folder
 * is opened rather than after a rescan nobody remembered to run.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAY_ORDER_ONE = 0,     /* stop at the end of the track */
    PLAY_ORDER_ALL,         /* on to the next, stop at the end of the folder */
    PLAY_ORDER_SHUFFLE,     /* random, without repeating until exhausted */
    PLAY_ORDER_REPEAT_ONE,  /* this track again, until told otherwise */
} play_order_t;

/*
 * ONE and REPEAT_ONE are both about one track and they are opposites.
 *
 * ONE stops when the track ends; REPEAT_ONE plays it again. Neither is
 * about the skip button -- pressing next under either still moves to
 * the next track, because a press is not the end of a track. The names
 * are as close as the concepts, so both call sites map them the same
 * way and say why.
 */

/* Replace the list with the playable files in dir, sorted case-insensitively.
 *
 * Directories are not descended. An album that is one folder deep with
 * disc subfolders is two choices rather than one, which is the honest
 * rendering of what is on the card -- a recursive scan of a card root
 * would be a several-thousand-entry list and a long stall on the touch
 * that asked for it. */
esp_err_t playlist_load_dir(const char *dir);

/* Empty the list and forget the folder. */
void playlist_clear(void);

int playlist_count(void);

/* NULL when i is out of range. The returned pointer is owned by the
 * playlist and is invalidated by the next playlist_load_dir(). */
const char *playlist_path(int i);

/* Index of path in the current list, or -1. Compares whole paths, so a
 * track chosen from a different folder does not match a same-named track
 * in this one. */
int playlist_index_of(const char *path);

/* Where playback is. -1 when the current track is not in the list --
 * which is the normal state for a single file chosen and then played
 * while the list holds something else. */
int playlist_current(void);
void playlist_set_current(int i);

/*
 * Next path to play under this order, or NULL to stop.
 *
 * Shuffle keeps a played-bitmap rather than picking uniformly at random,
 * so a twelve-track album plays twelve different tracks. The bitmap
 * clears when it fills, which makes the next pass a fresh shuffle rather
 * than a repeat of the same order.
 */
const char *playlist_next(play_order_t order);

/*
 * What playlist_next() would return, without consuming anything.
 *
 * For prefetch. Returns the CURRENT path under PLAY_ORDER_REPEAT_ONE,
 * which is the honest prediction and costs nothing -- the file is
 * already open and its sidecar already held, so priming it again is a
 * cache hit rather than a read. Returns NULL under PLAY_ORDER_ONE
 * (nothing follows) and,
 * deliberately, under PLAY_ORDER_SHUFFLE: shuffle's choice is made by
 * esp_random() at the moment it is asked, so there is no next track to
 * predict. Predicting one would mean fixing the choice early, which
 * turns "random when you get there" into "decided a song ago" and makes
 * the played-bitmap lie if the track is skipped. Shuffle simply does not
 * prefetch, and that is the honest answer rather than a missing feature.
 */
const char *playlist_peek_next(play_order_t order);

/*
 * Whether pressing next would do anything, for the UI to grey the button.
 *
 * Distinct from playlist_peek_next() != NULL, and the difference is the
 * whole reason it exists: under shuffle there is no predictable next
 * track, but there is certainly a next track, so peek returns NULL and
 * this returns true. Greying the button under shuffle would say the
 * playlist had ended when it had not.
 *
 * PLAY_ORDER_ONE is mapped to ALL, matching what UI_ACTION_NEXT actually
 * does: repeat-one governs what happens when a track ENDS, not what the
 * skip button means. Greying next in repeat-one would be wrong about a
 * button that works.
 */
bool playlist_has_next(play_order_t order);

/* Previous entry in list order, or NULL at the top. Shuffle deliberately
 * does not have a history: a back button that undoes a random choice
 * needs a stack, and the button is there to skip back one track. */
const char *playlist_prev(void);

/* The folder this list came from, or "" -- shown in the chooser. */
const char *playlist_dir(void);

/* Largest number of tracks in one folder. A folder past this is truncated
 * and logs; the cap exists because each entry is a strdup of a path up to
 * 512 bytes, and an unbounded readdir on a card root is an unbounded
 * allocation on a touch event. */
#define PLAYLIST_MAX    (1024)

#ifdef __cplusplus
}
#endif
