/*
 * netdec.h -- decode a live stream out of netstream's ring.
 *
 * Phase 2. Phase 1 put compressed bytes in a ring; this turns them into
 * PCM. It is deliberately not `decoder.c`, and the reason is worth
 * stating because the two look like they should be one thing.
 *
 * WHY NOT decoder.c
 *
 * `decoder.c` is built around a file: it opens a path, builds or loads a
 * seek index, answers `decoder_duration_sec()` and `decoder_seek_sec()`,
 * and its MP3 backend is `mp3dec_ex`, which wants a seekable source and
 * an index over the whole stream. **A live stream has no path, no
 * length, no index and no seek.** Threading a stream through that
 * facade would mean teaching every one of those to say "not applicable",
 * and the result would be a file decoder with a stream-shaped hole in
 * it.
 *
 * What is shared is the decoder itself. minimp3 has two doors:
 * `mp3dec_ex_*`, the indexed file API that `decoder.c` uses, and
 * `mp3dec_decode_frame()`, which takes a buffer and returns how many
 * bytes of it were a frame. The second is already a stream interface.
 * Same library, same vendored copy, same renamed symbols -- entered
 * differently.
 *
 * (For the avoidance of doubt: none of this is a hardware limitation.
 * The P4 has no audio decoder in silicon; every format this player
 * handles, files included, is decoded in software.)
 *
 * WHAT IS HERE AND WHAT IS NOT
 *
 * MP3 through minimp3 and ADTS AAC through `esp_audio_simple_dec` --
 * the same two backends, and the same division of labour, as the file
 * path. MP3 landed first, against the order in the plan, because the
 * benchmark station had become WUOM and the path that can be watched on
 * hardware is worth more than the one written down first.
 *
 * AAC is opened with `use_frame_dec = false`, which lets the decoder's
 * own parser find ADTS boundaries in whatever the window hands it. That
 * is the same setting the file path uses for a file read as a stream,
 * and it is the reason `_AAC` works here when `_ALAC`, `_VORBIS`,
 * `_RAW_OPUS`, `_ADPCM` and `_LC3` could not: those require exactly one
 * encoded frame per call, and a sliding window cannot promise that.
 *
 * No seeking, no duration, no pause. A live stream has no position to
 * hold; phase 3's pause is a stop and a fresh connection.
 *
 * THE CALLER'S STACK MUST BE 16 KB
 *
 * **`mp3dec_decode_frame()` puts its scratch buffers on the caller's
 * stack, and the whole call needs about 17.8 KB.** That is minimp3's
 * design; there is no option to heap them.
 *
 * The number is measured, from two panics that agree to the byte:
 *
 *     stack  6140 bytes, SP 11604 below the floor -> 17744 used
 *     stack 16380 bytes, SP  1364 below the floor -> 17744 used
 *
 * Both runs died at the same instruction having consumed exactly the
 * same amount, which is what a fixed-size scratch on a deterministic
 * path looks like. **The first fix read 11604 as the requirement rather
 * than as the overshoot, raised the stack to 16384, and panicked
 * again** -- so the figure to size against is the total, and the first
 * panic never showed it.
 *
 * The file path has always known. `play_file()` calls `decoder_read()`
 * and therefore minimp3, and it runs on the **main task**, which
 * `sdkconfig` gives `CONFIG_ESP_MAIN_TASK_STACK_SIZE=24576`. Not
 * `media_task`'s 16384, which is what the first fix cited and which
 * would have failed identically. 24576 leaves about 6.8 KB over the
 * measured demand.
 *
 * `netdec_open()` checks the headroom of whatever task calls it and
 * refuses if it is short.
 *
 * This is the second stack fault in the stream path and the opposite of
 * the first. 0112 was a 4392-byte struct of *ours* on a task stack, and
 * the fix was to move it. Here nothing of ours is on the stack at all --
 * the window and the decoder state are both in PSRAM -- and the space is
 * consumed entirely inside a library. Only the caller can fix it, and
 * only if it is told.
 *
 * THREADING
 *
 * One caller, which is whatever is driving the writer. `netdec_read()`
 * blocks for up to its timeout waiting on the ring and may be slow; it
 * must not be called from ui_task. There is exactly one stream and
 * exactly one decoder, so the state is module-scope, as netstream's is
 * and for the same reason.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codecplan.h"          /* stream_codec_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Largest number of int16 one netdec_read() can produce.
 *
 * MP3 alone would be 1152 * 2: an MPEG1 Layer II frame is 1152 samples
 * and stereo doubles it, which is also minimp3's own
 * MINIMP3_MAX_SAMPLES_PER_FRAME (netdec.c asserts they agree rather than
 * trusting it).
 *
 * **AAC needs far more.** AAC-LC is 1024 samples a frame, but HE-AAC
 * doubles the output rate through SBR, so a stereo frame is 2048 * 2 =
 * 4096 int16 -- and WNZK, the AAC station this was written for, announces
 * `audio/aacp` with a 48 kHz core, which is exactly that case. An
 * MP3-sized buffer would have been overrun by the first AAC frame.
 *
 * So this matches DECODER_MAX_INT16, which the file path already sized
 * for the worst case across both backends. One number, sized once, for
 * a decoder that is the same decoder.
 */
#define NETDEC_MAX_INT16    (9216 * 2)

/*
 * Stack a task must have to call netdec_read(). See above: the call
 * measures 17744 bytes, and 24576 is what the main task -- which decodes
 * files through the same library -- is given by sdkconfig.
 */
#define NETDEC_MIN_STACK    (24576)

/*
 * Free stack netdec_open() insists on seeing. Above the measured 17744
 * so that a task which merely *looks* generous still gets refused: 16384
 * did, and it took a second panic to find out.
 */
#define NETDEC_STACK_FLOOR  (20000)

typedef struct {
    int sample_rate;        /* Hz; can change mid-stream, so compare */
    int channels;           /* 1 or 2 */
    int bitrate_kbps;       /* 0 until a frame has been decoded */
    stream_codec_t codec;
} netdec_info_t;

/*
 * Allocate and reset. Safe to call again; a second call closes the
 * first. The working buffers are PSRAM, for the reason 0115 exists:
 * internal RAM is what the Wi-Fi transport needs for DMA, and this path
 * has already starved it once.
 */
/*
 * MUST be called from the task that will call netdec_read(): it measures
 * that task's remaining stack against NETDEC_STACK_FLOOR, which is the
 * only check available for a requirement a library imposes on a caller.
 */
bool netdec_open(void);

void netdec_close(void);

/*
 * Which codec was identified, or STREAM_CODEC_NONE before enough bytes
 * have arrived. Decided once from the first bytes by codecplan_choose()
 * and then kept -- deciding repeatedly is what 0119 got wrong.
 */
stream_codec_t netdec_codec(void);

/*
 * Decode. Fills `out` with interleaved int16 and returns how many were
 * written.
 *
 *   > 0   samples, and *info describes them
 *   0     nothing yet: the ring had no data within timeout_ms, or the
 *         bytes so far are not a whole frame. **Not an error and not end
 *         of stream** -- a live stream has neither. The caller waits.
 *   < 0   this stream cannot be decoded: an unsupported codec, or the
 *         bytes are not frames at all. The caller stops.
 *
 * `out` must have room for NETDEC_MAX_INT16.
 */
int netdec_read(int16_t *out, int max_int16, netdec_info_t *info);

/*
 * Start again on the same codec after a reconnect. The window's contents
 * belong to a body that has ended and the new one starts at its own
 * frame boundary, so they are dropped; the codec and the statistics are
 * kept. Mirrors icydemux_reconnect() and exists for the same reason.
 */
void netdec_reconnect(void);

/* Counters, for the log and for the probe. */
uint32_t netdec_frames(void);
uint64_t netdec_samples(void);
uint32_t netdec_resyncs(void);      /* bytes skipped hunting for a frame */

#ifdef __cplusplus
}
#endif
