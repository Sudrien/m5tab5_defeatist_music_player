/*
 * Format-agnostic decode facade.
 *
 * Two backends sit behind this:
 *
 *   .mp3          -> minimp3 (minimp3_ex), MIT, vendored by
 *                    tools/fetch_vendored.sh
 *   everything    -> espressif/esp_audio_codec, pulled from the component
 *   else             registry
 *
 * The split is deliberate. esp_audio_codec has an MP3 decoder too, but it
 * is the Android/Helix-derived one -- Layer III only, no free format, no
 * gapless. minimp3 handles Layers I, II and III, free-format streams, and
 * minimp3_ex already parses Xing/LAME and trims encoder delay and
 * padding. On MP3 specifically it is strictly the better of the two, so
 * esp_audio_codec's MP3 decoder is deliberately left unregistered.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct decoder decoder_t;

typedef struct {
    int sample_rate;        /* Hz */
    int channels;           /* 1 or 2 */
    int bitrate_kbps;       /* 0 when the backend does not report one */
    const char *codec;      /* "mp3", "flac", ... for logging */
} decoder_info_t;

/* Extensions decoder_open() will accept, for the directory scan. */
bool decoder_supports(const char *path);

/* NULL on failure. Logs the reason. */
decoder_t *decoder_open(const char *path);

/* Fill out with interleaved int16. Returns the number of int16 values
 * written, 0 at end of stream, or negative on an unrecoverable error.
 *
 * A backend is allowed to return 0 mid-stream only at true EOF; damaged
 * frames are skipped internally and never surface as a short read, so
 * the caller can treat 0 as "done" without a retry loop.
 *
 * info is updated on every call. The sample rate can legitimately change
 * mid-stream (concatenated streams, some podcast feeds), so the caller
 * must compare rather than latch the first value. */
int decoder_read(decoder_t *d, int16_t *out, int max_int16, decoder_info_t *info);

void decoder_close(decoder_t *d);

/* Total length in seconds, or 0 when the backend cannot say.
 *
 * minimp3_ex knows this because MP3D_SEEK_TO_SAMPLE builds the index up
 * front. The esp_audio_codec simple decoder does not expose a total --
 * its info struct carries frame_size, not stream length -- so FLAC, WAV
 * and the rest return 0 and the seek bar renders empty rather than
 * lying. */
uint32_t decoder_duration_sec(decoder_t *d);

/* Jump to a position in seconds. ESP_ERR_NOT_SUPPORTED when the backend
 * cannot seek, which the caller should treat as "carry on playing", not
 * as a failure.
 *
 * Only minimp3 can. MP3D_SEEK_TO_SAMPLE built a sample-accurate index at
 * open time, so mp3dec_ex_seek() is exact rather than a byte-offset
 * guess. The esp_audio_codec simple decoder has no seek entry point at
 * all -- its parsers are forward-only over a stream. */
esp_err_t decoder_seek_sec(decoder_t *d, uint32_t sec);

/*
 * Can this backend seek at all?
 *
 * Separate from having a duration, which is the mistake the first version
 * of the drag guard made. Once duration.c started reading lengths out of
 * containers, an Ogg had a full seek bar and no way to seek within it --
 * the thumb followed the finger and the player logged "seek ignored" on
 * release. Length and seekability are two different questions and have to
 * be asked separately.
 */
bool decoder_can_seek(decoder_t *d);

/* Largest number of int16 a single decoder_read() can produce. Sized for
 * the worst case across both backends: an MPEG-1 Layer II frame is 1152
 * samples, FLAC blocks reach 4608, and esp_audio_codec's AAC-Plus path
 * doubles again. */
#define DECODER_MAX_INT16   (9216 * 2)

#ifdef __cplusplus
}
#endif
