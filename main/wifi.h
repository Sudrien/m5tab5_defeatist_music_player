/*
 * wifi.h -- the radio, which is on another chip.
 *
 * The P4 has no radio. The Tab5 carries an ESP32-C6-MINI-1U beside it,
 * reached over SDIO2, running slave firmware that speaks ESP-Hosted; the
 * `esp_wifi_remote` component makes that look like an ordinary esp_wifi
 * on this side. Everything below is about getting the C6 to exist.
 *
 * The radio has a lifetime now rather than a single probe. wifi_start()
 * powers the C6, brings ESP-Hosted up and starts the driver in STA mode;
 * wifi_stop() unwinds all of it and cuts the power. Between them the
 * radio is on, and joins saved networks -- see "Joining" below.
 *
 * WHY A TEARDOWN EXISTS AT ALL
 *
 * Two callers need one and neither could have it before.
 *
 * The Wi-Fi switch on the NET tab is documented in settings.h as taking
 * effect at the next boot, which it should not: the only reader ran at a
 * track boundary, so a switch thrown with the panel open did nothing
 * visible until a power cycle. Turning the radio off has to mean off
 * now.
 *
 * And the portal needs the radio in a known state on the way in and back
 * to STA on the way out. A one-shot spike whose s_up never cleared could
 * not give it either.
 *
 * WHAT IS SAFE TO CALL FROM WHERE
 *
 * Neither of these may be called from ui_task. wifi_start() takes about
 * two seconds -- most of it CONFIG_ESP_HOSTED_HOST_CP_RESET_SETTLE_MS
 * waiting for the C6 to come up -- and wifi_stop() blocks as long as
 * esp_wifi needs to change mode. ui_task is the single writer of the
 * framebuffer, so either one blocks the transport bar over live audio.
 * The panel asks for a change; something else performs it.
 *
 * TWO GATES, IN ORDER, AND NEITHER IS OPTIONAL
 *
 * WLAN_3.3V is switched. U38 is a load switch from SOC_3.3V, enabled by
 * WLAN_PWR_EN, which is P0 of the PI4IOE5V6408 at 0x44 -- the second
 * expander, the one usbhost.c already drives for USB5V_EN on P3. Until
 * that bit is written the C6 is not merely uninitialised, it is
 * unpowered, and SDIO finds nothing on the other end.
 *
 * Then RF_C6_RST, the C6's EN pin from GPIO15 (SOC_EXTRF_RST) through 1K
 * against a 10K pulldown -- but that one is esp_hosted's to drive, as
 * part of bringing the transport up. This file does the power only; two
 * owners of one reset pin would be decided by whichever ran second.
 *
 * The failure when the power is missed looks exactly like a host-side
 * fault -- sdmmc_card_init failing over and over while every pin on this
 * side logs correctly -- which is a long way to go to find an unwritten
 * expander bit.
 *
 * THE PINS ARE SET HERE, NOT IN A MENU
 *
 * esp_hosted >= 3.0.2 carries per-board GPIO defaults and the Tab5 is
 * among them, but this file sets the pins itself with
 * esp_hosted_sdio_set_config() before esp_hosted_init(). A board's
 * wiring is not a build option: it cannot be chosen wrongly by anyone
 * holding this hardware, and a project that asks someone to pick it in a
 * menu has invented a way to get it wrong. Three builds went out with
 * the P4-Function-EV-Board pins because that menu had not been visited.
 *
 * PI4IOE2_IO_DIR is already 0xB9 and bit 0 is set, so P0 has been
 * configured as an output since the first boot of this program. Nothing
 * has ever driven it.
 *
 * THE ANTENNA IS ALREADY RIGHT
 *
 * RF_PTH_L_INT_H_EXT selects between the on-board 3D antenna and the
 * external MMCX through U29. It is P0 of expander 1, and PI4IOE1_OUT_SET
 * is 0x76 -- bit 0 clear, which is the internal antenna. Nothing here
 * touches it; it is noted so the next person does not go looking.
 *
 * SDIO2 IS NOT THE CARD
 *
 * The card is SDIO1 on GPIO39-44. The C6 is SDIO2 on GPIO8-13. Separate
 * pins and a separate slot, so storage_io.c's leases are unaffected and
 * the decode loop's read path is untouched by any of this.
 *
 * They do share the SDMMC host driver, which logs
 * "SDMMC host already initialized, skipping init flow" when the card got
 * there first. Harmless, and it is why this runs after storage_init().
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The expander the power switch is on, remembered for later.
 *
 * `exp2` is the device at 0x44, the same handle usbhost_init() takes.
 * Called once at boot, before any of the rest of this is used. Passing it
 * to every call instead would mean the panel had to hold an I2C handle in
 * order to ask for the radio to go off, which is a bus detail leaking
 * into a settings screen.
 */
void wifi_init(i2c_master_dev_handle_t exp2);

/*
 * Bring the radio up in STA mode, or take it down.
 *
 * wifi_start() powers the C6, sets the SDIO pins, initialises
 * ESP-Hosted, connects to the slave, and starts esp_wifi as a station.
 * Roughly two seconds. Idempotent: ESP_OK and no work if already up.
 *
 * wifi_stop() reverses it in order -- esp_wifi_stop, esp_wifi_deinit,
 * the netif, esp_hosted_deinit, then the power bit -- and is safe to
 * call when already down.
 *
 * ORDER IS NOT A PREFERENCE HERE. esp_hosted owns tasks that outlive
 * esp_wifi_deinit(), so cutting the rail before esp_hosted_deinit()
 * leaves a write task talking to an unpowered chip. The component's
 * response to a transport it cannot recover is abort() -- see
 * eh_host_port_restart_host() -- so the device reboots, and it reboots
 * on the next wifi_start() rather than at the stop that caused it.
 *
 * Neither may be called from ui_task. See above.
 *
 * wifi_up() is the question the panel actually asks, and is a plain
 * value read rather than a call into the driver, so it is safe anywhere.
 */
esp_err_t wifi_start(void);
esp_err_t wifi_stop(void);
bool wifi_up(void);

/*
 * Apply settings_wifi_enabled(): start the radio if it should be up,
 * stop it if it should not, do nothing if it already matches.
 *
 * This is the one the settings push calls, and the one a switch press
 * should end up at. Keeping the comparison here rather than at each call
 * site means there is a single place where "what the setting says" and
 * "what the hardware is doing" are reconciled, and no caller can forget
 * half of it.
 *
 * With the setting false this stops the radio if it was running and
 * otherwise does nothing at all -- no power, no ESP-Hosted, no driver --
 * so "does this still behave like v0.3.0" stays a one-bit question
 * rather than an audit.
 *
 * CALL IT FROM THE SETTINGS PUSH, NOT FROM app_main().
 *
 * settings_init() starts the writer task and loads nothing: which volume
 * the settings live on is not known until a volume turns up, so the file
 * is read by the first settings_note_path() that adopts one. Its own
 * header says the caller then has to push the values wherever they are
 * acted on, because nothing reads them by itself -- and the radio is one
 * of those values, beside the volume.
 *
 * Called from app_main() instead, this read the built-in default and
 * nothing else, forever: on the board that was `wifi=true` in the file at
 * 1960 ms and the file being read at 2232.
 *
 * Idempotent, because that push runs at the start of every track and
 * because a switch press calls it too.
 *
 * Not ESP_ERROR_CHECK material at the call site: a player that will not
 * boot because its radio did not is worse than one that plays the card
 * in silence. Every failure path here logs and returns.
 */
esp_err_t wifi_apply_settings(void);

/*
 * Ask for wifi_apply_settings() to be run soon, and return at once.
 *
 * THIS is the one a switch press calls. It is the only entry point here
 * safe from ui_task: it posts to a worker and does not wait, so the
 * framebuffer's owner never blocks on a radio.
 *
 * Needed because the alternatives are both wrong. Calling
 * wifi_apply_settings() from panel_touch() would freeze the screen for
 * about two seconds over live audio while the C6 comes up. Leaving it to
 * the settings push means the switch does nothing until the next track
 * begins -- which on a five-minute piece is five minutes, and on a
 * paused player is never.
 *
 * Coalescing, not queueing. Several presses while the worker is busy
 * collapse into one more run afterwards, because what is being applied
 * is a current value rather than a sequence of events: turning the
 * switch off and on again quickly should leave the radio on, not make
 * it stop and start twice.
 */
void wifi_request_apply(void);

/*
 * Scan and log what is in the air. Requires the radio to be up.
 *
 * What the spike did, kept because it is the only way to see that the
 * radio works without a portal, and because the authmode column is how
 * the WPA2-versus-WPA3 split that wifistore is built around stops being
 * hypothetical. It will become the scan the portal's page is built from.
 *
 * Blocking, several seconds. Not from ui_task.
 */
esp_err_t wifi_scan_log(void);

/*
 * ---- Joining ---------------------------------------------------------
 *
 * The radio joins a network in two ways. When it comes up, and every
 * minute after while it has no address, the worker scans and tries every
 * saved network it sees, strongest first, until one joins
 * (wifistore_rank()). And the portal
 * joins a network somebody has just typed in, to find out whether the
 * credential works before anything is stored.
 *
 * The driver keeps its station config in RAM only
 * (esp_wifi_set_storage), so wifistore is the one place a credential is
 * ever written down.
 *
 * Neither retries in a loop. A failed join disconnects and reports, and
 * retrying belongs to the worker's minute -- slow enough that a player
 * carried out of range is not spending its radio hopping channels.
 *
 * The join always happens on the worker or the portal's task. The track
 * loop's settings push only starts and stops the radio and wakes the
 * worker; a fifteen-second join on the decode task would be a stall.
 */
#define WIFI_JOIN_TIMEOUT_MS    (15000)

/*
 * Join `ssid` with `secret` -- a passphrase, or 64 hex digits of PSK,
 * which the driver tells apart by length. Blocks until an address, a
 * refusal, or the timeout. Not from ui_task.
 *
 *   ESP_OK                 joined, address assigned; NTP started if on
 *   ESP_ERR_NOT_FOUND      the AP was not seen (reason NO_AP_FOUND)
 *   ESP_ERR_WIFI_PASSWORD  any other refusal -- a wrong secret, or a PSK
 *                          offered to an AP that only speaks SAE
 *   ESP_ERR_TIMEOUT        no answer either way
 *
 * Reason 2 (AUTH_EXPIRE) is retried once inside the call, as a whole new
 * attempt from set_config, because on hardware a first attempt expires
 * with a correct secret and a bare second connect never reaches the AP.
 * Worst case is two timeouts, about thirty seconds.
 *   ESP_ERR_INVALID_STATE  the radio is not up
 *
 * Never logs the secret.
 */
esp_err_t wifi_join(const char *ssid, const char *secret, uint32_t timeout_ms);

/* Whether the station has an address. A value, safe anywhere. */
bool wifi_connected(void);

/* The joined network's SSID, copied out. False and "" when not joined. */
bool wifi_sta_ssid(char *out, size_t out_size);

/*
 * The portal's access point: an open AP alongside the station, on the
 * radio that is already up. wifi_ap_begin() switches to APSTA and
 * configures the AP; wifi_ap_end() switches back to STA. Neither powers
 * anything or touches ESP-Hosted, and wifi_stop() ends the AP too.
 *
 * Not from ui_task. wifi_ap_netif() is NULL while the AP is down.
 */
esp_err_t wifi_ap_begin(const char *ssid);
esp_err_t wifi_ap_end(void);
int wifi_ap_clients(void);
struct esp_netif_obj *wifi_ap_netif(void);

/* One scanned network, copied out. `auth` is a wifi_auth_mode_t. */
typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t auth;
} wifi_seen_t;

/*
 * Scan, and copy up to `max` named networks into `out`, strongest first
 * as the driver sorts them. Hidden networks are skipped. Returns how
 * many, or -1 if the radio is down or the scan failed. Blocking, a few
 * seconds; logs each network as the old scan did.
 */
int wifi_scan_list(wifi_seen_t *out, int max);

/* "WPA2", "WPA2/WPA3", ... for a wifi_seen_t's auth. */
const char *wifi_auth_name(uint8_t auth);

#ifdef __cplusplus
}
#endif
