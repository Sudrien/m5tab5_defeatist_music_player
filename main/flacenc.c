/*
 * flacenc -- see flacenc.h, and 5104 in ARCHITECTURE.md.
 */
#include "flacenc.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* Block buffers are touched once per block, sequentially: PSRAM first,
 * so the internal RAM the radio's DMA needs is left alone (polyrsp.c's
 * rule). */
static void *fe_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}
#define fe_free heap_caps_free
#else
#define fe_alloc malloc
#define fe_free free
#endif

#define MAX_PORDER 6

struct flacenc {
    unsigned ch, bps, rate, bs, fill;
    flacenc_write_fn wr;
    void *ctx;
    uint32_t frame_no, minfs, maxfs;
    uint64_t total;
    int32_t *buf[4];            /* L R M S, or one channel */
    int32_t *res;
    uint8_t *out;
    /* Partition search scratch: 640 bytes, kept off the caller's stack. */
    uint64_t sums[1 << MAX_PORDER];
    uint8_t ks[1 << MAX_PORDER], kbest[1 << MAX_PORDER];
};

/* ---- bits ---- */

typedef struct { uint8_t *p; uint32_t acc; unsigned fill; } bw_t;

static inline void put(bw_t *w, uint32_t v, unsigned n)     /* 1..24 bits */
{
    w->acc = (w->acc << n) | (v & ((1u << n) - 1));
    w->fill += n;
    while (w->fill >= 8) {
        w->fill -= 8;
        *w->p++ = (uint8_t)(w->acc >> w->fill);
    }
}

static void putw(bw_t *w, uint32_t v, unsigned n)           /* 0..32 bits */
{
    if (n > 24) { put(w, v >> 16, n - 16); put(w, v & 0xffff, 16); }
    else if (n) put(w, v, n);
}

static void zeros(bw_t *w, uint32_t n)
{
    while (n > 24) { put(w, 0, 24); n -= 24; }
    if (n) put(w, 0, n);
}

/* ---- checks ---- */

static uint16_t s_crc16[256];

static void crc16_init(void)
{
    for (unsigned i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (int b = 0; b < 8; b++) c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1);
        s_crc16[i] = c;
    }
}

static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0;
    while (n--) c = (uint16_t)((c << 8) ^ s_crc16[(c >> 8) ^ *p++]);
    return c;
}

/* ---- prediction ---- */

static inline unsigned ilog2_64(uint64_t v)
{
    unsigned r = 0;
    if (v >> 32) { r = 32; v >>= 32; }
    uint32_t x = (uint32_t)v;
    return r + (x ? 31u - (unsigned)__builtin_clz(x) : 0u);
}

/* Bits a Rice code of u-sum U over m samples costs at its best k. */
static uint64_t rice_best(uint64_t U, uint32_t m, unsigned kmax, unsigned *kout)
{
    unsigned k = (m && U > m) ? ilog2_64(U / m) : 0;
    if (k > kmax) k = kmax;
    uint64_t best = (uint64_t)m * (k + 1) + (U >> k);
    unsigned bk = k;
    if (k > 0) {
        uint64_t c = (uint64_t)m * k + (U >> (k - 1));
        if (c < best) { best = c; bk = k - 1; }
    }
    if (k < kmax) {
        uint64_t c = (uint64_t)m * (k + 2) + (U >> (k + 1));
        if (c < best) { best = c; bk = k + 1; }
    }
    *kout = bk;
    return best;
}

/* Sum of |residual| for orders 0..4 over x[4..n); returns the best order.
 * The sums only choose an order, so they are taken at 16-bit precision
 * (shift) in 32-bit chunks of 512 samples: no 64-bit add per sample. */
static unsigned best_order(const int32_t *x, unsigned n, unsigned shift, uint64_t *sum_out)
{
    if (n < 5) { *sum_out = ~0ull >> 8; return 0; }
    uint64_t s[5] = { 0, 0, 0, 0, 0 };
    int32_t p0 = x[3], p1 = x[3] - x[2], p2 = p1 - (x[2] - x[1]);
    int32_t p3 = p2 - ((x[2] - x[1]) - (x[1] - x[0]));
    for (unsigned i = 4; i < n; ) {
        const unsigned end = (n - i > 512) ? i + 512 : n;
        uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0;
        for (; i < end; i++) {
            int32_t e0 = x[i], e1 = e0 - p0, e2 = e1 - p1, e3 = e2 - p2, e4 = e3 - p3;
            s0 += (uint32_t)(e0 < 0 ? -e0 : e0) >> shift;
            s1 += (uint32_t)(e1 < 0 ? -e1 : e1) >> shift;
            s2 += (uint32_t)(e2 < 0 ? -e2 : e2) >> shift;
            s3 += (uint32_t)(e3 < 0 ? -e3 : e3) >> shift;
            s4 += (uint32_t)(e4 < 0 ? -e4 : e4) >> shift;
            p0 = e0; p1 = e1; p2 = e2; p3 = e3;
        }
        s[0] += s0; s[1] += s1; s[2] += s2; s[3] += s3; s[4] += s4;
    }
    unsigned o = 0;
    for (unsigned i = 1; i < 5; i++) if (s[i] < s[o]) o = i;
    *sum_out = s[o] << shift;
    return o;
}

/* A channel's cost for choosing the stereo pair: the Rice estimate, but
 * never more than the verbatim subframe it would fall back to. Without
 * the cap, noise picks a side channel on a rounding difference and then
 * stores it verbatim at one bit more per sample than either input. */
static uint64_t est_bits(uint64_t sum, unsigned n, unsigned sbps)
{
    unsigned k;
    const uint64_t rice = rice_best(sum * 2, n, 30, &k);
    const uint64_t verb = (uint64_t)n * sbps;
    return rice < verb ? rice : verb;
}

static void residual(const int32_t *x, unsigned n, unsigned o, int32_t *r)
{
    unsigned i;
    switch (o) {
    case 0: memcpy(r, x, n * sizeof(*r)); break;
    case 1: for (i = 1; i < n; i++) r[i - 1] = x[i] - x[i - 1]; break;
    case 2: for (i = 2; i < n; i++) r[i - 2] = x[i] - 2 * x[i - 1] + x[i - 2]; break;
    case 3: for (i = 3; i < n; i++) r[i - 3] = x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3]; break;
    default: for (i = 4; i < n; i++) r[i - 4] = x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4]; break;
    }
}

/* ---- subframes ---- */

static void subframe(flacenc_t *e, bw_t *w, const int32_t *x, unsigned n, unsigned sbps, unsigned o)
{
    const uint32_t mask = sbps >= 32 ? 0xffffffffu : (1u << sbps) - 1;

    unsigned i = 1;
    while (i < n && x[i] == x[0]) i++;
    if (i == n) {                                   /* CONSTANT */
        put(w, 0x00, 8);
        putw(w, (uint32_t)x[0] & mask, sbps);
        return;
    }

    const unsigned m = n - o;
    int32_t *r = e->res;
    residual(x, n, o, r);

    /* partition sums at the finest order, then merged upward */
    unsigned P = 0;
    while (P < MAX_PORDER && (n % (2u << P)) == 0 && (n >> (P + 1)) > o) P++;
    uint64_t *sums = e->sums;
    const unsigned parts = 1u << P, psz = n >> P;
    unsigned idx = 0;
    for (unsigned p = 0; p < parts; p++) {
        const unsigned cnt = p ? psz : psz - o;
        uint64_t U = 0;
        for (unsigned j = 0; j < cnt; j++, idx++) {
            const int32_t v = r[idx];
            U += ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
        }
        sums[p] = U;
    }

    uint64_t best = ~0ull;
    unsigned best_p = 0, best_meth = 0;
    uint8_t *ks = e->ks, *kbest = e->kbest;
    for (int lv = (int)P; lv >= 0; lv--) {
        const unsigned np = 1u << lv, sz = n >> lv;
        uint64_t bits = 0;
        unsigned kmax_used = 0;
        for (unsigned p = 0; p < np; p++) {
            unsigned k;
            bits += rice_best(sums[p], p ? sz : sz - o, 30, &k);
            ks[p] = (uint8_t)k;
            if (k > kmax_used) kmax_used = k;
        }
        const unsigned meth = kmax_used > 14;
        bits += np * (meth ? 5u : 4u);
        if (bits < best) { best = bits; best_p = (unsigned)lv; best_meth = meth; memcpy(kbest, ks, np); }
        if (lv) for (unsigned p = 0; p < np / 2; p++) sums[p] = sums[2 * p] + sums[2 * p + 1];
    }

    if (best + 6 + (uint64_t)o * sbps >= (uint64_t)n * sbps) { /* VERBATIM */
        put(w, 0x02, 8);
        for (i = 0; i < n; i++) putw(w, (uint32_t)x[i] & mask, sbps);
        return;
    }

    put(w, (0x08 | o) << 1, 8);                     /* FIXED, order o */
    for (i = 0; i < o; i++) putw(w, (uint32_t)x[i] & mask, sbps);
    put(w, best_meth, 2);
    put(w, best_p, 4);
    const unsigned np = 1u << best_p, sz = n >> best_p;
    idx = 0;
    for (unsigned p = 0; p < np; p++) {
        const unsigned k = kbest[p], cnt = p ? sz : sz - o;
        const uint32_t km = (1u << k) - 1;
        put(w, k, best_meth ? 5 : 4);
        for (unsigned j = 0; j < cnt; j++, idx++) {
            const int32_t v = r[idx];
            const uint32_t u = ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
            const uint32_t q = u >> k;
            if (q + k + 1 <= 24) {
                put(w, (1u << k) | (u & km), q + k + 1);
            } else {
                zeros(w, q);
                put(w, 1, 1);
                putw(w, u & km, k);
            }
        }
    }
    (void)m;
}

/* ---- frames ---- */

static unsigned rate_code(unsigned r)
{
    switch (r) {
    case 88200: return 1;  case 176400: return 2; case 192000: return 3;
    case 8000: return 4;   case 16000: return 5;  case 22050: return 6;
    case 24000: return 7;  case 32000: return 8;  case 44100: return 9;
    case 48000: return 10; case 96000: return 11; default: return 0;
    }
}

static unsigned size_code(unsigned b)
{
    switch (b) { case 8: return 1; case 12: return 2; case 16: return 4;
                 case 20: return 5; case 24: return 6; default: return 7; }
}

static bool frame(flacenc_t *e, unsigned n)
{
    bw_t w = { e->out, 0, 0 };
    unsigned bcode;
    switch (n) {
    case 192: bcode = 1; break;
    case 576: bcode = 2; break;  case 1152: bcode = 3; break;
    case 2304: bcode = 4; break; case 4608: bcode = 5; break;
    case 256: bcode = 8; break;  case 512: bcode = 9; break;
    case 1024: bcode = 10; break; case 2048: bcode = 11; break;
    case 4096: bcode = 12; break;
    default: bcode = n <= 256 ? 6 : 7; break;
    }

    unsigned chan = e->ch - 1;
    const int32_t *a = e->buf[0], *b = e->buf[1];
    unsigned abps = e->bps, bbps = e->bps, oa = 0, ob = 0;
    const unsigned sh = e->bps - 16;
    if (e->ch == 2) {
        int32_t *L = e->buf[0], *R = e->buf[1], *M = e->buf[2], *S = e->buf[3];
        for (unsigned i = 0; i < n; i++) { M[i] = (L[i] + R[i]) >> 1; S[i] = L[i] - R[i]; }
        uint64_t s;
        uint64_t cl, cr, cm, cs;
        const unsigned ol = best_order(L, n, sh, &s); cl = est_bits(s, n, e->bps);
        const unsigned orr = best_order(R, n, sh, &s); cr = est_bits(s, n, e->bps);
        const unsigned om = best_order(M, n, sh, &s); cm = est_bits(s, n, e->bps);
        const unsigned os = best_order(S, n, sh, &s); cs = est_bits(s, n, e->bps + 1);
        uint64_t c = cl + cr;
        oa = ol; ob = orr;
        if (cl + cs < c) { c = cl + cs; chan = 8; a = L; b = S; bbps = e->bps + 1; oa = ol; ob = os; }
        if (cr + cs < c) { c = cr + cs; chan = 9; a = S; b = R; abps = e->bps + 1; bbps = e->bps; oa = os; ob = orr; }
        if (cm + cs < c) { c = cm + cs; chan = 10; a = M; b = S; abps = e->bps; bbps = e->bps + 1; oa = om; ob = os; }
    } else {
        uint64_t s;
        oa = best_order(a, n, sh, &s);
    }

    put(&w, 0xFFF8, 16);
    put(&w, (bcode << 4) | rate_code(e->rate), 8);
    put(&w, (chan << 4) | (size_code(e->bps) << 1), 8);
    const uint32_t fn = e->frame_no;
    if (fn < 0x80) put(&w, fn, 8);
    else {
        unsigned len = fn < 0x800 ? 2 : fn < 0x10000 ? 3 : fn < 0x200000 ? 4 : fn < 0x4000000 ? 5 : 6;
        put(&w, ((0xFF00u >> len) & 0xFF) | (fn >> (6 * (len - 1))), 8);
        for (unsigned i = len - 1; i > 0; i--) put(&w, 0x80 | ((fn >> (6 * (i - 1))) & 0x3F), 8);
    }
    if (bcode == 6) put(&w, n - 1, 8);
    else if (bcode == 7) put(&w, n - 1, 16);
    put(&w, crc8(e->out, (size_t)(w.p - e->out)), 8);

    subframe(e, &w, a, n, abps, oa);
    if (e->ch == 2) subframe(e, &w, b, n, bbps, ob);
    if (w.fill) put(&w, 0, 8 - w.fill);
    const uint16_t c16 = crc16(e->out, (size_t)(w.p - e->out));
    put(&w, c16, 16);

    const uint32_t len = (uint32_t)(w.p - e->out);
    if (!e->minfs || len < e->minfs) e->minfs = len;
    if (len > e->maxfs) e->maxfs = len;
    e->frame_no++;
    e->total += n;
    return e->wr(e->ctx, e->out, len);
}

/* ---- the stream ---- */

static void streaminfo(const flacenc_t *e, uint8_t *o)
{
    bw_t w = { o, 0, 0 };
    put(&w, e->bs, 16);
    put(&w, e->bs, 16);
    put(&w, e->minfs, 24);
    put(&w, e->maxfs, 24);
    put(&w, e->rate, 20);
    put(&w, e->ch - 1, 3);
    put(&w, e->bps - 1, 5);
    put(&w, (uint32_t)(e->total >> 32) & 0xF, 4);
    putw(&w, (uint32_t)e->total, 32);
    memset(w.p, 0, 16);                             /* MD5 not computed */
}

flacenc_t *flacenc_open(unsigned channels, unsigned bps, unsigned rate,
                        unsigned blocksize, flacenc_write_fn wr, void *ctx)
{
    if (channels < 1 || channels > 2 || (bps != 16 && bps != 24) ||
        blocksize < 16 || blocksize > FLACENC_MAX_BLOCK || !rate || rate > 655350) return NULL;
    flacenc_t *e = fe_alloc(sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    e->ch = channels; e->bps = bps; e->rate = rate; e->bs = blocksize;
    e->wr = wr; e->ctx = ctx;
    const unsigned nbuf = channels == 2 ? 4 : 1;
    const size_t out_cap = 18 + channels * (((bps + 1) * blocksize + 7) / 8 + 8) + 2;
    for (unsigned i = 0; i < nbuf; i++) e->buf[i] = fe_alloc(blocksize * sizeof(int32_t));
    e->res = fe_alloc(blocksize * sizeof(int32_t));
    e->out = fe_alloc(out_cap);
    bool ok = e->res && e->out;
    for (unsigned i = 0; i < nbuf; i++) ok = ok && e->buf[i];
    if (!ok) { flacenc_close(e, NULL); return NULL; }
    if (!s_crc16[1]) crc16_init();

    uint8_t hdr[8 + FLACENC_STREAMINFO_BYTES] = { 'f', 'L', 'a', 'C', 0x80, 0, 0, FLACENC_STREAMINFO_BYTES };
    streaminfo(e, hdr + 8);
    if (!wr(ctx, hdr, sizeof(hdr))) { flacenc_close(e, NULL); return NULL; }
    return e;
}

bool flacenc_write(flacenc_t *e, const int32_t *pcm, unsigned frames)
{
    while (frames) {
        unsigned k = e->bs - e->fill;
        if (k > frames) k = frames;
        if (e->ch == 2) {
            int32_t *L = e->buf[0] + e->fill, *R = e->buf[1] + e->fill;
            for (unsigned i = 0; i < k; i++) { L[i] = pcm[2 * i]; R[i] = pcm[2 * i + 1]; }
        } else {
            memcpy(e->buf[0] + e->fill, pcm, k * sizeof(int32_t));
        }
        pcm += k * e->ch; frames -= k; e->fill += k;
        if (e->fill == e->bs) {
            e->fill = 0;
            if (!frame(e, e->bs)) return false;
        }
    }
    return true;
}

bool flacenc_close(flacenc_t *e, uint8_t si[FLACENC_STREAMINFO_BYTES])
{
    if (!e) return false;
    bool ok = true;
    if (si && e->fill) ok = frame(e, e->fill);
    if (si) streaminfo(e, si);
    for (unsigned i = 0; i < 4; i++) if (e->buf[i]) fe_free(e->buf[i]);
    if (e->res) fe_free(e->res);
    if (e->out) fe_free(e->out);
    fe_free(e);
    return ok;
}
