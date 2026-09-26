/*
 * beam -- see beam.h, and 5109 in ARCHITECTURE.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "beam.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
 * 5111: and while NOT adapting, the weights relax toward zero over a
 * few seconds. Weights kept after the side source has gone subtract
 * nothing useful and add the difference channel's own noise -- where
 * the two capsules' self-noise is uncorrelated (the board's treble,
 * -65 dBFS), L-R is as loud as L+R and anything subtracted adds. They
 * are also wrong after the tablet is turned. A source that returns is
 * re-learned in well under a second (MU), so forgetting costs little.
 * Applied every IDLE_EVERY samples to keep it off the per-sample path.
 */
#ifndef IDLE_TAU_S
#define IDLE_TAU_S      (3.0f)
#endif
#define IDLE_EVERY      (16u)

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

/*
 * 5111: the band the canceller is allowed to touch.
 *
 * The first version adapted on and subtracted the whole of u. On the
 * board's recordings -- 99% of the energy under 1 kHz, a fridge and
 * handling -- the error the filter chased was almost all low frequency,
 * which u (near zero there from every direction) cannot model; the
 * weights grew to their bound trying, and what they then carried into
 * the output was u's treble: +4.3 dB in 8-16 kHz and a peak 4.6 dB
 * over the plain sum's on one file, 11 dB on another. The simulation,
 * with flat-spectrum sources, could not show it.
 *
 * So the reference is u band-passed to 1-8 kHz (second-order Butterworth
 * each side), and the output is the delayed sum minus the filter applied
 * to that: nothing outside the band can be put back in. Adaptation
 * minimises the output's error IN THE BAND, which is the band-pass of
 * the output -- BP(b) - w * BP(BP(u)) -- so it adapts against b
 * band-passed once and u band-passed twice (the filtered-reference
 * form). Low frequencies no longer drive the weights at all.
 *
 * Not "replace the sum's band with the residual": the first attempt at
 * this did that, and a recursive band-pass is not phase-complementary
 * to the raw signal, so the replaced band and the raw band did not line
 * up and in-band cancellation fell from 30 dB to 9.
 */
#ifndef BAND_LO_HZ
#define BAND_LO_HZ      (1000.0f)
#endif
#ifndef BAND_HI_HZ
#define BAND_HI_HZ      (8000.0f)
#endif

static void bq_design(beam_bq_t *q, float fc, bool high)
{
    const float w0 = 2.0f * (float)M_PI * fc / 48000.0f;
    const float c = cosf(w0), al = sinf(w0) / (2.0f * 0.70710678f);
    const float a0 = 1.0f + al;
    const float b0 = high ? (1.0f + c) / 2.0f : (1.0f - c) / 2.0f;
    const float b1 = high ? -(1.0f + c) : (1.0f - c);
    q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b0 / a0;
    q->a1 = -2.0f * c / a0; q->a2 = (1.0f - al) / a0;
    q->z1 = q->z2 = 0.0f;
}

static inline float bq(beam_bq_t *q, float x)
{
    const float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

void beam_init(beam_t *s)
{
    memset(s, 0, sizeof(*s));
    s->g = 1.0f;
    bq_design(&s->bhp, BAND_LO_HZ, true);
    bq_design(&s->blp, BAND_HI_HZ, false);
    s->uhp = s->bhp;
    s->ulp = s->blp;
    s->vhp = s->bhp;
    s->vlp = s->blp;
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

        /* 5111: the band the canceller works in */
        const float bband = bq(&s->blp, bq(&s->bhp, bb));
        const float uband = bq(&s->ulp, bq(&s->uhp, uu));
        const float vband = bq(&s->vlp, bq(&s->vhp, uband));

        /* the reference histories, newest at upos, mirrored at upos+N:
         * u once band-passed (applied to the output), v twice (adapted
         * against) */
        s->upos = s->upos ? s->upos - 1 : BEAM_TAPS - 1;
        const float old = s->u[s->upos];
        s->u[s->upos] = uband;
        s->u[s->upos + BEAM_TAPS] = uband;
        s->unorm += uband * uband - old * old;
        const float vold = s->v[s->upos];
        s->v[s->upos] = vband;
        s->v[s->upos + BEAM_TAPS] = vband;
        s->vnorm += vband * vband - vold * vold;
        if ((s->count & 1023u) == 0) {          /* float drift */
            float e = 0.0f, f = 0.0f;
            for (int i = 0; i < BEAM_TAPS; i++) {
                e += s->u[s->upos + i] * s->u[s->upos + i];
                f += s->v[s->upos + i] * s->v[s->upos + i];
            }
            s->unorm = e;
            s->vnorm = f;
        }
        if (s->unorm < 0.0f) s->unorm = 0.0f;
        if (s->vnorm < 0.0f) s->vnorm = 0.0f;

        /* the beam and its band, delayed so the canceller can look both
         * ways */
        const float bd = s->b[s->bpos];
        const float bfd = s->bf[s->bpos];
        s->b[s->bpos] = bb;
        s->bf[s->bpos] = bband;
        s->bpos = (s->bpos == BEAM_DELAY - 1) ? 0 : s->bpos + 1;

        /* 3. cancel: the delayed sum minus the filter on the band-limited
         * reference. */
        const float *u = &s->u[s->upos];
        const float *vv = &s->v[s->upos];
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
            /* The output's error in the band: BP(b) - w * BP(BP(u)). One
             * more pass of MACs than the full-band version had. */
            float ev = 0.0f;
            for (int i = 0; i < BEAM_TAPS; i++) ev += s->w[i] * vv[i];
            const float e = bfd - ev;
            const float step = MU * e / (s->vnorm + EPS);
            const float keep = 1.0f - LEAK;
            for (int i = 0; i < BEAM_TAPS; i++) s->w[i] = keep * s->w[i] + step * vv[i];
            s->adapt_count++;
            if ((s->adapt_count & 63u) == 0) {
                float wn = 0.0f;
                for (int i = 0; i < BEAM_TAPS; i++) wn += s->w[i] * s->w[i];
                if (wn > W_NORM_MAX) {
                    const float sc = sqrtf(W_NORM_MAX / wn);
                    for (int i = 0; i < BEAM_TAPS; i++) s->w[i] *= sc;
                }
            }
        } else if (!s->frozen && (s->count % IDLE_EVERY) == 0) {
            const float keep = 1.0f - (float)IDLE_EVERY / (IDLE_TAU_S * 48000.0f);
            for (int i = 0; i < BEAM_TAPS; i++) s->w[i] *= keep;
        }
        s->count++;

        out[n] = sat24(y);
    }
}
