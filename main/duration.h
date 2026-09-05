/*
 * duration.h -- track length read out of the container, without decoding.
 *
 * The backup for `decoder_duration_sec()`, which only minimp3 can answer:
 * esp_audio_codec's simple decoder exposes frame_size, not stream length,
 * and its parsers are forward-only, so FLAC, WAV and Ogg all reported 0
 * and the seek bar stayed empty.
 *
 * That is an API ceiling, not a missing feature. Every one of these
 * formats states its own length in a header or a trailing page, in a
 * fixed place, readable with two or three fread()s. None of this decodes
 * a single audio frame.
 *
 * Deliberately NOT file size / bitrate. That is right for CBR and drifts
 * badly on VBR, and a seek bar that lies is worse than one that stays
 * empty -- which is the call the existing code already makes.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "storage_io.h"   /* storage_io_class_t: callers state which read this is */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Seconds, or 0 when the container does not say.
 *
 * The file position is saved and restored, so this is safe to call on a
 * decoder's own open handle before the first read.
 *
 * Format is chosen by sniffing the first bytes rather than by extension:
 * the caller has already picked a decoder by extension, and a probe that
 * trusted the same wrong extension would return a plausible number for a
 * mislabelled file instead of nothing.
 *
 * `cls` is the arbiter class the reads are filed under, and it is a
 * parameter rather than a constant inside because the answer depends on
 * who is asking, not on what is being read. The only caller today is
 * decoder_duration_sec() on the decode loop, which is PLAYBACK. A
 * prefetch that ever wants a length ahead of time passes PREFETCH and
 * gets queued behind the track being listened to, which is the point.
 */
/*
 * A LENGTH IN SECONDS IS ROUNDED, NOT TRUNCATED.
 *
 * Every length here is a division: samples by rate, bytes by byte rate,
 * ticks by timescale, granule by rate. Truncating each of them costs up
 * to a second, and which way that lands is decided by the last frame of
 * the file rather than by anything anybody chose.
 *
 * It showed up as a real CBR AAC file reading `seekable, 59 s` -- 960805
 * bytes at 16014 B/s is 59.997, and the bar was a second short of a
 * track that is sixty seconds long by every other measure in the
 * player. 0712 already rounds the length measured from a complete play,
 * `(frames_out + rate / 2) / rate`, so a file could be handed a length
 * of 59 by its container and 60 by its own audio depending on which
 * arrived first.
 *
 * Both arguments unsigned, evaluated once each -- these are macros
 * because the widths differ at every call site.
 */
#define ROUND_DIV(n, d)  (((n) + (d) / 2) / (d))

uint32_t duration_probe(FILE *f, storage_io_class_t cls);

#ifdef __cplusplus
}
#endif
