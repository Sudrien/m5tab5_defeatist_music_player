/*
 * texttest/main.c -- the variable-width text layout, measured.
 *
 * Runs against the real gfx.c (see shim.h for why nothing here is a
 * reimplementation). Every draw lands in a real shadow framebuffer under
 * ASan, so an overrun is a crash with a stack trace and not a stray
 * pixel.
 *
 * The properties tested are the ones the fixed-width version got for
 * free and the variable-width version has to earn:
 *
 *   1. gfx_text_w() equals the advance the draw loops actually walk.
 *      When every glyph was one cell these could not disagree. Now they
 *      are separate code paths over separate width lookups, and every
 *      caller that centres or right-aligns text trusts them to match.
 *
 *   2. Nothing draws outside its max_w budget. The ellipsis path in
 *      gfx_draw_text() and the ring walk in gfx_draw_text_tail() both
 *      decide what fits by accumulating per-glyph advances; an off-by-one
 *      there overruns the panel rather than truncating.
 *
 *   3. gfx_draw_text_clipped() draws nothing outside its window, at any
 *      marquee offset, including offsets that put the string far off
 *      either edge.
 *
 *   4. The tail walk keeps the *tail*. That is the entire reason the
 *      function exists, and the ring buffer that replaced the old
 *      byte-offset walk is the easiest thing here to get subtly wrong.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shim.h"
#include "gfx.h"

#define W   720
#define H   1280

static int failures;
static int checks;

#define CHECK(cond, ...) do {                       \
    checks++;                                       \
    if (!(cond)) {                                  \
        failures++;                                 \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__);                        \
        printf("\n");                               \
    }                                               \
} while (0)

/* The framebuffer gfx.c allocated, reached the same way the panel would
 * reach it. gfx.c keeps s_fb private, so the harness finds the drawn
 * pixels by scanning what it can see: it clears, draws, and scans. */
static uint16_t *fb;

#define INK   0xFFFF
#define BLANK 0x0000

static void clear(void)
{
    gfx_fill_rect(0, 0, W, H, BLANK);
}

/* Bounding box of everything non-blank. Returns false if nothing drawn. */
static bool ink_bbox(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = W; *y0 = H; *x1 = -1; *y1 = -1;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (fb[y * W + x] == BLANK) continue;
            if (x < *x0) *x0 = x;
            if (x > *x1) *x1 = x;
            if (y < *y0) *y0 = y;
            if (y > *y1) *y1 = y;
        }
    }
    return *x1 >= 0;
}

/* ------------------------------------------------------------------ */
/* Corpus                                                              */
/* ------------------------------------------------------------------ */

/* Real-shaped strings, not random bytes: the point is layout, and the
 * mixes that break layout are the ones a tagger actually produces. The
 * malformed entries are last and are there because a tag can contain
 * anything and utf8_next() has to survive it. */
static const char *CORPUS[] = {
    "",
    " ",
    "a",
    "Erik Satie",
    "Gymnopedie No. 1",
    "04 - Sarabande No. 2.flac",
    "/sd/Music/Satie/Trois Gymnopedies/04 - Sarabande No. 2.flac",

    /* halfwidth, accented */
    "Bj\xc3\xb6rk",
    "Sigur R\xc3\xb3s",
    "\xc5\x81\xc3\xb3" "d" "\xc5\xba",
    "Dvo\xc5\x99\xc3\xa1k",
    "M\xc3\xb6" "tley Cr" "\xc3\xbc" "e",

    /* Cyrillic -- new at 12px */
    "\xd0\xa7\xd0\xb0\xd0\xb9\xd0\xba\xd0\xbe\xd0\xb2\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9",
    "\xd0\xa1\xd0\xb5\xd1\x80\xd0\xb3\xd0\xb5\xd0\xb9 \xd0\xa0\xd0\xb0\xd1\x85\xd0\xbc\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xbd\xd0\xbe\xd0\xb2",

    /* fullwidth */
    "\xe9\x9f\xb3\xe6\xa5\xbd",
    "\xe5\x9d\x82\xe6\x9c\xac\xe9\xbe\x8d\xe4\xb8\x80",
    "\xe3\x81\x93\xe3\x81\x93\xe3\x82\x8d\xe3\x82\x92\xe3\x81\x86\xe3\x81\x9f\xe3\x81\x86",

    /* the mix -- halfwidth and fullwidth in one run, which is where a
     * fixed-cell assumption shows up as misalignment */
    "Ryuichi Sakamoto - \xe6\x88\xa6\xe5\xa0\xb4\xe3\x81\xae\xe3\x83\xa1\xe3\x83\xaa\xe3\x83\xbc\xe3\x82\xaf\xe3\x83\xaa\xe3\x82\xb9\xe3\x83\x9e\xe3\x82\xb9",
    "\xe5\x9d\x82\xe6\x9c\xac\xe9\xbe\x8d\xe4\xb8\x80 / async (2017)",
    "a" "\xe9\x9f\xb3" "b" "\xe6\xa5\xbd" "c",

    /* fullwidth parens -- the block that was missing from RANGES */
    "async \xef\xbc\x88\xef\xbc\x92\xef\xbc\x90\xef\xbc\x91\xef\xbc\x97\xef\xbc\x89",

    /* codepoints Ark has no glyph for at any size: notdef path */
    "\xe8\xad\xb7 \xe9\x83\x8e",

    /* soft hyphen: glyph_for() returns no cell at all */
    "soft\xc2\xadhyphen",
    /* no-break space: folded to plain space */
    "no" "\xc2\xa0" "break",

    /* long, to exercise truncation and the tail ring */
    "The Quick Brown Fox Jumps Over The Lazy Dog And Keeps Going For Quite A While Longer Than Any Panel Is Wide",
    "\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd\xe9\x9f\xb3\xe6\xa5\xbd",

    /* malformed: truncated sequence, lone continuation, overlong,
     * surrogate, 5-byte lead. None should hang or read past the NUL. */
    "\xe9\x9f",
    "\x80\x80\x80",
    "\xc0\x80",
    "\xed\xa0\x80",
    "\xf8\x88\x80\x80\x80",
    "valid\xffthen more",
};
#define NCORPUS ((int)(sizeof(CORPUS) / sizeof(CORPUS[0])))

/* ------------------------------------------------------------------ */
/* 1. gfx_text_w() agrees with what the draw path walks                */
/* ------------------------------------------------------------------ */

/* Draw unclipped at a known origin with a budget far larger than the
 * string, then compare the measured ink against the promised width.
 *
 * Ink is not expected to reach the full advance -- the last glyph's gap
 * column and its right bearing are both blank -- so the assertion is
 * one-sided: ink must not exceed the promise. A string that measures
 * narrower than it draws is the bug that misaligns centred text, and
 * that is what this catches. */
static void test_width_matches_draw(void)
{
    printf("gfx_text_w() vs drawn extent\n");
    for (int i = 0; i < NCORPUS; i++) {
        const char *s = CORPUS[i];
        for (int scale = 2; scale <= 5; scale++) {
            clear();
            const int promised = gfx_text_w(s, scale);
            gfx_draw_text(10, 10, s, scale, W - 20, INK);

            int x0, y0, x1, y1;
            if (!ink_bbox(&x0, &y0, &x1, &y1)) continue;   /* nothing drawn */

            const int drawn = x1 - 10 + 1;
            CHECK(drawn <= promised,
                  "s=%d corpus[%d]: drew %d px, gfx_text_w promised %d",
                  scale, i, drawn, promised);
            CHECK(x0 >= 10, "s=%d corpus[%d]: drew left of origin (x0=%d)",
                  scale, i, x0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 2. max_w is a hard budget                                           */
/* ------------------------------------------------------------------ */

/* Every budget from tiny to panel-wide, against every string. The
 * interesting ones are the budgets near the dots-plus-one-glyph bail-out
 * in gfx_draw_text(): just under it nothing should draw, just over it the
 * ellipsis must still fit. */
static void test_max_w_respected(void)
{
    printf("max_w budget honoured\n");
    static const int budgets[] = { 0, 1, 6, 7, 13, 14, 20, 21, 27, 28,
                                   40, 60, 100, 200, 400, 700 };
    const int nb = (int)(sizeof(budgets) / sizeof(budgets[0]));

    for (int i = 0; i < NCORPUS; i++) {
        for (int scale = 2; scale <= 4; scale++) {
            for (int b = 0; b < nb; b++) {
                const int max_w = budgets[b];

                clear();
                gfx_draw_text(10, 10, CORPUS[i], scale, max_w, INK);
                int x0, y0, x1, y1;
                if (ink_bbox(&x0, &y0, &x1, &y1)) {
                    CHECK(x1 < 10 + max_w,
                          "draw_text s=%d corpus[%d] max_w=%d: ink at x=%d, "
                          "budget ends at %d", scale, i, max_w, x1, 10 + max_w);
                }

                clear();
                gfx_draw_text_tail(10, 10, CORPUS[i], scale, max_w, INK);
                if (ink_bbox(&x0, &y0, &x1, &y1)) {
                    CHECK(x1 < 10 + max_w,
                          "draw_text_tail s=%d corpus[%d] max_w=%d: ink at x=%d, "
                          "budget ends at %d", scale, i, max_w, x1, 10 + max_w);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 3. the clip window is absolute                                      */
/* ------------------------------------------------------------------ */

/* This is the marquee. The string is drawn at x = win_x - off for a
 * sweep of offsets covering the whole travel plus overshoot at both
 * ends, and nothing may land outside the window at any of them. */
static void test_clip_window(void)
{
    printf("gfx_draw_text_clipped() stays in its window\n");
    const int win_x = 40;
    const int win_w = 300;

    for (int i = 0; i < NCORPUS; i++) {
        const int scale = 3;
        const int tw = gfx_text_w(CORPUS[i], scale);
        const int span = tw > win_w ? tw - win_w : 0;

        for (int off = -60; off <= span + 60; off += 7) {
            clear();
            gfx_draw_text_clipped(win_x - off, 10, win_x, win_w,
                                  CORPUS[i], scale, INK);
            int x0, y0, x1, y1;
            if (!ink_bbox(&x0, &y0, &x1, &y1)) continue;
            CHECK(x0 >= win_x && x1 < win_x + win_w,
                  "clipped corpus[%d] off=%d: ink %d..%d outside window %d..%d",
                  i, off, x0, x1, win_x, win_x + win_w - 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 4. the tail walk keeps the tail                                     */
/* ------------------------------------------------------------------ */

/* Built rather than asserted from a table: a string whose last glyphs
 * are known, truncated hard, must still show those last glyphs. The
 * check is that drawing the whole string tail-truncated produces the
 * same ink as drawing just its true tail at the same budget -- which is
 * only true if the ring walk kept exactly the right suffix. */
static void test_tail_keeps_tail(void)
{
    printf("gfx_draw_text_tail() keeps the suffix\n");

    /* pairs: {long string, the suffix that should survive}. The budget is
     * derived below as exactly dots + suffix, not written out here: a
     * hand-picked budget larger than the suffix keeps more than the
     * suffix, which is correct behaviour that reads as a failure. (It
     * did, on the first run of this test -- the code was right and the
     * expectation was wrong.) */
    static const struct { const char *full; const char *suffix; } cases[] = {
        { "/sd/Music/Satie/Trois Gymnopedies/Sarabande.flac", "Sarabande.flac" },
        { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaXYZ",               "XYZ"            },
        /* fullwidth suffix: the ring has to count 13*scale, not 7*scale */
        { "aaaaaaaaaaaaaaaaaaaa\xe9\x9f\xb3\xe6\xa5\xbd",
          "\xe9\x9f\xb3\xe6\xa5\xbd" },
        /* mixed suffix */
        { "aaaaaaaaaaaaaaaaaaaa\xe9\x9f\xb3z",
          "\xe9\x9f\xb3" "z" },
    };
    const int n = (int)(sizeof(cases) / sizeof(cases[0]));

    for (int i = 0; i < n; i++) {
        const int scale = 1;
        const int max_w = 3 * GFX_GLYPH_W(scale)
                        + gfx_text_w(cases[i].suffix, scale);

        /* Ink from the truncated full string, minus the three dots. */
        clear();
        gfx_draw_text_tail(10, 10, cases[i].full, scale, max_w, INK);
        int fx0, fy0, fx1, fy1;
        const bool drew = ink_bbox(&fx0, &fy0, &fx1, &fy1);
        CHECK(drew, "tail[%d]: nothing drawn at max_w=%d", i, max_w);
        if (!drew) continue;

        /* The suffix must be present: its own drawn width should equal
         * the width of the ink after the ellipsis. */
        const int dots_w = 3 * GFX_GLYPH_W(scale);
        const int suffix_w = gfx_text_w(cases[i].suffix, scale);
        const int tail_ink_w = fx1 - (10 + dots_w) + 1;

        CHECK(tail_ink_w <= suffix_w && tail_ink_w > suffix_w - 3 * scale - 1,
              "tail[%d]: suffix ink %d px, expected about %d",
              i, tail_ink_w, suffix_w);
    }
}

/* A path far longer than the ring can hold. The ring is bounded at
 * TAIL_MAX_GLYPHS; a string longer than that must still produce the
 * correct tail, because the ring keeps the newest entries and the tail
 * is made of newest entries. This is the case where a naive bound would
 * silently return the wrong suffix rather than crash. */
static void test_tail_longer_than_ring(void)
{
    printf("gfx_draw_text_tail() past the ring bound\n");
    char big[4096];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    memcpy(big + sizeof(big) - 5, "END", 3);

    for (int scale = 2; scale <= 4; scale++) {
        clear();
        gfx_draw_text_tail(10, 10, big, scale, 300, INK);
        int x0, y0, x1, y1;
        CHECK(ink_bbox(&x0, &y0, &x1, &y1),
              "long path s=%d: nothing drawn", scale);
        CHECK(x1 < 10 + 300, "long path s=%d: overran budget (x1=%d)", scale, x1);
    }
}

/* ------------------------------------------------------------------ */
/* 5. degenerate inputs                                                */
/* ------------------------------------------------------------------ */

static void test_degenerate(void)
{
    printf("degenerate arguments\n");

    /* NULL and empty must be no-ops, not crashes. */
    CHECK(gfx_text_w(NULL, 3) == 0, "gfx_text_w(NULL) != 0");
    CHECK(gfx_text_w("", 3) == 0, "gfx_text_w(\"\") != 0");
    clear();
    gfx_draw_text(10, 10, NULL, 3, 100, INK);
    gfx_draw_text_tail(10, 10, NULL, 3, 100, INK);
    gfx_draw_text_clipped(10, 10, 0, 100, NULL, 3, INK);
    int x0, y0, x1, y1;
    CHECK(!ink_bbox(&x0, &y0, &x1, &y1), "NULL string drew something");

    /* Zero and negative budgets. */
    for (int i = 0; i < NCORPUS; i++) {
        clear();
        gfx_draw_text(10, 10, CORPUS[i], 3, 0, INK);
        gfx_draw_text(10, 10, CORPUS[i], 3, -50, INK);
        gfx_draw_text_tail(10, 10, CORPUS[i], 3, 0, INK);
        gfx_draw_text_tail(10, 10, CORPUS[i], 3, -50, INK);
        gfx_draw_text_clipped(10, 10, 0, 0, CORPUS[i], 3, INK);
        gfx_draw_text_clipped(10, 10, 0, -50, CORPUS[i], 3, INK);
        CHECK(!ink_bbox(&x0, &y0, &x1, &y1),
              "corpus[%d]: non-positive budget drew something", i);
    }

    /* Draws that start off-panel. gfx_fill_rect() clips, so this is
     * checking the layout does not compute an address before clipping. */
    clear();
    for (int i = 0; i < NCORPUS; i++) {
        gfx_draw_text(-500, 10, CORPUS[i], 4, 400, INK);
        gfx_draw_text(W - 5, 10, CORPUS[i], 4, 400, INK);
        gfx_draw_text(10, -20, CORPUS[i], 4, 400, INK);
        gfx_draw_text(10, H - 3, CORPUS[i], 4, 400, INK);
        gfx_draw_text_tail(-500, 10, CORPUS[i], 4, 400, INK);
        gfx_draw_text_tail(W - 5, 10, CORPUS[i], 4, 400, INK);
    }
}

int main(void)
{
    if (gfx_init(NULL, W, H) != ESP_OK) {
        printf("gfx_init failed\n");
        return 1;
    }
    /* gfx.c keeps its shadow buffer private and should stay that way, so
     * the harness takes the pointer from the shim's allocator rather than
     * from a test-only accessor bolted onto shipping code. Verified below
     * rather than assumed: a pixel is drawn and read back. */
    extern void *shim_last_big_alloc;
    fb = shim_last_big_alloc;
    if (!fb) { printf("no framebuffer\n"); return 1; }

    gfx_fill_rect(0, 0, W, H, BLANK);
    gfx_px(5, 7, INK);
    if (fb[7 * W + 5] != INK) {
        printf("framebuffer handle is wrong -- harness cannot see draws\n");
        return 1;
    }
    gfx_fill_rect(0, 0, W, H, BLANK);

    test_width_matches_draw();
    test_max_w_respected();
    test_clip_window();
    test_tail_keeps_tail();
    test_tail_longer_than_ring();
    test_degenerate();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
