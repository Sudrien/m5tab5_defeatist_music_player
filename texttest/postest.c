/*
 * postest.c -- which ring the seek bar reads during a boundary.
 *
 * The rule is one line, and getting it wrong is invisible in code review
 * and obvious on the glass. What is checked here is that the position
 * and the LENGTH always come from the same track -- the pairing, not the
 * ring index, is the property that matters.
 */
#include <stdio.h>
#include <stdbool.h>

/* Which ring the position is read from. */
static int pos_ring(int play, int fill, bool released)
{
    if (released && play != fill) return fill;
    return play;
}

/* The length the screen shows comes from track_commit(), which fires on
 * the same flag. So the pairing test is: do these two agree? */
static int len_ring(int play, int fill, bool released)
{
    (void)play;
    return released ? fill : -1;   /* -1 == "still the outgoing track" */
}

static bool agree(int play, int fill, bool released)
{
    const int p = pos_ring(play, fill, released);
    const int l = len_ring(play, fill, released);
    return released ? (p == l) : (p == play);
}

static int fails;
static void ck(const char *what, bool ok)
{
    printf("%-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    /* Ordinary playback: one ring, nothing pending. */
    ck("single ring, not released -> plays its own ring",
       pos_ring(0, 0, false) == 0);
    ck("single ring, released -> still its own ring",
       pos_ring(0, 0, true) == 0);

    /* Decode-ahead, tail still audible: the screen has not turned over,
     * so the bar must stay on the outgoing ring. */
    ck("tail playing, not released -> outgoing ring",
       pos_ring(0, 1, false) == 0);
    ck("  and the bar agrees with the title", agree(0, 1, false));

    /* Mid-crossfade past the midpoint: the screen has turned over and
     * s_ring_play has NOT. This is the case that was broken. */
    ck("overlap past midpoint -> incoming ring",
       pos_ring(0, 1, true) == 1);
    ck("  and the bar agrees with the title", agree(0, 1, true));

    /* Overlap complete: s_ring_play caught up. */
    ck("overlap done -> the one ring, either way",
       pos_ring(1, 1, true) == 1 && pos_ring(1, 1, false) == 1);

    /* The mirrored ring assignment must behave identically -- the rings
     * alternate, so ring 1 outgoing is just as common as ring 0. */
    ck("mirrored: outgoing 1, incoming 0, not released",
       pos_ring(1, 0, false) == 1);
    ck("mirrored: outgoing 1, incoming 0, released",
       pos_ring(1, 0, true) == 0);

    /* The property that actually matters, over every combination. */
    bool all = true;
    for (int play = 0; play < 2; play++)
        for (int fill = 0; fill < 2; fill++)
            for (int r = 0; r < 2; r++)
                if (!agree(play, fill, r != 0)) all = false;
    ck("position and length never come from different tracks", all);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
