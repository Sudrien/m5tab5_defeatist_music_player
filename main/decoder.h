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

/*
 * A seek table, handed in from outside.
 *
 * Offsets are absolute byte positions of real frame headers; `frame` is
 * the PCM frame position (PER CHANNEL) reached at that offset. Frames
 * rather than minimp3's own units, which count int16 values across all
 * channels: this record is written to a file that outlives the decoder
 * that produced it, and "sample position" in a stored format should not
 * mean something different for a mono file. decoder.c multiplies by the
 * channel count on the way in.
 *
 * Both arrays must be strictly increasing and `count` entries long.
 */
typedef struct {
    int count;
    uint32_t spacing_sec;       /* what the entries are actually spaced at */
    const uint32_t *offset;
    const uint32_t *frame;
} decoder_index_t;

/* NULL on failure. Logs the reason. */
decoder_t *decoder_open(const char *path);

/*
 * As decoder_open(), but with a table the caller already has.
 *
 * A usable table is the difference between an open that reads the whole
 * file and one that reads a frame: minimp3 builds its index by walking
 * every frame header in the file, which is 1.2 to 1.8 seconds of every
 * play of a Xing-less MP3 and essentially all of the delay before the
 * first sound. Given a table, MP3D_DO_NOT_SCAN skips the walk and the
 * table stands in for what it would have produced.
 *
 * ix may be NULL, and a table that fails validation is ignored rather
 * than half-installed -- the open then behaves exactly as decoder_open()
 * does, which is the correct fallback for every reason a table can be
 * bad. It is only ever an accelerator.
 */
decoder_t *decoder_open_indexed(const char *path, const decoder_index_t *ix);

/*
 * Decimate whatever index the decoder is holding into caller storage,
 * so it can be written to a sidecar and handed back next time.
 *
 * True when something was written. False when the decoder has no index,
 * which is the ordinary state on a backend that is not minimp3 and on a
 * track nobody has seeked in.
 *
 * `spacing_sec` is what the entries came out spaced at, which is not
 * necessarily what was asked for: a long file doubles it to stay within
 * `max`. The caller must store the value rather than the constant.
 */
bool decoder_index_extract(decoder_t *d, uint32_t *offset, uint32_t *frame,
                           int max, uint32_t want_spacing_sec,
                           int *count, uint32_t *spacing_sec);

/*
 * Whether this stream has no way to seek EXCEPT a table somebody
 * records for it.
 *
 * True only for raw ADTS that failed cbr_probe() -- a file whose every
 * frame header says `buffer_fullness = 0x7FF`, which is the stream
 * declaring itself variable, and which every encoder ffmpeg ships does.
 * There is nothing in such a file that maps time to offset, and no
 * container to ask, so the only honest source is a play that watched it
 * happen.
 *
 * Narrow on purpose. Every other format either proves a rate, bisects,
 * or reads a table it already has; a stream that reaches here is one
 * where those have all been tried and declined.
 */
bool decoder_needs_table(decoder_t *d);

/*
 * Give a decoder a table part way through a track.
 *
 * The walk that produces one for a raw ADTS stream reads the whole file
 * and so runs behind the music; this is how its answer gets in without
 * reopening anything. Only meaningful where decoder_needs_table() was
 * true, and validated exactly as the open's table is.
 *
 * MUST be called from the thread that owns the decoder -- the decode
 * loop -- and not from whatever background task did the walking.
 */
bool decoder_install_index(decoder_t *d, const decoder_index_t *ix);

/*
 * File offset of the next byte the decoder will consume.
 *
 * Exact, not approximate: the input window's unread tail is subtracted,
 * so this is the byte the next frame starts at or within, not wherever
 * the read pointer happens to have run ahead to. That is what makes a
 * recorded pair worth storing -- an offset half a window late would put
 * every later seek half a second past where the bar said.
 */
long decoder_stream_pos(decoder_t *d);

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
 * its info struct carries frame_size, not stream length -- so the answer
 * comes from the container instead (duration.c), and failing that from a
 * proven-constant byte rate over a known extent (cbrseek.c), which is
 * what gives raw ADTS and AMR a length again. Still 0 when none of the
 * three can say, and the seek bar renders empty rather than lying. */
uint32_t decoder_duration_sec(decoder_t *d);

/* Jump to a position in seconds. ESP_ERR_NOT_SUPPORTED when the backend
 * cannot seek, which the caller should treat as "carry on playing", not
 * as a failure.
 *
 * Two mechanisms behind it, and they are not the same kind of thing.
 *
 * minimp3 seeks the DECODER: MP3D_SEEK_TO_SAMPLE built a sample-accurate
 * index at open, so mp3dec_ex_seek() lands exactly.
 *
 * The esp_audio_codec backend seeks the FILE. Its simple decoder has no
 * seek entry point -- the parsers are forward-only over a stream -- but a
 * forward-only parser does not care where the stream came from, so where
 * the mapping from time to byte offset is a straight line, an fseek and
 * a reset of the input window is a seek. cbrseek.c establishes whether
 * that line exists for this file: PCM WAV always, ADTS and AMR when a
 * sampled walk shows the frame rate really is constant. Everything else
 * on that backend -- FLAC, Ogg, m4a, ts -- still returns
 * ESP_ERR_NOT_SUPPORTED. */
esp_err_t decoder_seek_sec(decoder_t *d, uint32_t sec);

/*
 * As decoder_seek_sec(), but reports where it actually landed.
 *
 * Not every mechanism lands on the second asked for. minimp3 decodes
 * forward to the exact sample and always does; the FLAC bisection lands
 * on the frame CONTAINING the target, which starts at or before it; the
 * CBR path lands on the first frame at or after. The differences are
 * small -- a FLAC frame is 93 ms at 4096 samples -- and they are not
 * zero, and the caller re-anchors its position counter from this.
 *
 * Anchoring to what was asked for rather than to what was reached is
 * how a clock comes to disagree with the audio by the width of a frame
 * and stay that way for the rest of the track. `landed` may be NULL.
 */
esp_err_t decoder_seek_sec_at(decoder_t *d, uint32_t sec, uint32_t *landed);

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
