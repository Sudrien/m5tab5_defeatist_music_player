/*
 * rateconv.h -- the one sample-rate converter, and when it runs.
 *
 * A wrapper around polyrsp (polyrsp.h), the fixed-point polyphase
 * resampler the USB path uses. Until 5040 it wrapped esp_audio_effects'
 * esp_ae_rate_cvt, which measured 223% of real time on this board for
 * 44.1 -> 48 kHz where polyrsp measures 9% -- see 5039 and 5040.
 *
 * It exists for one job: letting a crossfade span a sample-rate change.
 * A track whose rate differs from what the output is clocked at is
 * converted to the OUTPUT's rate on its way into the ring, so the two
 * rings a crossfade mixes always hold the same rate and the clock is
 * never reconfigured under a track that is still playing.
 *
 * It is NOT a general "always resample to 48 kHz" stage. Everything
 * that is not a crossfade (or the gapless continuation of one) keeps
 * the old behaviour: the clock follows the file, and the samples reach
 * the DAC exactly as decoded.
 *
 * Decode task only. There is one converter, it carries state from one
 * block to the next, and the decode loop is the only thing that feeds
 * it. Nothing here is safe to call from the writer.
 *
 * Always 16-bit stereo interleaved, because that is the only thing the
 * PCM rings ever hold -- mono is duplicated before it gets here.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start converting in_rate to out_rate.
 *
 * `keep` asks to continue an already-running conversion between the
 * same two rates without clearing its state -- the gapless case, where
 * the next track is the continuation of the last and the filter's
 * history is the right history. A different pair of rates, or keep ==
 * false, starts from silence.
 *
 * Returns false, with the converter off, when polyrsp refuses the pair
 * (a ratio needing more than its coefficient cap) or cannot allocate. The caller then does what it did before this file
 * existed.
 */
bool rateconv_begin(uint32_t in_rate, uint32_t out_rate, bool keep);

/* Stop converting and free everything. A no-op when already off. */
void rateconv_end(void);

/* Clear the filter history, for a seek: the samples it holds are from
 * a position the listener has just left. */
void rateconv_reset(void);

bool     rateconv_active(void);
uint32_t rateconv_in_rate(void);
uint32_t rateconv_out_rate(void);

/*
 * Convert `frames` stereo frames. Returns a buffer owned by this file,
 * valid until the next call, with *out_frames set -- which may be zero
 * on the first blocks, while the filter fills. NULL on failure, with
 * the block lost: playing it unconverted would be the wrong pitch.
 */
const int16_t *rateconv_run(const int16_t *in, size_t frames, size_t *out_frames);

#ifdef __cplusplus
}
#endif
