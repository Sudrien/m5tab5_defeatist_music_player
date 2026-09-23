/*
 * mediadir.h -- one folder's entries with their sizes and dates, in one
 * pass, for the media walk.
 *
 * POSIX readdir() gives a name and a type and nothing else, so the walk
 * paid a stat() per file for its stamp. On FAT that is worse than it
 * sounds: stat() finds the name by reading the folder from the top, so
 * a folder of a hundred tracks is read a hundred times over. The 5018
 * log put an unchanged pass over 1192 tracks at 9.5 s, nearly all of it
 * this. MEDIA-INDEX.md saw it coming -- "a walk that wants mtimes pays a
 * stat() per file, and the walk is the expensive part".
 *
 * FatFs's own f_readdir() fills a FILINFO with the size and the date
 * and time words as it goes, which is how ESP-IDF's readdir() works
 * underneath -- it throws them away. This reads them. FAT32 and exFAT
 * alike: FILINFO's size is 64-bit when exFAT is compiled in, and FatFs
 * gives exFAT's times in the same two words.
 *
 * The names are the ones ESP-IDF's readdir() gives the chooser, byte
 * for byte, because that readdir() copies them out of the same FILINFO.
 * The walk and the chooser cannot disagree about what a folder holds.
 *
 * One folder open at a time -- the walk reads a folder whole and closes
 * it before it descends -- so the state is static, and nothing here is
 * reentrant. Each FatFs call takes the BACKGROUND lease for itself.
 *
 * The device has mediadir.c. texttest supplies its own, over POSIX, so
 * the walk's host test runs the real mediawalk.c on a real tree.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "mediaindex.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char  *name;      /* good until the next mdir_next() */
    bool         is_dir;
    midx_stamp_t stamp;     /* size, and midx_fat_time() of the entry */
} mdir_ent_t;

/* Open a folder by its VFS path ("/sd/Artist"). False if it cannot be
 * opened, or its volume's FatFs drive cannot be named for certain. */
bool mdir_open(const char *path);

/* 1 with an entry, 0 at the end, -1 on an error -- which a walk must
 * treat as a folder it could not finish, never as the end. */
int mdir_next(mdir_ent_t *out);

void mdir_close(void);

#ifdef __cplusplus
}
#endif
