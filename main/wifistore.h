/*
 * wifistore.h -- the networks this player has been told about.
 *
 * A short list, oldest evicted, keyed by SSID. Nothing here brings the
 * radio up or joins anything; it stores what a join would need and says
 * which of the saved networks a scan just saw.
 *
 * WHY THIS IS NOT IN .defeatist.dat
 *
 * That file is plaintext on a card people pull out and read on a
 * computer, and settings.h argues for that at length -- a line saying
 * `volume=35` is something someone can read, edit and delete. A PSK is
 * the opposite kind of value and does not belong on the same line.
 *
 * WHY THIS IS NOT ON THE CARD AT ALL
 *
 * Everything else this program persists lives beside the music, and this
 * deliberately does not.
 *
 * The reason is the threat. A credential store's realistic risk is a
 * misplaced SD card: the card leaves the device, someone reads it, and
 * the home network's key is on it. Defending that means encrypting at
 * rest against something the card does not carry -- the factory eFuse
 * MAC -- so the file is inert anywhere else.
 *
 * But that same binding destroys the only reason to be on the card.
 * A card carried to a second Tab5 would hold a file the second Tab5
 * cannot decrypt: present, correct, and silently useless, which is a
 * worse failure than not being there. So the card buys nothing and costs
 * a key-derivation path, an IV, an HMAC and a version byte.
 *
 * In NVS the risk does not arise. NVS cannot be misplaced separately
 * from the device it is soldered to, and a stolen *device* is not
 * defensible by any scheme that stores usable credentials -- the radio
 * has to present something a router accepts, so anything the device can
 * read, someone holding the device can read. That is true of an
 * encrypted file too; encrypting it only ever protected the card.
 *
 * So: no encryption here, and that is not a corner cut. If you want the
 * device itself covered, the answer is CONFIG_NVS_ENCRYPTION with flash
 * encryption enabled, which is a build-level decision this file should
 * not be second-guessing with a scheme of its own.
 *
 * WHAT IS STORED, AND WHY IT IS SOMETIMES THE PASSPHRASE
 *
 * WPA2 runs PBKDF2-HMAC-SHA1(passphrase, ssid, 4096) to get a 256-bit
 * PSK, and wpa_supplicant takes that directly as 64 hex characters. So
 * on WPA2 the passphrase -- which people reuse across services -- never
 * has to be stored, and is not. That includes the Wi-Fi driver's own
 * copy: wifi.c sets esp_wifi_set_storage(WIFI_STORAGE_RAM), without which
 * every join attempt would be written to the driver's NVS as well, raw
 * passphrase and mistyped attempts included.
 *
 * WPA3-SAE derives its key by a different route and has no precomputable
 * equivalent, so on SAE networks the passphrase itself is what must be
 * kept. Routers advertising "WPA2/WPA3-Personal" are in transition mode
 * and a WPA3-capable client picks SAE, which makes the passphrase the
 * common case on current hardware rather than the exception. `is_psk`
 * records which kind a record holds; nothing else in the program should
 * have to guess.
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
 * Eight.
 *
 * Not a memory limit -- eight records is under a kilobyte. It is the
 * number of networks a portable music player is plausibly carried
 * between: home, work, a relative's, a couple of cafes. A list long
 * enough to need scrolling on the NET tab is a list nobody prunes, and
 * the eviction below is only defensible while the thing being evicted is
 * genuinely stale.
 */
#define WIFISTORE_MAX       (8)

/* 32 characters plus a terminator. An SSID is up to 32 BYTES and is not
 * required to be text at all; it is kept as bytes and only ever compared,
 * never parsed. */
#define WIFISTORE_SSID_MAX  (32)

/* 64 hex characters of PSK, or a passphrase of 8..63. The larger wins. */
#define WIFISTORE_SECRET_MAX (64)

typedef struct {
    char ssid[WIFISTORE_SSID_MAX + 1];
    char secret[WIFISTORE_SECRET_MAX + 1];
    bool is_psk;            /* false when `secret` is a raw passphrase */
} wifistore_cred_t;

/*
 * Read the list into memory. Call once, after nvs_flash_init().
 *
 * Never fails in a way worth checking, for settings.c's reason: a player
 * that will not start because it could not read its network list is a
 * worse program than one that starts with no networks. A missing or
 * unreadable namespace leaves an empty list and logs once.
 */
void wifistore_init(void);

/* How many records are held, 0..WIFISTORE_MAX. */
int wifistore_count(void);

/*
 * Copy record `i` out. Returns false if `i` is out of range.
 *
 * COPY-OUT, NOT A BORROW, and every accessor here is the same.
 *
 * The list is written from whatever task runs the portal and read by
 * ui_task to draw the NET tab. CLAUDE.md's rule is that cross-task code
 * publishes a value rather than a handle, because a guarded pointer can
 * still be loaded before the guard and used after the free -- which is
 * the media_task/s_pcm crash, and it presented as a TLSF assert on an
 * unrelated free. A 100-byte copy under the lock is cheaper than the
 * afternoon spent finding that.
 */
bool wifistore_get(int i, wifistore_cred_t *out);

/*
 * Store a network, replacing any record with the same SSID.
 *
 * Replace rather than append, because a saved network whose password
 * changed is the same network. Two records for one SSID would leave the
 * join order deciding which password is tried, and the wrong one would
 * work exactly until the router was rebooted.
 *
 * At WIFISTORE_MAX the oldest record is dropped. "Oldest" is insertion
 * order, not last-used: tracking last-used means a write on every
 * successful join, which is a flash erase cycle for a fact nobody asked
 * to keep.
 *
 * Returns ESP_ERR_INVALID_ARG for an empty SSID or a secret outside the
 * lengths its kind allows -- 64 hex for a PSK, 8..63 for a passphrase.
 * The caller is expected to have verified the credential works before
 * calling; see portal.h.
 */
esp_err_t wifistore_save(const char *ssid, const char *secret, bool is_psk);

/* Forget one network by SSID. ESP_ERR_NOT_FOUND if it was not held. */
esp_err_t wifistore_forget(const char *ssid);

/* Forget all of them. */
esp_err_t wifistore_clear(void);

/*
 * The saved network to try, given what a scan just found.
 *
 * `seen` is the SSIDs from the scan and `rssi` their strengths, both of
 * length `n`. Returns the index into the store of the strongest saved
 * network present, or -1 if none of them are.
 *
 * This exists because ESP-IDF will not do it. esp_wifi_connect() joins
 * one configured AP; there is no list and no "try these in order". So
 * something has to intersect what is saved with what is in the air, and
 * doing it by RSSI rather than by store order is the difference between
 * joining the strong AP in the building and joining whichever one was
 * saved first.
 *
 * Separate from the joining so it can be tested: this is the part with
 * the logic in it and it needs no radio to run.
 */
int wifistore_best(const char *const *seen, const int8_t *rssi, int n);

/*
 * Every saved network the scan saw, strongest first, COPIED into `out`
 * (up to `max`). Returns how many.
 *
 * wifistore_best() answers "which one", and a single answer is not enough
 * once more than one saved network is in range: if the strongest refuses
 * -- its password changed, or it is a guest network that has expired --
 * the next one should be tried rather than nothing until the next scan.
 *
 * Copies rather than indices, because the join that follows takes
 * seconds per network and the portal can save in the middle of it. An
 * index taken before an eviction names a different network after it.
 *
 * One SSID seen on several APs counts once, at its strongest reading.
 * Ties go to the earlier record, as in wifistore_best().
 */
int wifistore_rank(const char *const *seen, const int8_t *rssi, int n,
                   wifistore_cred_t *out, int max);

#ifdef __cplusplus
}
#endif
