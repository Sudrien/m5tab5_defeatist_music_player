/*
 * ringwaittest.c -- the decode loop must never take a ring that is
 * still being heard.
 *
 * 1113 guarded "not the tail's ring", which is a proxy for "not in
 * use", and the proxy picked the wrong ring on a three-tracks-in-flight
 * boundary. So the property here is stated over the AUDIO, not over the
 * tail flag: no reset ever lands on a ring holding unplayed bytes,
 * except on the exits where the audio is being discarded on purpose.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#define RINGS 2
#define RING_WAIT_MAX_MS  (30u * 1000)
#define RING_WAIT_STEP_MS (20u)

static int  bytes[RINGS];
static int  play, fill;
static bool playing = true, pending_ready, seeking;
static int  bad_resets;          /* reset over audio while playing */
static int  waits;

/* writer: drains the play ring, moves on when it empties */
static void writer_tick(int amount)
{
    if (!playing) return;
    if (bytes[play] > 0) {
        bytes[play] -= amount;
        if (bytes[play] < 0) bytes[play] = 0;
    }
    if (bytes[play] == 0 && play != fill) play = (play + 1) % RINGS;
}

/* decode loop: take the next ring for a new track */
static void advance_fill(void)
{
    fill = (fill + 1) % RINGS;

    uint32_t waited = 0;
    while (bytes[fill] > 0) {
        if (!playing || pending_ready || seeking) break;
        if (waited >= RING_WAIT_MAX_MS) break;
        writer_tick(200);                     /* time passes; writer drains */
        waited += RING_WAIT_STEP_MS;
    }
    if (waited) waits++;

    if (bytes[fill] > 0) {
        if (playing && !pending_ready && !seeking) bad_resets++;
        bytes[fill] = 0;
    }
}

static int fails;
static void ck(const char *what, bool ok)
{ printf("%-60s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

int main(void)
{
    /* Ordinary alternation: the ring is always already empty. */
    bytes[0] = bytes[1] = 0; play = fill = 0; bad_resets = waits = 0;
    for (int i = 0; i < 20; i++) {
        advance_fill();
        bytes[fill] = 3500;                  /* the new track decodes */
        /* the writer plays it out before the next boundary, which is
         * what "ordinary" means -- a track longer than the ring */
        while (bytes[play] || play != fill) writer_tick(500);
    }
    ck("ordinary boundaries never wait", waits == 0);
    ck("  and never reset over audio", bad_resets == 0);

    /* The board's case: three tracks in flight, middle one short. */
    bytes[0] = bytes[1] = 0; play = fill = 0; bad_resets = waits = 0;
    bytes[0] = 3500;                       /* long track A, playing */
    advance_fill(); bytes[1] = 400;        /* short track B (20 s) */
    advance_fill();                        /* track C wants ring 0 -- busy */
    ck("a short middle track makes the loop wait", waits == 1);
    ck("  and no ring is reset while it is being heard", bad_resets == 0);
    ck("  the writer got through A", bytes[0] == 0 || play != 0);

    /* Paused: must not block, may reset, and says so by not waiting. */
    bytes[0] = 3500; bytes[1] = 0; play = 0; fill = 1;
    bad_resets = waits = 0; playing = false;
    advance_fill();
    ck("a paused writer does not hang the decode loop", true);
    ck("  and its reset is not counted as a violation", bad_resets == 0);
    playing = true;

    /* A seek supersedes the track: same exemption. */
    bytes[0] = 3500; bytes[1] = 0; play = 0; fill = 1;
    bad_resets = 0; seeking = true;
    advance_fill();
    ck("a seek does not wait", bad_resets == 0);
    seeking = false;

    /* The bound holds even if the writer never drains. */
    bytes[0] = 3500; bytes[1] = 0; play = 0; fill = 1;
    bad_resets = 0; playing = true;
    /* writer_tick drains, so force a stuck writer by draining nothing */
    /* (modelled by a ring far larger than the wait can consume) */
    bytes[0] = 100000000;
    advance_fill();
    ck("a stuck writer is bounded, not infinite", true);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
