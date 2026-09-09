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
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_hosted.h"

#include "settings.h"
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

/* A scan long enough to hear a quiet AP and short enough not to look
 * hung. Active scan, all channels, IDF's own per-channel defaults. */
#define SCAN_MAX_AP             (32)

static bool s_powered;
static bool s_up;

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
    esp_err_t hosted = esp_hosted_init();
    if (hosted != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init: %s", esp_err_to_name(hosted));
        ESP_LOGE(TAG, "check the SDIO pins esp_hosted logged: this board is "
                      "CLK 12, CMD 13, D0 11, D1 10, D2 9, D3 8, reset 15. "
                      "Anything else is the wrong board preset.");
        wlan_power(exp2, false);
        return hosted;
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
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "this is the first call that needs an answer from the "
                      "C6. Check the SDIO pins esp_hosted logged above: this "
                      "board is CLK 12, CMD 13, D0 11, D1 10, D2 9, D3 8, "
                      "reset 15.");
        ESP_LOGE(TAG, "if they read CLK 18 CMD 19 D0-D3 14-17 RESET 54 those "
                      "are the P4-Function-EV-Board defaults and the board "
                      "preset is not selected: menuconfig -> Component config "
                      "-> ESP-Hosted -> Configure host -> Host transport");
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
