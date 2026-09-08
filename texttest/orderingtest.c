/*
 * orderingtest.c -- the rings are a queue, not a pair.
 *
 * The writer used to hand off with `play = fill`, which jumps to the
 * NEWEST ring. With two tracks in flight that is also the next ring and
 * it never showed. With three it skips the one in between, permanently,
 * because nothing goes back for it.
 *
 * The property is stated over the AUDIO: every byte written to a ring is
 * eventually played, in the order the tracks were decoded.
 */
#include <stdio.h>
#include <stdbool.h>

#define MAXR 4

static int  R;                       /* PCM_RINGS under test */
static int  bytes[MAXR];
static int  ident[MAXR];             /* which track's audio is in there */
static int  play, fill;
static int  played[64], nplayed;
static int  skips;

static void advance_play(void)
{
    const int next = (play + 1) % R;
    if (next != fill && bytes[next] == 0) { skips++; play = fill; return; }
    play = next;
}

/* writer: drain the play ring, then hand off */
static void writer_run(void)
{
    while (1) {
        if (bytes[play] > 0) {
            played[nplayed++] = ident[play];
            bytes[play] = 0;
            continue;
        }
        if (play == fill) return;
        advance_play();
    }
}

/* decode loop: a new track takes the next ring.
 *
 * The wait is 1114: never reuse a ring that still holds unplayed audio.
 * It is modelled here because 1114 and 1115 only work together -- the
 * wait stops the overwrite, the sequential handoff stops the skip, and
 * either alone leaves a track unheard. */
static void writer_run(void);
static void decode_track(int id, int amount)
{
    fill = (fill + 1) % R;
    if (bytes[fill] > 0) writer_run();          /* wait for it to play out */
    bytes[fill] = amount;
    ident[fill] = id;
}

static void reset(int rings)
{
    R = rings; play = fill = 0; nplayed = 0; skips = 0;
    for (int i = 0; i < MAXR; i++) { bytes[i] = 0; ident[i] = 0; }
}

static int fails;
static void ck(const char *what, bool ok)
{ printf("%-58s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

int main(void)
{
    /* Three tracks in flight: A still playing, B short, C arrives.
     * With two rings there is no valid assignment -- C must reuse A's
     * ring while B is unplayed. */
    reset(2);
    bytes[0] = 100; ident[0] = 1;          /* A queued, not yet played */
    decode_track(2, 100);                  /* B -> ring 1 */
    decode_track(3, 100);                  /* C -> ring 0, over A */
    writer_run();
    bool got_b = false;
    for (int i = 0; i < nplayed; i++) if (played[i] == 2) got_b = true;
    ck("two rings cannot express three tracks", !got_b);

    /* Three rings: every track gets played, in order. */
    reset(3);
    bytes[0] = 100; ident[0] = 1;
    decode_track(2, 100);
    decode_track(3, 100);
    writer_run();
    ck("three rings play all three tracks", nplayed == 3);
    ck("  and in the order they were decoded",
       played[0] == 1 && played[1] == 2 && played[2] == 3);
    ck("  with no out-of-turn skip", skips == 0);

    /*
     * Repeated short tracks, each separated by a track long enough to
     * drain -- which is what an album with interludes looks like. Three
     * rings has to hold across all of them, not just the first.
     */
    reset(3);
    bool order_ok = true;
    int expect = 0;
    for (int t = 1; t <= 20; t++) {
        if (t == 1) { bytes[0] = 100; ident[0] = 1; expect++; continue; }
        decode_track(t, 100);
        expect++;
        if (t % 2 == 0) continue;        /* short: leave it queued */
        writer_run();                    /* long: it drains */
    }
    writer_run();
    for (int i = 1; i < nplayed; i++)
        if (played[i] < played[i - 1]) order_ok = false;
    ck("an album of alternating short tracks stays in order",
       nplayed == expect && order_ok);

    /*
     * FOUR tracks in flight is beyond what three rings can express, and
     * this records that rather than hiding it. Two consecutive tracks
     * shorter than a ring put four in flight, there is no free ring, and
     * the decode loop blocks in 1114's wait until the writer drains one.
     * That is the designed backstop; a fourth ring would be the same
     * argument again, one 3.5 MB later.
     */
    reset(3);
    bytes[0] = 100; ident[0] = 1;
    decode_track(2, 100);
    decode_track(3, 100);
    ck("three rings are full with three tracks queued",
       bytes[0] && bytes[1] && bytes[2]);
    ck("  a fourth would have to wait, not overwrite",
       bytes[(fill + 1) % R] > 0);

    /* The fallback: a flush empties a ring out of turn. */
    reset(3);
    bytes[0] = 100; ident[0] = 1;
    decode_track(2, 100);
    bytes[1] = 0;                       /* flushed, as a seek would */
    decode_track(3, 100);
    writer_run();
    ck("an out-of-turn empty ring is skipped, not stalled", skips == 1);
    ck("  and the newest track still plays",
       nplayed >= 1 && played[nplayed - 1] == 3);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
