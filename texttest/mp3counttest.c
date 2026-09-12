/*
 * mp3counttest.c -- MPEG frame lengths, against arithmetic done twice.
 *
 * The point of interest is that an MP3 header does not carry its frame
 * length. A wrong table entry does not produce a rejected frame, it
 * produces a frame of the *wrong length*, the stream desynchronises, and
 * the damage appears as bytes lost hunting for a sync -- which is the
 * exact figure the probe uses to certify that icydemux and the ring are
 * not losing data. A bad table here would frame the ring for a fault it
 * did not commit.
 *
 * So the frame lengths below are computed independently, from the
 * published formula written out longhand, rather than compared against
 * the same tables the header uses. And the headline property is the same
 * one the AAC path gets for free: a synthetic stream of known frames
 * must count every frame and lose zero bytes, fed in pieces of any size.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codecplan.h"
#include "mp3count.h"
#include "streamsniff.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do {                           \
    checks++;                                           \
    if (!(cond)) {                                      \
        failures++;                                     \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
    }                                                   \
} while (0)

/* ------------------------------------------------------------------ */
/* Building headers                                                    */
/* ------------------------------------------------------------------ */

/* vbits: 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5. lbits: 3 = L1, 2 = L2, 1 = L3. */
static void hdr(uint8_t *h, unsigned vbits, unsigned lbits, unsigned bi,
                unsigned sri, unsigned pad, unsigned mono)
{
    h[0] = 0xFF;
    h[1] = (uint8_t)(0xE0 | (vbits << 3) | (lbits << 1) | 1 /* no CRC */);
    h[2] = (uint8_t)((bi << 4) | (sri << 2) | (pad << 1));
    h[3] = (uint8_t)(mono ? (3 << 6) : (1 << 6));
}

/* The formula, written out longhand, independent of mp3count.h's tables. */
static unsigned want_len(unsigned version, unsigned layer, unsigned kbps,
                         unsigned srate, unsigned pad)
{
    if (layer == 1) return (12 * kbps * 1000 / srate + pad) * 4;
    const unsigned spf = (layer == 3 && version != 1) ? 576 : 1152;
    return (spf / 8) * kbps * 1000 / srate + pad;
}

/* ------------------------------------------------------------------ */
/* A synthetic stream of real frames                                   */
/* ------------------------------------------------------------------ */

typedef struct { uint8_t *b; size_t len, cap; } buf_t;

static void put(buf_t *s, const void *p, size_t n)
{
    if (s->len + n > s->cap) { s->cap = (s->len + n) * 2 + 64; s->b = realloc(s->b, s->cap); }
    memcpy(s->b + s->len, p, n);
    s->len += n;
}

/* One frame: header, then len-4 bytes of body that deliberately contain
 * 0xFF bytes -- a counter that trusted sync alone would trip on them. */
static void put_frame(buf_t *s, const uint8_t *h, unsigned len)
{
    put(s, h, 4);
    uint8_t *body = malloc(len - 4);
    for (unsigned i = 0; i < len - 4; i++) {
        body[i] = (uint8_t)((i % 7 == 0) ? 0xFF : (i * 31 + 5));
    }
    put(s, body, len - 4);
    free(body);
}

int main(void)
{
    printf("mp3counttest\n");

    /* ---------------------------------------------------------------- */
    /* Frame lengths against the longhand formula, every combination     */
    /* ---------------------------------------------------------------- */
    {
        static const unsigned vb[3] = { 3, 2, 0 };          /* MPEG1, 2, 2.5 */
        static const unsigned vnum[3] = { 1, 2, 25 };
        static const unsigned srtab[4][3] = {
            { 11025, 12000, 8000 }, { 0, 0, 0 },
            { 22050, 24000, 16000 }, { 44100, 48000, 32000 },
        };
        static const unsigned br1[4][16] = {
            { 0 },
            { 0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0 },
            { 0,32,48,56, 64, 80, 96,112,128,160,192,224,256,320,384,0 },
            { 0,32,40,48, 56, 64, 80, 96,112,128,160,192,224,256,320,0 },
        };
        static const unsigned br2[4][16] = {
            { 0 },
            { 0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0 },
            { 0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0 },
            { 0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0 },
        };

        int combos = 0;
        for (int v = 0; v < 3; v++) {
            for (unsigned lbits = 1; lbits <= 3; lbits++) {
                const unsigned layer = 4 - lbits;
                for (unsigned bi = 1; bi <= 14; bi++) {
                    for (unsigned sri = 0; sri < 3; sri++) {
                        for (unsigned pad = 0; pad <= 1; pad++) {
                            const unsigned srate = srtab[vb[v]][sri];
                            const unsigned kbps = (vnum[v] == 1)
                                                ? br1[layer][bi] : br2[layer][bi];
                            if (!srate || !kbps) continue;

                            uint8_t h[4];
                            hdr(h, vb[v], lbits, bi, sri, pad, 0);
                            unsigned spf = 0, rate = 0, got_br = 0, ver = 0, lay = 0, ch = 0;
                            const unsigned got = mp3_frame_len(h, &spf, &rate,
                                                               &got_br, &ver, &lay, &ch);
                            const unsigned want = want_len(vnum[v], layer, kbps, srate, pad);

                            CHECK(got == want,
                                  "MPEG%u L%u %u kbit/s %u Hz pad %u: len %u, wanted %u",
                                  vnum[v], layer, kbps, srate, pad, got, want);
                            CHECK(rate == srate, "rate %u, wanted %u", rate, srate);
                            CHECK(got_br == kbps, "bitrate %u, wanted %u", got_br, kbps);
                            CHECK(ver == vnum[v], "version %u, wanted %u", ver, vnum[v]);
                            CHECK(lay == layer, "layer %u, wanted %u", lay, layer);
                            CHECK(spf == ((layer == 1) ? 384
                                        : ((layer == 3 && vnum[v] != 1) ? 576 : 1152)),
                                  "MPEG%u L%u: %u samples a frame", vnum[v], layer, spf);
                            combos++;
                        }
                    }
                }
            }
        }
        printf("  %d header combinations checked against the longhand formula\n", combos);
        CHECK(combos > 400, "only %d combinations covered", combos);
    }

    /* The specific frame a 128 kbit/s 44.1 kHz MPEG1 Layer III station
     * sends, which is what an ordinary station is: 417 bytes, 418 padded,
     * 1152 samples, 26.12 ms. */
    {
        uint8_t h[4];
        hdr(h, 3, 1, 9, 0, 0, 0);       /* MPEG1 L3 128 kbit/s 44100 */
        unsigned spf = 0, rate = 0, br = 0, v = 0, l = 0, ch = 0;
        CHECK(mp3_frame_len(h, &spf, &rate, &br, &v, &l, &ch) == 417,
              "128k/44.1k frame is %u bytes, wanted 417",
              mp3_frame_len(h, &spf, &rate, &br, &v, &l, &ch));
        CHECK(spf == 1152 && rate == 44100 && br == 128 && v == 1 && l == 3,
              "128k/44.1k: %u spf, %u Hz, %u kbit/s, MPEG%u L%u", spf, rate, br, v, l);
        CHECK(ch == 2, "joint stereo read as %u channels", ch);
        hdr(h, 3, 1, 9, 0, 1, 0);
        CHECK(mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, NULL) == 418,
              "the padded frame is not 418");
        hdr(h, 3, 1, 9, 0, 0, 1);
        mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, &ch);
        CHECK(ch == 1, "mono read as %u channels", ch);
    }

    /* ---------------------------------------------------------------- */
    /* Refused headers                                                   */
    /* ---------------------------------------------------------------- */
    {
        uint8_t h[4];
        hdr(h, 3, 1, 9, 0, 0, 0);
        CHECK(mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, NULL) != 0, "setup");

        h[0] = 0xFE;
        CHECK(!mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, NULL), "bad first sync byte accepted");
        hdr(h, 3, 1, 9, 0, 0, 0); h[1] &= 0x1F;
        CHECK(!mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, NULL), "bad second sync byte accepted");
        hdr(h, 1, 1, 9, 0, 0, 0);       /* reserved version */
        CHECK(!mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, NULL), "reserved version accepted");
        hdr(h, 3, 0, 9, 0, 0, 0);       /* reserved layer */
        CHECK(!mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, NULL), "reserved layer accepted");
        hdr(h, 3, 1, 0, 0, 0, 0);       /* free format */
        CHECK(!mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, NULL),
              "free format accepted -- its length is not in the header");
        hdr(h, 3, 1, 15, 0, 0, 0);      /* reserved bitrate */
        CHECK(!mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, NULL), "reserved bitrate accepted");
        hdr(h, 3, 1, 9, 3, 0, 0);       /* reserved sample rate */
        CHECK(!mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, NULL), "reserved rate accepted");
        CHECK(!mp3_frame_len(NULL, NULL, NULL, NULL, NULL, NULL, NULL), "NULL accepted");
    }

    /* ---------------------------------------------------------------- */
    /* The headline property: a known stream, no bytes lost, any pieces  */
    /* ---------------------------------------------------------------- */
    {
        buf_t s = { 0 };
        uint8_t h[4];
        hdr(h, 3, 1, 9, 0, 0, 0);               /* 128 kbit/s 44.1 kHz L3 */
        const unsigned len = 417;
        const int frames = 200;
        for (int i = 0; i < frames; i++) {
            /* Alternate the padding bit, as a real encoder does to keep
             * 44.1 kHz on rate: 417, 418, 417, ... */
            hdr(h, 3, 1, 9, 0, (unsigned)(i & 1), 0);
            put_frame(&s, h, len + (unsigned)(i & 1));
        }

        static const size_t pieces[] = { 0, 1, 2, 3, 7, 417, 418, 1000, 2048, 65536 };
        for (size_t p = 0; p < sizeof(pieces) / sizeof(pieces[0]); p++) {
            mp3_count_t c;
            memset(&c, 0, sizeof(c));
            for (size_t i = 0; i < s.len; ) {
                size_t take = s.len - i;
                if (pieces[p] && take > pieces[p]) take = pieces[p];
                mp3_count_bytes(&c, s.b + i, take);
                i += take;
            }
            CHECK(c.frames == (uint32_t)frames,
                  "piece %zu: %u frames, wanted %d", pieces[p], c.frames, frames);
            CHECK(c.lost == 0,
                  "piece %zu: %llu bytes lost on a stream of real frames -- "
                  "this is the figure the probe uses to certify the ring",
                  pieces[p], (unsigned long long)c.lost);
            CHECK(c.rate == 44100 && c.bitrate == 128 && c.layer == 3 && c.version == 1,
                  "piece %zu: %u Hz, %u kbit/s, MPEG%u L%u",
                  pieces[p], c.rate, c.bitrate, c.version, c.layer);
            CHECK(c.min_len == 417 && c.max_len == 418,
                  "piece %zu: lengths %u..%u, wanted 417..418",
                  pieces[p], c.min_len, c.max_len);
            /* 200 frames x 1152 samples / 44100 = 5224 ms. */
            CHECK(mp3_ms(&c) == (uint64_t)frames * 1152 * 1000 / 44100,
                  "piece %zu: %llu ms, wanted %llu", pieces[p],
                  (unsigned long long)mp3_ms(&c),
                  (unsigned long long)((uint64_t)frames * 1152 * 1000 / 44100));
        }
        free(s.b);
    }

    /* Mid-frame start, which is what a live stream gives: the bytes
     * before the first sync are lost and counted, and everything after
     * is counted correctly. */
    {
        buf_t s = { 0 };
        uint8_t junk[100];
        for (int i = 0; i < 100; i++) junk[i] = (uint8_t)(i * 13 + 1);
        put(&s, junk, sizeof(junk));
        uint8_t h[4];
        hdr(h, 3, 1, 9, 0, 0, 0);
        for (int i = 0; i < 20; i++) put_frame(&s, h, 417);

        mp3_count_t c;
        memset(&c, 0, sizeof(c));
        mp3_count_bytes(&c, s.b, s.len);
        CHECK(c.frames == 20, "%u frames after a mid-frame start", c.frames);
        CHECK(c.lost >= 90 && c.lost <= 110,
              "%llu bytes lost on a 100-byte junk prefix",
              (unsigned long long)c.lost);
        free(s.b);
    }

    /* ---------------------------------------------------------------- */
    /* Random bytes: no crash, and nothing that claims to be much audio  */
    /* ---------------------------------------------------------------- */
    {
        srand(20260911);
        uint64_t total_frames = 0, total_bytes = 0;
        for (int iter = 0; iter < 3000; iter++) {
            uint8_t b[512];
            const size_t n = 1 + (size_t)(rand() % sizeof(b));
            for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(rand() % 256);
            mp3_count_t c;
            memset(&c, 0, sizeof(c));
            mp3_count_bytes(&c, b, n);
            CHECK(c.lost + c.samples / 8 <= n + 4096, "iter %d: accounting ran away", iter);
            total_frames += c.frames;
            total_bytes += n;
        }
        /* Eleven bits of sync will hit sometimes -- that is why the
         * counter exists and why "bytes lost" is the signal rather than
         * "frames found". What must not happen is random data parsing as
         * a mostly-valid stream. */
        printf("  random data: %llu frames claimed over %llu bytes (%.2f%%)\n",
               (unsigned long long)total_frames, (unsigned long long)total_bytes,
               100.0 * (double)total_frames * 417.0 / (double)total_bytes);
        CHECK((double)total_frames * 417.0 < (double)total_bytes * 0.25,
              "random bytes parsed as %.0f%% valid frames",
              100.0 * (double)total_frames * 417.0 / (double)total_bytes);
    }

    /* ---------------------------------------------------------------- */
    /* MP3 data must not be counted as ADTS                              */
    /*                                                                   */
    /* A real run picked the ADTS counter on an MP3 station because the   */
    /* probe tested `adts.frames != 0` and ADTS false-positives on MP3    */
    /* data: 93 "frames" at 88200 Hz with 0 channels and 442947 bytes     */
    /* lost. Choosing per-window then made the audio clock jump between   */
    /* two counters, go backwards, and wrap an unsigned subtraction --    */
    /* the log printed 18446744073709544819 ms of audio.                  */
    /*                                                                   */
    /* So: on real MP3 frames, the sniffer and codecplan must both say    */
    /* MP3, and the ADTS counter must look as wrong as it is.             */
    /* ---------------------------------------------------------------- */
    {
        buf_t s = { 0 };
        uint8_t h[4];
        for (int i = 0; i < 300; i++) {
            hdr(h, 3, 1, 9, 0, (unsigned)(i & 1), 0);
            put_frame(&s, h, 417 + (unsigned)(i & 1));
        }

        CHECK(sniff_bytes(s.b, s.len) == SNIFF_MP3,
              "the sniffer did not recognise real MP3 frames");

        const codecplan_t plan = codecplan_choose(sniff_bytes(s.b, s.len),
                                                  s.len, "audio/mpeg");
        CHECK(plan.codec == STREAM_CODEC_MP3,
              "codecplan chose %s for MP3 frames", stream_codec_name(plan.codec));
        CHECK(plan.why == CODECPLAN_FROM_BYTES, "chosen from %d, not the bytes",
              (int)plan.why);

        mp3_count_t m;
        memset(&m, 0, sizeof(m));
        mp3_count_bytes(&m, s.b, s.len);
        CHECK(m.frames == 300 && m.lost == 0,
              "the MP3 counter: %u frames, %llu lost",
              m.frames, (unsigned long long)m.lost);

        adts_count_t a;
        memset(&a, 0, sizeof(a));
        adts_count_bytes(&a, s.b, s.len);
        /*
         * It is allowed to find junk -- eleven bits of sync guarantee it
         * will. The finding worth recording is that **its own numbers do
         * not reliably give it away.**
         *
         * The first version of this test asserted `a.lost > s.len / 2`,
         * on the strength of the real station's 442947 bytes lost out of
         * 803 KB, about 54%. On this synthetic stream it loses 4.5%.
         * The reason is that a bogus frame length makes the ADTS counter
         * *skip* a large block, and skipped bytes are counted as a frame
         * body rather than as lost -- which is exactly why the real run
         * reported frame lengths of "25-8187 bytes". How much it appears
         * to lose therefore depends on what the audio data happens to
         * look like.
         *
         * So a selector must not be built on these figures at all. The
         * decision belongs to the sniffer and codecplan, asserted above.
         * What is checked here is only the weaker, robust thing: the
         * wrong counter finds an order of magnitude fewer frames, and it
         * reports a channel count that is not a channel count.
         */
        CHECK(a.frames * 5 < m.frames,
              "ADTS claimed %u frames against MPEG's %u on the same bytes",
              a.frames, m.frames);
        CHECK(a.channels == 0 || a.channels > 2 || a.rate != m.rate,
              "ADTS's junk header looked plausible: %u Hz, %u channels",
              a.rate, a.channels);
        printf("  MP3 data: MPEG %u frames / %llu lost, ADTS %u frames / %llu lost\n",
               m.frames, (unsigned long long)m.lost,
               a.frames, (unsigned long long)a.lost);
        free(s.b);
    }

    /* A zeroed counter reports nothing rather than dividing by zero. */
    {
        mp3_count_t c;
        memset(&c, 0, sizeof(c));
        CHECK(mp3_ms(&c) == 0, "an empty counter reported %llu ms",
              (unsigned long long)mp3_ms(&c));
        CHECK(mp3_ms(NULL) == 0, "NULL reported time");
        mp3_count_bytes(NULL, (const uint8_t *)"x", 1);
        mp3_count_bytes(&c, NULL, 1);
        CHECK(c.frames == 0, "NULL input produced frames");
    }

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
