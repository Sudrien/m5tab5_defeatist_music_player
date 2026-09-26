/*
 * beamtest.c -- main/beam.c, compiled as is, against sources simulated
 * at chosen angles to a 34 mm pair.
 *
 * Free field, point sources far enough away to be plane waves, each
 * microphone a windowed-sinc fractional delay of the source, plus a
 * little uncorrelated self-noise per microphone. That is the model the
 * canceller is built on and the one it can be held to; a room, the
 * case's shadow and real MEMS phase mismatch are for the board.
 *
 * What is asserted:
 *   - straight ahead passes through unchanged, delayed BEAM_DELAY --
 *     with matched microphones and with 1.5 dB of mismatch between them;
 *   - white noise from the side is cut, by the canceller well beyond
 *     what the plain sum manages;
 *   - with a talker ahead and noise at 60 degrees together, the talker
 *     loses under a dB while the noise loses much more;
 *   - adaptation actually ran on the side source and not on the talker.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "beam.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

#define FS      (48000)
#define C_SOUND (343.0)
#define BASE    (32)            /* integer part of every simulated delay */
#define FD_TAPS (64)

static uint32_t s_seed = 12345;
static double gauss(void)
{
    double u1, u2;
    do { s_seed = s_seed * 1664525u + 1013904223u; u1 = (s_seed >> 8) / 16777216.0; } while (u1 <= 0);
    s_seed = s_seed * 1664525u + 1013904223u; u2 = (s_seed >> 8) / 16777216.0;
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

/* A source signal: white noise, or band-passed by a one-pole low-pass
 * and a one-pole high-pass. */
enum { WHITE, SPEECH, MIDBAND };
static void make_source(double *x, int n, int kind, double rms)
{
    double lp = 0, hp_y = 0, hp_x = 0, e = 0;
    /* SPEECH ~300 Hz-4 kHz; MIDBAND ~1-8 kHz, a TV or a second voice
     * where the canceller claims to work. */
    const double la = kind == SPEECH ? 0.42 : 0.65, ha = kind == SPEECH ? 0.96 : 0.88;
    for (int i = 0; i < n; i++) {
        double v = gauss();
        if (kind != WHITE) {
            lp += la * (v - lp);
            const double y = ha * (hp_y + lp - hp_x);
            hp_y = y; hp_x = lp;
            v = y;
        }
        x[i] = v;
        e += v * v;
    }
    const double g = rms / sqrt(e / n);
    for (int i = 0; i < n; i++) x[i] *= g;
}

/* Delay by BASE + frac samples: a Hann-windowed sinc. */
static void frac_delay(const double *x, double *y, int n, double frac)
{
    double h[FD_TAPS];
    const double centre = BASE + frac;
    for (int k = 0; k < FD_TAPS; k++) {
        const double t = k - centre;
        const double sinc = fabs(t) < 1e-9 ? 1.0 : sin(M_PI * t) / (M_PI * t);
        const double win = 0.5 - 0.5 * cos(2 * M_PI * (k + 0.5) / FD_TAPS);
        h[k] = sinc * win;
    }
    for (int i = 0; i < n; i++) {
        double acc = 0;
        for (int k = 0; k < FD_TAPS && k <= i; k++) acc += h[k] * x[i - k];
        y[i] = acc;
    }
}

/* Adds a source at `deg` from straight ahead (positive toward MIC2) to
 * the two microphone signals, with a right-channel gain. */
static void add_source(const double *src, int n, double deg, double rgain,
                       double *ml, double *mr)
{
    const double tau = BEAM_MIC_SPACING_M * sin(deg * M_PI / 180.0) / C_SOUND * FS;
    double *a = malloc(sizeof(double) * n), *b = malloc(sizeof(double) * n);
    frac_delay(src, a, n, +tau / 2);    /* MIC1 hears it later */
    frac_delay(src, b, n, -tau / 2);
    for (int i = 0; i < n; i++) { ml[i] += a[i]; mr[i] += b[i] * rgain; }
    free(a); free(b);
}

static void to_frames(const double *ml, const double *mr, int n, int32_t *fr)
{
    s_seed ^= 0x9e3779b9u;
    for (int i = 0; i < n; i++) {
        /* self-noise near -90 dBFS, independent per microphone */
        const double l = ml[i] + 3e-5 * gauss(), r = mr[i] + 3e-5 * gauss();
        fr[2 * i]     = (int32_t)lrint(fmax(-1, fmin(1, l)) * 8388607.0);
        fr[2 * i + 1] = (int32_t)lrint(fmax(-1, fmin(1, r)) * 8388607.0);
    }
}

static double pow_i32(const int32_t *x, int from, int to, int stride, int off)
{
    double e = 0;
    for (int i = from; i < to; i++) { const double v = x[i * stride + off] / 8388608.0; e += v * v; }
    return e / (to - from);
}

static double db(double r) { return 10 * log10(r); }

/*
 * Power in 1-8 kHz only: a 255-tap Hann-windowed band-pass (difference
 * of two sincs), steep enough that what the canceller cannot touch --
 * below 1 kHz, near 10 kHz -- is not counted against what it claims.
 */
#define BP_TAPS (255)
static double s_bp[BP_TAPS];
static void bp_init(void)
{
    const double f1 = 1000.0 / FS, f2 = 8000.0 / FS;
    for (int k = 0; k < BP_TAPS; k++) {
        const double t = k - (BP_TAPS - 1) / 2.0;
        const double h = t == 0 ? 2 * (f2 - f1)
                       : (sin(2 * M_PI * f2 * t) - sin(2 * M_PI * f1 * t)) / (M_PI * t);
        s_bp[k] = h * (0.5 - 0.5 * cos(2 * M_PI * (k + 0.5) / BP_TAPS));
    }
}
static double band_pow(const int32_t *x, int from, int to, int stride, int off)
{
    double e = 0;
    for (int i = from; i < to; i++) {
        double acc = 0;
        for (int k = 0; k < BP_TAPS; k++) acc += s_bp[k] * (x[(i - k) * stride + off] / 8388608.0);
        e += acc * acc;
    }
    return e / (to - from);
}

/* Power of (y - ref delayed by BASE + BEAM_DELAY) over [from, to). */
static double err_vs(const int32_t *y, const double *ref, int from, int to)
{
    double e = 0;
    for (int i = from; i < to; i++) {
        const double d = y[i] / 8388608.0 - ref[i - BASE - BEAM_DELAY];
        e += d * d;
    }
    return e / (to - from);
}

int main(void)
{
    printf("beamtest\n");
    bp_init();
    const int N = FS * 5;
    double *src = malloc(sizeof(double) * N), *nse = malloc(sizeof(double) * N);
    double *ml = calloc(N, sizeof(double)), *mr = calloc(N, sizeof(double));
    int32_t *fr = malloc(sizeof(int32_t) * 2 * N), *y = malloc(sizeof(int32_t) * N);
    static beam_t bs;

    printf("  straight ahead passes through, delayed %d samples\n", BEAM_DELAY);
    for (int mm = 0; mm < 2; mm++) {
        const double rg = mm ? pow(10, -1.5 / 20) : 1.0;
        make_source(src, N, SPEECH, 0.05);
        memset(ml, 0, sizeof(double) * N); memset(mr, 0, sizeof(double) * N);
        add_source(src, N, 0, rg, ml, mr);
        to_frames(ml, mr, N, fr);
        beam_init(&bs);
        beam_process(&bs, fr, y, N);
        const double ref = 0.05 * 0.05;
        const double e = err_vs(y, src, 3 * FS, N);
        printf("    mismatch %.1f dB: error %.1f dB under the talker, gain fix %.1f dB, adapted %u%%\n",
               mm ? 1.5 : 0.0, -db(e / ref), beam_gain_centi(&bs) / 10.0, beam_adapt_pct(&bs));
        CHECK(db(e / ref) < -20, "front source changed by the beam: error %.1f dB", db(e / ref));
        if (mm) CHECK(abs(beam_gain_centi(&bs) - 15) <= 3, "gain match %d centi-dB, want ~15", beam_gain_centi(&bs));
    }

    /*
     * The side. Two kinds of noise, because the canceller only has
     * something to work with where L-R has energy: above ~1 kHz, and not
     * near c/d = 10 kHz where a side source is a whole wavelength across
     * the pair and vanishes from L-R too. 1-8 kHz is the band it claims,
     * and gets the hard number; white noise, with half its power above
     * 8 kHz, gets a softer one. Measured on the board-free model: 21-24
     * dB off 1-8 kHz, about 5 dB below 1 kHz, 7 dB at 8-12 kHz.
     */
    printf("  noise from the side\n");
    for (int a = 0; a < 6; a++) {
        const double deg = (a % 3) == 0 ? 90 : (a % 3) == 1 ? -60 : 40;
        const int kind = a < 3 ? MIDBAND : WHITE;
        const double want = kind == MIDBAND ? -15 : -8;
        make_source(nse, N, kind, 0.05);
        memset(ml, 0, sizeof(double) * N); memset(mr, 0, sizeof(double) * N);
        add_source(nse, N, deg, 1.0, ml, mr);
        to_frames(ml, mr, N, fr);
        /* 1-8 kHz for the band-limited source, everything for white. */
        const bool band = kind == MIDBAND;
        const double in = band ? band_pow(fr, 3 * FS, N, 2, 0) : pow_i32(fr, 3 * FS, N, 2, 0);

        beam_init(&bs);
        beam_freeze(&bs, true);                 /* the plain sum */
        beam_process(&bs, fr, y, N);
        const double fixed = band ? band_pow(y, 3 * FS, N, 1, 0) : pow_i32(y, 3 * FS, N, 1, 0);

        beam_init(&bs);
        beam_process(&bs, fr, y, N);
        const double gsc = band ? band_pow(y, 3 * FS, N, 1, 0) : pow_i32(y, 3 * FS, N, 1, 0);
        printf("    %s %+.0f deg: sum %.1f dB, canceller %.1f dB, adapted %u%%\n",
               kind == MIDBAND ? "1-8 kHz" : "white  ", deg, db(fixed / in), db(gsc / in),
               beam_adapt_pct(&bs));
        CHECK(db(gsc / in) < want, "%+.0f deg: canceller only %.1f dB", deg, db(gsc / in));
        CHECK(db(gsc / fixed) < -5, "%+.0f deg: canceller %.1f dB better than the sum only",
              deg, -db(gsc / fixed));
        CHECK(beam_adapt_pct(&bs) > 50, "%+.0f deg: adapted only %u%%", deg, beam_adapt_pct(&bs));
    }

    printf("  a talker ahead, 1-8 kHz noise at 60 degrees, 0 dB SNR\n");
    {
        make_source(src, N, SPEECH, 0.05);
        make_source(nse, N, MIDBAND, 0.05);
        memset(ml, 0, sizeof(double) * N); memset(mr, 0, sizeof(double) * N);
        add_source(src, N, 0, 1.0, ml, mr);
        add_source(nse, N, 60, 1.0, ml, mr);
        to_frames(ml, mr, N, fr);
        beam_init(&bs);
        beam_process(&bs, fr, y, N);            /* learn on the mix */
        const unsigned pct = beam_adapt_pct(&bs);

        /* Then the learned filter, frozen, on each part alone. */
        static beam_t t, z;
        int32_t *ft = malloc(sizeof(int32_t) * 2 * N), *fn = malloc(sizeof(int32_t) * 2 * N);
        memset(ml, 0, sizeof(double) * N); memset(mr, 0, sizeof(double) * N);
        add_source(src, N, 0, 1.0, ml, mr);
        to_frames(ml, mr, N, ft);
        memset(ml, 0, sizeof(double) * N); memset(mr, 0, sizeof(double) * N);
        add_source(nse, N, 60, 1.0, ml, mr);
        to_frames(ml, mr, N, fn);
        t = bs; z = bs;
        beam_freeze(&t, true); beam_freeze(&z, true);
        int32_t *yt = malloc(sizeof(int32_t) * N), *yn = malloc(sizeof(int32_t) * N);
        beam_process(&t, ft, yt, N);
        beam_process(&z, fn, yn, N);
        const double tin = pow_i32(ft, FS, N, 2, 0), nin = pow_i32(fn, FS, N, 2, 0);
        const double tout = pow_i32(yt, FS, N, 1, 0), nout = pow_i32(yn, FS, N, 1, 0);
        const double gain_t = db(tout / tin), gain_n = db(nout / nin);
        printf("    talker %+.1f dB, noise %+.1f dB: SNR up %.1f dB, adapted %u%% of the mix\n",
               gain_t, gain_n, gain_t - gain_n, pct);
        CHECK(fabs(gain_t) < 1.0, "the talker changed by %.1f dB", gain_t);
        CHECK(gain_t - gain_n > 6.0, "SNR improved only %.1f dB", gain_t - gain_n);
        free(ft); free(fn); free(yt); free(yn);
    }

    /*
     * The cone. A talker is rarely dead centre: a hand-held tablet puts
     * them ten or twenty degrees off. Inside +/-20 degrees the canceller
     * must not adapt and the talker keeps their level (allowing the
     * plain sum's own treble loss off axis); from 30 degrees out, noise
     * is cancelled. Found by this test: the first detector took 6.5 dB
     * off a talker at 9 degrees.
     */
    printf("  the beam's width: talkers inside it, noise outside it\n");
    for (int deg = -20; deg <= 20; deg += 10) {
        make_source(src, N, SPEECH, 0.05);
        memset(ml, 0, sizeof(double) * N); memset(mr, 0, sizeof(double) * N);
        add_source(src, N, deg, 1.0, ml, mr);
        to_frames(ml, mr, N, fr);
        const double in = pow_i32(fr, FS, N, 2, 0);
        beam_init(&bs);
        beam_freeze(&bs, true);
        beam_process(&bs, fr, y, N);
        const double sum = db(pow_i32(y, FS, N, 1, 0) / in);
        beam_init(&bs);
        beam_process(&bs, fr, y, N);
        const double g = db(pow_i32(y, FS, N, 1, 0) / in);
        printf("    talker %+3d deg: %+.2f dB (the plain sum alone %+.2f), adapted %u%%\n",
               deg, g, sum, beam_adapt_pct(&bs));
        CHECK(beam_adapt_pct(&bs) < 10, "talker at %d deg adapted %u%%", deg, beam_adapt_pct(&bs));
        /* The sum's own treble loss off axis is geometry; the canceller
         * must add nothing to it inside the cone. */
        CHECK(fabs(g - sum) < 0.5, "talker at %d deg: canceller took %.1f dB more than the sum",
              deg, sum - g);
        CHECK(sum > -3.5, "talker at %d deg: the sum alone lost %.1f dB", deg, -sum);
    }
    for (int deg = 30; deg <= 90; deg += 30) {
        make_source(nse, N, MIDBAND, 0.05);
        memset(ml, 0, sizeof(double) * N); memset(mr, 0, sizeof(double) * N);
        add_source(nse, N, -deg, 1.0, ml, mr);
        to_frames(ml, mr, N, fr);
        beam_init(&bs);
        beam_process(&bs, fr, y, N);
        const double g = db(band_pow(y, 3 * FS, N, 1, 0) / band_pow(fr, 3 * FS, N, 2, 0));
        printf("    noise  %+3d deg: %+.1f dB in 1-8 kHz\n", -deg, g);
        CHECK(g < -15, "noise at %d deg only %.1f dB", -deg, g);
    }

    printf("  a talker alone does not teach it anything\n");
    {
        make_source(src, N, SPEECH, 0.05);
        memset(ml, 0, sizeof(double) * N); memset(mr, 0, sizeof(double) * N);
        add_source(src, N, 0, 1.0, ml, mr);
        to_frames(ml, mr, N, fr);
        beam_init(&bs);
        beam_process(&bs, fr, y, N);
        float wn = 0;
        for (int i = 0; i < BEAM_TAPS; i++) wn += bs.w[i] * bs.w[i];
        printf("    adapted %u%%, |w|^2 %.2e\n", beam_adapt_pct(&bs), wn);
        CHECK(beam_adapt_pct(&bs) < 5, "adapted %u%% of a front-only talker", beam_adapt_pct(&bs));
    }

    printf("  aliasing: out may be in\n");
    {
        make_source(nse, FS, WHITE, 0.05);
        memset(ml, 0, sizeof(double) * N); memset(mr, 0, sizeof(double) * N);
        add_source(nse, FS, 30, 1.0, ml, mr);
        to_frames(ml, mr, FS, fr);
        static beam_t a, b2;
        beam_init(&a); beam_init(&b2);
        beam_process(&a, fr, y, FS);
        beam_process(&b2, fr, fr, FS);
        CHECK(memcmp(y, fr, sizeof(int32_t) * FS) == 0, "in-place output differs");
    }

    free(src); free(nse); free(ml); free(mr); free(fr); free(y);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
