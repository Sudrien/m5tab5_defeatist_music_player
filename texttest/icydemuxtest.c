/*
 * icydemuxtest.c -- the ICY demultiplexer, fed every way a socket can
 * cut it.
 *
 * The property this exists for is split-invariance: the same body fed in
 * 1-byte pieces, 3-byte pieces, 1000-byte pieces and one shot must give
 * byte-identical audio and the same titles. On hardware the reads are
 * 2048 bytes and metaint is 16000, so a length byte lands on a read
 * boundary about one time in eight and a metadata block is split about
 * as often -- often enough to happen on the first flash, rarely enough
 * that a wrong version would look fine for a minute first.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icydemux.h"

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
/* A synthetic body, built the way a station builds one                 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t   len, cap;
} body_t;

static void body_put(body_t *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->buf = realloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

/* `metaint` audio bytes of a known pattern, then a metadata block. */
static void body_segment(body_t *b, body_t *audio_expected, int metaint,
                         const char *meta, uint8_t *counter)
{
    for (int i = 0; i < metaint; i++) {
        const uint8_t v = (*counter)++;
        body_put(b, &v, 1);
        body_put(audio_expected, &v, 1);
    }
    if (!meta) {
        const uint8_t zero = 0;
        body_put(b, &zero, 1);
        return;
    }
    const size_t n = strlen(meta);
    const size_t blocks = (n + 15) / 16;
    const uint8_t l = (uint8_t)blocks;
    body_put(b, &l, 1);
    uint8_t pad[ICY_META_MAX];
    memset(pad, 0, sizeof(pad));
    memcpy(pad, meta, n);
    body_put(b, pad, blocks * 16);
}

/* Feed a body in fixed-size pieces; collect audio and the final title. */
static void run(const uint8_t *body, size_t len, int metaint, size_t piece,
                body_t *audio_out, char *title_out, uint32_t *titles_out)
{
    icydemux_t d;
    icydemux_init(&d, metaint);
    /* piece == 0 means one shot, so the scratch has to hold the whole
     * body -- audio out is never more than bytes in. */
    const size_t scratch_n = piece ? piece : len;
    uint8_t *scratch = malloc(scratch_n ? scratch_n : 1);
    for (size_t i = 0; i < len; ) {
        size_t take = len - i;
        if (piece && take > piece) take = piece;
        const size_t got = icydemux_feed(&d, body + i, take, scratch);
        body_put(audio_out, scratch, got);
        i += take;
    }
    free(scratch);
    strcpy(title_out, d.title);
    *titles_out = d.titles;
}

int main(void)
{
    printf("icydemuxtest\n");

    /* -------------------------------------------------------------- */
    /* 1. No metaint: everything is audio, and it is a memcpy          */
    /* -------------------------------------------------------------- */
    {
        icydemux_t d;
        icydemux_init(&d, 0);
        const uint8_t in[5] = { 1, 2, 3, 4, 5 };
        uint8_t out[5] = { 0 };
        const size_t n = icydemux_feed(&d, in, 5, out);
        CHECK(n == 5, "metaint 0 passed %zu of 5", n);
        CHECK(memcmp(in, out, 5) == 0, "metaint 0 changed the bytes");
        CHECK(d.titles == 0, "metaint 0 invented a title");
        CHECK(d.audio_bytes == 5, "metaint 0 counted %llu audio bytes",
              (unsigned long long)d.audio_bytes);
    }

    /* A negative or absurd metaint is refused into "no metadata" rather
     * than desynchronising a body it cannot parse. */
    {
        icydemux_t d;
        icydemux_init(&d, -1);
        CHECK(d.metaint == 0, "metaint -1 became %d", d.metaint);
        icydemux_init(&d, 1 << 25);
        CHECK(d.metaint == 0, "an absurd metaint became %d", d.metaint);
    }

    /* -------------------------------------------------------------- */
    /* 2. In place: out may be in                                      */
    /* -------------------------------------------------------------- */
    {
        icydemux_t d;
        icydemux_init(&d, 4);
        /* 4 audio, L=1, 16 metadata, 4 audio */
        uint8_t buf[4 + 1 + 16 + 4];
        memset(buf, 0, sizeof(buf));
        for (int i = 0; i < 4; i++) buf[i] = (uint8_t)(0xA0 + i);
        buf[4] = 1;
        memcpy(buf + 5, "StreamTitle='x';", 16);
        for (int i = 0; i < 4; i++) buf[21 + i] = (uint8_t)(0xB0 + i);

        const size_t n = icydemux_feed(&d, buf, sizeof(buf), buf);
        CHECK(n == 8, "in-place gave %zu audio bytes, wanted 8", n);
        CHECK(buf[0] == 0xA0 && buf[3] == 0xA3, "in-place lost the first run");
        CHECK(buf[4] == 0xB0 && buf[7] == 0xB3, "in-place lost the second run");
        CHECK(strcmp(d.title, "x") == 0, "in-place title \"%s\"", d.title);
    }

    /* -------------------------------------------------------------- */
    /* 3. Split-invariance, the reason this file exists                */
    /* -------------------------------------------------------------- */
    {
        const int metaint = 97;     /* small and coprime with every piece */
        body_t body = { 0 }, want = { 0 };
        uint8_t counter = 0;

        body_segment(&body, &want, metaint, "StreamTitle='One';StreamUrl='';", &counter);
        body_segment(&body, &want, metaint, NULL, &counter);          /* L = 0 */
        body_segment(&body, &want, metaint, "StreamTitle='Two - Artist';", &counter);
        body_segment(&body, &want, metaint, NULL, &counter);
        body_segment(&body, &want, metaint, "StreamTitle='';", &counter);
        body_segment(&body, &want, metaint, "StreamTitle='Th ree';", &counter);
        /* A trailing partial audio run: a connection drops mid-run. */
        for (int i = 0; i < 40; i++) {
            const uint8_t v = counter++;
            body_put(&body, &v, 1);
            body_put(&want, &v, 1);
        }

        static const size_t pieces[] = { 0, 1, 2, 3, 7, 16, 97, 98, 1000, 4096 };
        for (size_t p = 0; p < sizeof(pieces) / sizeof(pieces[0]); p++) {
            body_t got = { 0 };
            char title[ICY_TITLE_MAX];
            uint32_t titles = 0;
            run(body.buf, body.len, metaint, pieces[p], &got, title, &titles);

            CHECK(got.len == want.len, "piece %zu: %zu audio bytes, wanted %zu",
                  pieces[p], got.len, want.len);
            CHECK(got.len == want.len && memcmp(got.buf, want.buf, want.len) == 0,
                  "piece %zu: audio bytes differ", pieces[p]);
            CHECK(strcmp(title, "Th ree") == 0,
                  "piece %zu: last title \"%s\"", pieces[p], title);
            /* Four blocks carried a StreamTitle; two were L=0 and one of
             * the four was empty, which still counts as told. */
            CHECK(titles == 4, "piece %zu: %u titles, wanted 4", pieces[p], titles);
            free(got.buf);
        }
        free(body.buf);
        free(want.buf);
    }

    /* -------------------------------------------------------------- */
    /* 4. The maximum block, and a block that is all padding           */
    /* -------------------------------------------------------------- */
    {
        icydemux_t d;
        icydemux_init(&d, 1);
        uint8_t buf[1 + 1 + ICY_META_MAX];
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x5A;
        buf[1] = 255;                       /* the largest legal block */
        char big[ICY_META_MAX + 1];
        memset(big, 0, sizeof(big));
        strcpy(big, "StreamTitle='");
        memset(big + 13, 'q', 300);
        strcpy(big + 313, "';");
        memcpy(buf + 2, big, ICY_META_MAX);

        uint8_t out[sizeof(buf)];
        const size_t n = icydemux_feed(&d, buf, sizeof(buf), out);
        CHECK(n == 1 && out[0] == 0x5A, "max block: %zu audio bytes", n);
        CHECK(d.titles == 1, "max block: %u titles", d.titles);
        /* 300 characters into a 256-byte field: truncated, terminated,
         * and still reported as a title. Titles this long are junk from
         * a badly-configured encoder, not something to grow a buffer
         * for; what matters is that it does not run off the end. */
        CHECK(strlen(d.title) == ICY_TITLE_MAX - 1,
              "max block: title is %zu chars, wanted %d",
              strlen(d.title), ICY_TITLE_MAX - 1);
        CHECK(d.blocks == 1, "max block: %u blocks", d.blocks);
    }

    /* A metadata block with no StreamTitle at all reports no title and
     * leaves the previous one alone. */
    {
        icydemux_t d;
        icydemux_init(&d, 1);
        uint8_t a[] = { 0x11, 1, 'S','t','r','e','a','m','T','i','t','l','e','=','\'','A','\'',';' };
        uint8_t out[sizeof(a)];
        icydemux_feed(&d, a, sizeof(a), out);
        CHECK(strcmp(d.title, "A") == 0, "first title \"%s\"", d.title);

        uint8_t b[] = { 0x22, 1, 'S','t','r','e','a','m','U','r','l','=','\'','h','\'',';',' ',' ' };
        icydemux_feed(&d, b, sizeof(b), out);
        CHECK(strcmp(d.title, "A") == 0, "a title-less block overwrote with \"%s\"", d.title);
        CHECK(d.titles == 1, "a title-less block counted: %u", d.titles);
        CHECK(d.blocks == 2, "%u blocks, wanted 2", d.blocks);
    }

    /* -------------------------------------------------------------- */
    /* 5. Reconnect keeps the title, restarts the counters             */
    /* -------------------------------------------------------------- */
    {
        icydemux_t d;
        icydemux_init(&d, 1);
        uint8_t a[] = { 0x11, 1, 'S','t','r','e','a','m','T','i','t','l','e','=','\'','K','\'',';' };
        uint8_t out[sizeof(a)];
        icydemux_feed(&d, a, sizeof(a), out);
        CHECK(d.audio_bytes == 1, "before reconnect: %llu audio bytes",
              (unsigned long long)d.audio_bytes);

        icydemux_reconnect(&d, 8192);
        CHECK(strcmp(d.title, "K") == 0,
              "reconnect dropped the title (\"%s\") -- the screen would blank", d.title);
        CHECK(d.titles == 1, "reconnect reset the title count to %u", d.titles);
        CHECK(d.audio_bytes == 0, "reconnect kept %llu audio bytes",
              (unsigned long long)d.audio_bytes);
        CHECK(d.metaint == 8192, "reconnect kept metaint %d", d.metaint);
        CHECK(d.phase == ICY_IN_AUDIO, "reconnect left phase %d", (int)d.phase);
    }

    /* -------------------------------------------------------------- */
    /* 6. Random bodies, fed in random pieces, against one shot        */
    /* -------------------------------------------------------------- */
    {
        srand(20260911);
        for (int iter = 0; iter < 2000; iter++) {
            const int metaint = 1 + rand() % 200;
            body_t body = { 0 }, want = { 0 };
            uint8_t counter = (uint8_t)rand();
            const int segs = 1 + rand() % 6;
            for (int s = 0; s < segs; s++) {
                char meta[64];
                if (rand() % 3 == 0) {
                    body_segment(&body, &want, metaint, NULL, &counter);
                } else {
                    snprintf(meta, sizeof(meta), "StreamTitle='t%d';", s);
                    body_segment(&body, &want, metaint, meta, &counter);
                }
            }
            const int tail = rand() % (metaint + 1);
            for (int i = 0; i < tail; i++) {
                const uint8_t v = counter++;
                body_put(&body, &v, 1);
                body_put(&want, &v, 1);
            }

            body_t whole = { 0 }, split = { 0 };
            char t1[ICY_TITLE_MAX], t2[ICY_TITLE_MAX];
            uint32_t n1 = 0, n2 = 0;
            run(body.buf, body.len, metaint, 0, &whole, t1, &n1);

            /* Random, uneven pieces -- not a fixed stride, which is the
             * one thing a socket never gives. */
            icydemux_t d;
            icydemux_init(&d, metaint);
            uint8_t *scratch = malloc(body.len ? body.len : 1);
            for (size_t i = 0; i < body.len; ) {
                size_t take = 1 + (size_t)(rand() % 300);
                if (take > body.len - i) take = body.len - i;
                const size_t got = icydemux_feed(&d, body.buf + i, take, scratch);
                body_put(&split, scratch, got);
                i += take;
            }
            free(scratch);
            strcpy(t2, d.title);
            n2 = d.titles;

            if (whole.len != split.len || memcmp(whole.buf, split.buf, whole.len) != 0 ||
                strcmp(t1, t2) != 0 || n1 != n2) {
                CHECK(0, "iter %d (metaint %d): split differs from whole", iter, metaint);
                break;
            }
            checks++;
            if (whole.len != want.len || memcmp(whole.buf, want.buf, want.len) != 0) {
                CHECK(0, "iter %d (metaint %d): audio is not the audio put in",
                      iter, metaint);
                break;
            }
            checks++;

            free(body.buf); free(want.buf); free(whole.buf); free(split.buf);
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
