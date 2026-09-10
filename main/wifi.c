/*
 * wifi.c -- power the C6, bring ESP-Hosted up, scan, log.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
/* Declared by esp_netif, but it reads CONFIG_LWIP_SNTP_MAX_SERVERS and
 * lwip's ip_event_t, so both components are required. esp_sntp.h is the
 * older lower-level interface and declares none of this. */
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"

#include "settings.h"
#include <time.h>
#include "wifi.h"
#include "wifistore.h"
#include "portal.h"
#include "freertos/event_groups.h"

static const char *TAG = "tab5_wifi";

/* Same three registers usbhost.c writes for USB5V_EN, same reason: the
 * expander parks pins high-Z after reset, so OUT_SET alone leaves the
 * pin floating and the load switch never sees an enable. */
#define PI4IOE_REG_IO_DIR       (0x03)
#define PI4IOE_REG_OUT_SET      (0x05)
#define PI4IOE_REG_OUT_HIGH_Z   (0x07)

#define WLAN_PWR_EN_BIT         (1u << 0)   /* P0 of the expander at 0x44 */

#define I2C_TIMEOUT_MS          (100)

/*
 * How long the rail is given before the module is released, and the
 * module before SDIO is spoken to.
 *
 * Both are guesses and both are generous, which is the right way round
 * for a spike: the cost of waiting 100 ms on a boot that already takes
 * over a second is nothing, and the cost of being 5 ms early is a
 * timeout that reads like a wiring fault. Tighten them with a scope, not
 * with an opinion.
 */
#define POWER_SETTLE_MS         (100)

/*
 * The C6's SDIO2 pins, from the schematic, as P4 GPIO numbers.
 *
 * Set here rather than left to a board preset in menuconfig. The preset
 * exists -- esp_hosted >= 3.0.2 carries M5Stack Tab5 among its per-board
 * defaults -- but a board's wiring is not a build option: it cannot be
 * chosen wrongly by anyone holding this hardware, and a project that
 * asks a person to select it has invented a way to get it wrong. Three
 * builds on this board went out with the P4-Function-EV-Board pins
 * because the menu had not been visited.
 *
 * If a future preset disagrees with these numbers, the schematic wins
 * and this comment is the place to argue with it.
 */
#define SDIO_PIN_CLK            (12)
#define SDIO_PIN_CMD            (13)
#define SDIO_PIN_D0             (11)
#define SDIO_PIN_D1             (10)
#define SDIO_PIN_D2             (9)
#define SDIO_PIN_D3             (8)
#define SDIO_PIN_RESET          (15)    /* SOC_EXTRF_RST -> the C6's EN */

/* A scan long enough to hear a quiet AP and short enough not to look
 * hung. Active scan, all channels, IDF's own per-channel defaults. */
#define SCAN_MAX_AP             (32)

static bool s_powered;
static bool s_up;
static bool s_sntp_started;
static esp_netif_t *s_sta_netif;

/*
 * Joining, and the AP the portal borrows.
 *
 * s_connected is "has an address", set from the event loop and read
 * anywhere as a plain value, like s_up. The event group carries the one
 * answer wifi_join() waits for; s_join_reason is the disconnect reason
 * that came with a failure, kept because "refused" and "not there" lead
 * to different next steps in the portal.
 */
static esp_netif_t *s_ap_netif;
static volatile bool     s_connected;
static volatile bool     s_ap_on;
static volatile uint8_t  s_ap_clients;
static volatile uint16_t s_join_reason;
static char              s_sta_ssid[33];     /* under s_join_lock */
static EventGroupHandle_t s_join_bits;
static SemaphoreHandle_t  s_join_lock;
static esp_event_handler_instance_t s_wifi_evt, s_ip_evt;
static TickType_t         s_last_try;        /* worker only; 0 = never */

#define JOIN_GOT_IP     (1u << 0)
#define JOIN_FAILED     (1u << 1)

/*
 * NIST, and three of them.
 *
 * time.nist.gov is a round-robin across the NIST Internet Time Service;
 * the -a-g and -b-g names are individual Gaithersburg hosts, named so a
 * failure can be attributed to a machine rather than to the pool.
 *
 * Three because one reply is one voice. IDF's SNTP client does not
 * cross-check servers against each other -- settings_note_ntp_time()'s
 * floor is the actual check, applied to whichever reply lands -- but
 * more servers means fewer sessions where the only reachable source is
 * also the only one an attacker had to influence.
 *
 * The count must match CONFIG_LWIP_SNTP_MAX_SERVERS, which sizes the
 * array this list initialises. It defaults to 1, and overflowing it is a
 * warning rather than an error: the excess servers are silently dropped
 * and the build succeeds. sdkconfig.defaults sets it to 3 alongside
 * this; the constant exists so the two are visibly one number.
 */
#define NTP_SERVER_COUNT (3)

static esp_err_t wlan_power(i2c_master_dev_handle_t exp2, bool on)
{
    if (!exp2) return ESP_ERR_INVALID_STATE;

    const struct { uint8_t reg; bool set; } steps[] = {
        { PI4IOE_REG_IO_DIR,     true  },   /* output        */
        { PI4IOE_REG_OUT_HIGH_Z, false },   /* out of high-Z */
        { PI4IOE_REG_OUT_SET,    on    },   /* drive         */
    };

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        uint8_t reg = steps[i].reg, val;
        esp_err_t err = i2c_master_transmit_receive(exp2, &reg, 1, &val, 1,
                                                    I2C_TIMEOUT_MS);
        if (err != ESP_OK) return err;

        val = steps[i].set ? (uint8_t)(val | WLAN_PWR_EN_BIT)
                           : (uint8_t)(val & (uint8_t)~WLAN_PWR_EN_BIT);
        const uint8_t buf[2] = { reg, val };
        err = i2c_master_transmit(exp2, buf, sizeof(buf), I2C_TIMEOUT_MS);
        if (err != ESP_OK) return err;
    }

    ESP_LOGI(TAG, "WLAN_3.3V %s (expander 0x44, P0)", on ? "on" : "off");
    s_powered = on;
    return ESP_OK;
}

/*
 * The authentication mode, as a word.
 *
 * Printed because it is the one field of a scan result that decides how
 * a credential has to be stored: WPA2 takes a precomputed PSK and WPA3
 * does not. wifistore.h explains why that matters; this is where the
 * answer becomes visible for a real network rather than a hypothetical
 * one. On a transition-mode router IDF reports WPA2_WPA3_PSK, which is
 * the case the passphrase path exists for.
 */
static const char *authmode(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI";
    default:                        return "?";
    }
}

static esp_err_t scan_and_log(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(NULL, true), TAG, "scan");

    uint16_t n = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&n), TAG, "ap num");
    if (n > SCAN_MAX_AP) n = SCAN_MAX_AP;

    if (n == 0) {
        ESP_LOGW(TAG, "scan found nothing -- the radio answered, so this is "
                      "an antenna or a very quiet room, not the transport");
        return ESP_OK;
    }

    /* Heap rather than a stack array: 32 records is about 2 KB and this
     * runs on whatever task app_main() is, whose stack is not ours to
     * spend. Freed on every path. */
    wifi_ap_record_t *ap = calloc(n, sizeof(*ap));
    if (!ap) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_wifi_scan_get_ap_records(&n, ap);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%u network%s:", (unsigned)n, n == 1 ? "" : "s");
        for (uint16_t i = 0; i < n; i++) {
            /* The SSID is up to 32 bytes and is not required to be text.
             * %.32s bounds it; a name with a control byte in it prints
             * as rubbish, which is honest, rather than running off the
             * end of the record. */
            ESP_LOGI(TAG, "  %4d dBm  ch%-3d  %-10s  %.32s",
                     ap[i].rssi, ap[i].primary, authmode(ap[i].authmode),
                     (const char *)ap[i].ssid);
        }
    }

    free(ap);
    esp_wifi_scan_stop();
    return err;
}

/*
 * The sync callback. esp_netif_sntp fires this once per accepted reply,
 * on the SNTP task, after settimeofday() has already been called with
 * it -- so `tv` is what the system clock now holds, not a proposal.
 *
 * settings_note_ntp_time() is the plausibility gate, but by the time this
 * runs the clock has already moved: esp_netif_sntp does not offer a
 * pre-apply hook. So an implausible reply is rejected from the STORED
 * belief -- s_last_ntp_epoch does not advance to match it, and the next
 * boot's baseline stays the old, trusted value -- while the live
 * system clock for THIS session accepts whatever NTP said, because
 * nothing downstream (TLS validation, file timestamps) has a second
 * clock to fall back to mid-session. The stored value is what protects
 * the NEXT boot; this boot is protected by the day-wide slack being
 * larger than any plausible legitimate drift, which bounds how far a
 * single accepted reply can be wrong even though it was not blocked.
 */
static void on_sntp_sync(struct timeval *tv)
{
    if (!tv) return;
    const bool ok = settings_note_ntp_time((int64_t)tv->tv_sec,
                                           esp_timer_get_time());
    ESP_LOGI(TAG, "NTP sync: %lld%s", (long long)(int64_t)tv->tv_sec,
             ok ? "" : " (implausible vs. last known time; system clock "
                       "moved anyway, stored baseline did not)");
}

/*
 * Start SNTP, once. Idempotent for the same reason wifi_apply_settings()
 * is: whatever calls this may run at every track boundary.
 *
 * NOT CALLED YET. Nothing in this file joins a network, so there is no
 * IP and nothing for an SNTP request to reach. wifi_stop() does undo it,
 * so the pairing is already right for when it is called. This exists so the portal, when it lands, has
 * exactly one function to call after a successful join rather than a
 * second round of "where does NTP go" design. Until then it is dead
 * code with a host-testable half (settings_note_ntp_time()) and an
 * unreachable half (this).
 *
 * Marked unused rather than left to warn: -Wunused-function is right
 * about it and will keep being right until the portal calls it, and a
 * warning that is expected on every build is a warning nobody reads.
 * Remove the attribute when the call site appears.
 */
static void sntp_start(void)
{
    if (s_sntp_started || !settings_ntp_enabled()) return;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        NTP_SERVER_COUNT,
        ESP_SNTP_SERVER_LIST("time.nist.gov",
                             "time-a-g.nist.gov",
                             "time-b-g.nist.gov"));
    /* The config carries the callback, so there is no window between
     * starting the client and installing the hook in which a fast first
     * reply could land unobserved. */
    cfg.sync_cb = on_sntp_sync;

    const esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init: %s", esp_err_to_name(err));
        return;
    }
    s_sntp_started = true;

    ESP_LOGI(TAG, "SNTP started (%d servers)", NTP_SERVER_COUNT);
}

static i2c_master_dev_handle_t s_exp2;

/*
 * The worker, and the lock that keeps it away from the settings push.
 *
 * wifi_apply_settings() can now be reached from two places -- the track
 * loop and this worker -- so the start/stop it wraps needs a mutex.
 * Without one, a switch pressed as a track begins could run a stop
 * inside a start, and the second half of the start would then be
 * configuring a driver whose power had just been cut.
 *
 * The notification is a binary semaphore rather than a queue: presses
 * coalesce. See wifi_request_apply() in the header.
 */
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_wake;

/* Defined below; the worker is declared here because wifi_init() has to
 * create the task before the reader exists in the file. */
esp_err_t wifi_apply_settings(void);

/*
 * The event loop's half. Runs on the default event task, so it only
 * records: values and bits, nothing that waits.
 */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_join_reason = d ? d->reason : 0;
        if (s_connected) ESP_LOGW(TAG, "disconnected, reason %u", (unsigned)s_join_reason);
        s_connected = false;
        if (s_join_bits) xEventGroupSetBits(s_join_bits, JOIN_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        if (e) ESP_LOGI(TAG, "address " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        if (s_join_bits) xEventGroupSetBits(s_join_bits, JOIN_GOT_IP);
        /* The one call site NTP was waiting for. sntp_start() checks the
         * setting and its own started flag, so a reconnect is a no-op. */
        sntp_start();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        if (s_ap_clients < 255) s_ap_clients++;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
    }
}

esp_err_t wifi_join(const char *ssid, const char *secret, uint32_t timeout_ms)
{
    if (!s_up || !s_join_lock || !s_join_bits) return ESP_ERR_INVALID_STATE;
    if (!ssid || !secret) return ESP_ERR_INVALID_ARG;
    const size_t sl = strlen(ssid), pl = strlen(secret);
    if (sl == 0 || sl > 32 || pl > 64) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_join_lock, portMAX_DELAY);

    wifi_config_t cfg = { 0 };
    memcpy(cfg.sta.ssid, ssid, sl);
    memcpy(cfg.sta.password, secret, pl);     /* 64 bytes, no NUL needed */
    /* WPA2 as the floor, SAE allowed, PMF offered: the combination that
     * joins WPA2, WPA3 and transition-mode APs alike. */
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_wifi_disconnect();                     /* whatever it was doing */
    s_connected = false;
    s_sta_ssid[0] = '\0';

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);

    /*
     * ONE RETRY, AFTER A PAUSE, AND ONLY FOR REASON 2 (AUTH_EXPIRE).
     *
     * Measured on hardware, fivescore, a WPA2/WPA3 network on channel 2,
     * one secret throughout:
     *
     *   boot, first attempt        reason 2 after 6.6 s
     *   worker retry, +60 s        joined
     *   boot, first attempt        reason 2 after 6.6 s
     *   immediate retry (0011)     reason 205, rssi -128, after 2.4 s
     *   portal, first attempt      reason 2 after 3.6 s (AP up on ch 1)
     *   portal, second attempt     joined
     *
     * So reason 2 is a first attempt expiring with a correct secret, and
     * retrying at once is too soon: 205 with rssi -128 is a connect that
     * never reached the AP. A minute later is not. The pause between is
     * an ESTIMATE; nothing has measured where between 0 and 60 s the
     * retry starts working, and the log line below is how to find out.
     *
     * Also only an observation, not a cause: in all three logs, every
     * reason-2 failure was preceded by "rx RPC WifiEventNoArgs id=43" a
     * few seconds into the attempt, and neither success was. 43 is
     * probably WIFI_EVENT_HOME_CHANNEL_CHANGE on the slave's IDF, which
     * would fit a first connect that has to move the radio to channel 2
     * -- but the slave's event numbering has not been checked.
     *
     * No esp_wifi_disconnect() before the retry: the driver has already
     * reported the disconnect, and a second one just before connecting
     * is a thing 0011 did that the 205 may have come from.
     */
#define JOIN_RETRY_PAUSE_MS (5000)

    EventBits_t bits = 0;
    for (int attempt = 1; err == ESP_OK && attempt <= 2; attempt++) {
        if (attempt == 2) {
            ESP_LOGI(TAG, "join %.32s: reason 2 (auth expired), trying once "
                          "more in %d ms", ssid, JOIN_RETRY_PAUSE_MS);
            vTaskDelay(pdMS_TO_TICKS(JOIN_RETRY_PAUSE_MS));
        }
        /* A disconnect event can arrive late and would read as this
         * attempt failing. Cleared immediately before the connect. */
        xEventGroupClearBits(s_join_bits, JOIN_GOT_IP | JOIN_FAILED);
        s_join_reason = 0;
        err = esp_wifi_connect();
        if (err != ESP_OK) break;

        bits = xEventGroupWaitBits(s_join_bits, JOIN_GOT_IP | JOIN_FAILED,
                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        if (!(bits & JOIN_FAILED) || s_join_reason != WIFI_REASON_AUTH_EXPIRE) break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "join %.32s: %s", ssid, esp_err_to_name(err));
        xSemaphoreGive(s_join_lock);
        return err;
    }

    if (bits & JOIN_GOT_IP) {
        snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%.32s", ssid);
        err = ESP_OK;
        ESP_LOGI(TAG, "joined %.32s", ssid);
    } else {
        /* Stop the driver retrying on its own, so a failed attempt does
         * not keep hopping channels under an AP the portal is serving. */
        esp_wifi_disconnect();
        if (!(bits & JOIN_FAILED)) {
            err = ESP_ERR_TIMEOUT;
        } else if (s_join_reason == WIFI_REASON_NO_AP_FOUND) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            err = ESP_ERR_WIFI_PASSWORD;
        }
        /* The reason, never the secret -- see portal.h. */
        ESP_LOGW(TAG, "join %.32s failed: %s (reason %u)", ssid,
                 esp_err_to_name(err), (unsigned)s_join_reason);
    }
    xSemaphoreGive(s_join_lock);
    return err;
}

bool wifi_connected(void) { return s_connected; }

bool wifi_sta_ssid(char *out, size_t out_size)
{
    if (!out || !out_size) return false;
    out[0] = '\0';
    if (!s_connected || !s_join_lock) return false;
    xSemaphoreTake(s_join_lock, portMAX_DELAY);
    snprintf(out, out_size, "%s", s_sta_ssid);
    xSemaphoreGive(s_join_lock);
    return out[0] != '\0';
}

esp_err_t wifi_ap_begin(const char *ssid)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    if (s_ap_on) return ESP_OK;
    if (!ssid || !*ssid || strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "no AP netif -- is CONFIG_ESP_WIFI_SOFTAP_SUPPORT set?");
            return ESP_FAIL;
        }
    }

    /*
     * M5's 0.0.0 slave firmware accepts APSTA: "APSTA up" on the first
     * flash, with esp_hosted 3.0.7. The refusal branch stays, because a
     * different C6 firmware is a different answer. See portal.h.
     */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "APSTA refused: %s -- the slave firmware is the "
                      "first suspect, not this code", esp_err_to_name(err));
        return err;
    }

    wifi_config_t ap = { 0 };
    const size_t sl = strlen(ssid);
    memcpy(ap.ap.ssid, ssid, sl);
    ap.ap.ssid_len = (uint8_t)sl;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;          /* follows the STA's channel if it joins */
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP config: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return err;
    }

    s_ap_clients = 0;
    s_ap_on = true;
    ESP_LOGI(TAG, "APSTA up: %s is broadcasting", ssid);
    return ESP_OK;
}

esp_err_t wifi_ap_end(void)
{
    if (!s_ap_on) return ESP_OK;
    s_ap_on = false;
    s_ap_clients = 0;
    if (!s_up) return ESP_OK;
    const esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "back to STA%s", err == ESP_OK ? "" : " (mode change failed)");
    return err;
}

int wifi_ap_clients(void) { return s_ap_on ? s_ap_clients : 0; }

esp_netif_t *wifi_ap_netif(void) { return s_ap_on ? s_ap_netif : NULL; }

int wifi_scan_list(wifi_seen_t *out, int max)
{
    if (!s_up || !out || max <= 0) return -1;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return -1;

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > SCAN_MAX_AP) n = SCAN_MAX_AP;
    wifi_ap_record_t *ap = n ? calloc(n, sizeof(*ap)) : NULL;
    int got = 0;
    if (ap && esp_wifi_scan_get_ap_records(&n, ap) == ESP_OK) {
        for (uint16_t i = 0; i < n && got < max; i++) {
            if (!ap[i].ssid[0]) continue;       /* hidden: nothing to show */
            snprintf(out[got].ssid, sizeof(out[got].ssid), "%.32s",
                     (const char *)ap[i].ssid);
            out[got].rssi = ap[i].rssi;
            out[got].auth = (uint8_t)ap[i].authmode;
            ESP_LOGI(TAG, "  %4d dBm  ch%-3d  %-10s  %.32s", ap[i].rssi,
                     ap[i].primary, authmode(ap[i].authmode), out[got].ssid);
            got++;
        }
    }
    free(ap);
    esp_wifi_scan_stop();
    return got;
}

const char *wifi_auth_name(uint8_t auth) { return authmode((wifi_auth_mode_t)auth); }

/*
 * Join a saved network in range: the strongest first, and on refusal the
 * next, until one gives an address. Worker task only.
 *
 * Every saved network the scan saw is tried, not only the strongest. A
 * saved network whose password has changed would otherwise shadow every
 * other one in range for as long as it is the loudest.
 */
#define RETRY_MS    (60000)

static void connect_saved(bool force)
{
    if (!s_up || s_connected || portal_running() || wifistore_count() == 0) return;
    const TickType_t now = xTaskGetTickCount();
    if (!force && s_last_try && (now - s_last_try) < pdMS_TO_TICKS(RETRY_MS)) return;
    s_last_try = now ? now : 1;

    wifi_seen_t *seen = calloc(SCAN_MAX_AP, sizeof(*seen));
    wifistore_cred_t *cand = calloc(WIFISTORE_MAX, sizeof(*cand));
    if (!seen || !cand) {
        free(seen);
        free(cand);
        return;
    }

    const int n = wifi_scan_list(seen, SCAN_MAX_AP);
    if (n > 0) {
        const char *names[SCAN_MAX_AP];
        int8_t rssi[SCAN_MAX_AP];
        for (int i = 0; i < n; i++) { names[i] = seen[i].ssid; rssi[i] = seen[i].rssi; }
        const int k = wifistore_rank(names, rssi, n, cand, WIFISTORE_MAX);
        if (k == 0) {
            ESP_LOGI(TAG, "none of %d saved network%s in range",
                     wifistore_count(), wifistore_count() == 1 ? "" : "s");
        }
        for (int i = 0; i < k; i++) {
            /* The portal may have started while an earlier one timed out;
             * it owns the radio from then on. */
            if (!s_up || portal_running()) break;
            ESP_LOGI(TAG, "saved network %d of %d in range: %.32s", i + 1, k, cand[i].ssid);
            if (wifi_join(cand[i].ssid, cand[i].secret, WIFI_JOIN_TIMEOUT_MS) == ESP_OK) break;
        }
    }
    memset(cand, 0, WIFISTORE_MAX * sizeof(*cand));
    free(cand);
    free(seen);
}

static void wifi_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* A wake is a switch press or a radio that has just come up; a
         * timeout is the retry for a player carried back into range. */
        const bool woke = xSemaphoreTake(s_wake, pdMS_TO_TICKS(RETRY_MS)) == pdTRUE;
        if (woke) wifi_apply_settings();
        connect_saved(woke);
    }
}


void wifi_request_apply(void)
{
    /* Never blocks and never fails in a way worth reporting: if the
     * semaphore is already given, a run is pending and this press is
     * already covered by it. */
    if (s_wake) xSemaphoreGive(s_wake);
}

void wifi_init(i2c_master_dev_handle_t exp2)
{
    s_exp2 = exp2;

    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    s_join_lock = xSemaphoreCreateMutex();
    s_join_bits = xEventGroupCreate();
    if (!s_lock || !s_wake) {
        ESP_LOGE(TAG, "no lock or semaphore; the switch will only take "
                      "effect at a track boundary");
        return;
    }

    /*
     * Its own task because everything it does blocks: about two seconds
     * to bring the C6 up, and however long esp_wifi takes to change mode
     * on the way down. Priority below the decoder and the arbiter -- the
     * radio has no deadline and audio does. 4 KB is what the call chain
     * below needs; esp_hosted and esp_wifi run on their own tasks.
     */
    if (xTaskCreate(wifi_task, "wifi", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no wifi task; the switch will only take effect at a "
                      "track boundary");
    }
}

bool wifi_up(void) { return s_up; }

/*
 * Unwind, in the reverse of the order things were brought up.
 *
 * Order is the whole content of this function. Cutting the power first
 * leaves esp_wifi issuing RPCs to a chip that is no longer there, and
 * each of those waits out its own timeout -- seconds apiece, on whatever
 * task called this. Every step is attempted even if an earlier one
 * failed: a half-torn-down radio that still holds the power rail is
 * worse than one that reports an error on the way down, and the last
 * step is the one that actually makes the chip quiet.
 *
 * The netif and the default event loop are deliberately NOT destroyed.
 * esp_netif_init() and esp_event_loop_create_default() are process-wide
 * and are not meaningfully undoable -- esp_netif_deinit() is documented
 * as unsupported -- so they are created once and left. Only the STA
 * netif object, which is ours, is destroyed.
 */
esp_err_t wifi_stop(void)
{
    if (!s_up) return ESP_OK;

    esp_err_t first = ESP_OK;
    esp_err_t err;

    if (s_wifi_evt) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt);
        s_wifi_evt = NULL;
    }
    if (s_ip_evt) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt);
        s_ip_evt = NULL;
    }
    s_connected = false;
    s_ap_on = false;
    s_ap_clients = 0;

    if (s_sntp_started) {
        esp_netif_sntp_deinit();
        s_sntp_started = false;
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && first == ESP_OK) first = err;

    err = esp_wifi_deinit();
    if (err != ESP_OK && first == ESP_OK) first = err;

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    /*
     * ESP-HOSTED HAS TO COME DOWN TOO, AND BEFORE THE POWER.
     *
     * Leaving it up was a crash. esp_hosted_init() starts sdio_write_task
     * and the rx task, and they outlive esp_wifi_deinit() -- they belong
     * to the transport, not to the driver above it. Cutting the rail
     * under them leaves a write task issuing CMD53 to a chip that is no
     * longer powered, and the next wifi_start() lands in the middle of
     * that:
     *
     *     E sdmmc_io_rw_extended: sdmmc_send_cmd returned 0x107
     *     E eh_sdio: Unrecoverable host sdio state
     *     W eh_host_xport: TRANSPORT_FAILURE: forcing host reset
     *     abort() at eh_host_port_restart_host
     *
     * eh_host_port_restart_host() is abort(). The component's answer to
     * a transport it cannot recover is to reboot the device, so a
     * teardown that misses this step does not degrade, it panics -- and
     * it panics on the NEXT start rather than at the stop, which is why
     * a stop on its own looked clean.
     *
     * eh_host_deinit() stops the feature tasks, the RPC layer and the
     * serial transport, and posts TRANSPORT_DOWN. It explicitly does not
     * touch the default event loop, which is shared -- the same reason
     * this function leaves esp_netif_init() alone.
     *
     * Returns a negative errno, like connect_to_slave().
     */
    const int hosted = esp_hosted_deinit();
    if (hosted != 0) {
        ESP_LOGW(TAG, "esp_hosted_deinit: %d", hosted);
        if (first == ESP_OK) first = ESP_FAIL;
    }

    /* Only now, with nothing left holding the bus. */
    err = wlan_power(s_exp2, false);
    if (err != ESP_OK && first == ESP_OK) first = err;

    s_up = false;
    ESP_LOGI(TAG, "radio down");
    return first;
}

esp_err_t wifi_start(void)
{
    if (s_up) return ESP_OK;
    if (!s_exp2) {
        ESP_LOGE(TAG, "wifi_init() was never called");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(wlan_power(s_exp2, true), TAG, "power");
    vTaskDelay(pdMS_TO_TICKS(POWER_SETTLE_MS));

    /*
     * The pins, before the transport is brought up.
     *
     * esp_hosted_init() takes whatever configuration is in place, and
     * the default is the P4-Function-EV-Board's -- CLK 18, CMD 19,
     * D0-D3 14-17, reset 54. None of those are wired to the C6 here, and
     * the failure is quiet in the worst way: the SDIO peripheral
     * configures fine, "bus backend up" is logged, and nothing goes
     * wrong until esp_wifi_init() waits five seconds for an answer.
     *
     * The C6's reset is not set here even so. RF_C6_RST is the module's
     * EN pin on GPIO15, and esp_hosted drives it itself from
     * CONFIG_ESP_HOSTED_HOST_RESET_GPIO as part of connecting to the
     * slave -- pin_reset in this struct is not the field it reads. Two
     * owners of one pin would be decided by whichever ran second.
     */
    struct esp_hosted_sdio_config sdio = INIT_DEFAULT_HOST_SDIO_CONFIG();
    sdio.pin_clk.pin   = SDIO_PIN_CLK;
    sdio.pin_cmd.pin   = SDIO_PIN_CMD;
    sdio.pin_d0.pin    = SDIO_PIN_D0;
    sdio.pin_d1.pin    = SDIO_PIN_D1;
    sdio.pin_d2.pin    = SDIO_PIN_D2;
    sdio.pin_d3.pin    = SDIO_PIN_D3;
    sdio.pin_reset.pin = SDIO_PIN_RESET;

    esp_err_t err = esp_hosted_sdio_set_config(&sdio);
    if (err != ESP_OK) {
        /* Refused rather than ignored, which is worth failing on: going
         * ahead would bring the transport up on the wrong pins and spend
         * five seconds proving it. */
        ESP_LOGE(TAG, "esp_hosted_sdio_set_config: %s", esp_err_to_name(err));
        goto fail;
    }

    /*
     * Read back what the transport will actually use.
     *
     * eh_sdio's own banner prints the compile-time Kconfig macros rather
     * than the live configuration, unconditionally, whether or not an
     * override was accepted. Three builds were diagnosed off that line
     * as having the wrong pins when the pins were already right. This
     * prints the struct the driver copies, so the log says what is true.
     */
    struct esp_hosted_sdio_config *live = NULL;
    if (esp_hosted_sdio_get_config(&live) == ESP_OK && live) {
        ESP_LOGI(TAG, "SDIO in use: slot %u, %u-bit, CLK %d CMD %d "
                      "D0 %d D1 %d D2 %d D3 %d",
                 (unsigned)live->slot, (unsigned)live->bus_width,
                 live->pin_clk.pin, live->pin_cmd.pin, live->pin_d0.pin,
                 live->pin_d1.pin, live->pin_d2.pin, live->pin_d3.pin);
        ESP_LOGI(TAG, "the eh_sdio banner below prints build-time defaults, "
                      "not this -- believe this line");
    }

    /*
     * Called by hand because sdkconfig.defaults sets
     * CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n. A pre-main
     * init cannot follow a power-up that happens once a volume is
     * mounted, and it would make the Wi-Fi switch unenforceable.
     *
     * Succeeding here means the SDIO peripheral was configured. It does
     * NOT mean anything answered.
     */
    err = esp_hosted_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init: %s", esp_err_to_name(err));
        goto fail;
    }

    /*
     * Bringing the bus up and connecting to the slave are two calls, and
     * the second is the one that resets the C6.
     *
     * esp_hosted_init() initialises the SDIO peripheral -- that is the
     * "bus backend up" line. esp_hosted_connect_to_slave() reaches
     * ensure_slave_bus_ready(), which toggles the reset GPIO and runs
     * the card init. The auto-init path calls both; deferring it
     * replaced two calls with one, and for six builds the C6 was never
     * reset and the card init never attempted.
     *
     * Returns a negative errno rather than an esp_err_t: it is the
     * compat wrapper over eh_host_connect_to_slave(), so -ETIMEDOUT here
     * means the card init got no answer.
     */
    const int conn = esp_hosted_connect_to_slave();
    if (conn != 0) {
        ESP_LOGE(TAG, "esp_hosted_connect_to_slave: %d", conn);
        ESP_LOGE(TAG, "the reset and card init happen here, so eh_sdio's own "
                      "lines above this are the ones worth reading");
        err = ESP_FAIL;
        goto fail;
    }

    /* Process-wide and created once; see wifi_stop() on why these are
     * never undone. Both return ESP_ERR_INVALID_STATE when already done,
     * which is not an error here. */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;

    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            on_event, NULL, &s_wifi_evt) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_event, NULL, &s_ip_evt) != ESP_OK) {
        ESP_LOGW(TAG, "no event handlers; joining will time out");
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "no STA netif");
        err = ESP_FAIL;
        goto fail;
    }

    /*
     * esp_wifi_init() is the first call that talks to the C6 rather than
     * to the driver on this side, so a slave that is not running
     * surfaces here -- several seconds after every transport line has
     * reported success.
     */
    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "this is the first call that needs an answer from the "
                      "C6, so a silent slave surfaces here rather than at "
                      "any of the transport lines above -- all of which "
                      "report success against a module that is not running.");
        ESP_LOGE(TAG, "three things have to be right before the C6 runs: the "
                      "SDIO pins (logged above as 'SDIO in use'), the reset "
                      "GPIO (15), and the reset polarity (active LOW, so the "
                      "pin parks high and EN stays asserted). All three are "
                      "set in sdkconfig.defaults and wifi.c. Only after all "
                      "three does the slave firmware become the suspect.");
        goto fail;
    }

    /*
     * RAM, not flash: the driver's own copy of the station config is
     * never written down. Without this every esp_wifi_set_config() --
     * including a failed attempt with a mistyped password, and the raw
     * passphrase on the WPA3 fallback -- lands in the driver's NVS in
     * plain text, and with ESP-Hosted that is probably the C6's flash,
     * outside everything wifistore.h promises. wifistore is the one
     * place credentials are kept. The map project does the same on this
     * board via WiFi.persistent(false).
     */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_storage(RAM): %s -- joins may be "
                      "persisted by the driver", esp_err_to_name(err));
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        goto fail;
    }

    uint8_t mac[6] = { 0 };
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(TAG, "radio up, station MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    s_up = true;
    return ESP_OK;

fail:
    /* Everything above this point either did not power the module or has
     * just been undone; the rail is the last thing left holding it. */
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    wlan_power(s_exp2, false);
    return err;
}

esp_err_t wifi_scan_log(void)
{
    if (!s_up) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = scan_and_log();
    if (err != ESP_OK) ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
    return err;
}

esp_err_t wifi_apply_settings(void)
{
    /* Two callers now -- the track loop and the worker -- so the whole
     * comparison-and-act is one critical section. Reading the setting
     * inside it as well, so a press landing between the read and the act
     * is applied by whichever call gets the lock second rather than
     * lost. */
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    const bool want = settings_wifi_enabled();
    esp_err_t err = ESP_OK;

    /* The portal is running on this radio; it comes down first, so its
     * server and its AP are not pulled out from under it. Outside the
     * lock, because it waits for the portal's task. */
    if (!want && s_up && portal_running()) {
        if (s_lock) xSemaphoreGive(s_lock);
        portal_stop();
        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    const bool was_up = s_up;
    if (want != s_up) err = want ? wifi_start() : wifi_stop();

    if (s_lock) xSemaphoreGive(s_lock);

    /*
     * The join is the worker's, never this caller's: this runs from the
     * track loop's settings push too, and a fifteen-second join there is
     * a stalled decode. A radio that has just come up wakes the worker;
     * the worker's own call lands here with the radio already up and
     * posts nothing, so this cannot loop.
     */
    if (want && err == ESP_OK && s_up && !was_up) {
        s_last_try = 0;
        wifi_request_apply();
    }
    return err;
}
