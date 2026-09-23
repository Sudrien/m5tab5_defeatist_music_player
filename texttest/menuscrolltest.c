/*
 * menuscrolltest -- the settings pages' one scrollbar.
 *
 * The bar is the only way to reach the bottom of the sleep page in
 * landscape, where 860 px of content sit in a 504 px viewport, so its
 * arithmetic is load-bearing in a way that shows up as "the sleep timer
 * cannot be set" rather than as anything that looks like a bug in a
 * scrollbar. The failures worth catching are all off-by-a-little:
 *
 *   - a maximum that is short by a pixel leaves the last row forever
 *     half under the footer;
 *   - a bar that reaches the end of its track before the content
 *     reaches the end of itself makes the last of the content
 *     unreachable while looking finished;
 *   - and a minimum bar height that is not clamped to the track gives a
 *     bar taller than the thing it slides in, on any page long enough.
 *
 * Header-only, like cardtimetest: this compiles menuscroll.h itself.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "menuscroll.h"

static int checks, failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            printf("FAIL: ");                                              \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

/* The two shapes the pages actually run in: the sleep page portrait
 * (fits) and landscape (does not), and the panel's longest tab. */
#define PORTRAIT_VIEW   (1280 - 120 - 120)
#define LANDSCAPE_VIEW  (720 - 120 - 120)

int main(void)
{
    printf("menuscrolltest\n");

    printf("  nothing to scroll\n");
    {
        menuscroll_t s;
        memset(&s, 0, sizeof(s));
        int by = 0, bh = 0;
        CHECK(!menuscroll_geom(&s, 500, 120, PORTRAIT_VIEW, &by, &bh),
              "a page that fits still drew a bar");
        CHECK(menuscroll_max(500, PORTRAIT_VIEW) == 0, "max is not zero");
        /* Exactly full is not scrollable either -- an off-by-one here is
         * a one-pixel bar that can be grabbed and does nothing. */
        CHECK(!menuscroll_geom(&s, PORTRAIT_VIEW, 120, PORTRAIT_VIEW, &by, &bh),
              "a page exactly as tall as the view drew a bar");
        /* And a press in the strip is not consumed, so the row under it
         * still works on pages that fit. */
        CHECK(!menuscroll_touch(&s, true, true, 700, 400, 720,
                                500, 120, PORTRAIT_VIEW),
              "the strip ate a press on a page that fits");
    }

    printf("  the ends are reachable\n");
    {
        static const struct { int content, view; } cases[] = {
            { 860, LANDSCAPE_VIEW },    /* the sleep page, rotated */
            { 888, LANDSCAPE_VIEW },    /* twelve panel rows, rotated */
            { 748, LANDSCAPE_VIEW },    /* the AUDIO tab, rotated */
            { 1024, 200 }, { 201, 200 }, { 4000, 480 },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            const int c = cases[i].content, v = cases[i].view;
            menuscroll_t s;
            memset(&s, 0, sizeof(s));
            int by = 0, bh = 0;

            CHECK(menuscroll_geom(&s, c, 120, v, &by, &bh),
                  "%d in %d: no bar on content that overflows", c, v);
            CHECK(bh <= v, "%d in %d: bar %d is taller than its track %d",
                  c, v, bh, v);
            CHECK(by == 120, "%d in %d: at rest the bar is at %d, not the top",
                  c, v, by);

            /* A press at the very bottom of the track goes to the very
             * bottom of the content. This is the check that catches a
             * travel computed against the track instead of the bar. */
            menuscroll_touch(&s, true, true, 700, 120 + v - 1, 720, c, 120, v);
            CHECK(s.off == menuscroll_max(c, v),
                  "%d in %d: the bottom of the track is offset %d, not %d",
                  c, v, s.off, menuscroll_max(c, v));
            menuscroll_geom(&s, c, 120, v, &by, &bh);
            CHECK(by + bh == 120 + v,
                  "%d in %d: at the end the bar stops at %d, not %d",
                  c, v, by + bh, 120 + v);

            /* And at that offset the last pixel of the content is the
             * last pixel of the viewport: content drawn from
             * (view_y - off) ends at view_y - off + c. */
            CHECK(120 - s.off + c == 120 + v,
                  "%d in %d: fully scrolled, the content ends at %d not %d",
                  c, v, 120 - s.off + c, 120 + v);

            /* Back to the top the same way. */
            menuscroll_touch(&s, true, false, 700, 0, 720, c, 120, v);
            CHECK(s.off == 0, "%d in %d: the top of the track is offset %d",
                  c, v, s.off);
        }
    }

    printf("  the bar is never shorter than a thumb\n");
    {
        menuscroll_t s;
        memset(&s, 0, sizeof(s));
        int by = 0, bh = 0;
        /* Proportional would be four pixels here. */
        menuscroll_geom(&s, 40000, 120, 480, &by, &bh);
        CHECK(bh == MENUSCROLL_MIN_BAR, "a very long page gave a %d px bar", bh);
        /* Still reaches both ends at that clamp. */
        menuscroll_touch(&s, true, true, 700, 120 + 479, 720, 40000, 120, 480);
        menuscroll_geom(&s, 40000, 120, 480, &by, &bh);
        CHECK(by + bh == 600, "clamped bar stops at %d, not 600", by + bh);
    }

    printf("  monotonic, and clamped\n");
    {
        const int c = 860, v = LANDSCAPE_VIEW;
        menuscroll_t s;
        memset(&s, 0, sizeof(s));
        int prev_off = -1, prev_y = -1;
        for (int y = 120; y < 120 + v; y++) {
            int by = 0, bh = 0;
            menuscroll_touch(&s, true, y == 120, 700, y, 720, c, 120, v);
            menuscroll_geom(&s, c, 120, v, &by, &bh);
            CHECK(s.off >= prev_off, "offset went backwards at y=%d", y);
            CHECK(by >= prev_y, "the bar went backwards at y=%d", y);
            CHECK(s.off >= 0 && s.off <= menuscroll_max(c, v),
                  "offset %d out of range at y=%d", s.off, y);
            CHECK(by >= 120 && by + bh <= 120 + v,
                  "the bar left its track at y=%d", y);
            prev_off = s.off;
            prev_y = by;
        }
        CHECK(menuscroll_clamp(-500, c, v) == 0, "a negative offset survived");
        CHECK(menuscroll_clamp(99999, c, v) == menuscroll_max(c, v),
              "an offset past the end survived");
    }

    printf("  a drag outlives the strip it started in\n");
    {
        const int c = 860, v = LANDSCAPE_VIEW;
        menuscroll_t s;
        memset(&s, 0, sizeof(s));
        /* Down in the strip... */
        CHECK(menuscroll_touch(&s, true, true, 700, 300, 720, c, 120, v),
              "a press in the strip was not taken");
        CHECK(s.drag, "the press did not start a drag");
        /* ...then the finger wanders off it, as fingers do. The drag
         * must keep the scroll rather than handing the rows a press
         * halfway through a gesture. */
        CHECK(menuscroll_touch(&s, true, false, 20, 400, 720, c, 120, v),
              "the drag was dropped when the finger left the strip");
        /* And it ends on the up edge, once. */
        CHECK(menuscroll_touch(&s, false, false, 20, 400, 720, c, 120, v),
              "the release was not taken");
        CHECK(!s.drag, "the drag outlived the release");
        CHECK(!menuscroll_touch(&s, false, false, 20, 400, 720, c, 120, v),
              "the release was taken twice");
    }

    printf("  a press outside the strip is not ours\n");
    {
        const int c = 860, v = LANDSCAPE_VIEW;
        menuscroll_t s;
        memset(&s, 0, sizeof(s));
        CHECK(!menuscroll_touch(&s, true, true, 720 - MENUSCROLL_HIT_W - 1,
                                300, 720, c, 120, v),
              "a press left of the strip was taken");
        /* Above the viewport is the header, below it the footer, and
         * both own their own presses. */
        CHECK(!menuscroll_touch(&s, true, true, 700, 119, 720, c, 120, v),
              "a press in the header was taken");
        CHECK(!menuscroll_touch(&s, true, true, 700, 120 + v, 720, c, 120, v),
              "a press in the footer was taken");
        /* The strip is wide enough to aim at but not so wide it reaches
         * the slider knob, which stops 38 px from the right edge. */
        CHECK(MENUSCROLL_HIT_W < 38,
              "the strip (%d) overlaps the slider knob", MENUSCROLL_HIT_W);
        CHECK(MENUSCROLL_HIT_W > MENUSCROLL_W,
              "the strip is no easier to hit than the bar is to see");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
