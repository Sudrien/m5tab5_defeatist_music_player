/*
 * netstream.c -- see netstream.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "netstream.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "icydemux.h"
#include "streamsniff.h"

static const char *TAG = "tab5_netstream";

/* netstream.h publishes NETSTREAM_TITLE_MAX so callers need not include
 * icydemux.h to size a buffer. The two must agree: a caller's buffer
 * smaller than the demuxer's title would truncate on the way out, which
 * is a wrong title on screen and not a diagnostic. */
_Static_assert(NETSTREAM_TITLE_MAX == ICY_TITLE_MAX,
               "NETSTREAM_TITLE_MAX must match ICY_TITLE_MAX");

/* Read chunk. The probe used 2048 and measured 50-64 KB/s through it,
 * which is 25-32 reads a second; no reason to change what was measured. */
#define READ_CHUNK          (2048)

/* How long a ring send waits for room before giving up on this pass.
 * When the decoder is not draining -- it has not started yet, or it is
 * gone -- the task must still notice a stop request, so this is short
 * and the loop rechecks. */
#define SEND_SLICE_MS       (100)

/* Body read timeout. The stream is live, so a server that says nothing
 * for this long has dropped us whatever the socket thinks. */
#define READ_TIMEOUT_MS     (10000)

/* Sniffed prefix, logged once per connection. Phase 2 will ask
 * streamsniff.h the same question to pick a codec; this only records it. */
#define SNIFF_BYTES         (1024)

#define KBPS_WINDOW_US      (5000000)

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* The ring, and its storage. Allocated once in netstream_init() and
 * never freed: xStreamBufferCreateWithCaps() is banned here (see
 * CLAUDE.md, "Heap corruption: what actually happened"), and a static
 * buffer over a permanent allocation has no allocator to mispair a free
 * with. A stream that starts and stops forty times allocates nothing. */
static StreamBufferHandle_t s_ring;
static StaticStreamBuffer_t s_ring_struct;
static uint8_t             *s_ring_storage;

static TaskHandle_t      s_task;
static SemaphoreHandle_t s_lock;        /* guards the request and the strings */
static SemaphoreHandle_t s_idle;        /* given when the task reaches IDLE */

/* The request, written by any task under s_lock and read by ours. A
 * generation counter rather than a queue: a second play request while
 * one is connecting replaces it, which is what pressing next twice
 * means, and a queue would connect to both in turn. */
static char     s_req_url[NETSTREAM_URL_MAX];
static char     s_req_name[NETSTREAM_NAME_MAX];
static uint32_t s_req_gen;
static bool     s_req_stop;

/* Published. Written here, read by anyone. Integers, per the rule in
 * CLAUDE.md: an int cannot dangle. */
static volatile netstream_state_t s_state = NETSTREAM_IDLE;
static volatile int      s_kbps;
static volatile int      s_failures;
static volatile int      s_last_status;
static volatile uint32_t s_buffered;    /* the task's last reading */

/* Under s_lock. */
static char s_name[NETSTREAM_NAME_MAX];     /* icy-name, else the list's */
static char s_title[NETSTREAM_TITLE_MAX];
static bool s_has_title;

/*
 * The task's working set, at module scope rather than on its stack.
 *
 * icydemux_t is 4392 bytes -- almost all of it the meta[4081] buffer a
 * maximum-size ICY block needs -- and it was a local in
 * netstream_task(), together with 608 bytes of url and name and the
 * inlined connect_hops()'s own url[512]. That is the whole 6 KB stack
 * before a single byte is read, and the task died on its first pass
 * with a stack protection fault. See the note on the stack size in
 * netstream_init().
 *
 * These are safe at module scope for the same reason the ring is: there
 * is exactly one netstream task and exactly one stream at a time. That
 * is enforced by the design, not hoped for -- netstream_play() replaces
 * a running stream rather than starting a second one.
 */
static icydemux_t s_demux;
static char       s_url[NETSTREAM_URL_MAX];
static char       s_name_req[NETSTREAM_NAME_MAX];

/* Per-connection, owned by the task alone. */
static int  s_hdr_metaint;
static int  s_hdr_status_icy;
static char s_hdr_location[NETSTREAM_URL_MAX];
static char s_hdr_name[NETSTREAM_NAME_MAX];
static char s_hdr_ctype[64];

static void set_state(netstream_state_t st)
{
    if (s_state == st) return;
    ESP_LOGI(TAG, "%s -> %s", netstream_state_name(s_state),
             netstream_state_name(st));
    s_state = st;
}

static void publish_name(const char *name)
{
    if (!name || !name[0]) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_name, sizeof(s_name), "%s", name);
    xSemaphoreGive(s_lock);
}

static void publish_title(const char *title)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    /* An empty title is still a title the station sent: WNZK's is " - ",
     * and after trimming that is nothing. The flag is what lets a screen
     * fall back to the station name rather than showing a blank row. */
    s_has_title = title && title[0];
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* Headers                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_HEADER || !e->header_key) return ESP_OK;
    const char *k = e->header_key;
    const char *v = e->header_value ? e->header_value : "";

    if (strcasecmp(k, "location") == 0) {
        snprintf(s_hdr_location, sizeof(s_hdr_location), "%s", v);
    } else if (strcasecmp(k, "icy-metaint") == 0) {
        s_hdr_metaint = atoi(v);
    } else if (strcasecmp(k, "icy-name") == 0) {
        snprintf(s_hdr_name, sizeof(s_hdr_name), "%s", v);
    } else if (strcasecmp(k, "content-type") == 0) {
        snprintf(s_hdr_ctype, sizeof(s_hdr_ctype), "%s", v);
    }

    if (strcasecmp(k, "content-type") == 0 || strncasecmp(k, "icy-", 4) == 0) {
        ESP_LOGI(TAG, "  %s: %.120s", k, v);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

/* Has a newer request arrived? Every loop that can run for a long time
 * asks this, which is how a stop or a station change is serviced without
 * waiting for a connection to end on its own. */
static bool superseded(uint32_t gen)
{
    bool changed;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    changed = (s_req_gen != gen) || s_req_stop;
    xSemaphoreGive(s_lock);
    return changed;
}

/* A cancellable wait: the backoff, and the idle wait between streams. */
static bool wait_ms(int ms, uint32_t gen)
{
    const int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until) {
        if (superseded(gen)) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* One connection                                                      */
/* ------------------------------------------------------------------ */

/*
 * Open `url`, following redirects by hand so every hop logs and so the
 * hop limit is netplan's and not the client's. On success the client is
 * open with a body waiting. Returns the action decided for the final
 * response: PLAY, RETRY or FATAL.
 */
static netplan_action_t connect_hops(esp_http_client_handle_t c, uint32_t gen)
{
    netplan_action_t act = NETPLAN_RETRY;

    for (int hop = 0; hop <= NETPLAN_HOPS_MAX; hop++) {
        if (superseded(gen)) return NETPLAN_FATAL;

        s_hdr_metaint = 0;
        s_hdr_status_icy = 0;
        s_hdr_location[0] = '\0';
        s_hdr_name[0] = '\0';
        s_hdr_ctype[0] = '\0';

        char url[NETSTREAM_URL_MAX];
        if (esp_http_client_get_url(c, url, sizeof(url)) != ESP_OK) url[0] = '\0';
        ESP_LOGI(TAG, "hop %d: %.200s", hop + 1, url);

        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = esp_http_client_open(c, 0);
        const int64_t t1 = esp_timer_get_time();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "hop %d: open failed after %lld ms: %s", hop + 1,
                     (long long)((t1 - t0) / 1000), esp_err_to_name(err));
            /* A refused connection, a DNS failure or a TLS failure are
             * all transient as far as this file can tell: the radio may
             * have just come back, or the relay may be busy. */
            return NETPLAN_RETRY;
        }
        esp_http_client_fetch_headers(c);

        int status = esp_http_client_get_status_code(c);
        /* Shoutcast's `ICY 200 OK` is not an HTTP status line and the
         * parser may report nothing useful for it. netplan knows what it
         * means; whether esp_http_client hands it over at all is a
         * hardware question this file cannot answer from here. */
        if (status <= 0 && s_hdr_status_icy > 0) {
            ESP_LOGI(TAG, "hop %d: ICY status line, reading as %d",
                     hop + 1, s_hdr_status_icy);
            status = s_hdr_status_icy;
        }
        s_last_status = status;

        act = netplan_action(status);
        ESP_LOGI(TAG, "hop %d: HTTP %d -> %s, connect+TLS %lld ms, metaint %d",
                 hop + 1, status, netplan_action_name(act),
                 (long long)((t1 - t0) / 1000), s_hdr_metaint);

        if (act != NETPLAN_REDIRECT) return act;

        if (!netplan_may_redirect(hop + 1)) {
            ESP_LOGE(TAG, "more than %d redirects; giving up on this URL",
                     NETPLAN_HOPS_MAX);
            return NETPLAN_FATAL;
        }
        if (!s_hdr_location[0] ||
            esp_http_client_set_redirection(c) != ESP_OK) {
            ESP_LOGE(TAG, "hop %d: %d with no usable Location", hop + 1, status);
            return NETPLAN_FATAL;
        }
        esp_http_client_close(c);
    }
    return act;
}

/*
 * Read the body until it ends, fails, or a newer request arrives.
 * Returns the number of audio bytes that reached the ring, which is what
 * netplan_made_progress() is asked about.
 */
static uint64_t pump(esp_http_client_handle_t c, uint32_t gen, icydemux_t *d)
{
    /* Static, not on the stack: esp_http_client runs the TLS record layer
     * on this task and that wants the stack. Same reasoning as the
     * probe's buffers. */
    static uint8_t buf[READ_CHUNK];
    static uint8_t audio[READ_CHUNK];
    static uint8_t sniff[SNIFF_BYTES];
    size_t sniffed = 0;
    bool   sniff_logged = false;

    uint64_t produced = 0, window_bytes = 0;
    uint32_t last_titles = d->titles;
    int64_t  last_window = esp_timer_get_time();
    int64_t  last_progress = last_window;
    int      zero_reads = 0;

    while (!superseded(gen)) {
        const int n = esp_http_client_read(c, (char *)buf, sizeof(buf));
        if (n < 0) {
            ESP_LOGW(TAG, "read failed after %llu audio bytes",
                     (unsigned long long)produced);
            break;
        }
        if (n == 0) {
            /* esp_http_client_read() returns 0 both for "nothing yet"
             * and for a body that has ended, and a live stream never
             * ends on purpose. Told apart by time: a station that has
             * said nothing for READ_TIMEOUT_MS has dropped us. */
            if (esp_timer_get_time() - last_progress >
                (int64_t)READ_TIMEOUT_MS * 1000) {
                ESP_LOGW(TAG, "nothing for %d ms; treating as a drop",
                         READ_TIMEOUT_MS);
                break;
            }
            if (++zero_reads > 500) {
                ESP_LOGW(TAG, "stream ended after %llu audio bytes",
                         (unsigned long long)produced);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        zero_reads = 0;
        last_progress = esp_timer_get_time();
        window_bytes += (size_t)n;

        /* ICY metadata out, audio on. Tested by icydemuxtest, including
         * every way this read boundary can cut a metadata block. */
        const size_t got = icydemux_feed(d, buf, (size_t)n, audio);

        if (!sniff_logged && sniffed < sizeof(sniff) && got) {
            const size_t take = (sizeof(sniff) - sniffed) < got
                              ? (sizeof(sniff) - sniffed) : got;
            memcpy(sniff + sniffed, audio, take);
            sniffed += take;
            if (sniffed == sizeof(sniff)) {
                ESP_LOGI(TAG, "first audio bytes look like %s (content-type %s)",
                         sniff_name(sniff_bytes(sniff, sniffed)),
                         s_hdr_ctype[0] ? s_hdr_ctype : "absent");
                sniff_logged = true;
            }
        }

        if (d->titles != last_titles) {
            last_titles = d->titles;
            ESP_LOGI(TAG, "title: \"%.80s\"", d->title);
            publish_title(d->title);
        }

        /* Into the ring, in slices, so a decoder that is not draining
         * cannot stop this task from noticing a stop request. */
        for (size_t off = 0; off < got; ) {
            const size_t sent = xStreamBufferSend(s_ring, audio + off, got - off,
                                                  pdMS_TO_TICKS(SEND_SLICE_MS));
            off += sent;
            produced += sent;
            if (sent == 0) {
                if (superseded(gen)) return produced;
                /* A full ring is not an error: it is the decoder keeping
                 * up less than the network is delivering, which for a
                 * live stream means the server is ahead of real time.
                 * The bytes that do not fit are dropped rather than
                 * stalling the read, because stalling the read is how a
                 * server decides to disconnect us. */
                ESP_LOGW(TAG, "ring full; dropped %u bytes",
                         (unsigned)(got - off));
                break;
            }
        }

        s_buffered = (uint32_t)xStreamBufferBytesAvailable(s_ring);
        if (s_state == NETSTREAM_BUFFERING && produced > 0) {
            /* Phase 1 has no watermark of its own: "playing" here means
             * bytes are arriving and going somewhere. The 4 s / 1 s
             * watermarks in the plan belong to the PCM ring and to
             * phase 3, which owns the writer. */
            set_state(NETSTREAM_PLAYING);
        }

        const int64_t now = esp_timer_get_time();
        if (now - last_window >= KBPS_WINDOW_US) {
            const int64_t ms = (now - last_window) / 1000;
            s_kbps = ms ? (int)((int64_t)window_bytes * 8 / ms) : 0;
            ESP_LOGI(TAG, "%d kbit/s, ring %u%% (%u bytes), internal free %u, stack low water %u",
                     s_kbps, netstream_ring_pct(), (unsigned)s_buffered,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            window_bytes = 0;
            last_window = now;
        }
    }
    return produced;
}

/* ------------------------------------------------------------------ */
/* The task                                                            */
/* ------------------------------------------------------------------ */

static void netstream_task(void *arg)
{
    (void)arg;
    uint32_t gen = 0;
    /* url, name and the demuxer are at module scope: they are 5 KB
     * between them and this task has 8 KB including a TLS session. */
    char *const url = s_url;
    char *const name = s_name_req;

    for (;;) {
        /* Idle: wait for a request. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool have = (s_req_gen != gen) && !s_req_stop;
        if (have) {
            gen = s_req_gen;
            snprintf(url, sizeof(url), "%s", s_req_url);
            snprintf(name, sizeof(name), "%s", s_req_name);
        } else if (s_req_stop) {
            s_req_stop = false;
            gen = s_req_gen;
        }
        xSemaphoreGive(s_lock);

        if (!have) {
            if (s_state != NETSTREAM_IDLE && s_state != NETSTREAM_FAILED) {
                set_state(NETSTREAM_IDLE);
            }
            xSemaphoreGive(s_idle);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        ESP_LOGI(TAG, "stream requested: %.60s <%.160s>", name, url);
        publish_name(name);
        publish_title("");
        s_failures = 0;
        s_last_status = 0;
        s_kbps = 0;

        icydemux_init(&s_demux, 0);

        /* The ring starts empty for a new station: what is in it belongs
         * to the last one. A reconnect to the *same* station keeps it --
         * netplan_keep_buffer_on_reconnect() -- because those bytes are
         * still that station's audio from before the drop. */
        xStreamBufferReset(s_ring);
        s_buffered = 0;

        bool give_up = false;
        while (!give_up && !superseded(gen)) {
            set_state(NETSTREAM_CONNECTING);

            /* Always from the station URL. The redirect Zeno hands back
             * carries a token that lives sixty seconds, so a cached
             * resolved URL works in testing and fails in use. */
            const char *from = netplan_reconnect_from(url, NULL);

            esp_http_client_config_t cfg = {
                .url = from,
                .event_handler = on_event,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .disable_auto_redirect = true,
                .timeout_ms = READ_TIMEOUT_MS,
                .buffer_size = 4096,
                .buffer_size_tx = 1024,
                .user_agent = "DefeatistMusicPlayer/0.4",
            };
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (!c) {
                ESP_LOGE(TAG, "esp_http_client_init failed");
                s_failures++;
                goto backoff;
            }
            esp_http_client_set_header(c, "Icy-MetaData", "1");

            const netplan_action_t act = connect_hops(c, gen);
            if (act == NETPLAN_PLAY) {
                if (s_hdr_name[0]) publish_name(s_hdr_name);
                /* A reconnect keeps the title on screen and restarts the
                 * byte phase on the new body's own metaint. */
                icydemux_reconnect(&s_demux, s_hdr_metaint);
                set_state(NETSTREAM_BUFFERING);

                const uint64_t produced = pump(c, gen, &s_demux);

                if (netplan_made_progress(produced)) {
                    /* Audio flowed, so this is a fresh failure sequence
                     * rather than the fourth attempt at a dead station. */
                    if (s_failures) {
                        ESP_LOGI(TAG, "%llu bytes played; failure count reset",
                                 (unsigned long long)produced);
                    }
                    s_failures = 0;
                } else {
                    s_failures++;
                    ESP_LOGW(TAG, "only %llu audio bytes before the drop",
                             (unsigned long long)produced);
                }
            } else if (act == NETPLAN_FATAL) {
                ESP_LOGE(TAG, "giving up: status %d is not going to change",
                         s_last_status);
                give_up = true;
            } else {
                s_failures++;
            }

            esp_http_client_close(c);
            esp_http_client_cleanup(c);

        backoff:
            if (give_up || superseded(gen)) break;

            const int wait = netplan_backoff_ms(s_failures - 1);
            if (wait < 0) {
                ESP_LOGE(TAG, "%d attempts failed; giving up", s_failures);
                give_up = true;
                break;
            }
            set_state(NETSTREAM_RETRYING);
            ESP_LOGW(TAG, "attempt %d failed; retrying in %d ms",
                     s_failures, wait);
            if (!wait_ms(wait, gen)) break;
        }

        if (give_up) {
            set_state(NETSTREAM_FAILED);
        } else {
            set_state(NETSTREAM_STOPPING);
            /* Nothing downstream should play what is left of a stream
             * the listener has ended. */
            xStreamBufferReset(s_ring);
            s_buffered = 0;
            set_state(NETSTREAM_IDLE);
        }
        s_kbps = 0;
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

bool netstream_init(void)
{
    if (s_ring) return true;

    s_ring_storage = heap_caps_malloc(NETSTREAM_RING_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!s_ring_storage) {
        ESP_LOGE(TAG, "no PSRAM for a %d KB stream ring", NETSTREAM_RING_BYTES / 1024);
        return false;
    }
    /* +1 is the stream buffer's own requirement: it keeps one byte to
     * tell full from empty. */
    s_ring = xStreamBufferCreateStatic(NETSTREAM_RING_BYTES + 1, 1,
                                       s_ring_storage, &s_ring_struct);
    s_lock = xSemaphoreCreateMutex();
    s_idle = xSemaphoreCreateBinary();
    if (!s_ring || !s_lock || !s_idle) {
        ESP_LOGE(TAG, "stream ring or lock creation failed");
        return false;
    }

    /*
     * Priority 3: below the I2S writer (6) and ui_task (4), above
     * media_task (1). The network must never delay the writer, and the
     * ring is what covers the gap when it is descheduled.
     *
     * 8 KB of stack, not 6. The TLS record layer runs on this task, and
     * the first version put a 4392-byte icydemux_t on it as well and
     * died with a stack protection fault before it read a byte. The
     * demuxer and the URL buffers are at module scope now, so 6 KB
     * would very likely do -- this is 8 because the number that matters
     * is the one measured with a TLS session open, and until the log
     * below has printed it under load, headroom is cheaper than another
     * panic. The task reports its high-water mark every window, so the
     * right number is an observation rather than a guess.
     */
    if (xTaskCreate(netstream_task, "netstream", 8192, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "no task for netstream");
        return false;
    }
    ESP_LOGI(TAG, "ready: %d KB ring in PSRAM, %u byte demuxer at module scope",
             NETSTREAM_RING_BYTES / 1024, (unsigned)sizeof(s_demux));
    return true;
}

bool netstream_play(const char *url, const char *name)
{
    if (!s_ring || !url || !url[0]) return false;
    if (strlen(url) >= NETSTREAM_URL_MAX) {
        ESP_LOGE(TAG, "station URL is longer than %d characters", NETSTREAM_URL_MAX);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_req_url, sizeof(s_req_url), "%s", url);
    snprintf(s_req_name, sizeof(s_req_name), "%s", name ? name : "");
    s_req_stop = false;
    s_req_gen++;
    xSemaphoreGive(s_lock);
    return true;
}

void netstream_stop(void)
{
    if (!s_ring) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_req_stop = true;
    s_req_gen++;
    s_req_url[0] = '\0';
    xSemaphoreGive(s_lock);
}

bool netstream_stop_wait(int timeout_ms)
{
    if (!s_ring) return true;
    netstream_stop();
    const int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < until) {
        if (s_state == NETSTREAM_IDLE || s_state == NETSTREAM_FAILED) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "stop did not complete in %d ms; state is %s",
             timeout_ms, netstream_state_name(s_state));
    return false;
}

size_t netstream_read(void *buf, size_t n, int timeout_ms)
{
    if (!s_ring || !buf || !n) return 0;
    const size_t got = xStreamBufferReceive(s_ring, buf, n,
                                            pdMS_TO_TICKS(timeout_ms));
    return got;
}

netstream_state_t netstream_state(void) { return s_state; }
int    netstream_kbps(void)         { return s_kbps; }
int    netstream_failures(void)     { return s_failures; }
int    netstream_last_status(void)  { return s_last_status; }
size_t netstream_buffered(void)     { return s_buffered; }

int netstream_ring_pct(void)
{
    return (int)((uint64_t)s_buffered * 100 / NETSTREAM_RING_BYTES);
}

void netstream_name(char *out, size_t out_size)
{
    if (!out || !out_size) return;
    out[0] = '\0';
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, out_size, "%s", s_name);
    xSemaphoreGive(s_lock);
}

void netstream_title(char *out, size_t out_size)
{
    if (!out || !out_size) return;
    out[0] = '\0';
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, out_size, "%s", s_title);
    xSemaphoreGive(s_lock);
}

bool netstream_has_title(void) { return s_has_title; }
