/*
 * polyrsp -- see polyrsp.h, and 5039 in ARCHITECTURE.md.
 */
#include "polyrsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* The table is read a row per output sample and fits the cache; PSRAM
 * first, so the internal RAM the radio's DMA needs is left alone. */
static void *rsp_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}
#define rsp_free heap_caps_free
#else
#define rsp_alloc malloc
#define rsp_free free
#endif

/* Taps per phase when converting up; scaled by in/out when converting
 * down so the cut stays at the same fraction of the output rate. */
#define POLYRSP_TAPS        48
#define POLYRSP_MAX_TAPS    96
/* 64 KB of table. 44.1 -> 48 kHz is 160 x 48 = 7680; 11.025 -> 48 is
 * 640 x 48 = 30720, the largest ratio a UAC device is likely to ask. */
#define POLYRSP_MAX_COEFS   32768
/* Kaiser beta 6: about 60 dB of stopband. The transition that buys at
 * TAPS taps is about 3.6 * in_rate / TAPS wide, and it is placed to end
 * at the lower rate's Nyquist. */
#define POLYRSP_BETA        6.0

struct polyrsp {
    uint32_t L, M, T;
    uint32_t max_in;
    uint32_t pos;       /* input frame n of the next output, in buf */
    uint32_t phase;     /* its phase, 0..L-1 */
    int16_t *coef;      /* [L][T], taps reversed: row p applies to x[n-T+1 ..n] */
    int16_t *buf;       /* stereo frames: T-1 of history, then the block */
};

static uint32_t gcd_u32(uint32_t a, uint32_t b)
{
    while (b) { uint32_t t = a % b; a = b; b = t; }
    return a;
}

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 40; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

polyrsp_t *polyrsp_open(uint32_t in_rate, uint32_t out_rate, uint32_t max_in_frames)
{
    if (!in_rate || !out_rate || !max_in_frames) return NULL;
    const uint32_t g = gcd_u32(in_rate, out_rate);
    const uint32_t L = out_rate / g, M = in_rate / g;

    uint32_t T = POLYRSP_TAPS;
    if (in_rate > out_rate) {
        T = (uint32_t)ceil((double)POLYRSP_TAPS * in_rate / out_rate);
        T = (T + 1) & ~1u;
        if (T > POLYRSP_MAX_TAPS) return NULL;
    }
    if ((uint64_t)L * T > POLYRSP_MAX_COEFS) return NULL;

    polyrsp_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->L = L; r->M = M; r->T = T;
    r->max_in = max_in_frames;
    r->coef = rsp_alloc((size_t)L * T * sizeof(int16_t));
    r->buf = rsp_alloc((size_t)(T - 1 + max_in_frames) * 2 * sizeof(int16_t));
    float *h = malloc((size_t)L * T * sizeof(float));
    if (!r->coef || !r->buf || !h) {
        free(h);
        polyrsp_close(r);
        return NULL;
    }

    /* The prototype, at the upsampled rate in_rate * L. */
    const double fs_up = (double)in_rate * L;
    const double nyq = 0.5 * (in_rate < out_rate ? in_rate : out_rate);
    const double width = 3.6 * (double)in_rate / T;
    const double fc = (nyq - 0.5 * width) / fs_up;      /* cycles per sample */
    const uint32_t N = L * T;
    const double c = 0.5 * (N - 1);
    const double i0b = bessel_i0(POLYRSP_BETA);
    for (uint32_t i = 0; i < N; i++) {
        const double x = i - c;
        const double s = x == 0.0 ? 2.0 * fc : sin(2.0 * M_PI * fc * x) / (M_PI * x);
        const double u = 2.0 * i / (N - 1) - 1.0;
        const double w = bessel_i0(POLYRSP_BETA * sqrt(1.0 - u * u)) / i0b;
        h[i] = (float)(s * w * L);
    }

    /* Row p, reversed, each row normalised to unity gain in Q15 so there
     * is no ripple at the phase rate. The rounding error goes to the
     * largest tap. */
    for (uint32_t p = 0; p < L; p++) {
        int16_t *row = r->coef + (size_t)p * T;
        double sum = 0.0;
        for (uint32_t j = 0; j < T; j++) sum += h[p + j * L];
        int32_t isum = 0, big = 0;
        for (uint32_t j = 0; j < T; j++) {
            long v = lround(h[p + j * L] / sum * 32768.0);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            row[T - 1 - j] = (int16_t)v;
            isum += v;
            if (abs(row[T - 1 - j]) > abs(row[big])) big = (int32_t)(T - 1 - j);
        }
        long fix = row[big] + (32768 - isum);
        if (fix > 32767) fix = 32767;
        row[big] = (int16_t)fix;
    }
    free(h);

    memset(r->buf, 0, (size_t)(T - 1) * 2 * sizeof(int16_t));
    r->pos = T - 1;
    r->phase = 0;
    return r;
}

void polyrsp_close(polyrsp_t *r)
{
    if (!r) return;
    rsp_free(r->coef);
    rsp_free(r->buf);
    free(r);
}

uint32_t polyrsp_max_out(const polyrsp_t *r)
{
    return (uint32_t)(((uint64_t)r->max_in * r->L + r->M - 1) / r->M) + 1;
}

static inline int16_t sat16(int32_t v)
{
    v = (v + (1 << 14)) >> 15;
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

uint32_t polyrsp_process(polyrsp_t *r, const int16_t *in, uint32_t in_frames,
                         int16_t *out)
{
    if (in_frames > r->max_in) in_frames = r->max_in;
    const uint32_t T = r->T, L = r->L, M = r->M;
    int16_t *buf = r->buf;
    memcpy(buf + (size_t)(T - 1) * 2, in, (size_t)in_frames * 4);

    const uint32_t end = T - 1 + in_frames;
    uint32_t pos = r->pos, phase = r->phase, n = 0;
    while (pos < end) {
        const int16_t *c = r->coef + (size_t)phase * T;
        const int16_t *x = buf + (size_t)(pos + 1 - T) * 2;
        int32_t a = 0, b = 0;
        for (uint32_t j = 0; j < T; j++) {
            a += (int32_t)c[j] * x[2 * j];
            b += (int32_t)c[j] * x[2 * j + 1];
        }
        out[2 * n] = sat16(a);
        out[2 * n + 1] = sat16(b);
        n++;
        phase += M;
        if (phase >= L) {
            pos += phase / L;
            phase %= L;
        }
    }
    r->pos = pos - in_frames;
    r->phase = phase;
    memmove(buf, buf + (size_t)in_frames * 2, (size_t)(T - 1) * 4);
    return n;
}
