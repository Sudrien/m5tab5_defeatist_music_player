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
 * MP3 only, for now, and against the order in the plan. The plan said
 * ADTS AAC first because WNZK was the station under test; the benchmark
 * station is now WUOM, which is MP3, and **the path that can be watched
 * on hardware is worth more than the one that was written down first.**
 * AAC through `esp_audio_simple_dec` is the next patch;
 * `netdec_open()` refuses it cleanly until then rather than pretending.
 *
 * No seeking, no duration, no pause. A live stream has no position to
 * hold; phase 3's pause is a stop and a fresh connection.
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
 * Largest number of int16 one netdec_read() can produce. An MPEG1
 * Layer II frame is 1152 samples; stereo doubles it. minimp3's own
 * MINIMP3_MAX_SAMPLES_PER_FRAME is the same number, and netdec.c
 * asserts they agree rather than trusting that they do.
 */
#define NETDEC_MAX_INT16    (1152 * 2)

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
