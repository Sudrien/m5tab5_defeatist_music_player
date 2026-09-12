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
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "codecplan.h"
#include "netdec.h"
#include "mp3count.h"
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
/* Stop the stream and report the heap, shared by both probe modes. */
static void finish(int64_t t_start)
{
    (void)t_start;
    const int64_t t_stop = esp_timer_get_time();
    const bool stopped = netstream_stop_wait(5000);
    ESP_LOGI(TAG, "stop %s after %lld ms, state %s",
             stopped ? "completed" : "TIMED OUT",
             (long long)((esp_timer_get_time() - t_stop) / 1000),
             netstream_state_name(netstream_state()));
    heap_line("after");
}

#if STREAMPROBE_DECODE
/*
 * Decode the stream and throw the PCM away, AT REAL TIME.
 *
 * The pacing is the entire point. Every run so far drained the ring flat
 * out, so it sat at 0% and `stalled 0 ms` for six runs -- which means
 * **0121's backpressure has never executed.** A real decoder does not
 * drain flat out; it consumes one second of audio per second. WUOM hands
 * over about 43 seconds up front against a 33-second ring, so a paced
 * reader should fill the ring within the first fifteen seconds and hold
 * netstream in its send loop. If it does not, 0121 is wrong about
 * something.
 *
 * So this sleeps to keep decoded time level with wall time, and reports
 * the two against each other. It is a writer that writes nowhere: no
 * I2S, no resampling, no volume -- those are phase 3's, and putting them
 * here would mean this could fail for reasons that are not the stream
 * path's.
 *
 * What it certifies, in place of the "0 bytes lost hunting" that the raw
 * mode gave: `netdec_resyncs()`. Both are the same claim -- that the
 * bytes reaching the decoder are the bytes the station sent, in order --
 * measured on the far side of one more component.
 */
static void decode_paced(int64_t start)
{
    if (!netdec_open()) {
        ESP_LOGE(TAG, "netdec_open failed");
        return;
    }

    /* One frame of PCM. PSRAM: it is 4.6 KB and internal RAM is what the
     * Wi-Fi transport needs (0115). */
    int16_t *const pcm = heap_caps_malloc(NETDEC_MAX_INT16 * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
    if (!pcm) {
        ESP_LOGE(TAG, "no PSRAM for a PCM frame");
        netdec_close();
        return;
    }

    netdec_info_t info;
    memset(&info, 0, sizeof(info));

    uint64_t samples = 0;           /* per channel, total */
    uint64_t last_samples = 0;
    int64_t  last_win = esp_timer_get_time();
    int64_t  first_pcm = 0;
    int      rate = 0;
    unsigned ring_peak = 0, waits = 0, empties = 0;
    bool     fatal = false;

    while (esp_timer_get_time() - start < (int64_t)STREAMPROBE_SECONDS * 1000000) {
        const netstream_state_t st = netstream_state();
        if (st == NETSTREAM_FAILED) {
            ESP_LOGE(TAG, "netstream gave up (status %d, %d failures)",
                     netstream_last_status(), netstream_failures());
            break;
        }

        const unsigned pct = (unsigned)netstream_ring_pct();
        if (pct > ring_peak) ring_peak = pct;

        const int n = netdec_read(pcm, NETDEC_MAX_INT16, &info);
        if (n < 0) {
            ESP_LOGE(TAG, "netdec refused the stream; stopping");
            fatal = true;
            break;
        }
        if (n == 0) {
            empties++;
            continue;
        }
        if (!first_pcm) {
            first_pcm = esp_timer_get_time();
            ESP_LOGI(TAG, "first PCM at %lld ms: %d Hz, %d ch, %d kbit/s",
                     (long long)((first_pcm - start) / 1000),
                     info.sample_rate, info.channels, info.bitrate_kbps);
        }
        rate = info.sample_rate;
        samples += (uint64_t)(n / (info.channels > 0 ? info.channels : 1));

        /*
         * Pace. Sleep until wall time catches up with decoded time. A
         * frame is 26 ms at 44.1 kHz, so this sleeps most iterations
         * once the burst has been absorbed -- which is exactly when the
         * ring should start filling.
         */
        if (rate > 0 && first_pcm) {
            const int64_t due = first_pcm + (int64_t)(samples * 1000000ULL / (uint64_t)rate);
            const int64_t now = esp_timer_get_time();
            if (due > now) {
                const int ms = (int)((due - now) / 1000);
                if (ms > 0) {
                    waits++;
                    vTaskDelay(pdMS_TO_TICKS(ms > 100 ? 100 : ms));
                }
            }
        }

        const int64_t now = esp_timer_get_time();
        if (now - last_win >= 5000000) {
            const int64_t wall = (now - last_win) / 1000;
            const uint64_t dec_ms = rate ? (samples - last_samples) * 1000ULL / (uint64_t)rate : 0;
            ESP_LOGI(TAG, "%lld s: decoded %llu ms in %lld ms (%llu.%02llux), "
                          "ring %u%%, %u frames, %u resyncs, %u waits, %u empty",
                     (long long)((now - start) / 1000000),
                     (unsigned long long)dec_ms, (long long)wall,
                     (unsigned long long)(wall ? dec_ms / (uint64_t)wall : 0),
                     (unsigned long long)(wall ? (dec_ms * 100 / (uint64_t)wall) % 100 : 0),
                     (unsigned)netstream_ring_pct(), netdec_frames(),
                     netdec_resyncs(), waits, empties);
            last_samples = samples;
            last_win = now;
            waits = 0;
            empties = 0;
        }
    }

    const int64_t wall_ms = (esp_timer_get_time() - start) / 1000;
    const uint64_t total_ms = rate ? samples * 1000ULL / (uint64_t)rate : 0;
    ESP_LOGI(TAG, "decoded %u frames, %llu ms of audio in %lld ms, %u bytes resynced%s",
             netdec_frames(), (unsigned long long)total_ms, (long long)wall_ms,
             netdec_resyncs(), fatal ? " (stopped early)" : "");
    ESP_LOGI(TAG, "ring peak %u%% -- 0121's backpressure %s",
             ring_peak,
             ring_peak >= 90 ? "was exercised"
                             : "was NOT exercised; the ring never filled");

    free(pcm);
    netdec_close();
}
#endif /* STREAMPROBE_DECODE */

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

#if STREAMPROBE_DECODE
    decode_paced(start);
    finish(start);
    vTaskDelete(NULL);
    return;
#else
    /*
     * PSRAM, for the same reason netstream's buffers moved there: a
     * `static uint8_t buf[]` is internal RAM, and between this file and
     * netstream the stream path was holding about 41 KB of the 53-63 KB
     * free. What failed was not an allocation here -- it was
     * `dma_alloc(5120)` inside the Wi-Fi transport, and then DNS for the
     * rest of the run.
     */
    uint8_t *const work = heap_caps_malloc(READ_CHUNK + 2048, MALLOC_CAP_SPIRAM);
    if (!work) {
        ESP_LOGE(TAG, "no PSRAM for the probe's buffers");
        vTaskDelete(NULL);
        return;
    }
    uint8_t *const buf = work;
    uint8_t *const sniff = work + READ_CHUNK;
    const size_t sniff_size = 2048;

    /*
     * Both counters, fed every byte. Deciding between them from the
     * sniff result would mean not counting the bytes that arrive before
     * the sniff completes, and they are cheap: a few comparisons per
     * byte at 60 KB/s. Whichever one finds frames is the one the station
     * is sending, and if neither does, that is the finding.
     */
    static adts_count_t adts;       /* 40-odd bytes each; internal is fine */
    static mp3_count_t  mp3;
    memset(&adts, 0, sizeof(adts));
    memset(&mp3, 0, sizeof(mp3));

    /*
     * Which counter's clock is the real one, decided ONCE and then kept.
     *
     * The first version asked `adts.frames ? adts_ms() : mp3_ms()` every
     * window, and that is wrong twice over. The ADTS counter
     * false-positives on MP3 data -- a real run produced 93 "frames" at
     * 88200 Hz with 0 channels, frame lengths of 25 to 8187 bytes and
     * 442947 bytes lost hunting, which is obviously noise but is
     * obviously not zero. So the selector chose ADTS on an MP3 station.
     * Worse, it chose differently from window to window, so `ams` jumped
     * between two unrelated clocks, went backwards, and the unsigned
     * subtraction below wrapped: the log printed 18446744073709544819 ms
     * of audio and a rate of 3343012699113726.04x.
     *
     * codecplan.h exists for exactly this decision and was written
     * before this code and then not used here. It is used now, on the
     * first 64 bytes, and the answer is kept for the run.
     */
    stream_codec_t chosen = STREAM_CODEC_NONE;

    size_t sniffed = 0;
    bool   sniff_logged = false;

    int64_t  last = start, total = 0, window = 0;
    uint64_t last_audio_ms = 0;
    int      empties = 0, window_empties = 0;
    unsigned ring_max_pct = 0;
    char     title[NETSTREAM_TITLE_MAX], name[NETSTREAM_NAME_MAX];
    char     last_title[NETSTREAM_TITLE_MAX] = "";
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
        const size_t n = netstream_read(buf, READ_CHUNK, 200);
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
        mp3_count_bytes(&mp3, buf, n);

        if (!sniff_logged && sniffed < sniff_size) {
            const size_t take = (sniff_size - sniffed) < n
                              ? (sniff_size - sniffed) : n;
            memcpy(sniff + sniffed, buf, take);
            sniffed += take;
            if (sniffed == sniff_size) {
                ESP_LOGI(TAG, "first bytes out of the ring look like: %s",
                         sniff_name(sniff_bytes(sniff, sniffed)));
                sniff_logged = true;
            }
        }

        if (chosen == STREAM_CODEC_NONE && sniffed >= CODECPLAN_MIN_SNIFF_BYTES) {
            const codecplan_t plan =
                codecplan_choose(sniff_bytes(sniff, sniffed), sniffed, NULL);
            if (codecplan_ready(&plan)) {
                chosen = plan.codec;
                ESP_LOGI(TAG, "counting as %s", stream_codec_name(chosen));
            } else if (!codecplan_waiting(&plan)) {
                ESP_LOGW(TAG, "codecplan says: %s", plan.message);
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
            const uint64_t ams = (chosen == STREAM_CODEC_AAC_ADTS) ? adts_ms(&adts)
                               : (chosen == STREAM_CODEC_MP3)      ? mp3_ms(&mp3)
                               : 0;
            const int64_t wall = (t - last) / 1000;
            /* Never an unsigned subtraction without this. The clock is
             * monotonic by construction and went backwards anyway, and
             * the wrap produced a number 19 digits long in a log people
             * are meant to read. */
            const uint64_t got = ams > last_audio_ms ? ams - last_audio_ms : 0;
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
    /* Bytes lost hunting for a sync is the figure four runs of the AAC
     * station established at 0 -- it is how icydemux and the ring are
     * certified, and it has to be available on an MP3 station too. */
    const uint64_t audio_ms = (chosen == STREAM_CODEC_AAC_ADTS) ? adts_ms(&adts)
                            : (chosen == STREAM_CODEC_MP3)      ? mp3_ms(&mp3)
                            : 0;
    /* Both counters, always, with the chosen one named. The rejected
     * counter's figures are the evidence for the choice -- an MP3 run
     * shows ADTS finding a handful of frames and losing nearly every
     * byte, which is what a false positive looks like and is worth being
     * able to see rather than inferring. */
    ESP_LOGI(TAG, "counted as %s: ADTS %u frames / %llu lost, MPEG %u frames / %llu lost",
             stream_codec_name(chosen),
             (unsigned)adts.frames, (unsigned long long)adts.lost,
             (unsigned)mp3.frames, (unsigned long long)mp3.lost);
    if (chosen == STREAM_CODEC_MP3) {
        ESP_LOGI(TAG, "MP3: %u frames, MPEG%u layer %u, %u Hz, %u ch, %u kbit/s, frame %u-%u bytes, %llu bytes lost hunting",
                 (unsigned)mp3.frames, mp3.version, mp3.layer, mp3.rate,
                 mp3.channels, mp3.bitrate, mp3.min_len, mp3.max_len,
                 (unsigned long long)mp3.lost);
        ESP_LOGI(TAG, "MP3: %llu ms of audio in %lld ms, bitrate %llu kbit/s",
                 (unsigned long long)audio_ms, (long long)secs_ms,
                 audio_ms ? (unsigned long long)(total * 8 / (int64_t)audio_ms) : 0ULL);
    } else if (chosen == STREAM_CODEC_NONE) {
        ESP_LOGW(TAG, "no codec identified in %lld KB -- no duration to "
                      "compare against the clock",
                 (long long)(total / 1024));
    }
    if (chosen == STREAM_CODEC_AAC_ADTS) {
        ESP_LOGI(TAG, "ADTS: %u frames, AAC profile %u, %u Hz core, %u ch, frame %u-%u bytes, %llu bytes lost hunting",
                 (unsigned)adts.frames, adts.profile, adts.rate, adts.channels,
                 adts.min_len, adts.max_len, (unsigned long long)adts.lost);
        /* Bytes lost hunting for a sync is the demultiplexer's report
         * card. On the wire the old probe saw 0; anything above 0 here,
         * with the same station, is icydemux or the ring and not the
         * network. */
        ESP_LOGI(TAG, "ADTS: %llu ms of audio in %lld ms, bitrate %llu kbit/s",
                 (unsigned long long)audio_ms, (long long)secs_ms,
                 audio_ms ? (unsigned long long)(total * 8 / (int64_t)audio_ms) : 0ULL);
    }
    ESP_LOGI(TAG, "done: %lld KB drained in %lld ms from \"%s\", first byte %lld ms, ring peak %u%%, %d empty reads",
             (long long)(total / 1024), (long long)secs_ms, name,
             (long long)(first_byte_us ? (first_byte_us - start) / 1000 : -1),
             ring_max_pct, empties);

    /* The stop path matters as much as the start: nothing may leave a
     * TLS session open, and how long this takes is the number phase 3
     * needs for its pause. */
    finish(start);
    free(work);
#endif /* !STREAMPROBE_DECODE */
    vTaskDelete(NULL);
}

void streamprobe_kick(void)
{
    if (s_started || !STREAMPROBE_URL[0]) return;
    s_started = true;
    /*
     * NETDEC_MIN_STACK in decode mode: minimp3 puts about 11.6 KB of
     * scratch on the caller's stack, and 6144 died inside
     * mp3dec_decode_frame on the first frame. The raw mode never calls a
     * decoder and does not need it, but one number is easier to keep
     * right than two.
     */
    if (xTaskCreate(probe_task, "probe", NETDEC_MIN_STACK, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no task for the probe");
    }
}
