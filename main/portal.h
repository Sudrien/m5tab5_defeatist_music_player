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
 * ======================= SETTLED DECISIONS =======================
 *
 * PLAYBACK STOPS. The portal refuses to start while a track is playing,
 * and the panel says so rather than stopping the music underneath
 * somebody.
 *
 * Not because of the bus -- the C6 is on SDIO2 and the card on SDIO1, so
 * storage_io's leases never see the radio. Because softAP plus an HTTP
 * server plus a join attempt is real CPU and real PSRAM alongside a
 * decoder with a deadline, and the failure if that is wrong is a
 * starved ring: audible, intermittent, and blamed on the card. Setting
 * up a network is a deliberate act that takes a minute and happens once
 * per place; playing music is what the device is for. Making the rare
 * thing exclude the common one is cheaper than making them coexist and
 * discovering later that they mostly do.
 *
 * THE AP IS NAMED "Defeatist-XXXX", where XXXX is derived from the
 * station MAC. Two of these in one room have to be distinguishable --
 * a fixed name means picking blind on a phone -- and the MAC is
 * available by the time the AP is configured, since wifi.c already logs
 * it. Derived rather than the raw octets: the last two bytes of a MAC
 * are printed on nothing a person can see, so the hex is no more
 * meaningful to them than a hash and the hash does not publish the
 * address to everyone in range. It is a label, not an identifier, and it
 * only has to differ between two devices standing next to each other.
 *
 * FIVE MINUTES, then the AP comes down on its own.
 *
 * The portal is started deliberately from the NET tab, so it could
 * simply run until stopped -- but the case that matters is the one where
 * nobody stops it: someone opens it, is interrupted, and walks away
 * leaving an unattended device broadcasting an open network with a form
 * on it. Five minutes is long enough to find the phone, join, mistype
 * the passphrase once and retry, and short enough that walking away ends
 * it. PORTAL_TIMEDOUT is a distinct status from PORTAL_OFF so the panel
 * can say what happened rather than appearing to have done nothing.
 *
 *
 * ======================== ANSWERED ON HARDWARE ========================
 *
 * THIS C6'S FIRMWARE DOES APSTA. First flash, esp_hosted 3.0.7 against
 * M5's 0.0.0 slave:
 *
 *   eh_wifi: set_config iface=AP ssid="Defeatist-3E78" (14 bytes)
 *   tab5_wifi: APSTA up: Defeatist-3E78 is broadcasting
 *   esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF ...
 *   esp_netif_lwip: DHCP server assigned IP to a client, IP is: 192.168.4.2
 *   tab5_portal: submitted fivescore: password 14 bytes, 14 chars
 *   tab5_portal: trying fivescore (passphrase)
 *   tab5_wifi: join fivescore failed: ESP_ERR_WIFI_PASSWORD (reason 2)
 *   tab5_wifi: address 192.168.5.62
 *   tab5_portal: saved fivescore as passphrase
 *   tab5_wifi: NTP sync: 1789079956
 *   tab5_portal: dns: stopped after 86 answers
 *
 * The whole path works: AP, lease, DNS lie, form, join, save, NTP 1.4 s
 * after the address, AP down. "DHCP server started" prints twice per
 * start -- once for the AP coming up, once for dhcp_offer_dns()
 * restarting it -- and that is expected.
 *
 * WHAT THE FIRST ATTEMPT SHOWED ABOUT PSK-FIRST. fivescore is a WPA2/WPA3
 * transition network. The derived PSK was refused with reason 2
 * (AUTH_EXPIRE) after 3.6 s and the passphrase joined, so the passphrase
 * is what was stored. The station config offers SAE (sae_pwe_h2e, PMF
 * capable), and a transition AP apparently gets SAE, which cannot use a
 * precomputed key. Whether a PSK attempt with SAE withheld would join the
 * same AP's WPA2 side -- and so keep the passphrase off the device for
 * transition networks too -- is NOT answered. It is the next thing to
 * try, and it should be a log first.
 *
 * ======================== STILL UNANSWERED ========================
 *
 * WHAT THE PHONE SEES DURING THE JOIN. One radio, one channel: joining
 * the home network moves the AP to that network's channel, and a phone
 * on the setup AP may drop for a moment. The result is always on the
 * device's screen; the status page on the phone is a courtesy that may
 * not arrive.
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
 * Five minutes. See the note above: the case this exists for is the one
 * where nobody comes back.
 */
#define PORTAL_TIMEOUT_S    (300)

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

    /* The AP this device is broadcasting -- "Defeatist-XXXX" -- for the
     * panel to display, so someone knows which network to look for on a
     * phone that may be showing several. */
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

    /* Seconds until the portal gives itself up, counting down from
     * PORTAL_TIMEOUT_S; 0 if not running. Shown, because a countdown is
     * the difference between a portal that appears to have hung and one
     * that is visibly waiting. */
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
 * Also ESP_ERR_INVALID_STATE while a track is playing. The caller is
 * expected to have said so already: the panel greys the button and
 * explains, rather than presenting a control that fails when pressed.
 * The check is here as well because a greyed control is a courtesy and
 * this is the guarantee.
 *
 * Safe to call when already running: returns ESP_OK and changes nothing.
 */
esp_err_t portal_start(void);

/*
 * Called once at boot, before anything else here. `is_playing` answers
 * "is a track playing right now" for the refusal above; the portal has
 * no other way to ask the player, and the player has no header.
 */
void portal_init(bool (*is_playing)(void));

/* Whether the portal is up or coming up. A value, safe anywhere. */
bool portal_running(void);

/*
 * Ask for portal_stop() and return at once. The one ui_task calls.
 */
void portal_request_stop(void);

/*
 * Take the AP down and return the radio to STA.
 *
 * Safe to call when not running. Waits for the portal's task to take the
 * server and the AP down -- which can include the end of a join attempt
 * in progress, so up to about twenty seconds -- and must not be called
 * from ui_task. The panel uses portal_request_stop().
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
