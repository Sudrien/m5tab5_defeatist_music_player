/*
 * recorder -- see recorder.h, and 5106 in ARCHITECTURE.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "recorder.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "audio_out.h"
#include "beam.h"
#include "flacenc.h"
#include "heapmap.h"
#include "settings.h"
#include "storage.h"
#include "storage_io.h"

static const char *TAG = "tab5_rec";

#define REC_DIR             "Recordings"
#define REC_FRAME_BYTES     (AUDIO_CAPTURE_CHANNELS * sizeof(int32_t))

/* Two seconds of capture between the microphones and the card. A card's
 * worst ordinary stall (a FAT allocation, a wear-levelling pause) is a
 * few hundred ms; two seconds is the margin, at 768 KB of PSRAM. */
#define REC_RING_BYTES      (2u * AUDIO_CAPTURE_RATE * REC_FRAME_BYTES)

/* What rec_in moves per read: 5 ms, a quarter of the DMA's 20 ms. */
#define REC_IN_FRAMES       (240)

/* What rec_enc takes per pass: one FLAC block. */
#define REC_BLOCK           (4096)

/* Under FAT32's 4 GiB file limit with room for the last block and the
 * header rewrite. About 5 1/2 hours at the rate these files come out. */
#define REC_MAX_BYTES       (4000000000ull)

/* Stacks. rec_in reads into a module buffer and calls nothing deep.
 * rec_enc goes through flacenc (about 300 bytes, 5104) and fwrite into
 * FatFs, which is the deep part. Internal RAM, and only for the length
 * of a recording: created at start, deleted at the end. */
#define REC_IN_STACK        (3072)
#define REC_ENC_STACK       (6144)
#define REC_IN_PRIO         (6)     /* i2s_wr's: the DMA has 20 ms */
#define REC_ENC_PRIO        (3)

/* ---- state ---- */

/*
 * The ring and the two work buffers, allocated at the first recording
 * and never freed -- netstream.c's rule for a static stream buffer over a
 * permanent allocation, for its reason (CLAUDE.md). PSRAM: 768 KB + 32 KB
 * + 2 KB, none of it touched when nothing is recording.
 */
static StreamBufferHandle_t s_ring;
static StaticStreamBuffer_t s_ring_struct;
static uint8_t             *s_ring_storage;
static int32_t             *s_in_buf;       /* REC_IN_FRAMES frames */
static int32_t             *s_enc_buf;      /* REC_BLOCK frames */
static beam_t              *s_beam;         /* 5109: ~1.2 KB, PSRAM with the rest */
static bool                 s_use_beam;     /* this recording's choice */

static FILE      *s_file;
static flacenc_t *s_enc;

static volatile bool s_active;
static volatile bool s_stop;
static volatile bool s_in_done;
static volatile bool s_write_failed;

/* Counters: plain adds, under a spinlock so the ui_task's copy is never
 * torn. The notice strings are under a mutex -- snprintf does not belong
 * inside a critical section. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_text_lock;
static volatile bool s_log_end;     /* heapmap_log owed, from ui_task */
static uint64_t s_frames;           /* encoded */
static uint64_t s_bytes;            /* written */
static uint64_t s_dropped_frames;
static char     s_name[40];
static char     s_path[160];

static bool s_notice;
static char s_notice_head[40];
static char s_notice_body[96];

static void notice(const char *head, const char *body)
{
    xSemaphoreTake(s_text_lock, portMAX_DELAY);
    snprintf(s_notice_head, sizeof(s_notice_head), "%s", head);
    snprintf(s_notice_body, sizeof(s_notice_body), "%s", body);
    s_notice = true;
    xSemaphoreGive(s_text_lock);
}

/* ---- the file ---- */

static bool file_write(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    if (s_write_failed || !s_file) return false;
    if (s_bytes + n > REC_MAX_BYTES) {
        ESP_LOGW(TAG, "4 GB: ending the recording");
        s_write_failed = true;
        notice("Recording stopped", "It reached 4 GB, the most one file can hold.");
        return false;
    }
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    const size_t put = fwrite(buf, 1, n, s_file);
    storage_io_release();
    if (put != n) {
        ESP_LOGE(TAG, "write failed after %" PRIu64 " bytes (errno %d)", s_bytes, errno);
        s_write_failed = true;
        notice("Recording stopped", "The card would not take any more.");
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    s_bytes += n;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

/* "<mount>/Recordings/2026-09-26 18.04.33.flac", then " (2)" and on if
 * that exists -- two presses in one second, or a clock that has not
 * moved since the last boot. */
static bool pick_path(const char *mount)
{
    char dir[96];
    if (!storage_join_path(dir, sizeof(dir), mount, REC_DIR)) return false;
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s: errno %d", dir, errno);
        return false;
    }
    /*
     * 5107: settings_now(), not time(). Nothing sets the system clock
     * (settings.h), so time() is 1970 on every boot; settings_now() is
     * the player's own belief -- the last NTP reply, or failing that the
     * build time or the card's floor, carried forward on the monotonic
     * timer. time_t is 64 bits here, so gmtime_r() does not truncate
     * what settings.c keeps in an int64_t.
     */
    _Static_assert(sizeof(time_t) == 8, "time_t must be 64-bit for settings_now()");
    const time_t now = (time_t)settings_now();
    struct tm tm;
    gmtime_r(&now, &tm);
    char stem[24];
    strftime(stem, sizeof(stem), "%Y-%m-%d %H.%M.%S", &tm);
    struct stat sb;
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(s_name, sizeof(s_name), "%s.flac", stem);
        else        snprintf(s_name, sizeof(s_name), "%s (%d).flac", stem, n);
        if (!storage_join_path(s_path, sizeof(s_path), dir, s_name)) return false;
        if (stat(s_path, &sb) != 0) return true;
    }
    return false;
}

static void finish_file(void)
{
    uint8_t si[FLACENC_STREAMINFO_BYTES];
    const bool ok = flacenc_close(s_enc, s_write_failed ? NULL : si) && !s_write_failed;
    s_enc = NULL;
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    if (ok) {
        /* The header's final STREAMINFO: sample count and frame sizes.
         * A file that never gets here still plays -- see flacenc.h. */
        if (fflush(s_file) != 0 ||
            fseek(s_file, FLACENC_STREAMINFO_OFFSET, SEEK_SET) != 0 ||
            fwrite(si, 1, sizeof(si), s_file) != sizeof(si)) {
            ESP_LOGW(TAG, "could not rewrite STREAMINFO; the file plays, length unknown");
        }
    }
    storage_io_close(s_file);
    storage_io_release();
    s_file = NULL;
}

/* ---- the tasks ---- */

static void rec_in_task(void *arg)
{
    (void)arg;
    uint32_t reads = 0;
    while (!s_stop) {
        const size_t n = audio_out_capture_read(s_in_buf, REC_IN_FRAMES, 100);
        if (!n) continue;
        reads++;
        /* All of a read or none of it: a partial send would split a
         * frame and swap left and right for the rest of the file. */
        const size_t want = n * REC_FRAME_BYTES;
        if (xStreamBufferSpacesAvailable(s_ring) >= want) {
            xStreamBufferSend(s_ring, s_in_buf, want, 0);
        } else {
            portENTER_CRITICAL(&s_mux);
            s_dropped_frames += n;
            portEXIT_CRITICAL(&s_mux);
        }
    }
    audio_out_capture_end();
    ESP_LOGI(TAG, "microphones off after %" PRIu32 " reads", reads);
    s_in_done = true;
    vTaskDelete(NULL);
}

static void rec_enc_task(void *arg)
{
    (void)arg;
    uint64_t last_drop_logged = 0;
    for (;;) {
        /* Whole frames: rec_in sends only whole reads, one writer, so
         * what is available is always a multiple of a frame. */
        const size_t got = xStreamBufferReceive(s_ring, s_enc_buf,
                                                REC_BLOCK * REC_FRAME_BYTES,
                                                pdMS_TO_TICKS(100));
        const unsigned frames = (unsigned)(got / REC_FRAME_BYTES);
        if (frames && !s_write_failed) {
            /* 5109: the beam, in place -- two channels in, one out. */
            if (s_use_beam) beam_process(s_beam, s_enc_buf, s_enc_buf, frames);
            if (!flacenc_write(s_enc, s_enc_buf, frames)) s_stop = true;
            portENTER_CRITICAL(&s_mux);
            s_frames += frames;
            portEXIT_CRITICAL(&s_mux);
        } else if (frames && s_write_failed) {
            s_stop = true;          /* drain and discard */
        }
        if (s_dropped_frames != last_drop_logged) {
            ESP_LOGW(TAG, "ring full: %" PRIu64 " ms of audio dropped so far",
                     s_dropped_frames * 1000 / AUDIO_CAPTURE_RATE);
            last_drop_logged = s_dropped_frames;
        }
        if (s_in_done && xStreamBufferIsEmpty(s_ring)) break;
    }

    finish_file();

    const uint32_t secs = (uint32_t)(s_frames / AUDIO_CAPTURE_RATE);
    ESP_LOGI(TAG, "recorded %s: %" PRIu32 " s, %" PRIu64 " bytes, %" PRIu64 " ms dropped%s",
             s_path, secs, s_bytes, s_dropped_frames * 1000 / AUDIO_CAPTURE_RATE,
             s_write_failed ? ", ended by a write failure" : "");
    if (s_use_beam) {
        const int g = beam_gain_centi(s_beam);
        ESP_LOGI(TAG, "beam: canceller adapted on %u%% of it; MIC2 matched to MIC1 by %s%d.%d dB",
                 beam_adapt_pct(s_beam), g < 0 ? "-" : "+", abs(g) / 10, abs(g) % 10);
    }
    if (!s_write_failed) {
        char body[96];
        snprintf(body, sizeof(body), "%s/%s, %" PRIu32 ":%02" PRIu32,
                 REC_DIR, s_name, secs / 60, secs % 60);
        notice("Recording saved", body);
    }
    s_log_end = true;           /* the map prints from ui_task's stack */
    s_active = false;
    vTaskDelete(NULL);
}

/* ---- the interface ---- */

static bool buffers(void)
{
    if (s_ring) return true;
    if (!s_text_lock) s_text_lock = xSemaphoreCreateMutex();
    if (!s_text_lock) return false;
    s_ring_storage = heap_caps_malloc(REC_RING_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_in_buf  = heap_caps_malloc(REC_IN_FRAMES * REC_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_enc_buf = heap_caps_malloc(REC_BLOCK * REC_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_beam    = heap_caps_malloc(sizeof(beam_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring_storage || !s_in_buf || !s_enc_buf || !s_beam) {
        /* Kept for the next try rather than freed: see the rule above. */
        ESP_LOGE(TAG, "no PSRAM for the recording buffers");
        return false;
    }
    s_ring = xStreamBufferCreateStatic(REC_RING_BYTES + 1, 1, s_ring_storage, &s_ring_struct);
    return s_ring != NULL;
}

bool recorder_start(char *why, size_t why_len)
{
#define REFUSE(...) do { if (why) snprintf(why, why_len, __VA_ARGS__); return false; } while (0)
    if (s_active) REFUSE("Already recording.");

    storage_id_t vol = STORAGE_COUNT;
    if (storage_present(STORAGE_SD)) vol = STORAGE_SD;
    else if (storage_present(STORAGE_USB)) vol = STORAGE_USB;
    if (vol == STORAGE_COUNT) REFUSE("Insert a card or a USB drive to record to.");

    if (!buffers()) REFUSE("Not enough memory to record.");
    if (!pick_path(storage_mount_path(vol))) REFUSE("Could not make the Recordings folder.");

    s_file = storage_io_open(s_path, "wb");
    if (!s_file) REFUSE("Could not create %s.", s_name);

    s_frames = 0; s_bytes = 0; s_dropped_frames = 0;
    s_stop = false; s_in_done = false; s_write_failed = false;
    /* 5109: the beam is mono; stereo is the microphones as they are. */
    s_use_beam = !settings_mic_stereo();
    if (s_use_beam) beam_init(s_beam);
    s_enc = flacenc_open(s_use_beam ? 1 : AUDIO_CAPTURE_CHANNELS, AUDIO_CAPTURE_BITS,
                         AUDIO_CAPTURE_RATE, REC_BLOCK, file_write, NULL);
    if (!s_enc) {
        storage_io_close(s_file);
        s_file = NULL;
        remove(s_path);
        REFUSE("Could not start the file.");
    }

    const esp_err_t err = audio_out_capture_begin();
    if (err != ESP_OK) {
        flacenc_close(s_enc, NULL);
        s_enc = NULL;
        storage_io_close(s_file);
        s_file = NULL;
        remove(s_path);
        REFUSE("The microphones did not start (%s).", esp_err_to_name(err));
    }

    xStreamBufferReset(s_ring);
    s_active = true;
    if (xTaskCreate(rec_enc_task, "rec_enc", REC_ENC_STACK, NULL, REC_ENC_PRIO, NULL) != pdPASS) {
        audio_out_capture_end();
        flacenc_close(s_enc, NULL);
        s_enc = NULL;
        storage_io_close(s_file);
        s_file = NULL;
        remove(s_path);
        s_active = false;
        REFUSE("No memory for the recording task.");
    }
    if (xTaskCreate(rec_in_task, "rec_in", REC_IN_STACK, NULL, REC_IN_PRIO, NULL) != pdPASS) {
        /* rec_enc is running: let it close the (empty) file. */
        audio_out_capture_end();
        s_write_failed = true;
        s_stop = true;
        s_in_done = true;
        REFUSE("No memory for the recording task.");
    }
    ESP_LOGI(TAG, "recording to %s (%s)", s_path,
             s_use_beam ? "beam, mono" : "stereo, MIC1 left");
    heapmap_log("recording started");
    return true;
#undef REFUSE
}

void recorder_stop(void)
{
    if (s_active && !s_stop) {
        ESP_LOGI(TAG, "stop asked");
        s_stop = true;
    }
}

bool recorder_active(void) { return s_active; }

void recorder_status(recorder_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->stopping = s_active && s_stop;
    portENTER_CRITICAL(&s_mux);
    out->seconds = (uint32_t)(s_frames / AUDIO_CAPTURE_RATE);
    out->bytes = s_bytes;
    out->dropped_ms = (uint32_t)(s_dropped_frames * 1000 / AUDIO_CAPTURE_RATE);
    portEXIT_CRITICAL(&s_mux);
    snprintf(out->name, sizeof(out->name), "%s", s_name);
}

bool recorder_take_notice(char *head, size_t head_len, char *body, size_t body_len)
{
    if (s_log_end) {
        s_log_end = false;
        heapmap_log("recording stopped");
    }
    if (!s_text_lock || !s_notice) return false;
    xSemaphoreTake(s_text_lock, portMAX_DELAY);
    const bool had = s_notice;
    if (had) {
        snprintf(head, head_len, "%s", s_notice_head);
        snprintf(body, body_len, "%s", s_notice_body);
        s_notice = false;
    }
    xSemaphoreGive(s_text_lock);
    return had;
}
