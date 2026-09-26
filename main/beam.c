/*
 * beam -- see beam.h, and 5109 in ARCHITECTURE.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "beam.h"

#include <math.h>
#include <string.h>

#define FULL_SCALE      (8388608.0f)        /* 2^23 */

/* NLMS step, and its floor under the reference power so a near-silent
 * reference does not divide the step into a jump. 0.02-0.05 were within
 * a dB of each other in beamtest; 0.03 converges in well under a
 * second and keeps the misadjustment noise down. */
#ifndef MU
#define MU              (0.03f)
#endif
#define EPS             (1e-9f * BEAM_TAPS)

/* Leak per adapting sample, and the bound on |w|^2. A canceller that
 * learned the target anyway forgets it in 1/LEAK adapting samples
 * (about 2 s), and one that runs away cannot get past the bound. 1e-4
 * cost 2-3 dB of cancellation in beamtest; 1e-6 gained under 1 dB more
 * and would take 20 s to forget. */
#ifndef LEAK
#define LEAK            (1e-5f)
#endif
#define W_NORM_MAX      (4.0f)

/*
 * Adaptation control: the width of the beam.
 *
 * Short-term powers (~10 ms) of the beam and the blocking signal, both
 * band-passed to about 1-4 kHz, and the canceller adapts while u is
 * within ADAPT_RATIO of b. Broadside sound puts almost nothing in u; a
 * source at angle t puts tan^2(pi f d sin t / c) of b's power there.
 *
 * The band matters as much as the ratio. Below 1 kHz u is near zero
 * from every direction and the ratio says nothing. Above 4 kHz it says
 * too much: near c/d = 10 kHz even a talker 9 degrees off centre puts
 * as much in u as in b, and the first version of this -- high-pass only
 * -- adapted on exactly that and took 6.5 dB off a talker at 9 degrees.
 *
 * 0.3 over 1-4 kHz makes the beam about +/-25 degrees wide: in beamtest
 * a talker at 0, 10 and 20 degrees never adapts and loses 0, 0.8 and
 * 2.1 dB (the last is the plain sum's own treble loss off axis), and
 * noise at 30 degrees and beyond adapts and loses 20-33 dB in 1-8 kHz.
 * 0.1 and 0.2 put the edge at about 10 and 15 degrees.
 */
#define HP_A            (0.88f)             /* one-pole high-pass, ~1 kHz at 48 kHz */
#define POW_A           (1.0f / 480.0f)     /* ~10 ms */
#ifndef ADAPT_RATIO
#define ADAPT_RATIO     (0.3f)              /* ~ +/-25 degrees */
#endif
#define LP_A            (0.42f)             /* one-pole low-pass, ~4 kHz */
#ifndef ADAPT_HOLD
#define ADAPT_HOLD      (480u)              /* 10 ms past the ratio first */
#endif

/* Gain match: long-term channel powers over ~2 s, the correction
 * refreshed every GAIN_EVERY samples and held within +/-3 dB. */
#define GAIN_A          (1.0f / 96000.0f)
#define GAIN_EVERY      (256)
#define GAIN_MIN        (0.708f)
#define GAIN_MAX        (1.413f)

void beam_init(beam_t *s)
{
    memset(s, 0, sizeof(*s));
    s->g = 1.0f;
}

void beam_freeze(beam_t *s, bool frozen) { s->frozen = frozen; }

unsigned beam_adapt_pct(const beam_t *s)
{
    return s->count ? (unsigned)((uint64_t)s->adapt_count * 100u / s->count) : 0u;
}

int beam_gain_centi(const beam_t *s)
{
    return (int)lroundf(200.0f * log10f(s->g));
}

static inline int32_t sat24(float y)
{
    const float v = y * FULL_SCALE;
    if (v >= 8388607.0f) return 8388607;
    if (v <= -8388608.0f) return -8388608;
    return (int32_t)lrintf(v);
}

void beam_process(beam_t *s, const int32_t *in, int32_t *out, size_t frames)
{
    const float k = 1.0f / FULL_SCALE;
    for (size_t n = 0; n < frames; n++) {
        const float l = (float)in[2 * n] * k;
        const float r0 = (float)in[2 * n + 1] * k;

        /* 1. gain match */
        s->pl += GAIN_A * (l * l - s->pl);
        s->pr += GAIN_A * (r0 * r0 - s->pr);
        if ((s->count % GAIN_EVERY) == 0 && s->count >= 48000u && s->pr > 1e-14f) {
            float g = sqrtf(s->pl / s->pr);
            s->g = g < GAIN_MIN ? GAIN_MIN : g > GAIN_MAX ? GAIN_MAX : g;
        }
        const float r = r0 * s->g;

        /* 2. beam and blocking signal */
        const float bb = 0.5f * (l + r);
        const float uu = 0.5f * (l - r);

        /* the blocking history, newest at upos, mirrored at upos+N */
        s->upos = s->upos ? s->upos - 1 : BEAM_TAPS - 1;
        const float old = s->u[s->upos];
        s->u[s->upos] = uu;
        s->u[s->upos + BEAM_TAPS] = uu;
        s->unorm += uu * uu - old * old;
        if ((s->count & 1023u) == 0) {          /* float drift */
            float e = 0.0f;
            for (int i = 0; i < BEAM_TAPS; i++) e += s->u[s->upos + i] * s->u[s->upos + i];
            s->unorm = e;
        }
        if (s->unorm < 0.0f) s->unorm = 0.0f;

        /* the beam, delayed so the canceller can look both ways */
        const float bd = s->b[s->bpos];
        s->b[s->bpos] = bb;
        s->bpos = (s->bpos == BEAM_DELAY - 1) ? 0 : s->bpos + 1;

        /* 3. cancel */
        const float *u = &s->u[s->upos];
        float est = 0.0f;
        for (int i = 0; i < BEAM_TAPS; i++) est += s->w[i] * u[i];
        const float y = bd - est;

        /* 4. adapt only while the side dominates */
        const float hb = HP_A * (s->hb + bb - s->xb);
        const float hu = HP_A * (s->hu + uu - s->xu);
        s->hb = hb; s->xb = bb;
        s->hu = hu; s->xu = uu;
        s->lb += LP_A * (hb - s->lb);
        s->lu += LP_A * (hu - s->lu);
        s->sb += POW_A * (s->lb * s->lb - s->sb);
        s->su += POW_A * (s->lu * s->lu - s->su);

        /* Sustained, not momentary: a talker near the edge of the cone
         * crosses the ratio in bursts, and a filter learned from bursts
         * of the talker is a filter that cancels them (0.7 dB at 20
         * degrees in beamtest before this). */
        if (s->su > ADAPT_RATIO * s->sb) { if (s->over < ADAPT_HOLD) s->over++; }
        else s->over = 0;

        if (!s->frozen && s->over >= ADAPT_HOLD) {
            const float step = MU * y / (s->unorm + EPS);
            const float keep = 1.0f - LEAK;
            for (int i = 0; i < BEAM_TAPS; i++) s->w[i] = keep * s->w[i] + step * u[i];
            s->adapt_count++;
            if ((s->adapt_count & 63u) == 0) {
                float wn = 0.0f;
                for (int i = 0; i < BEAM_TAPS; i++) wn += s->w[i] * s->w[i];
                if (wn > W_NORM_MAX) {
                    const float sc = sqrtf(W_NORM_MAX / wn);
                    for (int i = 0; i < BEAM_TAPS; i++) s->w[i] *= sc;
                }
            }
        }
        s->count++;

        out[n] = sat24(y);
    }
}
