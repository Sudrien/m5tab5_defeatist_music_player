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

#include "polyrsp.h"

#include "rateconv.h"

static const char *TAG = "tab5_rateconv";

/*
 * 5040: polyrsp, fed in slices of RATECONV_SLICE frames -- it takes a
 * bounded block, and a decoded block can be larger. Its history buffer
 * is RATECONV_SLICE frames plus the filter, in PSRAM with the table.
 */
#define RATECONV_SLICE          (4096)

static polyrsp_t *s_h;
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
    polyrsp_close(s_h);
    s_h = NULL;
    ESP_LOGI(TAG, "off (%" PRIu32 " -> %" PRIu32 " Hz)", s_in, s_out);
    s_in = s_out = 0;
}

void rateconv_reset(void)
{
    if (s_h) polyrsp_reset(s_h);
}

bool rateconv_begin(uint32_t in_rate, uint32_t out_rate, bool keep)
{
    if (s_h && s_in == in_rate && s_out == out_rate) {
        if (!keep) polyrsp_reset(s_h);
        ESP_LOGI(TAG, "%s %" PRIu32 " -> %" PRIu32 " Hz",
                 keep ? "continuing" : "restarting", in_rate, out_rate);
        return true;
    }
    rateconv_end();
    if (in_rate == 0 || out_rate == 0 || in_rate == out_rate) return false;

    s_h = polyrsp_open(in_rate, out_rate, RATECONV_SLICE);
    if (!s_h) {
        ESP_LOGW(TAG, "cannot convert %" PRIu32 " -> %" PRIu32 " Hz",
                 in_rate, out_rate);
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

    /* Every slice can return one frame more than its exact share. */
    const size_t need = (size_t)(((uint64_t)frames * s_out + s_in - 1) / s_in) +
                        frames / RATECONV_SLICE + 2;
    if (need > s_buf_frames) {
        int16_t *nb = heap_caps_realloc(s_buf, need * 2 * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) {
            ESP_LOGE(TAG, "no room for %u converted frames", (unsigned)need);
            return NULL;
        }
        s_buf = nb;
        s_buf_frames = need;
    }

    size_t got = 0;
    while (frames) {
        const uint32_t n = frames > RATECONV_SLICE ? RATECONV_SLICE : (uint32_t)frames;
        got += polyrsp_process(s_h, in, n, s_buf + got * 2);
        in += (size_t)n * 2;
        frames -= n;
    }
    *out_frames = got;
    return s_buf;
}
