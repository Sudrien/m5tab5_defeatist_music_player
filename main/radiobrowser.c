/*
 * radiobrowser.c -- consuming the radio-browser.info API, and caching
 * what it says.
 *
 * radiobrowser.h built the URLs and never sent one. This sends them:
 * one request at a time, over TLS, through the mirror list, into a
 * small cache in PSRAM, and out as an M3U body that
 * stationlist_parse() already knows how to read.
 *
 * WHAT IS HERE AND WHAT IS DELIBERATELY NOT
 *
 * No task. Every caller is the player task -- the chooser asks for a
 * list the same way it asks for a station reload, as a request the
 * player task performs -- so this is a blocking call on a task that is
 * allowed to block, and there is no second task to synchronise a cache
 * against. A mutex guards the cache anyway, because `ui_task` reads the
 * status line and that is one word from another task.
 *
 * No JSON. Every endpoint reached from here has an M3U form. See
 * radiobrowser.h's browse section for why the first level of the tree
 * is pinned rather than fetched, which is the one thing that would need
 * a parser.
 *
 * No writing to the card. A browse result is a VIEW of the directory,
 * not the listener's station list, and overwriting a hand-edited
 * `stations.m3u` with fifty stations somebody was only looking at is
 * the kind of destruction that has no undo on a device with no undo.
 * Keeping one is a separate act and will be a separate patch.
 *
 * THE CACHE IS THE POINT, NOT AN OPTIMISATION
 *
 * The API asks for results to be held five to fifteen minutes and for
 * no more than two or three requests a second. Those are the terms on
 * which the service has no key and no account, and this player is in a
 * position to breach both without meaning to: the chooser redraws on a
 * dirty flag, a tap that lands twice is two presses, and walking back
 * up a level and down again is the same list a second later. Every one
 * of those is a request that the cache turns into a memcpy.
 *
 * So the cache is not there to make browsing quick. It is there so that
 * browsing is not rude, and being quick is what that happens to feel
 * like.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

/* 5052: esp_crt_bundle.h uses bool and only got <stdbool.h> by way of
 * the hardware AES headers. */
#include <stdbool.h>
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "albumart.h"
#include "jsonpick.h"
#include "radiobrowser.h"
#include "stations.h"

static const char *TAG = "tab5_rb";

/*
 * How long to wait on a mirror before trying the next one.
 *
 * Shorter than netstream's connect timeout, and for the opposite
 * reason: a station is the thing the listener asked for and is worth
 * waiting on, while a directory mirror is one of two interchangeable
 * names and the right response to a slow one is the other one. Six
 * seconds covers a TLS handshake to Europe from anywhere with a working
 * connection and gives up on anything that is not working.
 */
#define RB_TIMEOUT_MS       (6000)

/*
 * Entries, and the body each may hold.
 *
 * Four is the depth of the browse tree plus one: the tag level, two
 * lists either side of a mistap, and the chart somebody started from.
 * A fifth would hold a list nobody is going back to.
 *
 * STATIONS_FILE_MAX is what the parser is bounded by anyway, so a body
 * larger than that could not produce more stations -- it is the same
 * ceiling reached from the network rather than from the card. 50
 * stations of M3U with radio-browser's UUID comments is about 8 KB, so
 * the bound is four times the worst case a `limit=50` can produce and
 * exists to stop a mirror that answers with something else entirely.
 */
#define RB_CACHE_ENTRIES    (4)
#define RB_BODY_MAX         (32 * 1024)

typedef struct {
    char     url[RADIOBROWSER_URL_MAX];
    char    *body;
    size_t   len;
    uint32_t stamp_ms;
    bool     valid;
} rb_entry_t;

static rb_entry_t        s_cache[RB_CACHE_ENTRIES];
static SemaphoreHandle_t s_lock;
static uint32_t          s_last_req_ms;
static bool              s_ever_req;
/* Which mirror answered last, so the next request starts with the one
 * that is working rather than walking the dead one every time. Not a
 * health record: one success is not evidence, and the cost of being
 * wrong is one timeout. */
static int               s_host = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lock_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

/* ------------------------------------------------------------------ */
/* The cache                                                           */
/* ------------------------------------------------------------------ */

/*
 * Look `url` up, copying the body out.
 *
 * COPIED, NOT BORROWED, and that is the same call netstream_title() and
 * stations_get() already make: a pointer into a structure that can be
 * replaced is a race the caller cannot see. The bodies are single-digit
 * kilobytes and the copy happens once per screenful of browsing.
 */
static bool cache_get(const char *url, char *out, size_t out_size, size_t *out_len)
{
    bool hit = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < RB_CACHE_ENTRIES; i++) {
        if (!s_cache[i].valid || strcmp(s_cache[i].url, url) != 0) continue;
        if (!radiobrowser_cache_fresh(now_ms(), s_cache[i].stamp_ms)) {
            /* Stale rather than absent, and dropped rather than served.
             * The API's five minutes is a promise about how old an
             * answer may be, not a hint. */
            s_cache[i].valid = false;
            break;
        }
        if (s_cache[i].len < out_size) {
            memcpy(out, s_cache[i].body, s_cache[i].len);
            out[s_cache[i].len] = '\0';
            if (out_len) *out_len = s_cache[i].len;
            hit = true;
        }
        break;
    }
    xSemaphoreGive(s_lock);
    return hit;
}

/*
 * Store, replacing the oldest entry.
 *
 * Oldest by stamp rather than by a use counter. Browsing walks forward
 * -- tag, stations, back, another tag -- so the entry least likely to
 * be wanted is reliably the one fetched longest ago, and a counter
 * would be state that has to be maintained to say the same thing.
 *
 * A failure to allocate is not a failure to browse: the body is already
 * in the caller's hands, and all that is lost is the second request
 * being spared.
 */
static void cache_put(const char *url, const char *body, size_t len)
{
    if (len == 0 || len > RB_BODY_MAX) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int slot = 0;
    for (int i = 0; i < RB_CACHE_ENTRIES; i++) {
        if (!s_cache[i].valid) { slot = i; goto chosen; }
    }
    for (int i = 1; i < RB_CACHE_ENTRIES; i++) {
        if ((uint32_t)(s_cache[i].stamp_ms - s_cache[slot].stamp_ms) >
            0x80000000u) {
            slot = i;       /* i is older, wrap-safely */
        }
    }
chosen:
    ;
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (copy) {
        memcpy(copy, body, len);
        copy[len] = '\0';
        free(s_cache[slot].body);
        s_cache[slot].body = copy;
        s_cache[slot].len = len;
        s_cache[slot].stamp_ms = now_ms();
        snprintf(s_cache[slot].url, sizeof(s_cache[slot].url), "%s", url);
        s_cache[slot].valid = true;
    }
    xSemaphoreGive(s_lock);
}

void radiobrowser_cache_clear(void)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < RB_CACHE_ENTRIES; i++) {
        free(s_cache[i].body);
        s_cache[i].body = NULL;
        s_cache[i].valid = false;
        s_cache[i].len = 0;
    }
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* The request                                                         */
/* ------------------------------------------------------------------ */

/*
 * One GET, into `out`. Returns the body length, or 0.
 *
 * Read in a loop with an explicit bound rather than through
 * esp_http_client_perform() and an event handler: the body is wanted
 * whole, in one buffer, and the bound is the thing that matters -- a
 * mirror answering a station list with a gigabyte of something else
 * must stop at RB_BODY_MAX rather than at the heap.
 */
static size_t fetch_once(const char *url, char *out, size_t out_size)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    char ua[64];
    snprintf(ua, sizeof(ua), RADIOBROWSER_UA_FMT,
             desc && desc->version[0] ? desc->version : "0");

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = RB_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = ua,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return 0;

    size_t got = 0;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        ESP_LOGW(TAG, "no connection to %s", url);
        goto done;
    }
    esp_http_client_fetch_headers(c);

    const int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        /*
         * Reported and not retried here. A 4xx is this player asking
         * for something wrong and a second identical request will be
         * refused identically; a 5xx is this mirror, and the caller's
         * next mirror is the answer to that.
         */
        ESP_LOGW(TAG, "HTTP %d from the directory", status);
        goto close;
    }

    while (got + 1 < out_size) {
        const int n = esp_http_client_read(c, out + got, (int)(out_size - got - 1));
        if (n <= 0) break;          /* 0 is the end of a bounded body here */
        got += (size_t)n;
    }
    out[got] = '\0';

    if (got + 1 >= out_size) {
        /*
         * Truncated, and a truncated M3U is not a shorter list -- the
         * last entry is a name with no URL under it, which
         * stationlist_parse() counts as bad and everything above it is
         * still good. Said out loud because a list that is quietly two
         * stations short looks exactly like a directory that has two
         * fewer stations.
         */
        ESP_LOGW(TAG, "response filled the %u KB buffer; it is truncated",
                 (unsigned)(out_size / 1024));
    }

close:
    esp_http_client_close(c);
done:
    esp_http_client_cleanup(c);
    return got;
}

bool radiobrowser_list(radiobrowser_kind_t kind, const char *value,
                       char *out, size_t out_size, size_t *out_len)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (out_len) *out_len = 0;
    lock_init();

    static const char *const hosts[RADIOBROWSER_HOST_COUNT] =
        RADIOBROWSER_HOSTS;

    /*
     * The cache is keyed on the URL of the mirror that is about to be
     * tried, so it is asked once per mirror inside the loop rather than
     * once up front. That is not merely tidy: a list fetched from de1
     * and then asked for again after de1 has gone away is still the
     * same list, and a lookup keyed on the current mirror would miss it
     * and spend a request proving so.
     *
     * Which means the cache is checked for every mirror before any
     * request is sent. Two passes, and the first is four string
     * compares.
     */
    char url[RADIOBROWSER_URL_MAX];
    for (int h = 0; h < RADIOBROWSER_HOST_COUNT; h++) {
        const int idx = (s_host + h) % RADIOBROWSER_HOST_COUNT;
        if (!radiobrowser_list_url(url, sizeof(url), hosts[idx], kind, value)) {
            ESP_LOGW(TAG, "cannot build a URL for that");
            return false;
        }
        if (cache_get(url, out, out_size, out_len)) {
            ESP_LOGI(TAG, "cached: %s", url);
            return true;
        }
    }

    /*
     * The spacing the API asks for, enforced by waiting rather than by
     * refusing.
     *
     * A refusal would have to be handled by every caller and the only
     * sensible handling is to wait, so it is done once here. It is
     * bounded by construction: the gap is 400 ms and the request that
     * follows has a six-second timeout, so this can never be the slow
     * part of anything.
     */
    const uint32_t since_ms = now_ms() - s_last_req_ms;
    if (!radiobrowser_gap_ok(now_ms(), s_last_req_ms, s_ever_req)) {
        vTaskDelay(pdMS_TO_TICKS(RADIOBROWSER_MIN_GAP_MS - since_ms));
    }

    char *body = heap_caps_malloc(RB_BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "no PSRAM for a %u KB response", (unsigned)(RB_BODY_MAX / 1024));
        return false;
    }

    bool ok = false;
    for (int h = 0; h < RADIOBROWSER_HOST_COUNT; h++) {
        const int idx = (s_host + h) % RADIOBROWSER_HOST_COUNT;
        if (!radiobrowser_list_url(url, sizeof(url), hosts[idx], kind, value)) {
            break;
        }

        const int64_t t0 = esp_timer_get_time();
        s_last_req_ms = now_ms();
        s_ever_req = true;

        const size_t n = fetch_once(url, body, RB_BODY_MAX);
        if (n == 0) {
            /* Every reason to be here has already logged which one it
             * was. This line says what happens next, which is the part
             * a log of two mirrors needs. */
            ESP_LOGW(TAG, "mirror %s gave nothing; %s", hosts[idx],
                     (h + 1 < RADIOBROWSER_HOST_COUNT) ? "trying the other"
                                                       : "out of mirrors");
            continue;
        }

        ESP_LOGI(TAG, "%u bytes from %s in %d ms", (unsigned)n, hosts[idx],
                 (int)((esp_timer_get_time() - t0) / 1000));
        /* The mirror that answered becomes the one tried first. */
        s_host = idx;
        cache_put(url, body, n);

        if (n + 1 <= out_size) {
            memcpy(out, body, n);
            out[n] = '\0';
            if (out_len) *out_len = n;
            ok = true;
        } else {
            /* Cached anyway: the caller's buffer is too small for this
             * body and the next caller's may not be. */
            ESP_LOGW(TAG, "%u bytes will not fit the caller's %u",
                     (unsigned)n, (unsigned)out_size);
        }
        break;
    }

    free(body);
    return ok;
}

/*
 * One station, as JSON, for the one field M3U cannot carry.
 *
 * The response is a single-element array holding one flat object, which
 * is the shape jsonpick.h is bounded to and the reason it can be a
 * scanner rather than a parser.
 *
 * 8 KB rather than RB_BODY_MAX: one station's record is a few hundred
 * bytes and the bound is what stops a mirror answering this with a list
 * of fifty thousand. A body that fills the buffer is refused outright
 * rather than scanned, because a JSON document cut in half can still
 * contain a perfectly well-formed field and there is no way to tell
 * from inside it that the rest is missing.
 */
#define RB_JSON_MAX     (8 * 1024)

bool radiobrowser_favicon(const char *uuid, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    /* No uuid is the ordinary case -- every hand-written station -- so
     * it is a quiet false rather than a warning. */
    if (!uuid || strlen(uuid) != 36) return false;
    lock_init();

    for (size_t i = 0; i < strlen(uuid); i++) {
        const char c = uuid[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                        (c >= 'A' && c <= 'F') || c == '-';
        if (!ok) {
            /* It goes into a URL path. Anything outside a uuid's own
             * alphabet is either a mistake in the file or somebody
             * putting a path in a comment, and neither is worth
             * encoding for. */
            ESP_LOGW(TAG, "station id is not a uuid; no artwork lookup");
            return false;
        }
    }

    static const char *const hosts[RADIOBROWSER_HOST_COUNT] =
        RADIOBROWSER_HOSTS;

    char *body = heap_caps_malloc(RB_JSON_MAX, MALLOC_CAP_SPIRAM);
    if (!body) return false;

    bool ok = false;
    for (int h = 0; h < RADIOBROWSER_HOST_COUNT; h++) {
        const int idx = (s_host + h) % RADIOBROWSER_HOST_COUNT;
        char url[RADIOBROWSER_URL_MAX];
        const int n = snprintf(url, sizeof(url),
                               "https://%s/json/stations/byuuid/%s",
                               hosts[idx], uuid);
        if (n < 0 || (size_t)n >= sizeof(url)) break;

        if (cache_get(url, body, RB_JSON_MAX, NULL)) {
            ok = jsonpick_string(body, strlen(body), "favicon", out, out_size);
            break;
        }

        if (!radiobrowser_gap_ok(now_ms(), s_last_req_ms, s_ever_req)) {
            vTaskDelay(pdMS_TO_TICKS(RADIOBROWSER_MIN_GAP_MS));
        }
        s_last_req_ms = now_ms();
        s_ever_req = true;

        const size_t got = fetch_once(url, body, RB_JSON_MAX);
        if (!got) continue;
        if (got + 1 >= RB_JSON_MAX) {
            /* See RB_JSON_MAX: a truncated document can still hold a
             * well-formed field, so this refuses rather than reads. */
            ESP_LOGW(TAG, "station record filled the buffer; not reading it");
            break;
        }

        s_host = idx;
        cache_put(url, body, got);
        ok = jsonpick_string(body, got, "favicon", out, out_size);
        break;
    }

    free(body);
    if (ok && !out[0]) ok = false;      /* "" is no artwork */
    if (ok) ESP_LOGI(TAG, "artwork: %s", out);
    return ok;
}

/*
 * Report a play against a station, which is what the directory asks for
 * in return for having no key and no account.
 *
 * WHY THIS IS AN OBLIGATION AND NOT A NICETY. The browse menu is built
 * entirely on counts other people contributed: `Most voted`, `Most
 * listened`, and `order=votes` on every tag query. This player has been
 * reading those rankings since 0402 and adding nothing to them -- it
 * found Dance Wave because that station was second on Most voted. A
 * client that consumes the ordering and never reports a play is a free
 * rider on everyone else's clicks.
 *
 * NOT CACHED, AND THAT IS THE TRAP. Everything else here goes through
 * the five-minute cache, and a click served from cache is a click that
 * did not happen -- silently, and precisely for the station played most
 * often, since that is the one whose entry stays warm. So this does not
 * call cache_get() or cache_put() at all. The rate gap still applies,
 * because that rule is about the service's load rather than about
 * freshness.
 *
 * Fire and forget: the body is read and discarded, and a failure is one
 * log line at debug level. Nothing the listener asked for depends on
 * it, and a station must never fail to play because a counter did not
 * increment.
 */
void radiobrowser_click(const char *uuid)
{
    if (!radiobrowser_uuid_ok(uuid)) return;
    lock_init();

    static const char *const hosts[RADIOBROWSER_HOST_COUNT] =
        RADIOBROWSER_HOSTS;

    char url[RADIOBROWSER_URL_MAX];
    if (!radiobrowser_click_url(url, sizeof(url),
                                hosts[s_host % RADIOBROWSER_HOST_COUNT], uuid)) {
        return;
    }

    if (!radiobrowser_gap_ok(now_ms(), s_last_req_ms, s_ever_req)) {
        vTaskDelay(pdMS_TO_TICKS(RADIOBROWSER_MIN_GAP_MS));
    }
    s_last_req_ms = now_ms();
    s_ever_req = true;

    /*
     * One mirror, not both. A click is worth reporting once; retrying
     * it on the second mirror would risk counting the same play twice,
     * which is a worse failure than missing one -- the counts are the
     * thing being protected here.
     */
    char small[512];
    const size_t n = fetch_once(url, small, sizeof(small));
    ESP_LOGI(TAG, "click reported for %.8s...: %s", uuid, n ? "ok" : "no answer");
}

/*
 * Fetch a station's artwork.
 *
 * THESE ARE ARBITRARY THIRD-PARTY URLS AND THIS IS THE FILE THAT HAS TO
 * SAY NO. The URL comes from an icy-logo header or from a directory
 * entry anybody can edit; the bytes go to a hardware JPEG decoder. So
 * the limits are here rather than at the call site, and each is one a
 * station has a plausible way of tripping:
 *
 *   - SIZE. Bounded at RB_ART_MAX, and a response that fills the buffer
 *     is refused whole rather than decoded truncated. A half JPEG is a
 *     decoder reading a length field that describes bytes nobody sent,
 *     which is the one input shape most likely to find a bug in a codec
 *     this program did not write.
 *   - CONTENT TYPE. Must be an image. Plenty of these URLs have rotted
 *     into a parking page, and `<!DOCTYPE html>` reaching a JPEG
 *     decoder is a pointless risk when one header says not to.
 *   - MAGIC BYTES, checked against the same predicate the file path
 *     uses. A server that says image/jpeg and sends something else is
 *     not unusual, and albumart_is_supported_image() is already the one
 *     place that question is answered -- 0417's header made the same
 *     argument about not having two of anything.
 *   - REDIRECTS, followed by hand, at most RB_ART_HOPS, and never the
 *     plain-http downgrade 0418 allows for audio: a picture is not worth
 *     relaxing a policy for. esp_http_client_set_redirection() refuses
 *     https -> http itself. 5075: they were meant to be "left to
 *     esp_http_client's own limit", but open() and fetch_headers() --
 *     unlike perform() -- follow nothing, so every redirect was a silent
 *     blank. RFI's logo is http://www.rfi.fr/apple-touch-icon.png, a 301
 *     to https, which is the ordinary shape of it now: an upgrade.
 *
 * The caller owns the buffer on success and must free it.
 *
 * Blocks. Player task. Failure is the ordinary outcome and is a debug
 * line, not a warning: most stations have no artwork, many have a dead
 * link, and a blank square is not a fault.
 */
#define RB_ART_MAX      (192 * 1024)
#define RB_ART_HOPS     (3)         /* 5075: redirects followed */

bool radiobrowser_art_fetch(const char *url, uint8_t **out, size_t *out_len)
{
    if (!url || !url[0] || !out || !out_len) return false;
    *out = NULL;
    *out_len = 0;

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }

    uint8_t *buf = heap_caps_malloc(RB_ART_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) return false;

    const esp_app_desc_t *desc = esp_app_get_description();
    char ua[64];
    snprintf(ua, sizeof(ua), RADIOBROWSER_UA_FMT,
             desc && desc->version[0] ? desc->version : "0");

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = RB_TIMEOUT_MS,
        .buffer_size = 2048,
        .user_agent = ua,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(buf); return false; }

    size_t got = 0;
    bool ok = false;

    int status = 0;
    for (int hop = 0; ; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) goto done;
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);

        /* 5075: follow a redirect, upgrade included. See above. */
        const bool redirect = status == 301 || status == 302 || status == 303 ||
                              status == 307 || status == 308;
        if (!redirect || hop >= RB_ART_HOPS) break;
        const esp_err_t rerr = esp_http_client_set_redirection(c);
        if (rerr != ESP_OK) {
            ESP_LOGI(TAG, "artwork: %d not followed (%s)", status,
                     esp_err_to_name(rerr));
            goto close;
        }
        char to[160];
        if (esp_http_client_get_url(c, to, sizeof(to)) == ESP_OK) {
            ESP_LOGI(TAG, "artwork: %d to %.120s", status, to);
        }
        esp_http_client_close(c);
    }

    if (status != 200) {
        /* Said, at last: a blank square used to have no line at all. */
        ESP_LOGI(TAG, "artwork: HTTP %d; no picture", status);
        goto close;
    }

    {
        char *ctype = NULL;
        if (esp_http_client_get_header(c, "Content-Type", &ctype) == ESP_OK &&
            ctype && strncasecmp(ctype, "image/", 6) != 0) {
            ESP_LOGI(TAG, "artwork is %.32s, not an image; skipped", ctype);
            goto close;
        }
    }

    while (got < RB_ART_MAX) {
        const int n = esp_http_client_read(c, (char *)buf + got,
                                           (int)(RB_ART_MAX - got));
        if (n <= 0) break;
        got += (size_t)n;
    }

    if (got >= RB_ART_MAX) {
        /* See above: refused whole rather than decoded truncated. */
        ESP_LOGW(TAG, "artwork filled the %u KB buffer; not decoding it",
                 (unsigned)(RB_ART_MAX / 1024));
        goto close;
    }
    if (got < 16 || !albumart_is_supported_image(buf, got)) {
        ESP_LOGI(TAG, "artwork is not a JPEG or PNG; skipped");
        goto close;
    }

    ok = true;

close:
    esp_http_client_close(c);
done:
    esp_http_client_cleanup(c);
    if (!ok) { free(buf); return false; }

    ESP_LOGI(TAG, "artwork: %u bytes", (unsigned)got);
    *out = buf;
    *out_len = got;
    return true;
}
