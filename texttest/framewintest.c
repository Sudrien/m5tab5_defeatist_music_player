/*
 * framewintest.c -- the window between the ring and the decoder.
 *
 * The invariant is that **every byte written in comes out exactly once,
 * in order.** A window that loses a few bytes at each refill boundary
 * does not crash: the decoder desynchronises, resynchronises on the next
 * frame header, and the stream plays with a click every few seconds.
 * From outside that is "internet radio is a bit crackly" and there is no
 * file to compare against.
 *
 * So the main body of this is a simulated decode loop -- real MP3 frames
 * in, arbitrary chunk sizes, a decoder that sometimes consumes nothing --
 * asserting that the frames come out byte-identical and in order.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "framewin.h"
#include "mp3count.h"

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
/* A stream of real MP3 frames to push through it                      */
/* ------------------------------------------------------------------ */

typedef struct { uint8_t *b; size_t len, cap; } buf_t;

static void put(buf_t *s, const void *p, size_t n)
{
    if (s->len + n > s->cap) { s->cap = (s->len + n) * 2 + 64; s->b = realloc(s->b, s->cap); }
    memcpy(s->b + s->len, p, n);
    s->len += n;
}

/* WUOM's actual frame: MPEG1 L3, 64 kbit/s, 44100 Hz, mono, 208/209. */
static void wuom_header(uint8_t *h, unsigned pad)
{
    h[0] = 0xFF;
    h[1] = 0xFB;                                /* MPEG1, layer III, no CRC */
    h[2] = (uint8_t)((5 << 4) | (0 << 2) | (pad << 1));  /* 64 kbit/s, 44100 */
    h[3] = (uint8_t)(3 << 6);                   /* mono */
}

int main(void)
{
    printf("framewintest\n");

    /* The window must exceed the largest frame, or a frame can arrive
     * that never fits and the loop spins forever. */
    /* Not merely greater: greater with room to spare. A 16 KB window
     * passes a bare `> 2 * 8191` by two bytes, which is luck. */
    CHECK(FRAMEWIN_BYTES >= 2 * FRAMEWIN_MAX_FRAME + 4096,
          "the window (%d) clears two maximum frames (%d) by only %d bytes "
          "-- a frame that never fits is a hang, not a glitch",
          FRAMEWIN_BYTES, FRAMEWIN_MAX_FRAME,
          FRAMEWIN_BYTES - 2 * FRAMEWIN_MAX_FRAME);

    /* ---------------------------------------------------------------- */
    /* Basic bookkeeping                                                 */
    /* ---------------------------------------------------------------- */
    {
        uint8_t store[64];
        framewin_t w;
        framewin_init(&w, store, sizeof(store));

        CHECK(framewin_avail(&w) == 0, "a fresh window has %zu bytes",
              framewin_avail(&w));
        CHECK(framewin_empty(&w), "a fresh window is not empty");
        CHECK(framewin_space(&w) == sizeof(store), "space is %zu, wanted %zu",
              framewin_space(&w), sizeof(store));

        memcpy(framewin_tail(&w), "0123456789", 10);
        CHECK(framewin_commit(&w, 10), "commit refused");
        CHECK(framewin_avail(&w) == 10, "avail %zu after 10", framewin_avail(&w));
        CHECK(memcmp(framewin_data(&w), "0123456789", 10) == 0, "data wrong");

        CHECK(framewin_consume(&w, 4), "consume refused");
        CHECK(framewin_avail(&w) == 6, "avail %zu after consuming 4",
              framewin_avail(&w));
        CHECK(memcmp(framewin_data(&w), "456789", 6) == 0, "data wrong after consume");

        /* Overruns are refused, not clamped: believing a bad return
         * value would slide the window past bytes nothing ever saw. */
        CHECK(!framewin_consume(&w, 7), "consuming past the end was allowed");
        CHECK(framewin_avail(&w) == 6, "a refused consume moved the window");
        CHECK(!framewin_commit(&w, sizeof(store)), "an overlong commit was allowed");

        /* Compaction preserves the tail exactly. */
        const size_t sp = framewin_space(&w);
        CHECK(sp == sizeof(store) - 6, "space after compaction %zu", sp);
        CHECK(memcmp(framewin_data(&w), "456789", 6) == 0,
              "compaction corrupted the remainder");
        CHECK(framewin_data(&w) == store, "compaction did not move to the front");

        framewin_reset(&w);
        CHECK(framewin_empty(&w) && w.in == 10 && w.out == 4,
              "reset lost the totals: in %llu out %llu",
              (unsigned long long)w.in, (unsigned long long)w.out);
    }

    /* NULL and zero are survivable. */
    {
        CHECK(framewin_avail(NULL) == 0, "NULL avail");
        CHECK(framewin_data(NULL) == NULL, "NULL data");
        CHECK(framewin_space(NULL) == 0, "NULL space");
        CHECK(framewin_tail(NULL) == NULL, "NULL tail");
        CHECK(!framewin_consume(NULL, 1), "NULL consume");
        CHECK(!framewin_commit(NULL, 1), "NULL commit");
        CHECK(!framewin_stuck(NULL), "NULL stuck");
        CHECK(!framewin_skip_byte(NULL), "NULL skip");
        framewin_reset(NULL);
        framewin_init(NULL, NULL, 0);
    }

    /* ---------------------------------------------------------------- */
    /* The invariant, through a simulated decode loop                    */
    /* ---------------------------------------------------------------- */
    {
        /* 400 real frames, alternating padding as a 44.1 kHz encoder
         * does, with 0xFF bytes salted through the bodies. */
        buf_t src = { 0 };
        size_t frame_off[400], frame_len[400];
        for (int i = 0; i < 400; i++) {
            uint8_t h[4];
            const unsigned pad = (unsigned)(i & 1);
            wuom_header(h, pad);
            const unsigned len = mp3_frame_len(h, NULL, NULL, NULL, NULL, NULL, NULL);
            if (i == 0) {
                CHECK(len == 208, "the WUOM frame is %u bytes, wanted 208", len);
            }
            frame_off[i] = src.len;
            frame_len[i] = len;
            put(&src, h, 4);
            uint8_t *body = malloc(len - 4);
            for (unsigned k = 0; k < len - 4; k++) {
                body[k] = (uint8_t)((k % 5 == 0) ? 0xFF : (k * 37 + i));
            }
            put(&src, body, len - 4);
            free(body);
        }

        static const size_t chunks[] = { 1, 2, 3, 17, 208, 209, 512, 2048, 16384 };
        for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            static uint8_t store[FRAMEWIN_BYTES];
            framewin_t w;
            framewin_init(&w, store, sizeof(store));

            size_t fed = 0;             /* bytes handed to the window */
            int    got = 0;             /* frames the "decoder" took */
            bool   bad = false;
            buf_t  seen = { 0 };        /* everything consumed, in order */

            while (got < 400) {
                /* Refill, the way the real loop will from
                 * netstream_read(). */
                const size_t room = framewin_space(&w);
                if (room && fed < src.len) {
                    size_t take = chunks[c];
                    if (take > room) take = room;
                    if (take > src.len - fed) take = src.len - fed;
                    memcpy(framewin_tail(&w), src.b + fed, take);
                    if (!framewin_commit(&w, take)) { CHECK(0, "commit failed"); bad = true; break; }
                    fed += take;
                }

                /* The "decoder": takes a whole frame when one is there,
                 * nothing otherwise -- which is exactly what
                 * mp3dec_decode_frame does with a short buffer. */
                const size_t have = framewin_avail(&w);
                const size_t want = frame_len[got];
                if (have >= want) {
                    const uint8_t *p = framewin_data(&w);
                    if (memcmp(p, src.b + frame_off[got], want) != 0) {
                        CHECK(0, "chunk %zu: frame %d came out corrupted",
                              chunks[c], got);
                        bad = true;
                        break;
                    }
                    put(&seen, p, want);
                    if (!framewin_consume(&w, want)) { CHECK(0, "consume failed"); bad = true; break; }
                    got++;
                } else if (fed >= src.len) {
                    break;              /* input exhausted mid-frame */
                }

                if (framewin_stuck(&w)) {
                    CHECK(0, "chunk %zu: window reported stuck on good frames",
                          chunks[c]);
                    bad = true;
                    break;
                }
            }

            if (!bad) {
                CHECK(got == 400, "chunk %zu: %d frames of 400", chunks[c], got);
                CHECK(seen.len == src.len,
                      "chunk %zu: %zu bytes out of %zu in", chunks[c],
                      seen.len, src.len);
                CHECK(seen.len == src.len && memcmp(seen.b, src.b, src.len) == 0,
                      "chunk %zu: the bytes out are not the bytes in -- this is "
                      "the click-every-few-seconds bug", chunks[c]);
                CHECK(w.in == fed && w.out == seen.len,
                      "chunk %zu: totals in %llu/%zu out %llu/%zu", chunks[c],
                      (unsigned long long)w.in, fed,
                      (unsigned long long)w.out, seen.len);
            }
            free(seen.b);
        }
        free(src.b);
    }

    /* ---------------------------------------------------------------- */
    /* Random consume amounts: the window is not told frame boundaries   */
    /* ---------------------------------------------------------------- */
    {
        srand(20260911);
        for (int iter = 0; iter < 2000; iter++) {
            const size_t cap = 64 + (size_t)(rand() % 512);
            uint8_t *store = malloc(cap);
            framewin_t w;
            framewin_init(&w, store, cap);

            const size_t total = 1 + (size_t)(rand() % 4000);
            uint8_t *src = malloc(total);
            for (size_t i = 0; i < total; i++) src[i] = (uint8_t)(rand() % 256);

            buf_t seen = { 0 };
            size_t fed = 0;
            int spins = 0;
            while ((fed < total || !framewin_empty(&w)) && spins++ < 100000) {
                const size_t room = framewin_space(&w);
                if (room && fed < total) {
                    size_t take = 1 + (size_t)(rand() % 64);
                    if (take > room) take = room;
                    if (take > total - fed) take = total - fed;
                    memcpy(framewin_tail(&w), src + fed, take);
                    framewin_commit(&w, take);
                    fed += take;
                }
                const size_t have = framewin_avail(&w);
                if (have) {
                    size_t eat = (size_t)(rand() % (int)(have + 1));
                    if (fed >= total) eat = have;   /* drain at the end */
                    if (eat) {
                        put(&seen, framewin_data(&w), eat);
                        if (!framewin_consume(&w, eat)) {
                            CHECK(0, "iter %d: a legal consume was refused", iter);
                            break;
                        }
                    }
                }
            }
            if (seen.len != total || memcmp(seen.b, src, total) != 0) {
                CHECK(0, "iter %d: %zu bytes out of %zu, and not identical",
                      iter, seen.len, total);
                free(seen.b); free(src); free(store);
                break;
            }
            checks++;
            free(seen.b); free(src); free(store);
        }
    }

    /* ---------------------------------------------------------------- */
    /* Stuck detection, and getting unstuck                              */
    /* ---------------------------------------------------------------- */
    {
        uint8_t store[64];
        framewin_t w;
        framewin_init(&w, store, sizeof(store));
        memset(framewin_tail(&w), 0x5A, sizeof(store));
        framewin_commit(&w, sizeof(store));

        CHECK(!framewin_stuck(&w), "stuck after one full refill -- a decoder "
              "legitimately takes nothing while a frame is still arriving");
        /* Full, and nothing consumed, refill after refill. */
        for (int i = 1; i < FRAMEWIN_DRY_LIMIT; i++) {
            CHECK(framewin_space(&w) == 0, "space appeared in a full window");
            framewin_commit(&w, 0);
        }
        CHECK(framewin_stuck(&w), "a full window with %d dry refills is not stuck",
              FRAMEWIN_DRY_LIMIT);

        CHECK(framewin_skip_byte(&w), "could not skip a byte to resynchronise");
        CHECK(!framewin_stuck(&w), "still stuck after skipping");
        CHECK(framewin_avail(&w) == sizeof(store) - 1, "skip took %zu bytes",
              sizeof(store) - framewin_avail(&w));
        CHECK(framewin_space(&w) == 1, "space after compacting a skip is %zu",
              framewin_space(&w));

        /* A window with room is never stuck, however dry. */
        framewin_reset(&w);
        for (int i = 0; i < FRAMEWIN_DRY_LIMIT * 2; i++) framewin_commit(&w, 0);
        CHECK(!framewin_stuck(&w), "an empty window reported stuck");
    }

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
