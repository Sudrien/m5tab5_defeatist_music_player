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
 * JSON Lines, appended. Each save adds one object on its own line and
 * the last valid line wins, so a save is one open-write-close at the
 * end of the file rather than a temp file and a pair of renames -- both
 * quicker and easier on the flash, since it extends the last cluster
 * instead of allocating and freeing directory entries. The file is
 * capped at 64 KB, which is thousands of saves; on reaching it the
 * whole thing is rewritten as a single current record.
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

/*
 * The track that was playing when the player was last put down, or NULL
 * if the file does not name one.
 *
 * A path, taken from the file as written and not checked: the volume it
 * names may not be mounted and the file may have been deleted. The
 * caller decides what to do about that, because the caller is the only
 * one who knows whether it is about to try playing it.
 */
const char *settings_track(void);

/* Record the track being played. Same cost as settings_set_volume():
 * it stores and marks dirty, and the append happens later. */
void settings_set_track(const char *path);

/*
 * Whether ReplayGain is in use at all. Defaults to on.
 *
 * ONE SWITCH FOR BOTH HALVES, deliberately.
 *
 * ReplayGain here is two activities that look separable and are not:
 * measuring a track's loudness while it plays, and applying the stored
 * answer on later plays. Two switches would let a player accumulate
 * measurements it never uses, or -- worse -- apply gains from sidecars
 * while refusing to measure, so a library half-measured under one
 * setting plays at two different levels depending on which tracks
 * happen to have been played before the switch was thrown.
 *
 * Off means off: nothing is measured, nothing is applied, the indicator
 * does not appear, and the sidecars already on the card are left alone
 * rather than deleted. Turning it back on resumes with everything that
 * was learned before it was turned off.
 *
 * It takes effect at the next track. The gain is applied to samples as
 * they are decoded and up to a ring of them is already queued, so
 * changing it mid-track would either do nothing audible for twenty
 * seconds or produce a step in level partway through a song.
 */
bool settings_rg_enabled(void);
void settings_set_rg_enabled(bool on);

/*
 * Crossfade length, in whole seconds. 0 is off.
 *
 * Seconds rather than milliseconds because nobody wants 2,750 ms of
 * crossfade, and a control offering it invites fiddling with a number
 * that has no audible resolution at that scale. SETTINGS_CROSSFADE_MAX
 * is the ceiling.
 *
 * Whether an overlap actually happens at a given boundary is a separate
 * question -- rates must match, the next track has to be decoded far
 * enough ahead, and the boundary has to be a track ending rather than a
 * skip. This is the length it gets when all of that holds, not a
 * promise that it will.
 */
#define SETTINGS_CROSSFADE_MAX  (12)
uint8_t settings_crossfade_sec(void);
void settings_set_crossfade_sec(uint8_t sec);

/*
 * Whether to crossfade between two tracks from the same album.
 *
 * Off by default, and that default is the whole reason this is a
 * separate switch rather than a consequence of the one above.
 *
 * An album is frequently sequenced to run together -- a live record, a
 * DJ mix, a symphony split across movements, anything where track 4 ends
 * mid-phrase because track 5 begins there. Crossfading those is not a
 * softer transition, it is playing three seconds of two different bars
 * at once over a join the producer already made. The listener asked for
 * a crossfade between SONGS; the album boundary is where that intent
 * stops applying.
 *
 * "Same album" is decided by folder, not by tag. A folder is what the
 * playlist is built from, it is what the user picked, and it is right
 * for the untagged rips this player is full of -- the Ogg files in the
 * last log have no ALBUM tag at all, and a tag-based test would call
 * every one of them a different album and crossfade the lot.
 */
bool settings_crossfade_album(void);
void settings_set_crossfade_album(bool on);

#ifdef __cplusplus
}
#endif
