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

/*
 * Whether the radio is allowed to come up at all. Off by default, and
 * that default is load-bearing.
 *
 * This is a music player that works with no network, and every previous
 * version of it could not have one. Someone who upgrades and finds the
 * radio associating with a saved access point they configured months ago
 * has been surprised by their own device. Off until asked is the only
 * defensible starting position for a transmitter.
 *
 * It is also the switch that makes the network side testable as a whole:
 * with this false, nothing calls esp_wifi_start(), no scan runs, no SNTP
 * client exists and no stream can be opened, so "does the player still
 * work exactly as it did" has a one-bit answer rather than an audit.
 *
 * NO CREDENTIALS LIVE HERE.
 *
 * This file is plaintext on a card people pull out and read, and its
 * design note says so approvingly -- a line saying `volume=35` is a
 * feature. A PSK on that same line is not. The networks themselves live
 * in wifistore, encrypted against the factory eFuse MAC, and the count
 * of them is no business of this module: one switch governs the radio
 * whether there is one saved network or nine.
 *
 TAKES EFFECT AT THE NEXT START, WHICH IS NOT WHAT IT SHOULD DO.
 *
 * This header used to promise the change applied immediately, on the
 * grounds that there is no ring of decoded samples to make a mid-flight
 * change incoherent. That is still the right behaviour and it is not the
 * behaviour: the only reader is wifi_probe(), which runs from the
 * settings push at the start of a track, so a switch thrown while the
 * panel is open is stored and acted on at the next boot.
 *
 * Recorded rather than quietly fixed because it is the shape this
 * project already has a rule about -- a request needs a reader, and a
 * request with no reader is lost rather than pending. Closing it needs
 * wifi_start()/wifi_stop() and a teardown that unwinds esp_wifi and
 * esp_hosted in order without blocking ui_task while the C6 comes up.
 * That is worth doing once the radio has been seen to work, and is not
 * worth writing before then.
 */
bool settings_wifi_enabled(void);
void settings_set_wifi_enabled(bool on);

/*
 * Whether to set the clock from the network.
 *
 * TWO GETTERS, DELIBERATELY.
 *
 * settings_ntp_enabled() is the effective answer and the one anything
 * acting on it must use: it is false whenever the radio is off, because
 * an SNTP client on a device with no network is not a time source, it is
 * a task waking every few seconds to fail. settings_ntp_pref() is the
 * stored preference, which is what the panel draws -- a switch that
 * silently reads OFF because a different switch is off is a switch
 * nobody can learn.
 *
 * The alternative was one getter and a note telling every caller to
 * check the radio as well. Notes like that are correct until the second
 * caller.
 *
 * On by default. Someone who has gone to the trouble of enabling the
 * radio and saving a network has not done that in order to keep setting
 * the clock by hand.
 */
bool settings_ntp_enabled(void);
bool settings_ntp_pref(void);
void settings_set_ntp_enabled(bool on);

/*
 * The last time this player believed, as a signed Unix epoch, and the
 * matching monotonic reading -- what esp_timer_get_time() said at the
 * moment that belief was formed, in microseconds since boot.
 *
 * int64_t, not time_t. This project's time_t may be 32-bit on this
 * toolchain (unconfirmed; check sizeof before assuming either way), and
 * a value that is going to be compared across reboots for years should
 * not inherit a typedef that rolls over on 2038-01-19. Persisted in this
 * wider type regardless of what time() returns; only the final
 * settimeofday() call narrows, where the platform forces it.
 *
 * NOT SET BY NTP DIRECTLY.
 *
 * These exist to answer one question on the next boot: is a newly
 * received time plausible, or is it a large jump that a forged NTP
 * reply would produce -- most usefully, a jump backward to a date when a
 * since-revoked certificate was still valid, which is the specific
 * thing NTP exists here to protect against and so also the specific
 * thing worth not trusting blindly. The comparison is
 *
 *     expected = last_epoch + (esp_timer_get_time() - last_boot_us) / 1e6
 *
 * against the newly claimed time -- monotonic time since the last belief
 * was formed, which nothing on the network can move. A claim close to
 * `expected` is accepted; a claim far from it is not, and the last good
 * value keeps standing while wall-clock keeps advancing from it via the
 * same monotonic arithmetic.
 *
 * DEFAULT: THE BUILD TIME, NOT ZERO OR 1970.
 *
 * Zero fails the plausibility check against everything, including a
 * correct first sync -- "is this real time far from 1970" is true of
 * every real time. The build timestamp is a lower bound that is true by
 * construction: this firmware cannot be running before the moment it was
 * compiled. It is parsed once, from esp_app_get_description(), by
 * settings_init(), and used as the seed only until a real sync -- NTP or
 * a plausible hand-set value -- replaces it. See settings.c for the
 * parse; esp_app_desc_t's date and time fields are US-locale text
 * ("Sep  9 2026", "10:38:28") with no timezone marker and are treated as
 * UTC, which is wrong by whatever the builder's local offset was and
 * right to within the same margin the plausibility check already
 * tolerates.
 */
bool settings_note_ntp_time(int64_t epoch, int64_t boot_us);
int64_t settings_last_ntp_epoch(void);
int64_t settings_last_ntp_boot_us(void);

/*
 * The floor right now: the last accepted epoch plus the monotonic time
 * since it was accepted. What this player believes the time to be.
 *
 * This is what is written to the file, on every write, so `ntp_epoch` is
 * an updated-at rather than a synced-at. It advances across a session
 * whether or not anything syncs, so a device that never reaches a
 * network still records forward progress, and each reboot's floor is the
 * later of the build time and the last write.
 *
 * NOT time(). The system clock is not set from any of this -- nothing
 * calls settimeofday() yet -- so time() still returns 1970, which is the
 * one value the floor exists to refuse. This is derived arithmetic and
 * needs no clock.
 */
int64_t settings_now(void);

/*
 * NO ZONE SETTING, DELIBERATELY.
 *
 * NTP answers in UTC and nothing here displays a local time: there is no
 * clock on screen and no control that names an hour. A zone would be a
 * stored string with no reader.
 *
 * What the clock is actually for is TLS certificate validity -- every
 * chain is notBefore-invalid at 1970, so an unset clock fails the
 * handshake outright -- and FAT timestamps on the files this player
 * writes to the card, which is a volume people put in a computer. Both
 * want UTC and neither wants a zone.
 *
 * When something does display a local time, the zone arrives with it:
 * a `tz` key holding a POSIX TZ string, a generated name-to-rule table,
 * and a picker, because there is no keyboard on this device. That is
 * three pieces of work in service of one screen, and none of it is owed
 * until the screen exists.
 */


#ifdef __cplusplus
}
#endif
