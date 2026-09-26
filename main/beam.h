/*
 * beam -- the two top-edge microphones steered straight out of the
 * screen (5109).
 *
 * THE GEOMETRY. MIC1 and MIC2 sit side by side above the screen, 34 mm
 * apart (M5Stack's drawing puts the holes 34.2 mm apart; callipers say
 * 34), both facing out of the front. Straight out of the screen is
 * therefore BROADSIDE to the pair: sound from there reaches both holes
 * at once. Sound from either side arrives up to d/c = 99 us, 4.8 samples
 * at 48 kHz, earlier at one hole.
 *
 * WHAT TWO MICROPHONES CAN AND CANNOT DO HERE.
 *   - The sum, L+R, is the beam. It passes broadside unchanged and
 *     partly cancels the sides -- fully at c/2d = 5 kHz for a source
 *     exactly to the side, barely at all below 1 kHz, where the holes
 *     are a small fraction of a wavelength apart. Above c/d = 10 kHz
 *     there are grating lobes.
 *   - Nothing done to these two signals can tell front from back or
 *     above from below: every direction in the plane square to the pair
 *     arrives at both holes together and looks like "ahead". What front
 *     selectivity there is comes from the case, which faces the holes
 *     forward and shadows the rear at high frequencies.
 *   - A differential ("cardioid") pair would aim along the axis of the
 *     holes -- sideways -- with its null straight ahead. Not this.
 *
 * WHAT THIS DOES: a generalised sidelobe canceller.
 *   1. The two channels are gain-matched (MEMS parts differ by a dB or
 *      two, and a mismatch lets the target leak into step 2).
 *   2. Fixed beam  b = (L + R) / 2, delayed D samples.
 *      Blocking    u = (L - R) / 2, which has no broadside sound in it.
 *   3. An NLMS filter learns how the off-axis sound in u shows up in b
 *      and subtracts it: y = b(n-D) - w . u.
 *   4. It adapts only while u is loud relative to b in 1-4 kHz -- i.e.
 *      while something outside about +/-25 degrees of straight ahead
 *      dominates -- so a talker in front does not teach it to cancel
 *      them. Leaky, with its norm bounded, so a mistake decays instead
 *      of accumulating. The +/-25 degrees is the beam's width; see
 *      ADAPT_RATIO in beam.c.
 * The canceller works where u has energy: above about 1 kHz. Since
 * 5111 it is confined there: b and u are both band-passed to 1-8 kHz for
 * the filter, and only that band of the output is replaced; below 1 kHz
 * and above 8 kHz the output is the plain sum, i.e. omnidirectional.
 *
 * Mono out. Float, one instance, not thread-safe; the recorder runs it
 * on rec_enc between the ring and flacenc.
 *
 * Pure C, no ESP-IDF: texttest/beamtest.c simulates sources at chosen
 * angles and checks all of the above.
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

#define BEAM_MIC_SPACING_M  (0.034f)
#define BEAM_TAPS           (64)    /* canceller length */
#define BEAM_DELAY          (32)    /* beam delay: the filter's look-ahead */

/* One second-order section, transposed direct form II. */
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } beam_bq_t;

typedef struct {
    /* 5111: the canceller's band. u goes through the band-pass once for
     * the output's reference and again for adaptation; b once, for
     * adaptation. Same filter everywhere, so the relation the weights
     * learn is the one the output applies. */
    beam_bq_t bhp, blp, uhp, ulp, vhp, vlp;
    float bf[BEAM_DELAY];           /* the band-limited beam, delayed like b */
    float v[2 * BEAM_TAPS];         /* u band-passed twice: the adaptation reference */
    float vnorm;
    /* the canceller */
    float w[BEAM_TAPS];
    float u[2 * BEAM_TAPS];         /* blocking history, doubled: no modulo in the MACs */
    int   upos;
    float unorm;                    /* sum of u^2 over the window */
    /* the delayed beam */
    float b[BEAM_DELAY];
    int   bpos;
    /* gain match: long-term powers of each channel */
    float pl, pr, g;
    /* adaptation control: short-term powers of b and u, high-passed */
    float hb, hu, xb, xu;           /* one-pole high-pass states */
    float lb, lu;                   /* one-pole low-pass states */
    float sb, su;
    uint32_t over;                  /* consecutive samples past the ratio */
    bool  frozen;                   /* tests: hold w */
    uint32_t adapt_count, count;    /* samples adapted / processed */
} beam_t;

void beam_init(beam_t *s);

/* Interleaved stereo in (MIC1, MIC2), 24-bit values in int32; mono out,
 * the same scale, saturated to 24 bits. out may alias in (it is written
 * at half the rate it is read). */
void beam_process(beam_t *s, const int32_t *in, int32_t *out, size_t frames);

/* Stop or restart adaptation; the weights are kept. For measurement. */
void beam_freeze(beam_t *s, bool frozen);

/* Fraction of samples that adapted since init, in percent: the log's
 * answer to "did the canceller ever run". */
unsigned beam_adapt_pct(const beam_t *s);

/* The right channel's gain correction, in 0.1 dB. */
int beam_gain_centi(const beam_t *s);

#ifdef __cplusplus
}
#endif
