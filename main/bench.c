/*
 * bench.c -- see bench.h. Connect a station, read it as fast as it will
 * come, decode none of it, report what the transport managed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bench.h"
#include "netstream.h"
#include "stations.h"

static const char *TAG = "tab5_bench";

/*
 * The read size.
 *
 * NOT 2048, WHICH IS WHAT netdec USES, and the difference is the point.
 * netdec reads in 2048-byte pieces because that is what suits a frame
 * window; this reads in larger ones because it is asking what the
 * transport can do, not what the decoder happens to ask for. A drain
 * that copied the decoder's read size would carry the decoder's
 * per-call overhead into a measurement taken to exclude it.
 *
 * Internal memory, not PSRAM: the bytes are discarded immediately, so
 * this is a sink and never a cache, and 8 KB of internal is cheaper to
 * touch than a PSRAM round trip on every read.
 */
/*
 * THE TWO SIZES COMPARED, alternating window by window -- see the
 * kbps_read_big field in bench.h for why they are measured in one run
 * rather than two.
 *
 * SMALL IS NETSTREAM'S OWN 2048, deliberately, rather than something
 * chosen for symmetry: the question is whether netstream's read size
 * costs it anything, so the comparison has to include the size
 * netstream actually uses.
 *
 * BENCH_READ stays as the sink's size and must remain the LARGER of the
 * two, because that is the allocation every read writes into.
 */
#define BENCH_READ_BIG      (8192)
#define BENCH_READ_SMALL    (2048)
#define BENCH_READ          BENCH_READ_BIG

/* One reading a second, which is what makes a peak meaningful. */
#define BENCH_WINDOW_MS     (1000)

/* How long a single read may block. Short relative to the window so a
 * stalled second is recorded as a slow second rather than swallowing
 * the next one. */
#define BENCH_READ_MS       (250)

static SemaphoreHandle_t s_mu;
static bench_result_t    s_st;
static volatile bool     s_want;

static void lock_init(void)
{
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
}

void bench_state(bench_result_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    lock_init();
    if (!s_mu) return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_mu);
}

static void set_note(const char *why)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    snprintf(s_st.note, sizeof(s_st.note), "%s", why ? why : "");
    s_st.running = false;
    s_st.have = true;
    xSemaphoreGive(s_mu);
}

bool bench_request(void)
{
    lock_init();
    if (!s_mu) return false;
    /*
     * A REQUEST STILL WAITING IS NOT A RUN IN PROGRESS.
     *
     * s_want set means nobody has serviced the last request yet, which
     * used to leave the row reading BUSY for ever and the next press
     * answering "not now" -- with nothing running and nothing to wait
     * for. Re-arming is the honest answer: the press means the same
     * thing it meant the first time.
     */
    if (s_want) return true;

    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool busy = s_st.running;
    xSemaphoreGive(s_mu);
    if (busy) return false;

    /* Resolved here rather than in the panel: which station is selected
     * is stations.c's business, and the panel asking would be a second
     * place that knows how the list is indexed. */
    station_t st;
    if (!stations_get(stations_index(), &st) || !st.url[0]) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        memset(&s_st, 0, sizeof(s_st));
        s_st.have = true;
        snprintf(s_st.note, sizeof(s_st.note), "No station selected.");
        xSemaphoreGive(s_mu);
        return false;
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    memset(&s_st, 0, sizeof(s_st));
    s_st.running = true;
    snprintf(s_st.name, sizeof(s_st.name), "%s", st.name);
    xSemaphoreGive(s_mu);

    s_want = true;
    return true;
}

void bench_service(void)
{
    if (!s_want) return;
    s_want = false;
    lock_init();

    station_t st;
    if (!stations_get(stations_index(), &st) || !st.url[0]) {
        set_note("The station went away.");
        return;
    }

    uint8_t *sink = heap_caps_malloc(BENCH_READ, MALLOC_CAP_INTERNAL |
                                                 MALLOC_CAP_8BIT);
    if (!sink) {
        set_note("No memory to measure with.");
        return;
    }

    ESP_LOGI(TAG, "drain: %s <%s>", st.name, st.url);

    const int64_t t_start = esp_timer_get_time();
    if (!netstream_play(st.url, st.name)) {
        free(sink);
        set_note("Could not start the stream.");
        return;
    }

    /*
     * Wait for audio to be flowing rather than for a state: BUFFERING
     * already means bytes are arriving, and waiting for PLAYING would
     * fold the preroll into the connect time and out of the measurement
     * -- which is backwards, because the preroll is bytes moving and is
     * exactly what is being measured.
     */
    int connect_ms = 0;
    bool up = false;
    /*
     * IDLE IS THE STARTING STATE, NOT A FAILURE, and that distinction
     * is the whole of this loop's difficulty. netstream_play() posts a
     * request and returns; the netstream task logs `idle -> connecting`
     * a moment later. Treating IDLE as terminal therefore gave up on
     * the first pass, before the connection had begun, and reported
     * "Timed out connecting" in a few milliseconds.
     *
     * So IDLE only counts as an answer once the stream has been seen to
     * leave it. After that it means stopped, which is terminal and
     * worth reporting; before that it means not yet started.
     */
    bool left_idle = false;
    while (connect_ms < BENCH_CONNECT_MS) {
        const netstream_state_t s = netstream_state();
        if (s == NETSTREAM_BUFFERING || s == NETSTREAM_PLAYING) { up = true; break; }
        if (s != NETSTREAM_IDLE) left_idle = true;
        if (s == NETSTREAM_FAILED) break;
        if (s == NETSTREAM_IDLE && left_idle) break;
        vTaskDelay(pdMS_TO_TICKS(50));
        connect_ms = (int)((esp_timer_get_time() - t_start) / 1000);
    }
    connect_ms = (int)((esp_timer_get_time() - t_start) / 1000);

    if (!up) {
        netstream_stop_wait(2000);
        free(sink);
        /* Three outcomes and three sentences, because "timed out" was
         * being said for all of them and sent the wrong thing to be
         * investigated. */
        const netstream_state_t s = netstream_state();
        set_note(s == NETSTREAM_FAILED ? "The station did not answer."
                 : s == NETSTREAM_IDLE ? "The stream stopped before it started."
                 : "Timed out connecting.");
        return;
    }

    /* What it says it needs, asked once and now: a reconnect would
     * clear it, and the reading is judged against it. */
    const int declared = netstream_declared_kbps();

    uint32_t total = 0;
    int peak = 0;
    uint32_t win_bytes = 0;
    int64_t win_start = esp_timer_get_time();
    const int64_t t_drain = win_start;

    /*
     * Bytes and microseconds per read size, kept apart so each gets its
     * own rate. Alternated at every window boundary rather than run as
     * two halves: halves would give the first one TCP slow start and
     * the second one whatever the network was doing by then, which is
     * the confound this exists to avoid.
     */
    uint32_t sz_bytes[2] = { 0, 0 };
    uint64_t sz_us[2] = { 0, 0 };
    int which = 0;      /* 0 = big, 1 = small */

    while ((esp_timer_get_time() - t_drain) / 1000 < BENCH_RUN_MS) {
        const netstream_state_t s = netstream_state();
        if (s != NETSTREAM_BUFFERING && s != NETSTREAM_PLAYING) break;

        const size_t want = which ? BENCH_READ_SMALL : BENCH_READ_BIG;
        const size_t got = netstream_read(sink, want, BENCH_READ_MS);
        total += (uint32_t)got;
        win_bytes += (uint32_t)got;
        sz_bytes[which] += (uint32_t)got;

        const int64_t now = esp_timer_get_time();
        const int64_t win_us = now - win_start;
        if (win_us >= BENCH_WINDOW_MS * 1000) {
            /* bytes * 8 / ms IS kbit/s already: bits over
             * milliseconds is kilobits over seconds, because the two
             * thousands cancel. The extra * 1000 that used to be here
             * made it bits per second and reported 383228 for a link
             * doing 383. The mean below never had it, which is why one
             * number looked sane and the other did not. */
            const int kbps = (int)((uint64_t)win_bytes * 8ull
                                   / (uint64_t)(win_us / 1000));
            if (kbps > peak) peak = kbps;
            sz_us[which] += (uint64_t)win_us;
            win_bytes = 0;
            win_start = now;
            which ^= 1;         /* the other size next second */
        }
    }

    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t_drain) / 1000);
    netstream_stop_wait(2000);
    free(sink);

    const int avg = ms ? (int)((uint64_t)total * 8ull / (uint64_t)ms) : 0;

    /* Per size, over the seconds that size actually held. Zero when the
     * drain was too short to complete a window of it, which is honest:
     * one unfinished window is not a measurement. */
    int by_read[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        const uint64_t sms = sz_us[i] / 1000;
        if (sms) by_read[i] = (int)((uint64_t)sz_bytes[i] * 8ull / sms);
    }

    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_st.running = false;
    s_st.have = true;
    s_st.kbps_avg = avg;
    s_st.kbps_peak = peak;
    s_st.kbps_read_big = by_read[0];
    s_st.kbps_read_small = by_read[1];
    s_st.declared_kbps = declared;
    s_st.bytes = total;
    s_st.ms = ms;
    s_st.connect_ms = connect_ms;
    s_st.note[0] = '\0';
    xSemaphoreGive(s_mu);

    /*
     * Logged as well as shown, because the panel has three lines and
     * this is the line somebody will paste into a bug report.
     */
    ESP_LOGI(TAG, "drain: %s -- %d kbit/s mean, %d peak, %u KB in %u ms, "
                  "connect %d ms, station declares %d",
             st.name, avg, peak, (unsigned)(total / 1024), (unsigned)ms,
             connect_ms, declared);
    if (declared > 0) {
        ESP_LOGI(TAG, "drain: that is %d%% of what the station needs",
                 (avg * 100) / declared);
    }
    ESP_LOGI(TAG, "drain: %d-byte reads %d kbit/s, %d-byte reads %d kbit/s",
             BENCH_READ_BIG, by_read[0], BENCH_READ_SMALL, by_read[1]);
}
