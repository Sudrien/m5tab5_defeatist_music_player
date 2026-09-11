/*
 * streamprobe.c -- see streamprobe.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "streamprobe.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "streamsniff.h"

static const char *TAG = "tab5_probe";

#define HOPS_MAX        (5)
#define READ_CHUNK      (2048)
#define SNIFF_BYTES     (2048)
#define TITLES_MAX      (3)

static bool s_started;

/* Headers worth seeing, captured as they arrive on each hop. The Location
 * is copied whole: the first probe cut it at 160 characters, in the middle
 * of the token that says how long the redirect is good for. */
static int  s_metaint;
static char s_location[1024];

static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_HEADER || !e->header_key) return ESP_OK;
    const char *k = e->header_key, *v = e->header_value ? e->header_value : "";
    if (strcasecmp(k, "location") == 0) {
        snprintf(s_location, sizeof(s_location), "%s", v);
        return ESP_OK;          /* printed whole after the hop, in pieces */
    }
    if (strcasecmp(k, "content-type") == 0 ||
        strcasecmp(k, "server") == 0 || strcasecmp(k, "transfer-encoding") == 0 ||
        strncasecmp(k, "icy-", 4) == 0 || strncasecmp(k, "ice-", 4) == 0) {
        ESP_LOGI(TAG, "  %s: %.160s", k, v);
    }
    if (strcasecmp(k, "icy-metaint") == 0) s_metaint = atoi(v);
    return ESP_OK;
}

/* A long string in log-sized pieces. */
static void log_long(const char *label, const char *str)
{
    const size_t n = strlen(str);
    for (size_t i = 0; i < n; i += 120) {
        ESP_LOGI(TAG, "  %s[%u]: %.120s", label, (unsigned)(i / 120), str + i);
    }
}

/*
 * The redirect's token, if it has one: a query value that is a JWT. Its
 * payload is logged, and its exp against the clock, which answers how long
 * a redirected URL can be reused -- and so whether a reconnect can skip
 * the first hop.
 */
static void log_token(const char *url)
{
    const char *q = strchr(url, '?');
    while (q && *q) {
        const char *val = strchr(q, '=');
        if (!val) break;
        val++;
        const char *end = strchr(val, '&');
        const size_t n = end ? (size_t)(end - val) : strlen(val);
        static char payload[512];
        if (jwt_payload(val, n, payload, sizeof(payload))) {
            log_long("token", payload);
            int64_t exp = 0, iat = 0;
            const time_t now = time(NULL);
            if (json_int(payload, "exp", &exp)) {
                ESP_LOGI(TAG, "  token exp %lld: %lld s from now",
                         (long long)exp, (long long)(exp - (int64_t)now));
            } else {
                ESP_LOGI(TAG, "  token has no exp");
            }
            if (json_int(payload, "iat", &iat)) {
                ESP_LOGI(TAG, "  token iat %lld: %lld s ago", (long long)iat,
                         (long long)((int64_t)now - iat));
            }
            return;
        }
        q = end;
    }
}

static void heap_line(const char *when)
{
    ESP_LOGI(TAG, "heap %s: internal free %u (min %u, largest %u), psram free %u",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void probe_task(void *arg)
{
    (void)arg;
    /* Let the join settle -- the address arrives before routes and DNS
     * have been used once. Deliberately not waiting for NTP: whether TLS
     * needs the clock is one of the questions. */
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

    esp_http_client_config_t cfg = {
        .url = STREAMPROBE_URL,
        .event_handler = on_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_agent = "DefeatistMusicPlayer/0.4 (stream probe)",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        goto out;
    }
    esp_http_client_set_header(c, "Icy-MetaData", "1");

    int status = 0;
    int64_t length = -1;
    for (int hop = 1; hop <= HOPS_MAX; hop++) {
        char url[256];
        if (esp_http_client_get_url(c, url, sizeof(url)) != ESP_OK) url[0] = '\0';
        ESP_LOGI(TAG, "hop %d: %s", hop, url);
        s_metaint = 0;
        s_location[0] = '\0';

        const int64_t t0 = esp_timer_get_time();
        esp_err_t err = esp_http_client_open(c, 0);
        const int64_t t1 = esp_timer_get_time();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "hop %d: open failed after %lld ms: %s", hop,
                     (long long)((t1 - t0) / 1000), esp_err_to_name(err));
            heap_line("after a failed open");
            goto cleanup;
        }
        length = esp_http_client_fetch_headers(c);
        const int64_t t2 = esp_timer_get_time();
        status = esp_http_client_get_status_code(c);
        ESP_LOGI(TAG, "hop %d: HTTP %d, connect+TLS %lld ms, headers %lld ms, length %lld",
                 hop, status, (long long)((t1 - t0) / 1000),
                 (long long)((t2 - t1) / 1000), (long long)length);
        heap_line("connected");

        if (s_location[0]) {
            log_long("Location", s_location);
            log_token(s_location);
        }

        if (status >= 300 && status < 400) {
            if (esp_http_client_set_redirection(c) != ESP_OK) {
                ESP_LOGE(TAG, "hop %d: redirect with no usable Location", hop);
                goto cleanup;
            }
            esp_http_client_close(c);
            continue;
        }
        break;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "no stream: last status %d", status);
        goto cleanup;
    }

    {
        /* Static, not on the stack: esp_http_client runs the TLS handshake
         * and record layer on this task, and that wants the stack. */
        static char buf[READ_CHUNK];
        static uint8_t sniff[SNIFF_BYTES];
        static char meta[16 * 255 + 1];
        static char title[128];
        size_t sniffed = 0;
        int titles = 0;

        /* ICY framing: `metaint` audio bytes, a length byte L, 16*L bytes
         * of metadata, repeat. Tracked across reads byte by byte. */
        int64_t audio_left = s_metaint > 0 ? s_metaint : -1;
        int meta_left = -1, meta_len = 0;

        const int64_t start = esp_timer_get_time();
        int64_t last = start, total = 0, window = 0, audio_total = 0;
        static adts_count_t adts;
        memset(&adts, 0, sizeof(adts));
        uint64_t last_audio_ms = 0;
        static uint8_t aud[READ_CHUNK];
        size_t aud_n;
        int zero_reads = 0;

        while (esp_timer_get_time() - start < (int64_t)STREAMPROBE_SECONDS * 1000000) {
            const int n = esp_http_client_read(c, buf, sizeof(buf));
            if (n < 0) {
                ESP_LOGE(TAG, "read failed after %lld bytes", (long long)total);
                break;
            }
            if (n == 0) {
                if (++zero_reads > 50) {
                    ESP_LOGW(TAG, "stream ended after %lld bytes", (long long)total);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            zero_reads = 0;
            total += n;
            window += n;

            aud_n = 0;
            for (int i = 0; i < n; i++) {
                const uint8_t byte = (uint8_t)buf[i];
                if (meta_left < 0 && audio_left != 0) {
                    /* Audio. */
                    if (sniffed < sizeof(sniff)) sniff[sniffed++] = byte;
                    aud[aud_n++] = byte;
                    audio_total++;
                    if (audio_left > 0) audio_left--;
                } else if (meta_left < 0) {
                    /* The length byte. */
                    meta_left = byte * 16;
                    meta_len = 0;
                    if (meta_left == 0) { meta_left = -1; audio_left = s_metaint; }
                } else {
                    if (meta_len < (int)sizeof(meta) - 1) meta[meta_len++] = (char)byte;
                    if (--meta_left == 0) {
                        meta_left = -1;
                        audio_left = s_metaint;
                        if (titles < TITLES_MAX &&
                            icy_stream_title(meta, (size_t)meta_len, title, sizeof(title))) {
                            ESP_LOGI(TAG, "stream title: \"%s\"", title);
                            titles++;
                        }
                    }
                }
            }
            adts_count_bytes(&adts, aud, aud_n);

            if (sniffed == sizeof(sniff) && sniffed != SIZE_MAX) {
                ESP_LOGI(TAG, "first audio bytes look like: %s",
                         sniff_name(sniff_bytes(sniff, sniffed)));
                sniffed = SIZE_MAX;             /* once */
            }

            const int64_t t = esp_timer_get_time();
            if (t - last >= 5000000) {
                /* Audio seconds received against wall seconds: 1.0x is a
                 * server pacing to real time, more is a server sending
                 * ahead, less is a link that cannot keep up. */
                const uint64_t ams = adts_ms(&adts);
                const int64_t wall = (t - last) / 1000;
                const uint64_t got = ams - last_audio_ms;
                ESP_LOGI(TAG, "%lld s: %lld KB/s, %llu ms of audio in %lld ms (%llu.%02llux), %lld KB so far",
                         (long long)((t - start) / 1000000),
                         (long long)(window * 1000 / wall / 1024),
                         (unsigned long long)got, (long long)wall,
                         (unsigned long long)(got / (uint64_t)wall),
                         (unsigned long long)((got * 100 / (uint64_t)wall) % 100),
                         (long long)(total / 1024));
                last_audio_ms = ams;
                window = 0;
                last = t;
            }
        }
        if (sniffed != SIZE_MAX && sniffed > 0) {
            ESP_LOGI(TAG, "first audio bytes look like: %s",
                     sniff_name(sniff_bytes(sniff, sniffed)));
        }
        const int64_t secs_ms = (esp_timer_get_time() - start) / 1000;
        if (adts.frames) {
            ESP_LOGI(TAG, "ADTS: %u frames, AAC profile %u, %u Hz core, %u ch, frame %u-%u bytes, %llu bytes lost hunting",
                     (unsigned)adts.frames, adts.profile, adts.rate, adts.channels,
                     adts.min_len, adts.max_len, (unsigned long long)adts.lost);
            ESP_LOGI(TAG, "ADTS: %llu ms of audio in %lld ms, bitrate %llu kbit/s",
                     (unsigned long long)adts_ms(&adts), (long long)secs_ms,
                     adts_ms(&adts) ? (unsigned long long)(audio_total * 8 / adts_ms(&adts)) : 0ULL);
        }
        ESP_LOGI(TAG, "done: %lld KB in %lld ms, %lld kbit/s of audio, ICY every %d bytes, %d title%s",
                 (long long)(total / 1024), (long long)secs_ms,
                 secs_ms ? (long long)(audio_total * 8 / secs_ms) : 0LL,
                 s_metaint, titles, titles == 1 ? "" : "s");
    }

cleanup:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    heap_line("after");
out:
    vTaskDelete(NULL);
}

void streamprobe_kick(void)
{
    if (s_started || !STREAMPROBE_URL[0]) return;
    s_started = true;
    if (xTaskCreate(probe_task, "probe", 8192, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no task for the probe");
    }
}
