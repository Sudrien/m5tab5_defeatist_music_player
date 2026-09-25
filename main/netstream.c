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
#include "ethernet.h"
#include "wifi.h"

/* The route the open connection took, from net_route_describe() at its
 * last hop. Module scope: read by the rate line on the same task. */
static char s_route[48] = "no route";

/* Set when pump() ends a healthy connection on purpose to move it onto
 * the cable (netplan_should_move()), so the reconnect goes at once and
 * says why instead of reporting a drop. Same task only. */
static bool s_moving;

static const char *TAG = "tab5_netstream";

/* netstream.h publishes NETSTREAM_TITLE_MAX so callers need not include
 * icydemux.h to size a buffer. The two must agree: a caller's buffer
 * smaller than the demuxer's title would truncate on the way out, which
 * is a wrong title on screen and not a diagnostic. */
_Static_assert(NETSTREAM_TITLE_MAX == ICY_TITLE_MAX,
               "NETSTREAM_TITLE_MAX must match ICY_TITLE_MAX");

/*
 * Read chunk.
 *
 * 2048, MEASURED TWICE AND TRIED AT 8192 ONCE. Do not raise it again
 * without a reading that 0407's did not already take.
 *
 * The probe measured 50-64 KB/s through 2048-byte reads. Three later
 * board logs showed delivery never exceeding about 230 kbit/s across
 * three stations on two servers, including a 128 kbit/s station pegged
 * at 222-230 -- which looked like a server with plenty left being held
 * to a ceiling, and 29 KB/s against the probe's 50-64 looked like a
 * per-read cost that had grown with TLS.
 *
 * 0407 raised it to 8192 to settle that. It is not a ceiling:
 *
 *   peak 209, bytes 3% (8192), audio  4.35s   ... 159% of need
 *   peak 209, bytes 3% (8192), audio  7.49s
 *   peak 209, bytes 3% (8192), audio 12.64s
 *   peak 209, bytes 3% (8192), audio 17.62s
 *   peak 209, bytes 3% (8192), audio 17.68s   ... 100% of need
 *
 * Four times the bytes per read produced a peak of 209 where 2048 had
 * produced 222-230. Slightly lower, which is noise, and certainly not
 * four times anything.
 *
 * WHAT THE 230 ACTUALLY WAS, which is the part worth keeping. The
 * reserve climbs from 4.35 s to 17.7 s and stops, and delivery drops to
 * exactly 100% of need and stays there for a minute. Nothing is being
 * held to a rate: the PCM ring filled, the decode loop stopped asking,
 * and a reader that is not asked does not read. Delivery here is
 * PULL-limited by a full buffer, and the 1.6x during the fill is the
 * decode loop's own refill pacing above REFILL_PACE_UNTIL_PCT.
 *
 * So two different regimes produced numbers near each other and the
 * coincidence read as a ceiling. WUOM's probe run had already said as
 * much and it was not connected up: 6.6x burst for ten seconds with the
 * compressed ring pegged at 100%, through these same 2048-byte reads.
 * A path that does 6.6x on one station does not have a 230 kbit/s
 * ceiling.
 *
 * Which leaves the walmradio stations where they were, and honestly so:
 * 224 kbit/s of audio over TLS, delivered at about 167, is this link
 * being too slow for that station. There is nothing in this file to
 * fix.
 *
 * Back to 2048 because that is what was pre-registered for this
 * outcome, and because 8192 costs 12 KB of PSRAM for the two buffers
 * below and bought a result of nothing. The peak field that made this
 * readable stays.
 */
#define READ_CHUNK          (2048)

/* How long a ring send waits for room before giving up on this pass.
 * When the decoder is not draining -- it has not started yet, or it is
 * gone -- the task must still notice a stop request, so this is short
 * and the loop rechecks. */
#define SEND_SLICE_MS       (100)

/*
 * Two timeouts, because one constant was doing two unrelated jobs.
 *
 * SOCKET_TIMEOUT_MS is how long a single read blocks with nothing to
 * read. It bounds how quickly the task notices a stop, because a stop
 * request is checked between reads and not during one. Observed stop
 * latencies across six runs were 59, 59, 119, 119, 219 and 599 ms -- a
 * paced 64 kbit/s station delivers 2048 bytes every 256 ms, so a read
 * normally returns quickly, but the worst case was the whole 5 s.
 * Phase 3's pause is a stop, and five seconds of a button doing nothing
 * is not a pause.
 *
 * DROP_SILENCE_MS is how long a live stream may say nothing before it is
 * treated as gone. That is a property of the stream, not of the socket,
 * and it is measured from the last byte that actually arrived rather
 * than from any one read returning empty.
 *
 * Separating them lets the read be impatient and the diagnosis be
 * patient. They were the same number only because the first version had
 * one place to put it.
 *
 * CONNECT_TIMEOUT_MS is the third, and it exists because setting
 * `cfg.timeout_ms` to SOCKET_TIMEOUT_MS broke connecting outright.
 * **esp_http_client's timeout covers the header fetch as well as body
 * reads**, and this server takes 1.4 to 2 seconds between the TLS
 * handshake and its first header -- comfortably inside the old 5 s and
 * hopeless against 1 s. A run failed four connections with
 * `Connection timed out before data was ready!`, took 18 seconds to
 * reach first audio instead of 2, and only succeeded because the fifth
 * attempt happened to be quick.
 *
 * So the client is opened patient and made impatient afterwards, with
 * esp_http_client_set_timeout_ms() once the headers are in and the only
 * thing left is the body. Connecting is a slow, once-per-stream thing;
 * reading is a fast, constant thing; they want opposite settings and the
 * API allows both.
 */
#define CONNECT_TIMEOUT_MS  (5000)
/*
 * How long to wait for the radio to have an address before spending a
 * connection attempt. Longer than a cold join takes on this board --
 * about 16 s from boot in every log, and 14 s of that is the first
 * association expiring and being retried -- with room for a slow DHCP.
 *
 * Sliced so the wait can be abandoned: a station change during it is
 * serviced at the next slice rather than at the end.
 */
#define NET_WAIT_MAX_MS         (25000)
#define NET_WAIT_SLICE_MS       (100)
/* 5028. One echo to the gateway before every
 * attempt (5029: then 8.8.8.8); a LAN round trip is single-digit
 * milliseconds, so a second without an answer is not a slow network,
 * it is a dead one. */
#define NET_PROBE_TIMEOUT_MS    (1000)
#define NET_PROBE_EVERY_MS      (2000)

#define SOCKET_TIMEOUT_MS   (1000)
#define DROP_SILENCE_MS     (5000)

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
/*
 * All of it in PSRAM, allocated once in netstream_init() and never
 * freed.
 *
 * These were internal-RAM statics, which is the default for a `static
 * uint8_t buf[]`, and between the demuxer, the two 2 KB working buffers,
 * the sniff buffer and an 8 KB stack this file claimed about 26 KB of
 * internal RAM -- with the probe's own buffers, 41 KB against the 53-63
 * KB free that netstream.h itself identifies as the scarce resource.
 *
 * The symptom was not a failed allocation here. It was
 * `eh_sdio: dma_alloc(5120) failed; dropping read` inside the Wi-Fi
 * transport, then a dead socket, then getaddrinfo() failing for the rest
 * of the run. **The code that ran out of memory was not the code that
 * took it.** DMA-capable internal RAM is a subset of internal RAM, so it
 * runs out earlier than the free-heap figure suggests.
 *
 * None of this needs to be internal. The bytes arrive by memcpy out of
 * mbedTLS's buffer, not by DMA, and 54 KB/s through PSRAM is nothing.
 * Only the task stack has to stay internal.
 */
static icydemux_t *s_demux;     /* 4392 bytes */
static uint8_t    *s_rx;        /* READ_CHUNK, off the socket */
static uint8_t    *s_audio;     /* READ_CHUNK, after the demuxer */
static uint8_t    *s_sniff;     /* SNIFF_BYTES, first bytes of a body */
static char       *s_url;       /* NETSTREAM_URL_MAX */
static char       *s_name_req;  /* NETSTREAM_NAME_MAX */

/* Per-connection, owned by the task alone. */
static int  s_hdr_metaint;
/*
 * icy-br, the bitrate the STATION declares, in kbit/s.
 *
 * Kept because the decoder cannot always supply one. minimp3 reports a
 * bitrate per frame and the AAC decoder reports none, so WNZK decoded
 * happily and logged `0 kbit/s` -- and the artwork card, which exists to
 * say what the stream is, had nothing to put on its bitrate line.
 *
 * A declaration and not a measurement, which is the whole caveat: it is
 * what the station says it sends, it is absent on plenty of servers, and
 * on a variable-rate stream it is a nominal figure. That is still the
 * number every other player shows for AAC, and it is better than the
 * line being missing.
 */
static int  s_hdr_br;
/* What the audio actually costs, from the decoder. See the header. */
static volatile int s_actual_br;
/*
 * The best delivery window this station has managed.
 *
 * Kept after 0407 because it is what made that log readable. A claim
 * that a number is not being exceeded cannot be tested against an
 * average -- a station bursting to 600 and idling averages what one
 * held flat at 300 does -- and the peak is also how a pull-limited
 * stream is told from a starved one at a glance: a peak well above need
 * with delivery sitting at 100% is a full buffer, while a peak that
 * never reaches need is a station this link cannot carry.
 */
static int  s_kbps_peak;

/*
 * Seconds of DECODED audio the player has queued, x100, pushed in by
 * play_stream() so the statistics line can report it.
 *
 * Because the "ring 0%" in that line is the BYTE ring, and reading it as
 * the buffer is the mistake it invites. netdec pulls from the byte ring
 * as fast as bytes arrive, so at a steady state it is empty BY DESIGN --
 * 0% there means the decoder is keeping up, which is the good case.
 * Every log of this series reports 0% beside 0 rebuffers, and the two
 * facts agree.
 *
 * The reserve that protects against a network hiccup is the PCM ring,
 * downstream of the decoder, and nothing printed it. So the one number
 * a listener or a maintainer actually wants -- how many seconds of sound
 * are in hand -- was the one number missing from the line that exists
 * to answer that question.
 *
 * Pushed rather than pulled: the PCM ring is player.c's and netstream is
 * below it. Same direction as s_ring_pct, opposite direction to the
 * dependency.
 */
static volatile int s_audio_cs;
static int  s_hdr_status_icy;
static char s_hdr_location[NETSTREAM_URL_MAX];
static char s_hdr_name[NETSTREAM_NAME_MAX];
/*
 * icy-logo, the station's own artwork, straight from the server.
 *
 * The best of the three artwork sources and the only free one: it
 * arrives in the headers that are already parsed and was being logged
 * and dropped. Dance Wave sends it, walmradio sends it; SomaFM and
 * zeno.fm do not, which is why 0417's directory lookup exists as the
 * fallback rather than as the primary.
 */
static char s_hdr_logo[NETSTREAM_URL_MAX];
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
    } else if (strcasecmp(k, "icy-br") == 0) {
        /* Some servers send a list for a multi-rate mount ("128,64").
         * atoi() takes the first, which is the one being served on this
         * connection. */
        const int br = atoi(v);
        if (br > 0 && br < 10000) s_hdr_br = br;
    } else if (strcasecmp(k, "icy-logo") == 0) {
        snprintf(s_hdr_logo, sizeof(s_hdr_logo), "%s", v);
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
        s_hdr_br = 0;
        s_actual_br = 0;   /* a new station decodes to its own rate */
        s_kbps_peak = 0;
        s_hdr_status_icy = 0;
        s_hdr_location[0] = '\0';
        s_hdr_name[0] = '\0';
        s_hdr_logo[0] = '\0';
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
        /* The route this connection took, taken now and kept: an unplug
         * moves the default, not a socket that is already open, and the
         * rate line below reports the two separately. */
        net_route_describe(s_route, sizeof(s_route));
        ESP_LOGI(TAG, "hop %d: HTTP %d -> %s, connect+TLS %lld ms, metaint %d, via %s",
                 hop + 1, status, netplan_action_name(act),
                 (long long)((t1 - t0) / 1000), s_hdr_metaint, s_route);

        if (act != NETPLAN_REDIRECT) return act;

        if (!netplan_may_redirect(hop + 1)) {
            ESP_LOGE(TAG, "more than %d redirects; giving up on this URL",
                     NETPLAN_HOPS_MAX);
            return NETPLAN_FATAL;
        }
        if (!s_hdr_location[0]) {
            ESP_LOGE(TAG, "hop %d: %d with no Location at all", hop + 1, status);
            return NETPLAN_FATAL;
        }
        if (esp_http_client_set_redirection(c) != ESP_OK) {
            /*
             * THE HTTPS -> HTTP DOWNGRADE, WHICH IS ORDINARY FOR RADIO.
             *
             * esp_http_client refuses to follow a secure origin to a
             * plain one, which is right for anything carrying a
             * credential and wrong for this. Found on a station from
             * the directory:
             *
             *   HTTPS origin can only redirect to https:// targets
             *   (got http://stream1.dancewave.online:8080/dance.mp3)
             *   hop 1: 302 with no usable Location
             *   giving up: status 302 is not going to change
             *
             * A dead station, permanently, over a policy that is not
             * about it. The pattern is everywhere in the directory: an
             * https entry pointing at a load balancer that hands out
             * plain-http mount points, because Icecast mounts often are
             * plain http. SomaFM is http end to end and has always
             * played here.
             *
             * NOTHING IS BEING PROTECTED BY REFUSING. There is no
             * account, no cookie, no credential and no request body --
             * this player sends a GET and a user-agent. What TLS buys
             * on the first hop is that the DIRECTORY's answer was not
             * tampered with, and that hop stays encrypted; the audio
             * that follows is public and identical to what the same
             * station serves everyone. A listener who could not play an
             * http station at all would be the safer design, and this
             * program already plays them by name.
             *
             * So the scheme is checked and the hop is taken by hand,
             * and it is LOGGED as a downgrade rather than slipped
             * through -- somebody reading a log should be able to see
             * that the encryption stopped, and where.
             */
            const bool http  = strncmp(s_hdr_location, "http://", 7) == 0;
            const bool https = strncmp(s_hdr_location, "https://", 8) == 0;
            if (!http && !https) {
                /* Not a scheme this plays. A relative Location, or
                 * something stranger; either way set_redirection() has
                 * already declined and there is nothing to hand-build
                 * that would be safe to guess at. */
                ESP_LOGE(TAG, "hop %d: %d to an unusable Location",
                         hop + 1, status);
                return NETPLAN_FATAL;
            }
            if (esp_http_client_set_url(c, s_hdr_location) != ESP_OK) {
                ESP_LOGE(TAG, "hop %d: %d and the Location will not open",
                         hop + 1, status);
                return NETPLAN_FATAL;
            }
            if (http) {
                ESP_LOGW(TAG, "hop %d: following to plain http; "
                              "the audio from here is not encrypted",
                         hop + 1);
            }
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
    /* In PSRAM, not on the stack and not in internal RAM -- see the note
     * on the declarations. esp_http_client runs the TLS record layer on
     * this task and that wants the stack it has. */
    uint8_t *const buf = s_rx;
    uint8_t *const audio = s_audio;
    uint8_t *const sniff = s_sniff;
    size_t sniffed = 0;
    bool   sniff_logged = false;

    uint64_t produced = 0, window_bytes = 0;
    /*
     * Time spent waiting for room in the ring.
     *
     * **Measured as elapsed time in the send loop, not as a count of
     * timeouts.** The first version incremented by SEND_SLICE_MS only
     * when xStreamBufferSend() returned 0 after its full 100 ms, and
     * that almost never happens: a decoder draining at real time frees a
     * 208-byte frame every 26 ms, so a send blocks briefly and partially
     * succeeds. The run that finally filled the ring held it at 99% for
     * forty-five seconds while netstream's input rate fell from 408 to
     * 64 kbit/s -- exactly the drain rate, which is backpressure doing
     * precisely its job -- and this counter reported 0 ms throughout.
     * The mechanism was right and the instrument was measuring
     * something else.
     */
    int stalled_ms = 0, last_stall_log = 0;
    uint32_t last_titles = d->titles;
    int64_t  last_window = esp_timer_get_time();
    int64_t  last_progress = last_window;
    int      zero_reads = 0;

    while (!superseded(gen)) {
        const int n = esp_http_client_read(c, (char *)buf, READ_CHUNK);
        if (n < 0) {
            ESP_LOGW(TAG, "read failed after %llu audio bytes",
                     (unsigned long long)produced);
            /* 5030: the heap at the moment the link died, for the case
             * where a USB device arriving starved the Wi-Fi transport. */
            ESP_LOGW(TAG, "  internal %u free (largest %u), DMA %u free (largest %u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            break;
        }
        if (n == 0) {
            /* esp_http_client_read() returns 0 both for "nothing yet"
             * and for a body that has ended, and a live stream never
             * ends on purpose. Told apart by time: a station that has
             * said nothing for DROP_SILENCE_MS has dropped us. */
            if (esp_timer_get_time() - last_progress >
                (int64_t)DROP_SILENCE_MS * 1000) {
                ESP_LOGW(TAG, "nothing for %d ms; treating as a drop",
                         DROP_SILENCE_MS);
                break;
            }
            /* The time check above is the real one; this only catches a
             * read that returns 0 immediately and forever, which would
             * otherwise spin. */
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

        if (!sniff_logged && sniffed < SNIFF_BYTES && got) {
            const size_t take = (SNIFF_BYTES - sniffed) < got
                              ? (SNIFF_BYTES - sniffed) : got;
            memcpy(sniff + sniffed, audio, take);
            sniffed += take;
            if (sniffed == SNIFF_BYTES) {
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

        /*
         * Into the ring, in slices, so a decoder that is not draining
         * cannot stop this task from noticing a stop request.
         *
         * **This waits rather than dropping.** The first version dropped
         * what would not fit, on the argument that a full ring means the
         * server is ahead of real time and that stalling a read is how a
         * server decides to disconnect us. The first argument is
         * backwards and the second is not how HTTP works.
         *
         * WUOM measured it: StreamTheWorld front-loads about 43 seconds
         * of audio -- windows of 3.18x, 6.06x and 2.25x -- and then
         * paces at exactly real time, a mean of 1.000x over the next
         * forty seconds. At 64 kbit/s that burst is 454 KB against a
         * 256 KB ring, so with a decoder draining at 1x the old path
         * would have discarded about 198 KB, some 25 seconds of audio
         * the listener was going to hear. A gap in the middle of a
         * programme, from a station that did nothing wrong.
         *
         * Not reading is the correct response: the TCP window closes,
         * the server stops sending, and the bytes wait in its buffer
         * instead of being thrown away in ours. That is what every other
         * streaming client does. A server that disconnects an idle
         * reader exists, and netplan already handles a drop by
         * reconnecting -- which costs a reconnect, against a guaranteed
         * hole in the audio.
         */
        const int64_t send_start = esp_timer_get_time();
        for (size_t off = 0; off < got; ) {
            const size_t sent = xStreamBufferSend(s_ring, audio + off, got - off,
                                                  pdMS_TO_TICKS(SEND_SLICE_MS));
            off += sent;
            produced += sent;
            if (sent == 0 && superseded(gen)) return produced;
        }
        /* Whatever that cost, whether it came from one long block or
         * fifty short ones. Near zero while the ring has room. */
        const int send_ms = (int)((esp_timer_get_time() - send_start) / 1000);
        if (send_ms > 0) {
            stalled_ms += send_ms;
            if (stalled_ms - last_stall_log >= 5000) {
                last_stall_log = stalled_ms;
                ESP_LOGI(TAG, "ring full, waiting rather than dropping "
                              "(%d ms so far) -- the server is ahead and "
                              "TCP can hold it", stalled_ms);
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
            /* "bytes" is the byte ring and is meant to be near zero;
             * "audio" is the decoded reserve and is the one that
             * matters. Named rather than left as a second percentage,
             * because two percentages on one line is how they got
             * confused for each other. */
            const int acs = s_audio_cs;
            /*
             * THE FIRST FIGURE IS DELIVERY, NOT BITRATE, AND UNTIL 0404
             * THE LINE DID NOT SAY SO.
             *
             * It is bytes off the socket over the window. The station's
             * own rate is `icy-br`, twenty lines earlier in the log and
             * printed once at connect, so reading this line as "the
             * stream is 165 kbit/s" was the obvious mistake and it was
             * made -- on a board log of a 320 kbit/s station being
             * delivered at about half that, where the question asked
             * was whether the station was VBR. It was not. It was
             * starving, and every other figure on this line said so:
             * the reserve sawtoothing between 1.0 and 3.7 seconds and
             * never climbing.
             *
             * So the comparison is made here, where both numbers are in
             * hand, rather than left to whoever reads it. This is the
             * rule 0908 wrote down after the same thing cost a session
             * twice: a document that answers a question nobody thinks
             * to look up has not answered it, and neither has a log
             * line that carries half of one.
             *
             * `of N declared` rather than a verdict, and the percentage
             * beside it. Delivery below the declared rate is not
             * automatically a fault -- a server ahead of real time
             * catches up, and the window is five seconds -- so what is
             * printed is the ratio and what reads it is a person.
             * `SHORT` is appended only when the reserve is falling as
             * well, which is the pair that means something: under-
             * delivery with a full ring is a server pacing itself.
             */
            /* 64 for the same reason player.c's is: " of " plus two
             * ints at 11 apiece plus " declared (", "%)" and " SHORT"
             * is 46 in the worst case the types allow. */
            /*
             * THE DENOMINATOR IS WHAT THE AUDIO COSTS, NOT WHAT THE
             * STATION CALLS ITSELF -- corrected from 0404, which used
             * icy-br and got a station wrong by twenty points.
             *
             * `needed` is the decoded frame rate when there is one. A
             * station declaring 320 and emitting 224 is not lying, it
             * is describing a grade; the reader has to keep up with the
             * 224. Falls back to the declared value, which is all the
             * AAC path ever has.
             */
            if (s_kbps > s_kbps_peak) s_kbps_peak = s_kbps;
            /*
             * THE DENOMINATOR IS THE DECODER'S, AND 0415 IS WHY.
             *
             * 0414 divided by the ring's drain rate measured here, and
             * a board log came back reading 100% on almost every line:
             * 212 of 210, 127 of 128, 487 of 483, 424 of 424. That is a
             * tautology, not a measurement. The compressed ring sits at
             * 0%, so everything that arrives is taken immediately and
             * consumption EQUALS delivery by construction. The figure
             * could not have said anything else, and it was printed
             * beside a reserve visibly draining from 3.8 s to 1.0 s.
             *
             * What was wanted is bytes per SECOND OF AUDIO, which only
             * the decoder can see -- it is the only thing holding both
             * compressed bytes in and decoded samples out. netdec
             * measures it on a decoded-audio clock and pushes it here.
             *
             * CONFIRMED ON THE BOARD, 0416, on the two stations that
             * bracket the problem:
             *
             *   WNZK    of 511-522 needed, delivery 401-483, SHORT on
             *           every line, reserve sawtoothing 1.0-3.9 s.
             *   SomaFM  of 120-133 needed, delivery 238 falling to 128
             *           as the ring filled, then 95-103% with an 18 s
             *           reserve and SHORT on none of it.
             *
             * The denominator moves independently of the numerator now,
             * which is the whole thing 0414 could not do. And WNZK's
             * 512 is not a number anyone here chose: play_stream()'s
             * header recorded 512 kbit/s AAC from a probe run long
             * before any of this, by a different method.
             *
             * 0930: that sentence used to end "on a station that
             * declares nothing at all", and WNZK now sends
             * `icy-br: 320`. It costs 512. So the station does declare
             * something, and what it declares is wrong by sixty per
             * cent -- which is a better argument for this denominator
             * than a silent station ever was.
             *
             * THE GUARD IS WHAT MAKES THE FLAG MEAN SOMETHING. SomaFM
             * prints 95%, 96% and 97% while perfectly healthy -- a full
             * ring delivers exactly what is consumed and jitter does
             * the rest -- and those lines are not SHORT because the
             * reserve is 18 s. Under-delivery alone is not a fault.
             * Under-delivery with a falling reserve is.
             */
            const int needed = s_actual_br > 0 ? s_actual_br : s_hdr_br;
            char rate_note[64] = "";
            if (needed > 0) {
                snprintf(rate_note, sizeof(rate_note), " of %d needed (%d%%)%s",
                         needed, (s_kbps * 100) / needed,
                         (s_kbps < needed && s_audio_cs < 400) ? " SHORT" : "");
            }
            /* Where the bytes come from, and -- only when it differs --
             * where a new connection would go instead. The difference is
             * the window between an unplug and the reconnect. */
            char now_route[48], route_note[64] = "";
            net_route_describe(now_route, sizeof(now_route));
            if (strcmp(now_route, s_route) != 0) {
                snprintf(route_note, sizeof(route_note), " (default now %s)",
                         now_route);
            }
            ESP_LOGI(TAG, "%d kbit/s%s via %s%s, peak %d, bytes %u%% (%u), "
                          "audio %d.%02ds, "
                          "stalled %d ms, internal free %u, "
                          "stack low water %u",
                     s_kbps, rate_note, s_route, route_note, s_kbps_peak,
                     netstream_ring_pct(), (unsigned)s_buffered,
                     acs / 100, acs % 100,
                     stalled_ms,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            window_bytes = 0;
            last_window = now;

            /* Once per window, after the line that shows why. */
            if (netplan_should_move(strncmp(s_route, "cable", 5) == 0,
                                    ethernet_connected(), acs)) {
                char to[48];
                net_route_describe(to, sizeof(to));
                ESP_LOGI(TAG, "the cable is up and this connection is on %s; "
                              "moving it to %s with %d.%02ds in hand",
                         s_route, to, acs / 100, acs % 100);
                s_moving = true;
                break;
            }
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
    /*
     * url, name and the demuxer live at module scope: 5 KB between them,
     * against a task stack of 8 KB that also carries a TLS session.
     *
     * No local alias for them. The first version of this had
     * `char *const url = s_url;` to keep the body reading the same,
     * which silently turned every `sizeof(url)` in the function from 512
     * into 4 -- the width of a pointer -- and would have truncated every
     * station URL to three characters. gcc refused it outright
     * (-Wformat-truncation, "up to 511 bytes into a region of size 4"),
     * which is the good outcome; the same mistake behind a memcpy or a
     * strncpy compiles and ships.
     */

    for (;;) {
        /* Idle: wait for a request. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool have = (s_req_gen != gen) && !s_req_stop;
        if (have) {
            gen = s_req_gen;
            snprintf(s_url, NETSTREAM_URL_MAX, "%s", s_req_url);
            snprintf(s_name_req, NETSTREAM_NAME_MAX, "%s", s_req_name);
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

        ESP_LOGI(TAG, "stream requested: %.60s <%.160s>", s_name_req, s_url);
        publish_name(s_name_req);
        publish_title("");
        s_failures = 0;
        s_last_status = 0;
        s_kbps = 0;

        icydemux_init(s_demux, 0);

        /* The ring starts empty for a new station: what is in it belongs
         * to the last one. A reconnect to the *same* station keeps it --
         * netplan_keep_buffer_on_reconnect() -- because those bytes are
         * still that station's audio from before the drop. */
        xStreamBufferReset(s_ring);
        s_buffered = 0;

        bool give_up = false;
        while (!give_up && !superseded(gen)) {
            set_state(NETSTREAM_CONNECTING);

            /*
             * WAIT FOR A NETWORK BEFORE SPENDING AN ATTEMPT ON ONE.
             *
             * A station tapped before the join completes fails DNS
             * instantly -- `getaddrinfo() returns 202` -- and each of
             * those counted as a real attempt, so the backoff schedule
             * was consumed against a condition that had nothing to do
             * with the station:
             *
             *   attempt 1 failed after 4 ms; retrying in 996 ms
             *   attempt 2 failed after 5 ms; retrying in 1995 ms
             *   attempt 3 failed after 5 ms; retrying in 3995 ms
             *
             * Three of the four attempts gone in fourteen milliseconds
             * of work, and the fourth only succeeded because the
             * backoff had grown long enough to outlast the join by
             * accident. A slower join would have exhausted the schedule
             * and reported a dead station.
             *
             * So it is not a failure and is not counted as one. Polled
             * rather than waited on an event, because this loop already
             * has to answer superseded() promptly -- a listener who
             * changes their mind during the wait must not be held for
             * the remainder of it.
             *
             * Bounded, because "no network" can also be permanent: no
             * saved network in range, or the radio off. NET_WAIT_MAX_MS
             * past that and the attempt goes ahead and fails honestly,
             * which puts the real error in the log rather than leaving
             * the screen on "Connecting" for ever.
             */
            /*
             * AND A PING BEFORE THE LOOKUP -- 5028.
             *
             * An address on an interface is not a working link. The
             * board had Wi-Fi "connected" through a coprocessor that had
             * stopped moving packets (mempool OOM, RPCs timing out), and
             * each attempt then spent 5 s in select() or 14 s in
             * getaddrinfo() before failing -- counted, so the backoff
             * ran out against the same non-station fault the wait above
             * exists for. A gateway that does not answer an echo is
             * treated the same way: waited out, not counted, re-probed
             * every NET_PROBE_EVERY_MS, bounded by NET_WAIT_MAX_MS.
             */
            int gw_ms = -1, net_ms = -1;
            char probe[96];
            bool path = net_online() &&
                net_probe(NET_PROBE_TIMEOUT_MS, &gw_ms, &net_ms,
                          probe, sizeof(probe));
            if (!path) {
                if (!net_online()) {
                    ESP_LOGI(TAG, "no network yet; waiting before the first "
                                  "lookup");
                } else {
                    ESP_LOGW(TAG, "%s; waiting before the lookup", probe);
                }
                int waited = 0, since_probe = 0;
                while (!path && waited < NET_WAIT_MAX_MS &&
                       !superseded(gen)) {
                    vTaskDelay(pdMS_TO_TICKS(NET_WAIT_SLICE_MS));
                    waited += NET_WAIT_SLICE_MS;
                    since_probe += NET_WAIT_SLICE_MS;
                    if (net_online() && since_probe >= NET_PROBE_EVERY_MS) {
                        since_probe = 0;
                        path = net_probe(NET_PROBE_TIMEOUT_MS, &gw_ms, &net_ms,
                                         probe, sizeof(probe));
                    }
                }
                if (superseded(gen)) break;
                if (path) {
                    ESP_LOGI(TAG, "network up after %d ms (%s); connecting",
                             waited, probe);
                } else if (net_online()) {
                    ESP_LOGW(TAG, "%s after %d ms; trying anyway",
                             probe, waited);
                } else {
                    ESP_LOGW(TAG, "still no network after %d ms; trying "
                                  "anyway", waited);
                }
            } else {
                ESP_LOGI(TAG, "%s", probe);
            }

            const int64_t attempt_start = esp_timer_get_time();

            /* Always from the station URL. The redirect Zeno hands back
             * carries a token that lives sixty seconds, so a cached
             * resolved URL works in testing and fails in use. */
            const char *from = netplan_reconnect_from(s_url, NULL);

            esp_http_client_config_t cfg = {
                .url = from,
                .event_handler = on_event,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .disable_auto_redirect = true,
                .timeout_ms = CONNECT_TIMEOUT_MS,
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
                icydemux_reconnect(s_demux, s_hdr_metaint);
                set_state(NETSTREAM_BUFFERING);

                /* Headers are in; nothing slow is left. Tighten the
                 * socket so a stop is noticed within a second rather
                 * than within five. Doing this before the headers
                 * arrived is what 0122 got wrong. */
                esp_http_client_set_timeout_ms(c, SOCKET_TIMEOUT_MS);

                const uint64_t produced = pump(c, gen, s_demux);

                if (netplan_made_progress(produced)) {
                    /* Audio flowed, so this is a fresh failure sequence
                     * rather than the fourth attempt at a dead station.
                     * Not logged when the stream is being stopped: pump
                     * also returns on a stop request, and announcing a
                     * reset failure count immediately after "stopping"
                     * reads as though a reconnect were being prepared
                     * for a stream that is ending. */
                    if (s_failures && !superseded(gen)) {
                        ESP_LOGI(TAG, "%llu bytes played; failure count reset",
                                 (unsigned long long)produced);
                    }
                    s_failures = 0;
                } else if (superseded(gen)) {
                    /*
                     * A STOP IS NOT A DROP, AND THIS IS THE HALF THAT
                     * STILL SAID IT WAS.
                     *
                     * pump() returns on a stop request as well as on a
                     * dead socket, which the branch above already
                     * accounts for -- it withholds its line when the
                     * stream is being stopped, for exactly this reason.
                     * This branch did not, so a station ended
                     * deliberately and early enough to be under
                     * netplan_made_progress()'s threshold was announced
                     * as a network failure.
                     *
                     * The Ogg refusal is how it was found: the decoder
                     * gave up, play_stream() stopped the stream, and
                     * the log's last word on a station that had been
                     * delivering perfectly was `only 12288 audio bytes
                     * before the drop`. Two faults in one run, and the
                     * second one blames the network for the first.
                     *
                     * The count goes with the message. s_failures is
                     * reset when a station is requested, so the stray
                     * increment changes nothing today -- it is wrong in
                     * the same way the line is, and a failure count that
                     * counts stops is a trap for whatever reads it next.
                     */
                    ESP_LOGI(TAG, "stopped after %llu audio bytes",
                             (unsigned long long)produced);
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
            if (give_up || superseded(gen)) {
                s_moving = false;   /* the next station is not moving */
                break;
            }

            const int attempt_ms =
                (int)((esp_timer_get_time() - attempt_start) / 1000);

            /*
             * s_failures is the count *after* this attempt, and 0 means
             * the attempt played audio and the sequence restarted -- so
             * the wait is indexed from s_failures - 1, and a reset gives
             * table[0] through netplan_backoff_ms()'s clamp. Passing -1
             * and relying on a clamp read as an accident and logged
             * "attempt 0 failed", which is not a thing; the reset case
             * is now its own branch and says what happened.
             */
            int wait;
            if (s_moving) {
                /* Not a drop and not a failure: nothing to back off from. */
                s_moving = false;
                wait = 0;
                set_state(NETSTREAM_RETRYING);
            } else if (s_failures == 0) {
                wait = netplan_backoff_ms(0);
                set_state(NETSTREAM_RETRYING);
                ESP_LOGW(TAG, "dropped after playing; reconnecting in %d ms",
                         wait);
            } else {
                wait = netplan_backoff_ms(s_failures - 1);
                if (wait < 0) {
                    ESP_LOGE(TAG, "%d attempts failed; giving up", s_failures);
                    give_up = true;
                    break;
                }
                /*
                 * The schedule assumed attempts are cheap. They are not:
                 * a DNS failure took 13590 ms against a 1000 ms backoff,
                 * so "1, 2, 4, 8 and give up after 15 s" was really a
                 * minute of 14-second attempts. The backoff exists to
                 * stop hammering a server, and an attempt that already
                 * spent longer than the wait has done the waiting.
                 */
                if (attempt_ms >= wait) {
                    ESP_LOGW(TAG, "attempt %d failed after %d ms; that is "
                                  "longer than the %d ms backoff, retrying now",
                             s_failures, attempt_ms, wait);
                    wait = 0;
                } else {
                    ESP_LOGW(TAG, "attempt %d failed after %d ms; retrying in "
                                  "%d ms", s_failures, attempt_ms, wait - attempt_ms);
                    wait -= attempt_ms;
                }
                set_state(NETSTREAM_RETRYING);
            }
            if (wait && !wait_ms(wait, gen)) break;
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

    /* The working set, also PSRAM, also never freed. One block so a
     * partial failure cannot leave half of it live. */
    const size_t work = sizeof(icydemux_t) + READ_CHUNK * 2 + SNIFF_BYTES +
                        NETSTREAM_URL_MAX + NETSTREAM_NAME_MAX;
    uint8_t *w = heap_caps_malloc(work, MALLOC_CAP_SPIRAM);
    if (!w) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte working set", (unsigned)work);
        return false;
    }
    s_demux    = (icydemux_t *)w;   w += sizeof(icydemux_t);
    s_rx       = w;                 w += READ_CHUNK;
    s_audio    = w;                 w += READ_CHUNK;
    s_sniff    = w;                 w += SNIFF_BYTES;
    s_url      = (char *)w;         w += NETSTREAM_URL_MAX;
    s_name_req = (char *)w;
    s_url[0] = '\0';
    s_name_req[0] = '\0';
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
     * 6 KB of stack, measured rather than guessed. 0112 panicked at 6 KB
     * with a 4392-byte demuxer on it, went to 8 KB, and then reported a
     * high-water mark of 4848 free out of 8192 for a whole minute with a
     * TLS session open and a redirect walked -- a peak of 3344 bytes. 6
     * KB leaves 2.8 KB of margin on that, and the 2 KB handed back is
     * internal RAM, which is the resource that actually ran out.
     *
     * The stack is the one thing here that cannot go to PSRAM.
     */
    if (xTaskCreate(netstream_task, "netstream", 6144, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "no task for netstream");
        return false;
    }
    /* The read size is on this line so a log says which arm of 0407's
     * experiment it came from. A number that has to be inferred from
     * the binary is not evidence. */
    ESP_LOGI(TAG, "ready: %d KB ring + %u byte working set in PSRAM, "
                  "%d byte reads, internal free %u",
             NETSTREAM_RING_BYTES / 1024, (unsigned)work, READ_CHUNK,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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

/*
 * The station's declared bitrate, or 0 if it did not say.
 *
 * Cleared on each hop's request rather than at stream start, so a
 * redirect that lands on a mount with a different rate reports the
 * mount's figure and not the first server's. Read from the decode loop
 * and written from the HTTP event callback, which is the same task, so
 * there is nothing to publish here the way s_ring_pct is.
 */
int netstream_declared_kbps(void) { return s_hdr_br; }

bool netstream_logo(char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    /* Copied, like the title and the name, and for the same reason: a
     * pointer into a buffer the reader task rewrites on every hop is a
     * race the caller cannot see. */
    snprintf(out, out_size, "%s", s_hdr_logo);
    return out[0] != '\0';
}
void netstream_set_actual_kbps(int kbps) { if (kbps > 0) s_actual_br = kbps; }
int  netstream_actual_kbps(void)         { return s_actual_br; }

/* How much decoded audio the player has in hand, in hundredths of a
 * second. Pushed in every pass of the stream loop; see s_audio_cs. */
void netstream_note_audio_ms(int ms)
{
    s_audio_cs = ms > 0 ? ms / 10 : 0;
}
