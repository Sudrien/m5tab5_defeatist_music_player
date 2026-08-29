/*
 * settings.h -- the handful of things worth surviving a power cycle.
 *
 * One file, `.defeatist.dat`, in the root of the volume the music is
 * playing from, with `.defeatist.bak` beside it holding the previous
 * good copy. Both are dotfiles and both are flagged hidden on FAT, so
 * they stay out of the way on a card someone plugs into a computer to
 * add music.
 *
 * Nothing is written until a track plays, because until then there is
 * no answer to which volume the settings belong on. With no volume at
 * all -- no card, no drive -- the settings live in memory for the
 * session and are written to the first volume that turns up.
 *
 * Text, not a packed struct. This is a card people pull out and put in a
 * computer, and a line saying `volume=35` is something they can read,
 * edit and delete; a binary blob with a CRC is something they can only
 * wonder about. It also degrades in the right direction -- an
 * unrecognised key is skipped rather than invalidating the file, so a
 * card moved between two builds of this firmware loses the settings the
 * older one does not know about and keeps the rest.
 *
 * Nothing here is required for the player to work. Every failure --
 * no card, read-only card, corrupt file, no file at all -- lands on the
 * built-in defaults and is logged once, because a music player that will
 * not start because it could not read its preferences is a worse
 * program than one that forgets them.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load the file and start the writer task.
 *
 * Call after storage_init(), so a mounted volume can be found, and
 * before anything reads a setting. Never fails in a way worth checking:
 * a missing or unreadable file leaves the defaults in place.
 */
void settings_init(void);

/*
 * Tell settings which volume the music is on.
 *
 * Call with the path of each track as it starts. The settings file
 * lives beside the music rather than on whichever volume happened to be
 * mounted first: a card carried to another player carries its volume
 * with it, and a session played entirely off a USB drive is saved at
 * all, which it was not when the root was chosen at init and the drive
 * was still enumerating.
 *
 * Idempotent and cheap -- it returns without doing anything unless the
 * volume has changed. The first call of the session is the one that
 * loads the file; later ones carry the settings in force across to the
 * new volume rather than loading over them.
 *
 * Returns true when that first adoption has just happened, meaning the
 * values in this module are now the ones that apply and the caller has
 * to push them wherever they are acted on -- the codec does not read
 * them. False every other time, including every repeat call for the
 * same volume.
 */
bool settings_note_path(const char *path);

/*
 * The volume the player was last left at, 0-100, or 50 if it has never
 * been told otherwise.
 */
uint8_t settings_volume(void);

/*
 * Record a new volume. Cheap and non-blocking -- it stores the value and
 * marks the file dirty; the write happens later on a task of its own.
 *
 * Safe to call from a drag emitting fifty of these a second. See
 * SETTINGS_SETTLE_MS in settings.c for why the write does not follow each one.
 */
void settings_set_volume(uint8_t percent);

#ifdef __cplusplus
}
#endif
