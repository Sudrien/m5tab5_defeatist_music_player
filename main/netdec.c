/*
 * netdec.c -- see netdec.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "netdec.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Declarations only. MINIMP3_IMPLEMENTATION is defined in decoder.c and
 * must not be defined twice -- the implementation is emitted there and
 * this links against it. minimp3_prefix.h has to come first, exactly as
 * in decoder.c: it renames our copy to tab5_mp3dec_* so it stops
 * colliding with the identical symbols already inside
 * libesp_audio_codec.a.
 */
#include "minimp3_prefix.h"
#include "minimp3.h"

#include "framewin.h"
#include "netstream.h"
#include "streamsniff.h"

static const char *TAG = "tab5_netdec";

/*
 * minimp3's own bound and ours must agree. If a minimp3 update ever
 * widens it, the write into the caller's buffer below would overrun by
 * however much it grew, silently.
 */
_Static_assert(NETDEC_MAX_INT16 >= MINIMP3_MAX_SAMPLES_PER_FRAME,
               "NETDEC_MAX_INT16 is smaller than a minimp3 frame");

/*
 * How long a read waits on the ring. Short, because returning 0 is a
 * normal answer the caller is expected to handle, and a caller that also
 * wants to notice a stop should not be held here for long. Phase 1's
 * ring holds seconds; this timeout only governs how often the caller
 * gets the chance to do something else.
 */
#define RING_WAIT_MS        (100)

/*
 * Bytes to pull per refill. Matches netstream's own read chunk, so a
 * full ring drains at the rate it fills.
 */
#define REFILL_BYTES        (2048)

/* One stream, one decoder; module scope for the same reason netstream's
 * working set is, and in PSRAM for the reason 0115 exists. */
static framewin_t   s_win;
static uint8_t     *s_win_buf;
static mp3dec_t    *s_mp3;
static bool         s_open;
static stream_codec_t s_codec;

static uint32_t s_frames;
static uint64_t s_samples;
static uint32_t s_resyncs;
static bool     s_reported;     /* the format line has been logged */

bool netdec_open(void)
{
    netdec_close();

    /* One block: a partial failure cannot leave half of it live. */
    const size_t need = FRAMEWIN_BYTES + sizeof(mp3dec_t);
    uint8_t *w = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!w) {
        ESP_LOGE(TAG, "no PSRAM for a %u byte decode working set",
                 (unsigned)need);
        return false;
    }
    s_win_buf = w;
    s_mp3 = (mp3dec_t *)(w + FRAMEWIN_BYTES);

    framewin_init(&s_win, s_win_buf, FRAMEWIN_BYTES);
    mp3dec_init(s_mp3);

    /*
     * minimp3's scratch is about 11.6 KB on the caller's stack, so a
     * task that is comfortable everywhere else dies on the first frame.
     * Checked here rather than left to a panic: the panic names
     * mp3dec_decode_frame and a line number inside a vendored header,
     * which points at the library rather than at the caller that is
     * actually wrong.
     */
    const unsigned headroom = uxTaskGetStackHighWaterMark(NULL);
    if (headroom < NETDEC_STACK_FLOOR) {
        ESP_LOGE(TAG, "this task has %u bytes of stack left; minimp3 needs "
                      "about 11600 on the caller's stack. Create the calling "
                      "task with at least %d (media_task uses the same for "
                      "the file path).",
                 headroom, NETDEC_MIN_STACK);
        free(s_win_buf);
        s_win_buf = NULL;
        s_mp3 = NULL;
        return false;
    }

    s_codec = STREAM_CODEC_NONE;
    s_frames = 0;
    s_samples = 0;
    s_resyncs = 0;
    s_reported = false;
    s_open = true;

    ESP_LOGI(TAG, "ready: %u byte window + %u byte decoder in PSRAM",
             (unsigned)FRAMEWIN_BYTES, (unsigned)sizeof(mp3dec_t));
    return true;
}

void netdec_close(void)
{
    if (!s_open) return;
    ESP_LOGI(TAG, "closing after %u frames, %llu samples, %u bytes resynced",
             s_frames, (unsigned long long)s_samples, s_resyncs);
    free(s_win_buf);
    s_win_buf = NULL;
    s_mp3 = NULL;
    s_open = false;
    s_codec = STREAM_CODEC_NONE;
}

void netdec_reconnect(void)
{
    if (!s_open) return;
    /* The window holds bytes from a body that has ended; the new one
     * starts at its own frame boundary. The codec and the counters are
     * the station's, not the connection's, and survive. */
    framewin_reset(&s_win);
    mp3dec_init(s_mp3);
    ESP_LOGI(TAG, "reconnect: window dropped, %s kept",
             stream_codec_name(s_codec));
}

stream_codec_t netdec_codec(void) { return s_codec; }
uint32_t netdec_frames(void)      { return s_frames; }
uint64_t netdec_samples(void)     { return s_samples; }
uint32_t netdec_resyncs(void)     { return s_resyncs; }

/* Pull from the ring into the window. Returns bytes added. */
static size_t refill(void)
{
    const size_t room = framewin_space(&s_win);
    if (!room) return 0;
    const size_t want = room < REFILL_BYTES ? room : REFILL_BYTES;
    const size_t got = netstream_read(framewin_tail(&s_win), want, RING_WAIT_MS);
    if (got && !framewin_commit(&s_win, got)) {
        /* Cannot happen: got <= want <= room. Checked because believing
         * a bad length here is how the window loses its place. */
        ESP_LOGE(TAG, "refill of %u would overrun the window", (unsigned)got);
        return 0;
    }
    if (!got) framewin_commit(&s_win, 0);    /* counts a dry refill */
    return got;
}

/*
 * Identify the codec from the first bytes, once.
 *
 * codecplan.h is the home for this decision and 0119 is the reason it is
 * called rather than reimplemented: the probe decided per window from
 * whichever frame counter happened to be non-zero, picked ADTS on an MP3
 * stream, and wrapped a clock. Decided once, then kept.
 */
static void identify(void)
{
    if (s_codec != STREAM_CODEC_NONE) return;
    const size_t have = framewin_avail(&s_win);
    if (have < CODECPLAN_MIN_SNIFF_BYTES) return;

    const codecplan_t plan =
        codecplan_choose(sniff_bytes(framewin_data(&s_win), have), have, NULL);
    if (codecplan_ready(&plan)) {
        s_codec = plan.codec;
        ESP_LOGI(TAG, "stream is %s", stream_codec_name(s_codec));
    } else if (!codecplan_waiting(&plan)) {
        ESP_LOGW(TAG, "%s", plan.message);
        s_codec = STREAM_CODEC_NONE;
    }
}

int netdec_read(int16_t *out, int max_int16, netdec_info_t *info)
{
    if (!s_open || !out || max_int16 < NETDEC_MAX_INT16) return -1;

    refill();
    identify();

    if (s_codec == STREAM_CODEC_NONE) {
        /* Still sniffing, or the stream is not audio. codecplan has
         * already said which in the log; either way there is nothing to
         * decode yet and the caller should wait rather than stop. */
        return 0;
    }
    if (s_codec != STREAM_CODEC_MP3) {
        /* AAC is the next patch. Refused rather than half-attempted:
         * handing ADTS to minimp3 produces noise, not an error, and
         * noise from a live stream is hard to tell from a bad link. */
        ESP_LOGE(TAG, "%s is not decoded yet; MP3 only for now",
                 stream_codec_name(s_codec));
        return -1;
    }

    /*
     * One frame. mp3dec_decode_frame() reports two things separately and
     * they are not the same: `frame_bytes` is how much of the buffer it
     * consumed, and the return value is how many samples per channel it
     * produced. Those differ for an ID3 tag or junk before the first
     * sync -- bytes consumed, no samples -- which is a resync and not an
     * error.
     */
    mp3dec_frame_info_t fi;
    memset(&fi, 0, sizeof(fi));
    const int samples = mp3dec_decode_frame(s_mp3, framewin_data(&s_win),
                                            (int)framewin_avail(&s_win),
                                            out, &fi);

    if (fi.frame_bytes <= 0) {
        /*
         * Nothing usable in what we have. Normally that means a frame is
         * still arriving. If the window is full and this keeps happening
         * the stream is not frames at all, and dropping a byte is what
         * lets the decoder find a real sync instead of spinning on a
         * false one.
         */
        if (framewin_stuck(&s_win)) {
            framewin_skip_byte(&s_win);
            s_resyncs++;
            if (s_resyncs == 1 || s_resyncs % 4096 == 0) {
                ESP_LOGW(TAG, "no frame in a full window; %u bytes skipped",
                         s_resyncs);
            }
        }
        return 0;
    }

    if (!framewin_consume(&s_win, (size_t)fi.frame_bytes)) {
        /* The decoder reported consuming more than it was shown. Not
         * survivable: the window's position would be a guess from here
         * on, and a wrong position is the click-every-few-seconds bug. */
        ESP_LOGE(TAG, "decoder consumed %d of %u bytes shown",
                 fi.frame_bytes, (unsigned)framewin_avail(&s_win));
        return -1;
    }

    if (samples <= 0) {
        /* Consumed, produced nothing: a tag or junk, skipped cleanly. */
        s_resyncs += (uint32_t)fi.frame_bytes;
        return 0;
    }

    if (fi.channels < 1 || fi.channels > 2 || fi.hz <= 0) {
        ESP_LOGE(TAG, "frame claims %d channels at %d Hz", fi.channels, fi.hz);
        return -1;
    }

    const int produced = samples * fi.channels;
    if (produced > max_int16) {
        /* The _Static_assert above should make this impossible. It is
         * checked anyway because the alternative is writing past the
         * caller's buffer. */
        ESP_LOGE(TAG, "frame of %d int16 into a buffer of %d", produced, max_int16);
        return -1;
    }

    s_frames++;
    s_samples += (uint64_t)samples;

    if (info) {
        info->sample_rate = fi.hz;
        info->channels = fi.channels;
        info->bitrate_kbps = fi.bitrate_kbps;
        info->codec = s_codec;
    }

    if (!s_reported) {
        s_reported = true;
        ESP_LOGI(TAG, "first frame: MPEG layer %d, %d Hz, %d ch, %d kbit/s, "
                      "%d bytes -> %d samples",
                 fi.layer, fi.hz, fi.channels, fi.bitrate_kbps,
                 fi.frame_bytes, samples);
    }
    return produced;
}
