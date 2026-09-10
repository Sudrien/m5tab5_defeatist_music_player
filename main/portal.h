/*
 * portal.h -- the way a network gets into this player.
 *
 * There is no keyboard. A WPA passphrase is up to 63 characters of mixed
 * case, digits and punctuation, and building an on-screen keyboard for a
 * field typed once per network would be the largest thing on this
 * device's screen in service of its least frequent action -- larger than
 * panel.c and browser.c together, and squarely against the first UI
 * decision in CLAUDE.md, which is that a toolkit would be more code than
 * the thing it draws.
 *
 * So: an open access point, a DNS server that answers every query with
 * its own address, and a form served to whatever phone joins it. The
 * phone has the keyboard.
 *
 * This is the same conclusion the map project reached on the same
 * hardware, and portal.cpp there is working prior art. What follows is
 * mostly about the two things that must NOT be carried over.
 *
 *
 * ================= WHAT CHANGES FROM THE MAP PROJECT =================
 *
 * ONE: IT CANNOT BLOCK.
 *
 * portal_run(timeout_ms) there is a loop that owns the screen for
 * minutes and pumps M5.update() itself. That is correct in a map at
 * boot, where nothing else has started yet.
 *
 * Here it is not available. ui_task is the single writer of the
 * framebuffer -- panel_draw() and panel_touch() both run on it -- and
 * the decoder is a different task that keeps playing throughout. A
 * blocking portal freezes the transport bar and the seek drag over live
 * audio, which is worse than either a frozen screen or silence alone,
 * because the device looks crashed while sounding fine.
 *
 * Hence start/stop/state below rather than one call. The HTTP server
 * runs on its own task, which esp_http_server does anyway; the join
 * attempt runs there too. ui_task only ever reads a snapshot.
 *
 * TWO: STATE IS COPIED, NEVER BORROWED.
 *
 * portal_state() fills a caller-owned struct. It does not return a
 * pointer to anything the portal owns, and there is no
 * portal_current_ssid() returning a const char *.
 *
 * CLAUDE.md is unambiguous here and it is written up as a crash that
 * cost an afternoon: media_task polled s_pcm behind a null guard that
 * could not work, and it presented as a TLSF assert on an unrelated
 * free. A pointer can be loaded before the guard and used after the
 * free. The portal writes its state from the HTTP task while ui_task
 * reads it at 1 Hz, which is exactly that shape. A 100-byte copy under a
 * mutex is cheaper than finding it again.
 *
 * This is also why portal_state_t has no `const char *msg`. The status
 * is an enum and the SSID is a fixed array. Anything a person needs to
 * read in words is looked up from the enum by panel.c at draw time.
 *
 *
 * ===================== WHAT CARRIES OVER INTACT =====================
 *
 * VERIFY BEFORE STORING. The portal joins the network with the
 * credential before it writes anything. Saving first and discovering the
 * typo at the next boot is a far worse afternoon on a device whose only
 * input is a phone that has since disconnected.
 *
 * PSK FIRST, PASSPHRASE ON REFUSAL. WPA2 runs
 * PBKDF2-HMAC-SHA1(passphrase, ssid, 4096) to a 256-bit key that
 * wpa_supplicant accepts directly as 64 hex characters, so the
 * passphrase -- which people reuse across services -- never has to be
 * stored. WPA3-SAE derives its key by another route and has no
 * precomputable equivalent, so there the passphrase itself must be kept.
 * The portal tries the derived PSK, and on refusal retries with the raw
 * passphrase; whichever worked is what gets stored, and wifistore's
 * is_psk records which. The scan on this hardware showed WPA2/WPA3
 * transition-mode APs are common, so the passphrase path is ordinary
 * rather than exceptional.
 *
 * ESCAPE THE SSID. An SSID is up to 32 arbitrary bytes and goes into
 * HTML. "Dave's WiFi" is real and a beacon crafted to close a tag is
 * cheap.
 *
 * NEVER LOG THE PASSPHRASE, including on the failure path, where the
 * temptation is strongest.
 *
 *
 * ========================== OPEN QUESTIONS ==========================
 *
 * These are decisions, not oversights, and each is written where it has
 * to be answered rather than settled quietly in the implementation.
 *
 * Q1. WHAT HAPPENS TO PLAYBACK WHILE THE PORTAL IS UP? The C6 is a
 * separate chip on SDIO2 and the card is SDIO1, so there is no bus
 * contention -- storage_io's leases are untouched. But softAP plus HTTP
 * is real CPU and real PSRAM. My assumption below is that playback
 * continues untouched and the portal is simply another task. If the
 * decoder starves, storage_io_stats()' worst PLAYBACK wait is the number
 * that shows it, and the answer would be to refuse to start the portal
 * while playing rather than to throttle anything.
 *
 * Q2. WHAT IS THE AP CALLED? The map uses "Tab5-Map-Setup". A fixed name
 * means two of these devices in one room are indistinguishable. Adding
 * the last two MAC octets fixes that and makes the name uglier. Fixed
 * name assumed below; the MAC is available by then if it should change.
 *
 * Q3. TIMEOUT. The map takes it as an argument. Here the portal is
 * started deliberately from a settings tab rather than at boot, so it
 * could simply run until stopped. But an open AP left up because someone
 * walked away is a real if minor exposure, and an unattended device
 * broadcasting an open network indefinitely is worse than one that gives
 * up. A timeout is assumed, with the value below as a placeholder.
 *
 * Q4. DOES THE RADIO SURVIVE? Starting an AP means esp_wifi in
 * APSTA mode. wifi.c currently brings the radio up as STA and scans
 * once, and has no teardown at all -- wifi_probe() is a one-shot spike
 * with s_up that never clears. The portal needs the radio in a known
 * state on the way in and back to STA on the way out. That is wifi.c
 * work that this header depends on and does not describe.
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

/* 32 bytes plus a terminator, matching wifistore. */
#define PORTAL_SSID_MAX     (32)

/*
 * What the portal is doing, as one value.
 *
 * An enum rather than a string because it crosses a task boundary and
 * because panel.c has to render it in a fixed-width row; the words live
 * at the draw site, where the space available is known.
 */
typedef enum {
    PORTAL_OFF = 0,     /* not running                                  */
    PORTAL_STARTING,    /* AP coming up; not yet joinable               */
    PORTAL_WAITING,     /* AP up, nobody has submitted anything         */
    PORTAL_TRYING,      /* joining with a submitted credential          */
    PORTAL_SAVED,       /* joined and stored; about to stop             */
    PORTAL_FAILED,      /* the credential did not work; still waiting   */
    PORTAL_TIMEDOUT,    /* gave up; AP down                             */
    PORTAL_ERROR,       /* could not start at all                       */
} portal_status_t;

/*
 * A snapshot, filled into the caller's storage.
 *
 * Every field is a value. See the note above on why there is no pointer
 * here and never will be.
 */
typedef struct {
    portal_status_t status;

    /* The AP this device is broadcasting, for the panel to display so
     * someone knows what to look for on their phone. */
    char ap_ssid[PORTAL_SSID_MAX + 1];

    /* The network last submitted, for PORTAL_TRYING / FAILED / SAVED.
     * Empty otherwise. Never the passphrase. */
    char last_ssid[PORTAL_SSID_MAX + 1];

    /* Phones currently associated. Zero while waiting is the normal
     * state and is worth showing: it distinguishes "nobody has joined
     * the setup network yet" from "somebody is filling in the form",
     * which are the two halves of the only thing that can go wrong here
     * that a person can act on. */
    uint8_t clients;

    /* Seconds until the portal gives itself up, 0 if not running. */
    uint16_t seconds_left;
} portal_state_t;

/*
 * Bring the AP up. Returns as soon as the attempt is under way.
 *
 * Requires the radio: settings_wifi_enabled() must be true and wifi.c
 * must have it up. Returns ESP_ERR_INVALID_STATE otherwise rather than
 * turning the radio on as a side effect -- the Wi-Fi switch means what
 * it says, and a portal that silently enables a transmitter would make
 * that switch a lie in the one place it matters most.
 *
 * Safe to call when already running: returns ESP_OK and changes nothing.
 */
esp_err_t portal_start(void);

/*
 * Take the AP down and return the radio to STA.
 *
 * Safe to call when not running. Blocks only as long as esp_wifi needs
 * to change mode, so it must not be called from ui_task; the panel asks
 * for a stop by some means that does not wait for it.
 */
esp_err_t portal_stop(void);

/*
 * Copy the current state out. Safe from any task.
 *
 * Fills `out` with PORTAL_OFF and empty strings when nothing is running,
 * so a caller that ignores the return value still gets something
 * coherent to draw.
 */
void portal_state(portal_state_t *out);

#ifdef __cplusplus
}
#endif
