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
} play_order_t;

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
