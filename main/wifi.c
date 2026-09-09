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
#include "freertos/task.h"

#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"

#include "settings.h"
#include <time.h>
#include "wifi.h"

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
 * Start SNTP, once. Idempotent for the same reason wifi_probe() is:
 * whatever calls this runs at a track boundary, not once per boot.
 *
 * NOT CALLED YET. wifi_probe() ends at scan_and_log() -- there is no
 * station join in this file, so there is no IP and nothing for an SNTP
 * request to reach. This exists so the portal, when it lands, has
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
__attribute__((unused))
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

esp_err_t wifi_probe(i2c_master_dev_handle_t exp2)
{
    /* Idempotent, because the caller is the settings push and that runs
     * at the start of every track. Bringing the transport up a second
     * time is not a no-op -- esp_wifi_init() on an initialised driver
     * returns an error and esp_hosted_init() would reset the C6 out from
     * under a live association. */
    if (s_up) return ESP_OK;

    if (!settings_wifi_enabled()) {
        /* Once, not once per track. */
        static bool said;
        if (!said) {
            ESP_LOGI(TAG, "Wi-Fi is off; C6 left unpowered");
            said = true;
        }
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(wlan_power(exp2, true), TAG, "power");
    vTaskDelay(pdMS_TO_TICKS(POWER_SETTLE_MS));

    /*
     * The C6's reset is not ours to drive. RF_C6_RST is the module's EN
     * pin from GPIO15, and esp_hosted toggles it itself as part of
     * bringing the transport up -- which is why the board preset in
     * idf_component.yml matters: with the wrong preset it resets GPIO54,
     * a pin that goes nowhere on this board, and then finds nothing.
     *
     * Driving it here as well would mean two owners of one pin, and the
     * loser would be whichever ran second.
     */
    /*
     * Called here, by hand, because sdkconfig.defaults sets
     * CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n. A pre-main
     * init cannot be after a power-up that happens once a volume is
     * mounted, and it would also make the switch above unenforceable.
     *
     * Succeeding here means the SDIO peripheral was configured. It does
     * NOT mean anything answered: "bus backend up" is logged either way,
     * and the first call that needs a reply is esp_wifi_init() below.
     */
    /*
     * The pins, before the transport is brought up.
     *
     * esp_hosted_init() takes whatever configuration is in place, and
     * the default is the P4-Function-EV-Board's -- CLK 18, CMD 19,
     * D0-D3 14-17, reset 54. None of those are wired to the C6 here, and
     * the failure is quiet in the worst way: the SDIO peripheral
     * configures fine, "bus backend up" is logged, and nothing goes
     * wrong until esp_wifi_init() waits five seconds for an answer.
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
        wlan_power(exp2, false);
        return err;
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

    esp_err_t hosted = esp_hosted_init();
    if (hosted != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init: %s", esp_err_to_name(hosted));
        ESP_LOGE(TAG, "check the SDIO pins esp_hosted logged: this board is "
                      "CLK 12, CMD 13, D0 11, D1 10, D2 9, D3 8, reset 15. "
                      "Anything else is the wrong board preset.");
        wlan_power(exp2, false);
        return hosted;
    }

    /*
     * Bringing the bus up and connecting to the slave are two calls, and
     * the second is the one that resets the C6.
     *
     * esp_hosted_init() initialises the SDIO peripheral -- that is the
     * "bus backend up" line. esp_hosted_connect_to_slave() is what
     * reaches ensure_slave_bus_ready(), which toggles the reset GPIO and
     * then runs the card init. The auto-init path calls both, which is
     * why the very first build on this board logged
     *
     *     W eh_sdio: Reset co-processor using GPIO[54]
     *
     * and no build since did: deferring the init replaced two calls with
     * one. The C6 was never reset and the card init was never attempted,
     * so every failure after that was a module that had not been started
     * -- and the reset GPIO and polarity fixes had nothing to apply to.
     *
     * Returns a negative errno rather than an esp_err_t: this is the
     * compat wrapper over eh_host_connect_to_slave(), and -ETIMEDOUT
     * here means the card init got no answer.
     */
    const int conn = esp_hosted_connect_to_slave();
    if (conn != 0) {
        ESP_LOGE(TAG, "esp_hosted_connect_to_slave: %d", conn);
        ESP_LOGE(TAG, "the reset and card init happen here, so eh_sdio's own "
                      "lines above this are the ones worth reading");
        wlan_power(exp2, false);
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGE(TAG, "no STA netif");
        return ESP_FAIL;
    }

    /*
     * esp_wifi_init() is the first call that talks to the C6 rather than
     * to the driver on this side. esp_hosted_init() installing the bus
     * says only that the SDIO peripheral was configured; whether
     * anything answers on those pins is not known until here.
     *
     * So this is where a wrong pin map surfaces, several seconds after
     * the transport reported itself up, as an RPC that never gets a
     * reply. It is worth spelling out at the failure rather than in a
     * header, because the log line above it says "bus backend up" and
     * that reads like success.
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
                      "GPIO (15), and the reset polarity (active HIGH on this "
                      "board). All three are set in sdkconfig.defaults and "
                      "wifi.c. Only after all three does the slave firmware "
                      "become the suspect.");
        wlan_power(exp2, false);
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        wlan_power(exp2, false);
        return err;
    }

    uint8_t mac[6] = { 0 };
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        ESP_LOGI(TAG, "radio up, station MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    s_up = true;

    err = scan_and_log();
    if (err != ESP_OK) ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
    return err;
}
