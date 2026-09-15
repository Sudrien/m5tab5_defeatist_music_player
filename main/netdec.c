/*
 * netdec.c -- see netdec.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "netdec.h"

#include <inttypes.h>
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

#include "decoder.h"            /* decoder_register_codecs() */
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

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

/*
 * The sniff reached a verdict and the verdict was "not audio".
 *
 * Separate from s_codec because STREAM_CODEC_NONE means two things and
 * the enum cannot tell them apart -- exactly the shape 0203 found in
 * NETSTREAM_IDLE and 0209 found in stations_load()'s return. NONE is
 * "not identified yet" before the sniff and "identified, and it is a web
 * page" after it, and identify() wrote NONE for the second while its own
 * first line uses NONE as "keep trying".
 *
 * So the verdict was re-reached on every call. On the board a BBC URL
 * that 302s to a HTML page produced `The station returned a web page`
 * at the decode loop's rate -- about 1300 lines in thirteen seconds,
 * which is the entire log of that run -- and the stream never ended,
 * because netdec_read() answered 0 for "wait" to a condition that is
 * never going to change.
 */
static bool s_not_audio;

/*
 * One esp_audio_codec handle, opened for whichever of the two codecs it
 * serves. AAC and Ogg differ here only in `dec_type`: both are
 * parser-framed, both take arbitrary input lengths, and both report
 * their format the same way. Naming it for AAC, which is what it was
 * called until 0500, is how the second caller ends up as a copy.
 */
static esp_audio_simple_dec_handle_t s_esp;

static uint32_t s_frames;
static uint64_t s_samples;

/*
 * Compressed bytes in against decoded samples out, for the one figure
 * that says what a stream COSTS.
 *
 * 0414 divided the reader's throughput by the ring's drain rate and got
 * 100% on every line of a board log, which is a tautology rather than a
 * measurement: the compressed ring sits at 0%, so whatever arrives is
 * immediately taken, and delivery equals consumption by construction.
 * It said nothing about whether the station fits.
 *
 * The real question is bytes per SECOND OF AUDIO -- the rate the stream
 * has to be delivered at to play in real time -- and that is the one
 * ratio the decoder alone can see, because only it knows both halves.
 * It needs no header, works for AAC where nothing is declared, and is
 * correct for VBR where every declared figure is nominal.
 */
/*
 * AND THE BYTES ARE THE ONES THE DECODER TOOK, NOT THE ONES THE WINDOW
 * ACCEPTED -- corrected in 0502.
 *
 * This counted at refill(), which is bytes entering the WINDOW. The
 * window is not the decoder: bytes sit in it until a frame is complete,
 * so a reading covers one second of samples against whatever arrived
 * during it, and those two are separated by however much audio the
 * window holds.
 *
 * At 24 KB that was about a second at broadcast rates and the error
 * stayed inside a reading; WNZK and SomaFM both measured correctly in
 * 0416. 0500 made the window 160 KB to hold an Ogg page, which is 6.6
 * seconds of 192 kbit/s audio, and the same code then reported a
 * healthy Opus station as needing between 16 and 606 kbit/s -- `of 16
 * needed (1381%)` on one line and `of 606 needed (34%)` on another,
 * with a 16 second reserve and no drops throughout. Two lines flagged
 * SHORT while the reserve was climbing.
 *
 * `framewin_t` already keeps a running total of bytes consumed, which
 * is the exact quantity wanted: the compressed bytes that became these
 * samples. Taking the delta of that against the samples decoded from
 * them makes the ratio a property of the stream again, whatever size
 * the window is. Bytes dropped by framewin_reset() on a reconnect are
 * in neither total, which is right -- they produced no audio.
 */
static uint64_t s_cost_out_mark;    /* s_win.out at the last report */
static uint64_t s_cost_samples;
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
     * The minimp3 call measures 17744 bytes of the caller's stack, so a
     * task that is comfortable everywhere else dies on the first frame.
     * Checked here rather than left to a panic: the panic names
     * mp3dec_decode_frame and a line number inside a vendored header,
     * which points at the library rather than at the caller that is
     * actually wrong.
     *
     * NOT uxTaskGetStackHighWaterMark(), WHICH IS THE WORST THE TASK HAS
     * EVER BEEN AND NOT WHAT IS LEFT NOW.
     *
     * That function returns the smallest free stack recorded since the
     * task started. On the main task the smallest ever recorded is set
     * by the deepest thing that has run on it -- which is
     * mp3dec_decode_frame(), from play_file(), putting its 17744 bytes
     * there. So:
     *
     *   boot, chooser open, no file played yet   passes, stream plays
     *   after one file has played                2068, refused for ever
     *
     * and the refusal is permanent, because a high-water mark only ever
     * goes down. On the board that was every station after the first
     * file, with a message naming a stack size that was already correct.
     *
     * **2068 is the proof the check was wrong.** A task reporting 2068
     * bytes of worst-ever free has already SURVIVED a 17744-byte
     * minimp3 call -- that call is what put the figure there. The
     * measurement was reading the record of the demand being met as
     * evidence that it could not be.
     *
     * It is the same error as the one at the top of CLAUDE.md, from the
     * other direction: a stack protection fault reports the overshoot
     * rather than the demand, and a high-water mark reports the past
     * rather than the present. Neither is the headroom.
     *
     * What headroom actually is: the distance from this frame down to
     * the bottom of the task's stack. pxTaskGetStackStart() gives the
     * bottom, `&probe` is in this frame, and the stack grows down on
     * both the P4's cores, so the difference is what a callee may still
     * use. Measured, current, and unaffected by anything that has
     * already returned.
     */
    char probe;
    const uint8_t *floor = (const uint8_t *)pxTaskGetStackStart(NULL);
    const uint8_t *here  = (const uint8_t *)&probe;
    const unsigned headroom = (here > floor) ? (unsigned)(here - floor) : 0;
    if (headroom < NETDEC_STACK_FLOOR) {
        ESP_LOGE(TAG, "this task has %u bytes of stack below this frame; the "
                      "minimp3 call measures 17744 on the caller's stack. "
                      "Create the calling task with at least %d (the main "
                      "task, which decodes files through the same library, "
                      "has 24576).",
                 headroom, NETDEC_MIN_STACK);
        free(s_win_buf);
        s_win_buf = NULL;
        s_mp3 = NULL;
        return false;
    }

    s_codec = STREAM_CODEC_NONE;
    s_not_audio = false;
    s_cost_out_mark = 0;
    s_cost_samples = 0;
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
    if (s_esp) {
        esp_audio_simple_dec_close(s_esp);
        s_esp = NULL;
    }
    free(s_win_buf);
    s_win_buf = NULL;
    s_mp3 = NULL;
    s_open = false;
    s_codec = STREAM_CODEC_NONE;
    s_not_audio = false;
}

void netdec_reconnect(void)
{
    if (!s_open) return;
    /* The window holds bytes from a body that has ended; the new one
     * starts at its own frame boundary. The codec and the counters are
     * the station's, not the connection's, and survive. */
    framewin_reset(&s_win);
    mp3dec_init(s_mp3);
    if (s_esp) {
        /* Closed and reopened rather than carried over: the decoder
         * holds parser state for a body that has ended, and an ADTS
         * stream resumed mid-frame is the one thing its parser cannot
         * be told about. Cheap, and it happens once per drop.
         *
         * Ogg needs it more, not less. The Opus and Vorbis headers are
         * the first pages of a body, and a reconnected body has its own
         * -- Icecast sends them to every client at the top of the
         * stream. A decoder kept across the drop would be handed a
         * second OggS header page while already configured. */
        esp_audio_simple_dec_close(s_esp);
        s_esp = NULL;
    }
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
    /* And once the answer is "never". See s_not_audio: without this the
     * sniff is redone, and re-announced, for as long as the caller keeps
     * asking. */
    if (s_not_audio) return;
    const size_t have = framewin_avail(&s_win);
    if (have < CODECPLAN_MIN_SNIFF_BYTES) return;

    const codecplan_t plan =
        codecplan_choose(sniff_bytes(framewin_data(&s_win), have), have, NULL);
    if (codecplan_ready(&plan)) {
        s_codec = plan.codec;
        ESP_LOGI(TAG, "stream is %s", stream_codec_name(s_codec));
    } else if (!codecplan_waiting(&plan)) {
        /*
         * A verdict, not a delay. codecplan_waiting() is what separates
         * "need more bytes" from "these bytes are not a stream", and
         * this branch is the second -- a playlist, a web page, or a
         * codec not decoded here. None of those becomes audio if asked
         * again.
         *
         * Latched and said once. The message is the listener's, and it
         * is the same message whether it is printed once or a thousand
         * times.
         */
        ESP_LOGW(TAG, "%s", plan.message);
        s_codec = STREAM_CODEC_NONE;
        s_not_audio = true;
    }
}

/*
 * Open the esp_audio_codec decoder for whichever codec was identified.
 * Deferred until then, so an MP3 stream never pays for it.
 *
 * _AAC and _OGG are the two members of that component reachable from a
 * sliding window. Both let the decoder's own parser find boundaries in
 * whatever it is shown, which is the property that matters here and the
 * only thing this function has to know about either of them.
 */
static bool esp_dec_open(void)
{
    if (s_esp) return true;

    /* One owner for the registration flag; see decoder.h. */
    decoder_register_codecs();

    esp_audio_simple_dec_cfg_t cfg = {
        /*
         * _OGG is the CONTAINER parser and takes arbitrary input
         * lengths. _RAW_OPUS and _VORBIS are the bare codecs and want
         * exactly one encoded frame per call, which a window cannot
         * promise -- decoder.c routes .ogg and .opus files through _OGG
         * for that same reason, and a broadcast Opus stream is
         * Ogg-encapsulated in any case.
         */
        .dec_type = (s_codec == STREAM_CODEC_OGG)
                        ? ESP_AUDIO_SIMPLE_DEC_TYPE_OGG
                        : ESP_AUDIO_SIMPLE_DEC_TYPE_AAC,
        .dec_cfg = NULL,
        .cfg_size = 0,
        /*
         * false: the decoder's own parser finds frame or page boundaries
         * in whatever the window hands it. true would mean "this buffer
         * is exactly one frame", which a sliding window cannot promise
         * -- and which is why _ALAC, _VORBIS, _RAW_OPUS, _ADPCM and
         * _LC3 are not reachable from here at all.
         */
        .use_frame_dec = false,
    };
    /*
     * Internal free either side of the open, because this decoder is not
     * ours and allocates where it likes. The first AAC run bottomed at
     * 40268 bytes of internal RAM against the MP3 run's 56084 -- about
     * 15.8 KB -- and 0115's transport starvation happened on a run whose
     * minimum was 43560. That is close enough that the cost wants
     * attributing to a line in the log rather than inferred by
     * subtracting two runs.
     *
     * Opus has not been measured at all and the same line will say so
     * on the first run: it carries a resampler and a 48 kHz frame, so
     * there is no reason to expect the AAC figure to transfer.
     */
    const unsigned before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (esp_audio_simple_dec_open(&cfg, &s_esp) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_simple_dec_open(%s) failed",
                 stream_codec_name(s_codec));
        s_esp = NULL;
        return false;
    }
    const unsigned after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "%s decoder open (parser-framed); internal free "
                  "%u -> %u (cost %d)", stream_codec_name(s_codec),
             before, after, (int)before - (int)after);
    return true;
}

/* One frame out of the window. Same contract as netdec_read(). */
/*
 * Publish what the stream costs, about once per second of audio.
 *
 * On a DECODED-AUDIO clock rather than a wall clock, so a decoder
 * running ahead to fill the ring reports the same figure as one pacing
 * in real time. The number is a property of the stream and must not
 * change with how far ahead this player happens to be -- which is
 * exactly the trap 0414 fell into.
 *
 * The samples reset and the byte mark advances, so each reading is its
 * own second: a bitrate that changes mid-stream is followed rather than
 * averaged away across a session.
 */
static void cost_report(uint32_t rate)
{
    if (!rate || s_cost_samples < rate) return;     /* under a second */

    const uint64_t taken = s_win.out - s_cost_out_mark;
    const uint64_t kbps = (taken * 8ull * (uint64_t)rate)
                        / (s_cost_samples * 1000ull);
    s_cost_out_mark = s_win.out;
    s_cost_samples = 0;
    if (kbps > 0 && kbps < 100000) netstream_set_actual_kbps((int)kbps);
}

static int esp_read(int16_t *out, int max_int16, netdec_info_t *info)
{
    if (!esp_dec_open()) return -1;
    if (framewin_avail(&s_win) == 0) return 0;

    esp_audio_simple_dec_raw_t raw = {
        .buffer = (uint8_t *)framewin_data(&s_win),
        .len = (uint32_t)framewin_avail(&s_win),
        /* Never true: a live stream has no end to signal, and claiming
         * one would make the decoder flush and stop. */
        .eos = false,
    };
    esp_audio_simple_dec_out_t frame = {
        .buffer = (uint8_t *)out,
        .len = (uint32_t)((uint32_t)max_int16 * sizeof(int16_t)),
    };

    const esp_audio_err_t err =
        esp_audio_simple_dec_process(s_esp, &raw, &frame);

    /* Consumed first, and whatever the error: the decoder has moved past
     * those bytes and showing them again would decode them twice. */
    if (raw.consumed && !framewin_consume(&s_win, (size_t)raw.consumed)) {
        ESP_LOGE(TAG, "%s consumed %u of %u shown", stream_codec_name(s_codec),
                 (unsigned)raw.consumed, (unsigned)framewin_avail(&s_win));
        return -1;
    }

    if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        /* NETDEC_MAX_INT16 is sized for HE-AAC's doubled frame, which
         * also clears Vorbis's 8192-sample block and Opus's 120 ms
         * frame; a stream needing more than that has a block size
         * nothing here budgeted for, and truncating it would be silent
         * damage. */
        ESP_LOGE(TAG, "%s frame needs %u bytes, buffer is %u",
                 stream_codec_name(s_codec), (unsigned)frame.needed_size,
                 (unsigned)(max_int16 * sizeof(int16_t)));
        return -1;
    }
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "%s decode error %d", stream_codec_name(s_codec),
                 (int)err);
        return -1;
    }
    if (frame.decoded_size == 0) {
        /* Consumed without producing: a partial frame, or the parser
         * stepping over something. Not an error and not end of stream. */
        if (raw.consumed == 0 && framewin_stuck(&s_win)) {
            framewin_skip_byte(&s_win);
            s_resyncs++;
        }
        return 0;
    }

    /* Format is only valid once decoded_size is non-zero, which is why
     * it is asked here rather than at open. */
    esp_audio_simple_dec_info_t fi;
    memset(&fi, 0, sizeof(fi));
    if (esp_audio_simple_dec_get_info(s_esp, &fi) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "%s decoded %u bytes but reports no format",
                 stream_codec_name(s_codec), (unsigned)frame.decoded_size);
        return -1;
    }
    if (fi.bits_per_sample != 16) {
        /* The file path folds 24-bit and refuses 32. No broadcast AAC
         * or Opus is either -- both decode to 16 -- so this refuses
         * both rather than carrying that machinery into a path that
         * would never exercise it. */
        ESP_LOGE(TAG, "%s is %d-bit; only 16 is handled here",
                 stream_codec_name(s_codec), fi.bits_per_sample);
        return -1;
    }
    if (fi.channel < 1 || fi.channel > 2 || fi.sample_rate <= 0) {
        ESP_LOGE(TAG, "%s reports %d channels at %" PRIu32 " Hz",
                 stream_codec_name(s_codec), fi.channel,
                 (uint32_t)fi.sample_rate);
        return -1;
    }

    const int produced = (int)(frame.decoded_size / sizeof(int16_t));
    s_frames++;
    s_samples += (uint64_t)(produced / fi.channel);
    s_cost_samples += (uint64_t)(produced / fi.channel);
    cost_report((uint32_t)fi.sample_rate);

    if (info) {
        info->sample_rate = (int)fi.sample_rate;
        info->channels = fi.channel;
        info->bitrate_kbps = 0;     /* the simple decoder does not say */
        info->codec = s_codec;
    }
    if (!s_reported) {
        s_reported = true;
        ESP_LOGI(TAG, "first frame: %s, %" PRIu32 " Hz, %d ch, %d-bit, "
                      "%u bytes -> %d samples", stream_codec_name(s_codec),
                 (uint32_t)fi.sample_rate, fi.channel, fi.bits_per_sample,
                 (unsigned)raw.consumed, produced / fi.channel);
    }
    return produced;
}

int netdec_read(int16_t *out, int max_int16, netdec_info_t *info)
{
    if (!s_open || !out || max_int16 < NETDEC_MAX_INT16) return -1;

    refill();
    identify();

    if (s_not_audio) {
        /*
         * THE OLD COMMENT HERE WAS WRONG, AND THIS IS THE HALF IT GOT
         * WRONG. It said that sniffing and not-audio were the same to
         * the caller and that both mean wait. They are not: one ends,
         * and the other is a socket held open to a web server, feeding
         * a decoder that will never produce a sample, until somebody
         * presses something. On the board that was thirteen seconds and
         * only because the listener intervened.
         *
         * -1 rather than 0, which is the signal the unsupported-codec
         * branch below already uses and which bufferplan reads as the
         * source being finished. play_stream() then tears down and says
         * why, and streamplan_status() has "No signal" for the screen.
         */
        return -1;
    }
    if (s_codec == STREAM_CODEC_NONE) {
        /* Still sniffing: too few bytes to judge. This one really does
         * mean wait. */
        return 0;
    }
    if (s_codec == STREAM_CODEC_AAC_ADTS || s_codec == STREAM_CODEC_OGG) {
        return esp_read(out, max_int16, info);
    }
    if (s_codec != STREAM_CODEC_MP3) {
        ESP_LOGE(TAG, "%s is not decoded here", stream_codec_name(s_codec));
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
    s_cost_samples += (uint64_t)samples;
    cost_report((uint32_t)fi.hz);

    if (info) {
        info->sample_rate = fi.hz;
        info->channels = fi.channels;
        info->bitrate_kbps = fi.bitrate_kbps;
        info->codec = s_codec;
    }

    if (!s_reported) {
        s_reported = true;
        /* A first figure straight away, so the very first statistics
         * line has a denominator; cost_report() replaces it with a
         * measured one a second later, and keeps replacing it. */
        netstream_set_actual_kbps(fi.bitrate_kbps);
        ESP_LOGI(TAG, "first frame: MPEG layer %d, %d Hz, %d ch, %d kbit/s, "
                      "%d bytes -> %d samples",
                 fi.layer, fi.hz, fi.channels, fi.bitrate_kbps,
                 fi.frame_bytes, samples);
    }
    return produced;
}
