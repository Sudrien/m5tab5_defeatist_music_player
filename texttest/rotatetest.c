/*
 * rotatetest -- the rotation mapping, and the layout that rides on it.
 *
 * Three things, none of which the board can show cheaply and all of
 * which are wrong in ways that look like a driver fault:
 *
 *  1. gfx.c's forward map (logical -> panel) must stay on the panel at
 *     every angle and must be a bijection. A map that is merely
 *     plausible puts a band at the wrong end of the screen once in four
 *     angles, which reads as a DPI bug.
 *  2. touch.c's inverse must undo it EXACTLY. If it does not, every
 *     control is somewhere other than where it was drawn, and the error
 *     is a reflection rather than an offset -- so it looks like the
 *     touch panel is miscalibrated, not like the code is wrong.
 *  3. The square's layout arithmetic: that the control square fits, that
 *     the artwork band is what is left, and that the row offsets land
 *     inside the square at both orientations.
 *
 * The mappings are duplicated here rather than linked, for clocktest's
 * reason: gfx.c needs the DPI driver and touch.c needs esp_lcd_touch,
 * and neither builds on the host. The duplicate is small and exact, and
 * is kept adjacent to the originals by a comment in all three files; if
 * they drift this test is worthless, which is why it is said out loud.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond, ...) do {                                            \
    checks++;                                                            \
    if (!(cond)) {                                                       \
        failures++;                                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                    \
        printf(__VA_ARGS__);                                             \
        printf("\n");                                                    \
    }                                                                    \
} while (0)

/* The glass. Never swaps -- see gfx.c's s_pw/s_ph. */
#define PW  720
#define PH  1280

/* ---- the copies under test ----------------------------------------- */

/* gfx.c: logical -> panel. */
static void fwd(int rot, int lx, int ly, int *px, int *py)
{
    switch (rot) {
    case 1: *px = PW - 1 - ly; *py = lx;           break;
    case 2: *px = PW - 1 - lx; *py = PH - 1 - ly;  break;
    case 3: *px = ly;          *py = PH - 1 - lx;  break;
    default:*px = lx;          *py = ly;           break;
    }
}

/* touch.c: panel -> logical. */
static void inv(int rot, int px, int py, int *lx, int *ly)
{
    switch (rot) {
    case 1: *lx = py;          *ly = PW - 1 - px;  break;
    case 2: *lx = PW - 1 - px; *ly = PH - 1 - py;  break;
    case 3: *lx = PH - 1 - py; *ly = px;           break;
    default:*lx = px;          *ly = py;           break;
    }
}

static int logical_w(int rot) { return (rot & 1) ? PH : PW; }
static int logical_h(int rot) { return (rot & 1) ? PW : PH; }

/* ui.h */
#define UI_SQUARE   720
#define UI_ART_H    (1280 - UI_SQUARE)
#define BAR_PAD     24

/* ui.c's row table, offsets from the square's own top-left. */
static const struct { const char *name; int y; } k_rows[] = {
    { "envelope",  96 }, { "clocks",  112 }, { "title",   186 },
    { "album",    238 }, { "artist",  278 }, { "transport", 392 },
    { "aux",      500 }, { "volume",  623 },
};

int main(void)
{
    printf("rotatetest\n");

    /* --- 1. the forward map stays on the glass, and is a bijection --- */
    printf("  forward map\n");
    for (int rot = 0; rot < 4; rot++) {
        const int lw = logical_w(rot), lh = logical_h(rot);
        CHECK(lw * lh == PW * PH, "rot%d: %dx%d is not the panel's area",
              rot * 90, lw, lh);

        /* Every logical pixel lands on the panel exactly once. A byte
         * per panel pixel rather than a bitmap: 900 KB is nothing here
         * and the count catches a two-to-one map, which a bitmap of
         * "seen" would too but less legibly. */
        unsigned char *hit = calloc((size_t)PW * PH, 1);
        int off = 0, twice = 0;
        for (int ly = 0; ly < lh; ly++) {
            for (int lx = 0; lx < lw; lx++) {
                int px, py;
                fwd(rot, lx, ly, &px, &py);
                if (px < 0 || px >= PW || py < 0 || py >= PH) { off++; continue; }
                if (hit[(size_t)py * PW + px]++) twice++;
            }
        }
        CHECK(off == 0, "rot%d: %d logical pixels land off the glass", rot * 90, off);
        CHECK(twice == 0, "rot%d: %d panel pixels written twice", rot * 90, twice);

        int missed = 0;
        for (size_t i = 0; i < (size_t)PW * PH; i++) if (!hit[i]) missed++;
        CHECK(missed == 0, "rot%d: %d panel pixels never written", rot * 90, missed);
        free(hit);
    }

    /* --- 2. touch undoes gfx, exactly, everywhere -------------------- */
    printf("  touch is the inverse\n");
    for (int rot = 0; rot < 4; rot++) {
        const int lw = logical_w(rot), lh = logical_h(rot);
        int bad = 0;
        for (int ly = 0; ly < lh; ly++) {
            for (int lx = 0; lx < lw; lx++) {
                int px, py, rx, ry;
                fwd(rot, lx, ly, &px, &py);
                inv(rot, px, py, &rx, &ry);
                if (rx != lx || ry != ly) bad++;
            }
        }
        CHECK(bad == 0, "rot%d: %d points do not round-trip", rot * 90, bad);
    }

    /*
     * The corners, named. The bijection above proves the map is sound;
     * this proves it is the RIGHT sound map -- that 90 is clockwise and
     * not anticlockwise, which a bijection cannot tell you and which is
     * the difference between the controls being on the correct side of
     * the glass and the wrong one.
     */
    printf("  90 is clockwise\n");
    {
        int px, py;
        fwd(1, 0, 0, &px, &py);
        CHECK(px == PW - 1 && py == 0,
              "rot90: logical top-left should be the panel's top-RIGHT, got %d,%d",
              px, py);
        fwd(3, 0, 0, &px, &py);
        CHECK(px == 0 && py == PH - 1,
              "rot270: logical top-left should be the panel's bottom-LEFT, got %d,%d",
              px, py);
        fwd(2, 0, 0, &px, &py);
        CHECK(px == PW - 1 && py == PH - 1,
              "rot180: logical top-left should be the panel's bottom-right, got %d,%d",
              px, py);
    }

    /* --- 3. the square, and what is left for the artwork ------------- */
    printf("  the square and the band\n");
    {
        /* The split is forced: the square is the whole short edge. */
        CHECK(UI_SQUARE == PW, "the square must be the panel's short edge");
        CHECK(UI_ART_H == PH - UI_SQUARE, "the band is what is left of the long edge");
        CHECK(UI_ART_H == 560, "which is 560");
    }

    for (int rot = 0; rot < 4; rot++) {
        const int lw = logical_w(rot), lh = logical_h(rot);
        const bool landscape = (lw > lh);

        /* ui_relayout() */
        const int bar_x   = landscape ? lw - UI_SQUARE : 0;
        const int bar_top = landscape ? 0 : lh - UI_SQUARE;

        CHECK(bar_x >= 0 && bar_x + UI_SQUARE <= lw,
              "rot%d: square runs off horizontally (%d..%d in %d)",
              rot * 90, bar_x, bar_x + UI_SQUARE, lw);
        CHECK(bar_top >= 0 && bar_top + UI_SQUARE <= lh,
              "rot%d: square runs off vertically (%d..%d in %d)",
              rot * 90, bar_top, bar_top + UI_SQUARE, lh);

        /* ui_art_band() */
        const int aw = landscape ? lw - UI_SQUARE : lw;
        const int ah = landscape ? lh : lh - UI_SQUARE;
        CHECK(aw > 0 && ah > 0, "rot%d: no artwork band", rot * 90);

        /* The band and the square must not overlap, or the cover writes
         * over the controls -- which is what albumart.c's stride bug
         * did before the band became a region rather than a row count. */
        if (landscape) {
            CHECK(aw <= bar_x, "rot%d: band (w=%d) overlaps square at x=%d",
                  rot * 90, aw, bar_x);
        } else {
            CHECK(ah <= bar_top, "rot%d: band (h=%d) overlaps square at y=%d",
                  rot * 90, ah, bar_top);
        }

        /* The band is the same rectangle turned, at every angle. */
        CHECK(aw * ah == 560 * 720, "rot%d: band is %dx%d, not 560x720 turned",
              rot * 90, aw, ah);

        /* Every row lands inside the square. */
        for (size_t i = 0; i < sizeof(k_rows) / sizeof(k_rows[0]); i++) {
            const int y = bar_top + k_rows[i].y;
            CHECK(y >= bar_top && y < bar_top + UI_SQUARE,
                  "rot%d: row %s at %d is outside the square %d..%d",
                  rot * 90, k_rows[i].name, y, bar_top, bar_top + UI_SQUARE);
        }

        /* The content box, and the five aux icons on one pitch (5106:
         * the record button made it five). */
        const int x0 = bar_x + BAR_PAD, x1 = bar_x + UI_SQUARE - BAR_PAD;
        CHECK(x1 - x0 == 672, "rot%d: content is %d wide, not 672", rot * 90, x1 - x0);

        const int ih = 26;
        const int g0 = x0 + ih, span = (x1 - ih) - g0;
        const int naux = 5;
        const int apitch = span / (naux - 1), lead = (span - apitch * (naux - 1)) / 2;
        int prev = 0, pitch = 0;
        for (int i = 0; i < naux; i++) {
            const int cx = g0 + lead + apitch * i;
            CHECK(cx - ih >= x0 && cx + ih <= x1,
                  "rot%d: aux icon %d overhangs the content box", rot * 90, i);
            if (i == 1) pitch = cx - prev;
            /* ui.c's in_box() pads by HIT_PAD_X (14) each side, so the
             * hit boxes are 2 * (26 + 14) = 80 wide and must not touch. */
            if (i >= 1) {
                CHECK(cx - prev > 2 * (ih + 14),
                      "rot%d: aux hit boxes %d and %d overlap at pitch %d",
                      rot * 90, i - 1, i, cx - prev);
                CHECK(cx - prev == pitch,
                      "rot%d: aux pitch %d != %d between icons %d and %d",
                      rot * 90, cx - prev, pitch, i - 1, i);
            }
            prev = cx;
        }

        /*
         * The volume row: the two icons anchor to the content box and
         * the groove is derived from them.
         *
         * 5001 had it the other way round -- the icons hung off the
         * groove by a fixed 42 px -- so rebalancing the groove floated
         * the output icon 55 px off the left edge while the comment
         * claimed it was flush. A boot log caught that, not this test,
         * because this test did not look at the row at all. It does now.
         */
        {
            const int spk_half = 26, batt_w = 46, batt_nub = 5;
            const int blocks = 2 * spk_half + (batt_w + batt_nub);
            const int gut = ((x1 - x0) - blocks) / 8;

            const int spk = x0 + spk_half;
            const int batt = x1 - batt_nub - batt_w / 2;
            const int vx0 = spk + spk_half + gut;
            const int vx1 = batt - batt_w / 2 - gut;

            /*
             * No dead air. This is the property that broke, and the one
             * the log exposed: every pixel of the content box is either
             * a block or one of the two gutters, so nothing can float.
             *
             * The log's own number is deliberately NOT asserted. A mute
             * press landed at x=83, which proves the icon was at 105 on
             * that build -- but 83 is outside the flush box this now
             * produces, so asserting it would pin the bug in place.
             * What the log proved is the gap; the gap is what is
             * checked.
             */
            CHECK((spk - spk_half) - x0 == 0,
                  "rot%d: %d px of dead air before the output icon",
                  rot * 90, (spk - spk_half) - x0);
            CHECK(x1 - (batt + batt_w / 2 + batt_nub) == 0,
                  "rot%d: %d px of dead air after the battery",
                  rot * 90, x1 - (batt + batt_w / 2 + batt_nub));

            /* The groove clears both blocks, and by the same margin. */
            CHECK(vx0 > spk + spk_half,
                  "rot%d: groove starts inside the output icon", rot * 90);
            CHECK(vx1 < batt - batt_w / 2,
                  "rot%d: groove ends inside the battery", rot * 90);
            CHECK((vx0 - (spk + spk_half)) == ((batt - batt_w / 2) - vx1),
                  "rot%d: gutters differ, %d vs %d", rot * 90,
                  vx0 - (spk + spk_half), (batt - batt_w / 2) - vx1);

            const int covered = (2 * spk_half) + (vx1 - vx0) +
                                (batt_w + batt_nub) + 2 * gut;
            CHECK(covered == x1 - x0,
                  "rot%d: blocks and gutters cover %d of %d",
                  rot * 90, covered, x1 - x0);
        }

        /*
         * The transport: the pill and the skip glyphs must not touch,
         * including their padded hit boxes. This is the clearance that
         * moving from a 92 px disc to a 168 px pill ate into.
         */
        const int cx = bar_x + UI_SQUARE / 2;
        const int pill_half = 168 / 2, hit_pad_x = 14, skip_half = 35, skip_dx = 150;
        const int pill_edge = cx + pill_half + hit_pad_x;
        const int next_edge = cx + skip_dx - skip_half - hit_pad_x;
        CHECK(next_edge > pill_edge - 1,
              "rot%d: pill hit box (to %d) overlaps next's (from %d)",
              rot * 90, pill_edge, next_edge);
        CHECK(cx + skip_dx + skip_half <= x1,
              "rot%d: next glyph overhangs the content box", rot * 90);
    }

    /*
     * The clocks. Unpadded minutes, so the widths differ -- what must
     * hold is that the pair never collides in the 672 px row, including
     * at a length nobody will ever play.
     */
    printf("  clocks fit, unpadded\n");
    {
        const int adv = 7 * 3;      /* GFX_GLYPH_W(3), halfwidth */
        static const struct { const char *l, *r; } cases[] = {
            { "0:07", "-3:12" }, { "2:41", "-1:38" }, { "59:59", "-0:01" },
            { "101:23", "-12:45" }, { "999:59", "-999:59" },
            { "1440:00", "-1440:00" }, { "--:--", "--:--" },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            const int lw2 = (int)strlen(cases[i].l) * adv;
            const int rw = (int)strlen(cases[i].r) * adv;
            CHECK(lw2 + rw <= 672, "clocks %s / %s need %d px of 672",
                  cases[i].l, cases[i].r, lw2 + rw);
        }
        /* And the thing the seven-segment version got wrong: 101
         * minutes is 101, not 99. */
        const unsigned sec = 101 * 60 + 23;
        char buf[16];
        snprintf(buf, sizeof(buf), "%u:%02u", sec / 60, sec % 60);
        CHECK(strcmp(buf, "101:23") == 0, "101 minutes formats as %s", buf);
    }

    /*
     * The menu footer (5006). panel.c and sleeppage.c both draw a
     * filled bar with one centred CLOSE button, and both used to treat
     * the whole bar as the button -- which in landscape made 86% of a
     * 1280 px strip an invisible close. The box must be centred, must
     * sit inside the bar, and must leave live bar either side at every
     * width the two orientations produce.
     */
    printf("  footer close box\n");
    {
        const int foot_h = 120, btn_w = 180, pad = 16;
        static const int widths[] = { 720, 1280 };
        static const int heights[] = { 1280, 720 };
        for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
            const int w = widths[i], h = heights[i];
            const int bx = w / 2 - btn_w / 2;
            const int by = h - foot_h + pad;
            const int bh = foot_h - 2 * pad;
            CHECK(bx > 0 && bx + btn_w < w,
                  "%dx%d: close box spans the bar (%d..%d of %d)",
                  w, h, bx, bx + btn_w, w);
            CHECK(by >= h - foot_h && by + bh <= h,
                  "%dx%d: close box escapes the bar", w, h);
            CHECK(bx == w - (bx + btn_w),
                  "%dx%d: close box is not centred (%d left, %d right)",
                  w, h, bx, w - (bx + btn_w));
            /* A press just inside the bar's left edge is bar, not
             * button -- that is the whole point of the fix. */
            CHECK(!(4 >= bx && 4 < bx + btn_w),
                  "%dx%d: x=4 in the footer still reads as close", w, h);
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
