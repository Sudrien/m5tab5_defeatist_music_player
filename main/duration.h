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
 */
uint32_t duration_probe(FILE *f);

#ifdef __cplusplus
}
#endif
