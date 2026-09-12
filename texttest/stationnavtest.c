/*
 * stationnavtest.c -- moving through the station list.
 *
 * stationlisttest.c already covers the parser. This covers the two
 * things stations.c adds on top of it and that the parser cannot see:
 * the index arithmetic, and the one-station case.
 *
 * The wrap is worth a test on its own because `(i - 1) % n` is not
 * `n - 1` in C for i == 0 -- it is 0, or -1, depending on which value
 * you started from -- and a previous-station button that silently did
 * nothing on the first station would look like a dead button rather than
 * like a bug.
 *
 * stations.c itself needs FreeRTOS and storage_io, so the arithmetic is
 * reimplemented here against the same rules rather than linked. That is
 * a copy, and it is justified only because the rules are four lines and
 * the test states them: a divergence shows up as this file disagreeing
 * with the source it was written from, which is exactly what it is for.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "stationlist.h"

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

/* The rules, as stations.c states them. */
static int s_count;
static int s_index;

static int nav_next(void)
{
    if (s_count <= 0) return 0;
    s_index = (s_index + 1) % s_count;
    return s_index;
}

static int nav_prev(void)
{
    if (s_count <= 0) return 0;
    s_index = (s_index + s_count - 1) % s_count;
    return s_index;
}

static void nav_set(int i)
{
    if (s_count <= 0) { s_index = 0; return; }
    if (i < 0 || i >= s_count) return;
    s_index = i;
}


int main(void)
{
    printf("stationnavtest\n");

    /* ---------------------------------------------------------------- */
    /* Three stations: forward, wrap, backward, wrap                     */
    /* ---------------------------------------------------------------- */
    s_count = 3; s_index = 0;
    CHECK(nav_next() == 1, "0 -> 1");
    CHECK(nav_next() == 2, "1 -> 2");
    CHECK(nav_next() == 0, "2 wraps to 0");

    /* THE ONE THIS FILE EXISTS FOR. (0 - 1) % 3 is not 2 in C. */
    s_index = 0;
    CHECK(nav_prev() == 2, "0 wraps back to 2, not %d", s_index);
    CHECK(nav_prev() == 1, "2 -> 1");
    CHECK(nav_prev() == 0, "1 -> 0");

    /* ---------------------------------------------------------------- */
    /* One station: both buttons stay put rather than going out of range */
    /* ---------------------------------------------------------------- */
    s_count = 1; s_index = 0;
    CHECK(nav_next() == 0, "one station: next stays");
    CHECK(nav_prev() == 0, "one station: prev stays");

    /* ---------------------------------------------------------------- */
    /* No stations: never a negative index and never a divide by zero    */
    /* ---------------------------------------------------------------- */
    s_count = 0; s_index = 0;
    CHECK(nav_next() == 0, "empty: next is 0");
    CHECK(nav_prev() == 0, "empty: prev is 0");
    nav_set(5);
    CHECK(s_index == 0, "empty: set is clamped to 0");

    /* ---------------------------------------------------------------- */
    /* set_index refuses out of range rather than clamping              */
    /* ---------------------------------------------------------------- */
    s_count = 3; s_index = 1;
    nav_set(2);
    CHECK(s_index == 2, "in range");
    nav_set(3);
    CHECK(s_index == 2, "past the end is refused, not clamped to 2");
    nav_set(-1);
    CHECK(s_index == 2, "negative is refused");

    /* A full lap returns to where it started, at every length. This is
     * the property, rather than a list of transitions. */
    for (int n = 1; n <= 8; n++) {
        s_count = n;
        for (int start = 0; start < n; start++) {
            s_index = start;
            for (int k = 0; k < n; k++) nav_next();
            CHECK(s_index == start, "n=%d: a lap forward returns to %d, got %d",
                  n, start, s_index);
            for (int k = 0; k < n; k++) nav_prev();
            CHECK(s_index == start, "n=%d: a lap back returns to %d, got %d",
                  n, start, s_index);
            /* And next-then-prev is always identity. */
            nav_next(); nav_prev();
            CHECK(s_index == start, "n=%d: next then prev is identity", n);
        }
    }

    /* ---------------------------------------------------------------- */
    /* The parser and the navigation agree on what a list is             */
    /* ---------------------------------------------------------------- */
    {
        static const char file[] =
            "#EXTM3U\n"
            "#EXTINF:-1,Michigan Radio\n"
            "https://26433.live.streamtheworld.com/WUOMFM.mp3\n"
            "#EXTINF:-1,WNZK\n"
            "https://stream.zeno.fm/erunhwj5lekvv\n"
            "https://example.com/bare.mp3\n";
        station_t st[STATIONLIST_MAX];
        stationlist_stats_t stats;
        const int n = stationlist_parse(file, sizeof(file) - 1, st,
                                        STATIONLIST_MAX, &stats);
        CHECK(n == 3, "three stations, got %d", n);
        CHECK(strcmp(st[0].name, "Michigan Radio") == 0,
              "first name \"%s\"", st[0].name);
        CHECK(strcmp(st[1].name, "WNZK") == 0, "second name \"%s\"",
              st[1].name);
        /* The bare URL gets its host, which is the documented fallback
         * and the reason a pasted file is usable. */
        CHECK(strcmp(st[2].name, "example.com") == 0,
              "bare URL named \"%s\"", st[2].name);

        s_count = n; s_index = 0;
        CHECK(nav_next() == 1 && nav_next() == 2 && nav_next() == 0,
              "navigation over a parsed list wraps");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
