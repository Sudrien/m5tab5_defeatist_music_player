/*
 * flacenctest.c -- main/flacenc.c, compiled as is, against a decoder
 * written here from the format rather than from the encoder.
 *
 * The property is losslessness: whatever goes in comes back, sample for
 * sample, through a reader that checks every sync code, CRC-8 and
 * CRC-16 and refuses anything the encoder is not supposed to emit. The
 * decoder shares no code with the encoder -- its CRCs are bitwise where
 * the encoder's CRC-16 is a table, its bit reader is its own -- so a
 * bug has to be made twice, the same way, to pass.
 *
 * On the development machine the same streams were also checked with
 * the reference `flac -t` and decoded bit-exact by it (5104). That tool
 * is not assumed here.
 *
 * The signals are the ones that reach each branch: digital silence
 * (CONSTANT), full-scale noise (VERBATIM), left and right at opposite
 * rails (the widest side channel, one bit over the sample size), smooth
 * material (FIXED at order 2-4, mid/side), and lengths that end in a
 * partial block of 1, 5 and odd sizes.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "flacenc.h"

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

/* ---- the file, in memory ---- */

static uint8_t *s_file;
static size_t s_len, s_cap;
static int s_refuse_after = -1;     /* fail the Nth write, for the error path */
static int s_writes;

static bool sink(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (s_refuse_after >= 0 && s_writes++ >= s_refuse_after) return false;
    if (s_len + n > s_cap) {
        s_cap = (s_len + n) * 2;
        s_file = realloc(s_file, s_cap);
    }
    memcpy(s_file + s_len, b, n);
    s_len += n;
    return true;
}

/* ---- a decoder, from the format ---- */

typedef struct { const uint8_t *p; size_t len, pos; bool bad; } br_t;   /* pos in bits */

static uint32_t rd(br_t *r, unsigned n)
{
    uint32_t v = 0;
    while (n--) {
        if (r->pos >= r->len * 8) { r->bad = true; return 0; }
        v = (v << 1) | ((r->p[r->pos >> 3] >> (7 - (r->pos & 7))) & 1);
        r->pos++;
    }
    return v;
}

static int32_t rds(br_t *r, unsigned n)
{
    uint32_t v = rd(r, n);
    if (n < 32 && (v >> (n - 1))) v |= ~0u << n;
    return (int32_t)v;
}

static uint8_t crc8_bits(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            const int top = ((c >> 7) ^ (p[i] >> b)) & 1;
            c = (uint8_t)(c << 1);
            if (top) c ^= 0x07;
        }
    return c;
}

static uint16_t crc16_bits(const uint8_t *p, size_t n)
{
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            const int top = ((c >> 15) ^ (p[i] >> b)) & 1;
            c = (uint16_t)(c << 1);
            if (top) c ^= 0x8005;
        }
    return c;
}

static const unsigned k_rates[12] = { 0, 88200, 176400, 192000, 8000, 16000, 22050,
                                      24000, 32000, 44100, 48000, 96000 };
static const unsigned k_bps[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };

typedef struct {
    unsigned rate, ch, bps, bs, minfs, maxfs;
    uint64_t total;
    bool md5_zero;
} info_t;

static bool subframe_dec(br_t *r, int32_t *x, unsigned n, unsigned sbps)
{
    if (rd(r, 1)) return false;                     /* zero pad */
    const unsigned type = rd(r, 6);
    if (rd(r, 1)) return false;                     /* wasted bits: never emitted */
    if (type == 0) {
        const int32_t v = rds(r, sbps);
        for (unsigned i = 0; i < n; i++) x[i] = v;
        return !r->bad;
    }
    if (type == 1) {
        for (unsigned i = 0; i < n; i++) x[i] = rds(r, sbps);
        return !r->bad;
    }
    if (type < 8 || type > 12) return false;        /* only FIXED 0-4 besides */
    const unsigned o = type - 8;
    for (unsigned i = 0; i < o; i++) x[i] = rds(r, sbps);
    const unsigned meth = rd(r, 2);
    if (meth > 1) return false;
    const unsigned po = rd(r, 4), np = 1u << po;
    if ((n >> po) < o || (n % np)) return false;
    unsigned i = o;
    for (unsigned p = 0; p < np; p++) {
        const unsigned k = rd(r, meth ? 5 : 4);
        if (k == (meth ? 31u : 15u)) return false;  /* escape: never emitted */
        const unsigned cnt = (n >> po) - (p ? 0 : o);
        for (unsigned j = 0; j < cnt; j++, i++) {
            uint32_t q = 0;
            while (!rd(r, 1)) { if (r->bad || ++q > (1u << 30)) return false; }
            const uint32_t u = (q << k) | (k ? rd(r, k) : 0);
            const int32_t e = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
            int64_t pred = 0;
            switch (o) {
            case 1: pred = x[i - 1]; break;
            case 2: pred = 2LL * x[i - 1] - x[i - 2]; break;
            case 3: pred = 3LL * x[i - 1] - 3LL * x[i - 2] + x[i - 3]; break;
            case 4: pred = 4LL * x[i - 1] - 6LL * x[i - 2] + 4LL * x[i - 3] - x[i - 4]; break;
            }
            x[i] = (int32_t)(pred + e);
        }
    }
    return !r->bad;
}

/* Decodes the whole file into out (interleaved); returns frames or -1. */
static long decode(const uint8_t *f, size_t len, info_t *in, int32_t *out, long cap)
{
    if (len < 42 || memcmp(f, "fLaC", 4) || f[4] != 0x80 ||
        f[5] || f[6] || f[7] != FLACENC_STREAMINFO_BYTES) return -1;
    br_t h = { f + 8, 34, 0, false };
    const unsigned minbs = rd(&h, 16), maxbs = rd(&h, 16);
    in->minfs = rd(&h, 24); in->maxfs = rd(&h, 24);
    in->rate = rd(&h, 20); in->ch = rd(&h, 3) + 1; in->bps = rd(&h, 5) + 1;
    in->total = (uint64_t)rd(&h, 4) << 32; in->total |= rd(&h, 32);
    in->md5_zero = true;
    for (int i = 0; i < 16; i++) if (f[26 + i]) in->md5_zero = false;
    in->bs = maxbs;
    if (minbs != maxbs) return -1;

    static int32_t ch[4][FLACENC_MAX_BLOCK];
    size_t pos = 42;
    long frames = 0;
    uint32_t fno = 0;
    while (pos < len) {
        br_t r = { f + pos, len - pos, 0, false };
        if (rd(&r, 16) != 0xFFF8) return -1;
        const unsigned bcode = rd(&r, 4), rcode = rd(&r, 4);
        const unsigned chan = rd(&r, 4), scode = rd(&r, 3);
        if (rd(&r, 1)) return -1;
        /* frame number, UTF-8 */
        uint32_t v = rd(&r, 8);
        unsigned extra = 0;
        if (v >= 0x80) {
            while (v & (0x80 >> (extra + 1))) extra++;
            v &= 0x3F >> extra;
            for (unsigned i = 0; i < extra; i++) {
                const uint32_t c = rd(&r, 8);
                if ((c & 0xC0) != 0x80) return -1;
                v = (v << 6) | (c & 0x3F);
            }
        }
        if (v != fno) return -1;
        unsigned n;
        switch (bcode) {
        case 1: n = 192; break;
        case 2: case 3: case 4: case 5: n = 576u << (bcode - 2); break;
        case 6: n = rd(&r, 8) + 1; break;
        case 7: n = rd(&r, 16) + 1; break;
        default: if (bcode < 8) return -1; n = 256u << (bcode - 8); break;
        }
        if (rcode >= 12 || (rcode && k_rates[rcode] != in->rate)) return -1;
        if (k_bps[scode] != in->bps) return -1;
        const size_t hbytes = r.pos / 8;
        if (crc8_bits(f + pos, hbytes) != rd(&r, 8)) return -1;
        if (n > in->bs) return -1;

        const unsigned nch = chan < 8 ? chan + 1 : 2;
        if (nch != in->ch || chan > 10) return -1;
        for (unsigned c = 0; c < nch; c++) {
            const bool side = (chan == 8 && c == 1) || (chan == 9 && c == 0) || (chan == 10 && c == 1);
            if (!subframe_dec(&r, ch[c], n, in->bps + side)) return -1;
        }
        if (r.pos & 7) { if (rd(&r, 8 - (r.pos & 7))) return -1; }
        const size_t body = r.pos / 8;
        if (crc16_bits(f + pos, body) != rd(&r, 16) || r.bad) return -1;
        const size_t flen = r.pos / 8;
        if (flen < in->minfs || flen > in->maxfs) return -1;

        if (frames + (long)n > cap) return -1;
        for (unsigned i = 0; i < n; i++) {
            int32_t L = ch[0][i], R = nch == 2 ? ch[1][i] : 0;
            if (chan == 8) R = L - ch[1][i];
            else if (chan == 9) { R = ch[1][i]; L = ch[0][i] + R; }
            else if (chan == 10) {
                const int32_t S = ch[1][i];
                const int32_t M = (int32_t)(((uint32_t)ch[0][i] << 1) | (uint32_t)(S & 1));
                L = (M + S) >> 1; R = (M - S) >> 1;
            }
            out[(frames + i) * nch] = L;
            if (nch == 2) out[(frames + i) * 2 + 1] = R;
        }
        frames += n;
        pos += flen;
        fno++;
    }
    return frames;
}

/* ---- signals ---- */

static uint32_t s_seed = 1;
static int32_t rnd(int32_t lo, int32_t hi)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    const uint32_t span = (uint32_t)(hi - lo) + 1u;
    return lo + (int32_t)(span ? (s_seed >> 1) % span : s_seed);
}

enum { SIG_SILENCE, SIG_NOISE, SIG_RAILS, SIG_SMOOTH, SIG_MIXED, SIG_COUNT };
static const char *k_sig[SIG_COUNT] = { "silence", "noise", "rails", "smooth", "mixed" };

static void make(int sig, unsigned ch, unsigned bps, long frames, int32_t *pcm)
{
    const int32_t mx = (1 << (bps - 1)) - 1, mn = -mx - 1;
    for (long i = 0; i < frames; i++)
        for (unsigned c = 0; c < ch; c++) {
            int32_t v = 0;
            int s = sig == SIG_MIXED ? (int)((i / 3000) % 4) : sig;
            switch (s) {
            case SIG_SILENCE: v = 0; break;
            case SIG_NOISE:   v = rnd(mn, mx); break;
            case SIG_RAILS:   v = ((i / 7) & 1) ^ c ? mx : mn; break;
            case SIG_SMOOTH:  v = (int32_t)(mx * 0.7 * sin(i * 0.013 * (c + 1)))
                                  + rnd(-(mx >> 12), mx >> 12); break;
            }
            pcm[i * ch + c] = v;
        }
}

/* ---- the round trip ---- */

static void round_trip(int sig, unsigned ch, unsigned bps, unsigned rate,
                       unsigned bs, long frames, unsigned chunk)
{
    int32_t *pcm = malloc(sizeof(int32_t) * (size_t)(frames ? frames : 1) * ch);
    int32_t *back = malloc(sizeof(int32_t) * (size_t)(frames + 1) * ch);
    make(sig, ch, bps, frames, pcm);

    s_len = 0; s_writes = 0; s_refuse_after = -1;
    flacenc_t *e = flacenc_open(ch, bps, rate, bs, sink, NULL);
    CHECK(e != NULL, "open %s ch=%u bps=%u bs=%u", k_sig[sig], ch, bps, bs);
    if (!e) { free(pcm); free(back); return; }
    CHECK(s_len == 42, "header is 42 bytes before any audio, got %zu", s_len);
    bool ok = true;
    for (long f = 0; f < frames && ok; ) {
        unsigned k = (unsigned)(frames - f < (long)chunk ? frames - f : chunk);
        ok = flacenc_write(e, pcm + f * ch, k);
        f += k;
    }
    uint8_t si[FLACENC_STREAMINFO_BYTES];
    ok = flacenc_close(e, si) && ok;
    CHECK(ok, "encode %s", k_sig[sig]);
    memcpy(s_file + FLACENC_STREAMINFO_OFFSET, si, sizeof(si));

    info_t in;
    const long got = decode(s_file, s_len, &in, back, frames + 1);
    CHECK(got == frames, "%s ch=%u bps=%u bs=%u chunk=%u: decoded %ld of %ld frames",
          k_sig[sig], ch, bps, bs, chunk, got, frames);
    if (got == frames) {
        long bad = -1;
        for (long i = 0; i < frames * (long)ch; i++) if (back[i] != pcm[i]) { bad = i; break; }
        CHECK(bad < 0, "%s ch=%u bps=%u bs=%u: sample %ld is %d, was %d",
              k_sig[sig], ch, bps, bs, bad, bad < 0 ? 0 : back[bad], bad < 0 ? 0 : pcm[bad]);
        CHECK(in.total == (uint64_t)frames && in.rate == rate && in.ch == ch &&
              in.bps == bps && in.bs == bs && in.md5_zero,
              "STREAMINFO says %llu frames, %u Hz, %u ch, %u bit, bs %u",
              (unsigned long long)in.total, in.rate, in.ch, in.bps, in.bs);
    }
    free(pcm);
    free(back);
}

int main(void)
{
    printf("flacenctest\n");

    printf("  every signal, both widths, mono and stereo\n");
    for (int sig = 0; sig < SIG_COUNT; sig++)
        for (unsigned bps = 16; bps <= 24; bps += 8)
            for (unsigned ch = 1; ch <= 2; ch++)
                round_trip(sig, ch, bps, 48000, 4096, 20000, 1000);

    printf("  block sizes, with and without a header code\n");
    static const unsigned bss[] = { 16, 192, 576, 1000, 1152, 4096 };
    for (unsigned i = 0; i < sizeof(bss) / sizeof(bss[0]); i++) {
        round_trip(SIG_MIXED, 2, 24, 48000, bss[i], 13001, 777);
        round_trip(SIG_MIXED, 1, 16, 44100, bss[i], 13001, 4096);
    }

    printf("  a last block of 1, 5 and nothing; no audio at all\n");
    round_trip(SIG_SMOOTH, 2, 16, 44100, 4096, 4097, 4096);
    round_trip(SIG_SMOOTH, 2, 16, 44100, 4096, 4101, 13);
    round_trip(SIG_SMOOTH, 2, 16, 44100, 4096, 8192, 1);
    round_trip(SIG_SMOOTH, 1, 24, 48000, 1152, 0, 1);

    printf("  rates without a header code, and frame numbers past one byte\n");
    round_trip(SIG_SMOOTH, 2, 24, 11025, 1152, 1152 * 3, 500);
    round_trip(SIG_MIXED, 1, 16, 16000, 16, 16 * 3000, 4096);   /* 3000 frames: 2-byte UTF-8 */

    printf("  smooth stereo compresses; noise does not grow\n");
    {
        int32_t *pcm = malloc(sizeof(int32_t) * 48000 * 2);
        make(SIG_SMOOTH, 2, 24, 48000, pcm);
        s_len = 0; s_refuse_after = -1;
        flacenc_t *e = flacenc_open(2, 24, 48000, 4096, sink, NULL);
        flacenc_write(e, pcm, 48000);
        flacenc_close(e, (uint8_t[FLACENC_STREAMINFO_BYTES]){0});
        CHECK(s_len < 48000 * 6 * 2 / 3, "smooth: %zu bytes of %d", s_len, 48000 * 6);
        make(SIG_NOISE, 2, 24, 48000, pcm);
        s_len = 0;
        e = flacenc_open(2, 24, 48000, 4096, sink, NULL);
        flacenc_write(e, pcm, 48000);
        flacenc_close(e, (uint8_t[FLACENC_STREAMINFO_BYTES]){0});
        CHECK(s_len < 48000 * 6 + 48000 * 6 / 200, "noise: %zu bytes of %d", s_len, 48000 * 6);
        free(pcm);
    }

    printf("  what open refuses, and a writer that stops\n");
    CHECK(!flacenc_open(3, 16, 48000, 4096, sink, NULL), "three channels");
    CHECK(!flacenc_open(2, 20, 48000, 4096, sink, NULL), "20-bit");
    CHECK(!flacenc_open(2, 16, 48000, 4097, sink, NULL), "blocksize over the max");
    CHECK(!flacenc_open(2, 16, 48000, 15, sink, NULL), "blocksize under 16");
    CHECK(!flacenc_open(2, 16, 0, 4096, sink, NULL), "rate 0");
    s_len = 0; s_writes = 0; s_refuse_after = 0;
    CHECK(!flacenc_open(2, 16, 48000, 4096, sink, NULL), "header write refused");
    s_len = 0; s_writes = 0; s_refuse_after = 1;
    {
        int32_t pcm[2 * 100] = { 0 };
        flacenc_t *e = flacenc_open(2, 16, 48000, 64, sink, NULL);
        CHECK(e != NULL, "open with one write allowed");
        CHECK(!flacenc_write(e, pcm, 100), "a refused frame write is reported");
        flacenc_close(e, NULL);
    }
    s_refuse_after = -1;

    free(s_file);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
