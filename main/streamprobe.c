/*
 * streamprobe.c -- see streamprobe.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "streamprobe.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "netstream.h"
#include "streamsniff.h"

static const char *TAG = "tab5_probe";

#define READ_CHUNK      (2048)

static bool s_started;

static void heap_line(const char *when)
{
    ESP_LOGI(TAG, "heap %s: internal free %u (min %u, largest %u), psram free %u",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/*
 * What the probe is for now. It no longer opens a connection: netstream
 * does that, and the point of this file is to say whether netstream does
 * it correctly. So it drains the ring exactly as phase 2's decoder will
 * -- netstream_read() with a timeout, in a loop -- and reports the three
 * things that would tell us phase 1 is wrong:
 *
 *   1. Whether the stream comes up at all, and how the state moves.
 *      Every transition is logged by netstream itself; this logs the
 *      times, because CONNECTING -> BUFFERING -> PLAYING with the right
 *      shape and the wrong timings is a different bug from never
 *      reaching PLAYING.
 *
 *   2. Whether the bytes that come out of the ring are the bytes that
 *      went in. adts_count_bytes() over the drained side gives audio
 *      milliseconds per wall millisecond, the same x-real-time figure
 *      the old probe logged off the socket -- but now measured after the
 *      ICY demultiplexer and after the ring, which is where a
 *      desynchronised demuxer or a dropping ring would show up as lost
 *      bytes and a falling ratio.
 *
 *   3. Whether the ring's occupancy is sane. A reader this fast should
 *      keep it near empty; a ring sitting near full means netstream is
 *      dropping, and a ring that empties to nothing repeatedly at 0.97x
 *      is what phase 3's watermarks will have to ride.
 */
static void probe_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "starting in %d s -- start a track from the card now to "
                  "measure the download beside playback", STREAMPROBE_DELAY_S);
    vTaskDelay(pdMS_TO_TICKS(STREAMPROBE_DELAY_S * 1000));

    const time_t now = time(NULL);
    ESP_LOGI(TAG, "probing %s (clock %lld%s)", STREAMPROBE_URL, (long long)now,
             now < 1700000000 ? ", not set" : "");
#ifdef CONFIG_MBEDTLS_HAVE_TIME_DATE
    ESP_LOGI(TAG, "mbedTLS checks certificate dates (CONFIG_MBEDTLS_HAVE_TIME_DATE)");
#else
    ESP_LOGI(TAG, "mbedTLS does not check certificate dates");
#endif
    heap_line("before");

    /* Idempotent, and it moves to app_main() at phase 3, when something
     * other than the probe wants a stream. */
    if (!netstream_init()) {
        ESP_LOGE(TAG, "netstream_init failed; nothing to probe");
        vTaskDelete(NULL);
        return;
    }

    const int64_t start = esp_timer_get_time();
    if (!netstream_play(STREAMPROBE_URL, "probe")) {
        ESP_LOGE(TAG, "netstream_play refused the URL");
        vTaskDelete(NULL);
        return;
    }

    static uint8_t buf[READ_CHUNK];
    static adts_count_t adts;
    memset(&adts, 0, sizeof(adts));

    static uint8_t sniff[2048];
    size_t sniffed = 0;
    bool   sniff_logged = false;

    int64_t  last = start, total = 0, window = 0;
    uint64_t last_audio_ms = 0;
    int      empties = 0, window_empties = 0;
    unsigned ring_max_pct = 0;
    char     title[ICY_TITLE_MAX], name[NETSTREAM_NAME_MAX];
    char     last_title[ICY_TITLE_MAX] = "";
    netstream_state_t last_state = NETSTREAM_IDLE;
    int64_t  first_byte_us = 0;

    while (esp_timer_get_time() - start < (int64_t)STREAMPROBE_SECONDS * 1000000) {
        const netstream_state_t st = netstream_state();
        if (st != last_state) {
            ESP_LOGI(TAG, "%lld ms: state %s (status %d, failures %d)",
                     (long long)((esp_timer_get_time() - start) / 1000),
                     netstream_state_name(st), netstream_last_status(),
                     netstream_failures());
            last_state = st;
        }
        if (st == NETSTREAM_FAILED) {
            ESP_LOGE(TAG, "netstream gave up after %d failures, last status %d",
                     netstream_failures(), netstream_last_status());
            break;
        }

        const unsigned pct = (unsigned)netstream_ring_pct();
        if (pct > ring_max_pct) ring_max_pct = pct;

        /* 200 ms: long enough that an empty read means the ring really
         * was empty, short enough that a stall is noticed in the window
         * it happened in. */
        const size_t n = netstream_read(buf, sizeof(buf), 200);
        if (n == 0) {
            empties++;
            window_empties++;
            continue;
        }
        if (!first_byte_us) {
            first_byte_us = esp_timer_get_time();
            ESP_LOGI(TAG, "first audio byte out of the ring at %lld ms",
                     (long long)((first_byte_us - start) / 1000));
        }
        total += (int64_t)n;
        window += (int64_t)n;
        adts_count_bytes(&adts, buf, n);

        if (!sniff_logged && sniffed < sizeof(sniff)) {
            const size_t take = (sizeof(sniff) - sniffed) < n
                              ? (sizeof(sniff) - sniffed) : n;
            memcpy(sniff + sniffed, buf, take);
            sniffed += take;
            if (sniffed == sizeof(sniff)) {
                ESP_LOGI(TAG, "first bytes out of the ring look like: %s",
                         sniff_name(sniff_bytes(sniff, sniffed)));
                sniff_logged = true;
            }
        }

        netstream_title(title, sizeof(title));
        if (strcmp(title, last_title) != 0) {
            ESP_LOGI(TAG, "stream title: \"%s\"%s", title,
                     netstream_has_title() ? "" : " (station sent none)");
            snprintf(last_title, sizeof(last_title), "%s", title);
        }

        const int64_t t = esp_timer_get_time();
        if (t - last >= 5000000) {
            const uint64_t ams = adts_ms(&adts);
            const int64_t wall = (t - last) / 1000;
            const uint64_t got = ams - last_audio_ms;
            /* The same x-real-time figure as before, but measured after
             * the demuxer and the ring rather than off the socket. A
             * ratio that was 0.97x on the wire and is lower here is
             * netstream losing bytes, which is the failure this rewrite
             * exists to be able to see. */
            ESP_LOGI(TAG, "%lld s: %lld KB/s out, %llu ms of audio in %lld ms (%llu.%02llux), ring %u%%, %d empty reads",
                     (long long)((t - start) / 1000000),
                     (long long)(window * 1000 / wall / 1024),
                     (unsigned long long)got, (long long)wall,
                     (unsigned long long)(got / (uint64_t)wall),
                     (unsigned long long)((got * 100 / (uint64_t)wall) % 100),
                     (unsigned)netstream_ring_pct(), window_empties);
            ESP_LOGI(TAG, "   netstream says %d kbit/s in; internal free %u, min %u, largest %u",
                     netstream_kbps(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            last_audio_ms = ams;
            window = 0;
            window_empties = 0;
            last = t;
        }
    }

    if (!sniff_logged && sniffed) {
        ESP_LOGI(TAG, "first bytes out of the ring look like: %s",
                 sniff_name(sniff_bytes(sniff, sniffed)));
    }

    netstream_name(name, sizeof(name));
    const int64_t secs_ms = (esp_timer_get_time() - start) / 1000;
    if (adts.frames) {
        ESP_LOGI(TAG, "ADTS: %u frames, AAC profile %u, %u Hz core, %u ch, frame %u-%u bytes, %llu bytes lost hunting",
                 (unsigned)adts.frames, adts.profile, adts.rate, adts.channels,
                 adts.min_len, adts.max_len, (unsigned long long)adts.lost);
        /* Bytes lost hunting for a sync is the demultiplexer's report
         * card. On the wire the old probe saw 0; anything above 0 here,
         * with the same station, is icydemux or the ring and not the
         * network. */
        ESP_LOGI(TAG, "ADTS: %llu ms of audio in %lld ms, bitrate %llu kbit/s",
                 (unsigned long long)adts_ms(&adts), (long long)secs_ms,
                 adts_ms(&adts) ? (unsigned long long)(total * 8 / (int64_t)adts_ms(&adts)) : 0ULL);
    }
    ESP_LOGI(TAG, "done: %lld KB drained in %lld ms from \"%s\", first byte %lld ms, ring peak %u%%, %d empty reads",
             (long long)(total / 1024), (long long)secs_ms, name,
             (long long)(first_byte_us ? (first_byte_us - start) / 1000 : -1),
             ring_max_pct, empties);

    /* The stop path matters as much as the start: nothing may leave a
     * TLS session open, and how long this takes is the number phase 3
     * needs for its pause. */
    const int64_t t_stop = esp_timer_get_time();
    const bool stopped = netstream_stop_wait(5000);
    ESP_LOGI(TAG, "stop %s after %lld ms, state %s",
             stopped ? "completed" : "TIMED OUT",
             (long long)((esp_timer_get_time() - t_stop) / 1000),
             netstream_state_name(netstream_state()));
    heap_line("after");
    vTaskDelete(NULL);
}

void streamprobe_kick(void)
{
    if (s_started || !STREAMPROBE_URL[0]) return;
    s_started = true;
    if (xTaskCreate(probe_task, "probe", 6144, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no task for the probe");
    }
}
