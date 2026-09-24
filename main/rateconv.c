/*
 * rateconv.c -- see rateconv.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_ae_rate_cvt.h"

#include "rateconv.h"

static const char *TAG = "tab5_rateconv";

/*
 * Complexity 2 of 3, and the MEMORY variant.
 *
 * The complexity is the filter length. Everything this converts is on
 * its way to either the ES8388 or a USB device that will re-encode it
 * for Bluetooth, and the difference between 2 and 3 is below either.
 * The MEMORY variant keeps the library's internal-RAM tables small at
 * some CPU cost; internal RAM is the scarce thing on this board (about
 * 200 KB free at boot) and the decode task has the CPU -- it runs
 * several times faster than real time and is paced down to that.
 */
#define RATECONV_COMPLEXITY     (2)

static esp_ae_rate_cvt_handle_t s_h;
static uint32_t s_in;
static uint32_t s_out;

/* Output buffer, in PSRAM, grown as needed and kept for the life of the
 * program: a block is at most a few hundred KB after conversion and
 * freeing it at every track would fragment PSRAM for nothing. */
static int16_t *s_buf;
static size_t   s_buf_frames;

bool rateconv_active(void)   { return s_h != NULL; }
uint32_t rateconv_in_rate(void)  { return s_h ? s_in : 0; }
uint32_t rateconv_out_rate(void) { return s_h ? s_out : 0; }

void rateconv_end(void)
{
    if (!s_h) return;
    esp_ae_rate_cvt_close(s_h);
    s_h = NULL;
    ESP_LOGI(TAG, "off (%" PRIu32 " -> %" PRIu32 " Hz)", s_in, s_out);
    s_in = s_out = 0;
}

void rateconv_reset(void)
{
    if (s_h) esp_ae_rate_cvt_reset(s_h);
}

bool rateconv_begin(uint32_t in_rate, uint32_t out_rate, bool keep)
{
    if (s_h && s_in == in_rate && s_out == out_rate) {
        if (!keep) esp_ae_rate_cvt_reset(s_h);
        ESP_LOGI(TAG, "%s %" PRIu32 " -> %" PRIu32 " Hz",
                 keep ? "continuing" : "restarting", in_rate, out_rate);
        return true;
    }
    rateconv_end();
    if (in_rate == 0 || out_rate == 0 || in_rate == out_rate) return false;

    esp_ae_rate_cvt_cfg_t cfg = {
        .src_rate = in_rate,
        .dest_rate = out_rate,
        .channel = 2,
        .bits_per_sample = 16,
        .complexity = RATECONV_COMPLEXITY,
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_MEMORY,
    };
    const esp_ae_err_t err = esp_ae_rate_cvt_open(&cfg, &s_h);
    if (err != ESP_AE_ERR_OK || !s_h) {
        s_h = NULL;
        ESP_LOGW(TAG, "cannot convert %" PRIu32 " -> %" PRIu32 " Hz (%d)",
                 in_rate, out_rate, (int)err);
        return false;
    }
    s_in = in_rate;
    s_out = out_rate;
    ESP_LOGI(TAG, "on: %" PRIu32 " -> %" PRIu32 " Hz", in_rate, out_rate);
    return true;
}

const int16_t *rateconv_run(const int16_t *in, size_t frames, size_t *out_frames)
{
    *out_frames = 0;
    if (!s_h) return NULL;
    if (!frames) return s_buf;

    uint32_t need = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(s_h, (uint32_t)frames, &need)
            != ESP_AE_ERR_OK || need == 0) {
        return NULL;
    }
    if (need > s_buf_frames) {
        int16_t *nb = heap_caps_realloc(s_buf, (size_t)need * 2 * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) {
            ESP_LOGE(TAG, "no room for %" PRIu32 " converted frames", need);
            return NULL;
        }
        s_buf = nb;
        s_buf_frames = need;
    }

    uint32_t got = (uint32_t)s_buf_frames;
    /* The library takes its buffers as non-const void pointers; it does
     * not write the input. */
    if (esp_ae_rate_cvt_process(s_h, (esp_ae_sample_t)in, (uint32_t)frames,
                                (esp_ae_sample_t)s_buf, &got) != ESP_AE_ERR_OK) {
        return NULL;
    }
    *out_frames = got;
    return s_buf;
}
