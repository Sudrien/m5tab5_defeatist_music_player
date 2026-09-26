/*
 * beamcheck -- main/beam.c over a real stereo recording, against the
 * plain sum, by band (5111).
 *
 * The simulation in texttest/beamtest.c models free-field plane waves;
 * a room, a fridge, a hand on the case and two real capsules are not
 * that, and the first canceller's fault (5111) only showed on the
 * board's own recordings. This is how to look at one:
 *
 *   flac -d --force-raw-format --endian=little --sign=signed \
 *        -o take.raw "Recordings/2026-09-26 16.05.10.flac"
 *   cc -O2 -Imain tools/beamcheck.c main/beam.c -lm -o beamcheck
 *   ./beamcheck take.raw
 *
 * A STEREO recording (the AUDIO tab's Microphones: STEREO), 48 kHz,
 * 24-bit. Prints level and peak, then each band's level for the plain
 * sum and the beam. The beam should never be louder than the sum by
 * more than a fraction of a dB in any band; where it is quieter, in
 * 1-8 kHz, is what the canceller did.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "beam.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS    (48000)
#define TAPS  (511)

static double band_db(const float *x, long n, double f1, double f2)
{
    static double h[TAPS];
    const double a = f1 / FS, b = f2 / FS;
    for (int k = 0; k < TAPS; k++) {
        const double t = k - (TAPS - 1) / 2.0;
        h[k] = (t == 0 ? 2 * (b - a) : (sin(2 * M_PI * b * t) - sin(2 * M_PI * a * t)) / (M_PI * t))
             * (0.5 - 0.5 * cos(2 * M_PI * (k + 0.5) / TAPS));
    }
    double e = 0;
    for (long i = TAPS; i < n; i++) {
        double acc = 0;
        for (int k = 0; k < TAPS; k++) acc += h[k] * x[i - k];
        e += acc * acc;
    }
    return 10 * log10(e / (n - TAPS) + 1e-20);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s take.raw (48 kHz 24-bit stereo LE)\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f) / 6;
    rewind(f);
    unsigned char *raw = malloc((size_t)n * 6);
    if (!raw || fread(raw, 6, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    int32_t *fr = malloc(sizeof(int32_t) * 2 * (size_t)n), *y = malloc(sizeof(int32_t) * (size_t)n);
    float *sum = malloc(sizeof(float) * (size_t)n), *bm = malloc(sizeof(float) * (size_t)n);
    for (long i = 0; i < 2 * n; i++) {
        const unsigned char *p = raw + i * 3;
        fr[i] = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
    }
    static beam_t b;
    beam_init(&b);
    beam_process(&b, fr, y, (size_t)n);

    /* The sum -- with MIC2 at the beam's own gain correction, so a
     * level match is not counted as the canceller -- and the beam moved
     * back by its delay to line up. */
    const double gr = pow(10.0, beam_gain_centi(&b) / 200.0);
    double ps = 0, pb = 0, es = 0, eb = 0;
    for (long i = 0; i < n; i++) {
        sum[i] = (float)((fr[2 * i] + gr * fr[2 * i + 1]) / 2 / 8388608.0);
        bm[i] = i + BEAM_DELAY < n ? (float)(y[i + BEAM_DELAY] / 8388608.0) : 0.0f;
        if (fabs(sum[i]) > ps) ps = fabs(sum[i]);
        if (fabs(bm[i]) > pb) pb = fabs(bm[i]);
        es += (double)sum[i] * sum[i];
        eb += (double)bm[i] * bm[i];
    }
    const int g = beam_gain_centi(&b);
    printf("%s: %.1f s; canceller adapted on %u%%, MIC2 matched by %+.1f dB\n",
           argv[1], (double)n / FS, beam_adapt_pct(&b), g / 10.0);
    printf("  whole     sum %6.1f dBFS  beam %6.1f  (%+.1f dB)\n",
           10 * log10(es / n), 10 * log10(eb / n), 10 * log10(eb / es));
    printf("  peak      sum %6.1f dBFS  beam %6.1f  (%+.1f dB)\n",
           20 * log10(ps), 20 * log10(pb), 20 * log10(pb / ps));
    static const double edges[] = { 20, 250, 1000, 4000, 8000, 16000, 23900 };
    for (int k = 0; k < 6; k++) {
        const double s = band_db(sum, n, edges[k], edges[k + 1]);
        const double bb = band_db(bm, n, edges[k], edges[k + 1]);
        printf("  %5.0f-%-5.0f sum %6.1f dBFS  beam %6.1f  (%+.1f dB)%s\n", edges[k], edges[k + 1],
               s, bb, bb - s, (k >= 2 && k <= 3) ? "  <- the canceller's band" : "");
    }
    free(raw); free(fr); free(y); free(sum); free(bm);
    return 0;
}
