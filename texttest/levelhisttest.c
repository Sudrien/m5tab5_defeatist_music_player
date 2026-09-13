/*
 * levelhisttest.c -- the minute-wide level strip.
 *
 * Time arithmetic over a ring, which is the shape that has gone wrong
 * most often on this project: a bucket boundary landing between two
 * calls, a block longer than a bucket, a ring read from the wrong end,
 * a reserve rounding to zero at the exact moment it matters. All of it
 * runs on a host, so all of it is checked here rather than on a panel
 * where a one-column error is invisible.
 *
 * The case that matters most is the rebuffer: a stretch of silence has
 * to take up its own width, because a strip that closes the gap hides
 * the only thing it was built to show.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "levelhist.h"

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

/* How many of the last `n` columns are non-zero. */
static int tail_nonzero(const uint8_t *s, int n)
{
    int c = 0;
    for (int i = LEVELHIST_COLUMNS - n; i < LEVELHIST_COLUMNS; i++) {
        if (s[i]) c++;
    }
    return c;
}

int main(void)
{
    printf("levelhisttest\n");

    uint8_t strip[LEVELHIST_COLUMNS];
    levelhist_t h;

    /* ---------------------------------------------------------------- */
    /* Geometry                                                          */
    /* ---------------------------------------------------------------- */
    CHECK(LEVELHIST_BUCKET_MS == 250, "bucket is %d ms", LEVELHIST_BUCKET_MS);
    CHECK(LEVELHIST_COLUMNS * LEVELHIST_BUCKET_MS == LEVELHIST_SPAN_MS,
          "columns x bucket must be exactly the span");
    CHECK(LEVELHIST_NOW_COLUMN + LEVELHIST_AHEAD_COLUMNS == LEVELHIST_COLUMNS,
          "the mark and the room ahead must tile the strip");
    CHECK(LEVELHIST_AHEAD_COLUMNS * LEVELHIST_BUCKET_MS == 20000,
          "20 s of room ahead, got %d ms",
          LEVELHIST_AHEAD_COLUMNS * LEVELHIST_BUCKET_MS);

    /* ---------------------------------------------------------------- */
    /* An empty strip is silence, at full width                          */
    /* ---------------------------------------------------------------- */
    levelhist_reset(&h);
    levelhist_read(&h, strip);
    {
        int nz = 0;
        for (int i = 0; i < LEVELHIST_COLUMNS; i++) if (strip[i]) nz++;
        CHECK(nz == 0, "a reset strip is all zero, got %d set", nz);
    }
    /* Full width from the start, so the mark does not move for the first
     * minute of every station. */
    CHECK(h.filled == 0, "nothing filled yet");

    /* ---------------------------------------------------------------- */
    /* Buckets close on time, not per call                               */
    /* ---------------------------------------------------------------- */
    levelhist_reset(&h);
    /* Ten writer chunks of 25 ms make one column, not ten. */
    for (int i = 0; i < 10; i++) levelhist_push(&h, 32767, 25);
    CHECK(h.filled == 1, "10 x 25 ms closes one column, got %d", h.filled);
    levelhist_read(&h, strip);
    CHECK(strip[LEVELHIST_COLUMNS - 1] == 255,
          "full scale reads 255, got %d", strip[LEVELHIST_COLUMNS - 1]);

    /* The peak wins within a bucket: one loud chunk among quiet ones. */
    levelhist_reset(&h);
    levelhist_push(&h, 100, 100);
    levelhist_push(&h, 32767, 50);
    levelhist_push(&h, 100, 100);
    CHECK(h.filled == 1, "one column");
    levelhist_read(&h, strip);
    CHECK(strip[LEVELHIST_COLUMNS - 1] == 255,
          "the peak of the bucket wins, got %d", strip[LEVELHIST_COLUMNS - 1]);

    /* And does not leak into the next one. */
    levelhist_push(&h, 0, LEVELHIST_BUCKET_MS);
    levelhist_read(&h, strip);
    CHECK(strip[LEVELHIST_COLUMNS - 1] == 0,
          "a loud bucket does not stain the next, got %d",
          strip[LEVELHIST_COLUMNS - 1]);

    /* ---------------------------------------------------------------- */
    /* A block longer than a bucket fills every bucket it spans          */
    /* ---------------------------------------------------------------- */
    levelhist_reset(&h);
    levelhist_push(&h, 32767, LEVELHIST_BUCKET_MS * 3);
    CHECK(h.filled == 3, "a 750 ms block closes three columns, got %d",
          h.filled);
    levelhist_read(&h, strip);
    CHECK(tail_nonzero(strip, 3) == 3,
          "all three are loud, got %d", tail_nonzero(strip, 3));

    /* Two and a half buckets leaves half a bucket pending, not lost. */
    levelhist_reset(&h);
    levelhist_push(&h, 32767, LEVELHIST_BUCKET_MS * 2 + 125);
    CHECK(h.filled == 2, "two closed, got %d", h.filled);
    CHECK(h.pending_ms == 125, "125 ms pending, got %d", h.pending_ms);
    CHECK(h.pending == 255, "the spill keeps the block's level, got %d",
          h.pending);

    /* ---------------------------------------------------------------- */
    /* THE ONE THAT MATTERS: silence takes up room                       */
    /* ---------------------------------------------------------------- */
    levelhist_reset(&h);
    /* Ten seconds of music, two seconds of dropout, ten more of music --
     * which is what WNZK does every thirty-five seconds. */
    for (int i = 0; i < 40; i++) levelhist_push(&h, 20000, LEVELHIST_BUCKET_MS);
    for (int i = 0; i < 8;  i++) levelhist_silence(&h, LEVELHIST_BUCKET_MS);
    for (int i = 0; i < 40; i++) levelhist_push(&h, 20000, LEVELHIST_BUCKET_MS);

    CHECK(h.filled == 88, "88 columns of history, got %d", h.filled);
    levelhist_read(&h, strip);
    {
        /* The gap must be eight columns wide and in the right place:
         * 40 from the end of the last run, back 8. */
        const int end = LEVELHIST_COLUMNS;
        int gap = 0;
        for (int i = end - 48; i < end - 40; i++) if (strip[i] == 0) gap++;
        CHECK(gap == 8, "the dropout is 8 columns wide, got %d", gap);
        CHECK(strip[end - 41] == 0, "the column before the resume is silent");
        CHECK(strip[end - 40] != 0, "and the resume is not");
        CHECK(strip[end - 49] != 0, "nor is the column before the dropout");
    }

    /* ---------------------------------------------------------------- */
    /* The ring wraps and keeps the NEWEST minute                        */
    /* ---------------------------------------------------------------- */
    levelhist_reset(&h);
    /* Two minutes: the first at one level, the second at another. Only
     * the second should survive. */
    for (int i = 0; i < LEVELHIST_COLUMNS; i++)
        levelhist_push(&h, 8000, LEVELHIST_BUCKET_MS);
    for (int i = 0; i < LEVELHIST_COLUMNS; i++)
        levelhist_push(&h, 32767, LEVELHIST_BUCKET_MS);
    CHECK(h.wrapped, "the ring has wrapped");
    levelhist_read(&h, strip);
    {
        int wrong = 0;
        for (int i = 0; i < LEVELHIST_COLUMNS; i++) if (strip[i] != 255) wrong++;
        CHECK(wrong == 0, "the older minute is gone, %d columns stale", wrong);
    }

    /* Oldest first: a single loud column after a wrap must read at the
     * END of the strip, not the start. Reading the ring from the wrong
     * end would pass every test above and draw the minute backwards. */
    levelhist_push(&h, 0, LEVELHIST_BUCKET_MS * (LEVELHIST_COLUMNS - 1));
    levelhist_push(&h, 32767, LEVELHIST_BUCKET_MS);
    levelhist_read(&h, strip);
    CHECK(strip[LEVELHIST_COLUMNS - 1] == 255,
          "the newest column is last, got %d", strip[LEVELHIST_COLUMNS - 1]);
    CHECK(strip[0] == 0, "and the oldest is first, got %d", strip[0]);

    /* ---------------------------------------------------------------- */
    /* The reserve ahead of the mark                                     */
    /* ---------------------------------------------------------------- */
    CHECK(levelhist_ahead_columns(0) == 0, "no reserve, no grey");
    CHECK(levelhist_ahead_columns(-5) == 0, "negative is no reserve");

    /* Rounded UP, and this is the case it exists for: 200 ms is the
     * difference between playing and a dropout, and no columns would
     * draw the healthiest picture at the worst moment. */
    CHECK(levelhist_ahead_columns(1) == 1, "1 ms still draws a column");
    CHECK(levelhist_ahead_columns(200) == 1, "200 ms draws a column");
    CHECK(levelhist_ahead_columns(250) == 1, "exactly one bucket");
    CHECK(levelhist_ahead_columns(251) == 2, "just over rounds up");

    /* The figures from the board, so the strip can be read against a log. */
    CHECK(levelhist_ahead_columns(4090) == 17,
          "WNZK's 4.09s is 17 columns, got %d", levelhist_ahead_columns(4090));
    CHECK(levelhist_ahead_columns(1490) == 6,
          "and its 1.49s low is 6, got %d", levelhist_ahead_columns(1490));
    CHECK(levelhist_ahead_columns(1490) < levelhist_ahead_columns(4090),
          "the sawtooth must be visible as a difference in width");

    /* SomaFM's 21 s, and WUOM's 50 s on a PC, both clip. */
    CHECK(levelhist_ahead_columns(21000) == LEVELHIST_AHEAD_COLUMNS,
          "21 s clips to the full width");
    CHECK(levelhist_ahead_columns(50000) == LEVELHIST_AHEAD_COLUMNS,
          "so does 50 s");
    CHECK(!levelhist_ahead_clipped(20000), "exactly 20 s is not clipped");
    CHECK(levelhist_ahead_clipped(21000), "21 s is clipped and says so");
    CHECK(!levelhist_ahead_clipped(4090), "4 s is not clipped");
    CHECK(!levelhist_ahead_clipped(0), "nothing is not clipped");

    /* ---------------------------------------------------------------- */
    /* Nothing crashes on nothing                                        */
    /* ---------------------------------------------------------------- */
    levelhist_reset(NULL);
    levelhist_push(NULL, 100, 100);
    levelhist_silence(NULL, 100);
    levelhist_read(NULL, strip);
    levelhist_read(&h, NULL);
    levelhist_push(&h, 100, 0);         /* zero-length block is a no-op */
    levelhist_push(&h, 100, -1);
    CHECK(1, "null and zero arguments survive");

    /* Out-of-range levels are clamped rather than wrapped: a sample of
     * 40000 shifted down would otherwise alias to something quiet. */
    levelhist_reset(&h);
    levelhist_push(&h, 999999, LEVELHIST_BUCKET_MS);
    levelhist_read(&h, strip);
    CHECK(strip[LEVELHIST_COLUMNS - 1] == 255,
          "an over-range peak clamps loud, got %d",
          strip[LEVELHIST_COLUMNS - 1]);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
