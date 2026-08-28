/*
 * settings.h -- the handful of things worth surviving a power cycle.
 *
 * One file, `defeatist.dat`, in the root of the microSD if there is one
 * and the USB volume otherwise, with `defeatist.bak` beside it holding
 * the previous good copy.
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
