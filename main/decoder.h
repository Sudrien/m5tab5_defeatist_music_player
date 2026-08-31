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

/*
 * Whether the samples this backend hands out are exactly the recording.
 *
 * A lossy encoder cannot represent an arbitrary number of samples. MP3
 * works in 1152-sample granules and its filterbank has latency, so
 * encoding N samples produces silence at the front (LAME: 576 + 529
 * samples of encoder delay) and silence at the back (padding out the
 * last granule) that were never in the source. AAC has the same problem
 * with a different constant. Opus states a mandatory pre-skip in its ID
 * header. Each format carries the two counts somewhere -- Xing/Info/LAME
 * for MP3, iTunSMPB or the edit list for MP4, the ID header for Opus --
 * and a decoder that reads them reconstructs the original sample count
 * exactly.
 *
 * THIS IS NOT THE SAME QUESTION AS "IS IT GAPLESS".
 *
 * Gapless playback is what you get when the trim is right AND the next
 * track starts at the correct sample. This enum is only the first half:
 * has the encoder's silence been taken off the ends. Nothing here says
 * anything about what the player then does with the boundary.
 *
 * WHY IT IS EXPOSED RATHER THAN LOGGED AND FORGOTTEN
 *
 * It was logged and forgotten: minimp3_open() printed "gapless trim
 * active" or not, and dropped the fact on the floor. That was enough
 * while the boundary was a hard cut, because a hard cut at the wrong
 * sample is a click either way and the trim only made it quieter.
 *
 * It stops being enough for a crossfade. An overlap has to be positioned
 * against the trimmed end of the outgoing track and the trimmed start of
 * the incoming one; if either trim silently did not happen, both are
 * wrong by an unknown few tens of milliseconds that vary per file and
 * per encoder. A fade-out tolerates that. Two tracks played against each
 * other do not -- the error is audible as the incoming track entering
 * early or late, and on anything with a beat that is the artefact you
 * hear rather than the crossfade.
 *
 * So the caller needs to be able to ASK, and to refuse.
 */
typedef enum {
    /*
     * The backend does not say, and this format has ends that could
     * need trimming. Treat as "not exact": the honest answer for a
     * compressed format where nothing has verified the delay is being
     * removed. This is the default so that a format added to the table
     * without thought reports the cautious answer rather than the
     * flattering one.
     */
    DECODER_TRIM_UNKNOWN = 0,

    /*
     * The samples are the recording. Either the format has no encoder
     * delay to remove (WAV, FLAC) or the backend has read the metadata
     * and removed it (MP3 with a Xing/Info header).
     */
    DECODER_TRIM_EXACT,

    /*
     * The format has encoder delay and the metadata that describes it is
     * absent, so the ends carry silence of unknown length. Distinct from
     * UNKNOWN: this is a fact about the file, not a gap in what the
     * player has checked. A Xing-less MP3 is the case.
     */
    DECODER_TRIM_NONE,
} decoder_trim_t;

/* "exact", "untrimmed", "unknown". Never NULL. */
const char *decoder_trim_name(decoder_trim_t t);

typedef struct {
    int sample_rate;        /* Hz */
    int channels;           /* 1 or 2 */
    int bitrate_kbps;       /* 0 when the backend does not report one */
    const char *codec;      /* "mp3", "flac", ... for logging */
    /* Set at open and constant for the file. See decoder_trim_t. */
    decoder_trim_t trim;
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
