/*
 * xfadetest.c -- the crossfade block's exit conditions.
 *
 * The claim behind 1105 is that there is a reachable state in which the
 * overlap can neither advance nor end: the outgoing ring holds more than
 * zero bytes but less than one frame. Not empty, so the handoff at the
 * top of the writer loop does not fire; not a frame, so neither the mix
 * nor the solo path can take it.
 *
 * Transcribed from the writer loop, so it checks the arithmetic rather
 * than the board.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>

#define PCM_CHUNK_BYTES      (4 * 1024)
#define PCM_BYTES_PER_FRAME  (4)

typedef enum { HANDOFF, MIX, SOLO, FELL_THROUGH } exit_t;

static exit_t classify(size_t avail_a, size_t avail_b)
{
    /* Top of the loop: the handoff fires only on a truly empty ring. */
    if (avail_a == 0) return HANDOFF;

    size_t n = PCM_CHUNK_BYTES;
    if (avail_a < n) n = avail_a;
    if (avail_b < n) n = avail_b;
    n &= ~(size_t)(PCM_BYTES_PER_FRAME - 1);
    if (n) return MIX;

    size_t solo = (avail_a < PCM_CHUNK_BYTES)
                ? (avail_a & ~(size_t)(PCM_BYTES_PER_FRAME - 1))
                : PCM_CHUNK_BYTES;
    if (solo) return SOLO;

    return FELL_THROUGH;   /* neither advances nor ends the overlap */
}

static int fails;
static void ck(const char *what, bool ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    /* The ordinary cases still go where they went. */
    ck("both rings full -> mix",        classify(8192, 8192) == MIX);
    ck("incoming starved -> solo",      classify(8192, 0)    == SOLO);
    ck("incoming partial -> solo",      classify(8192, 3)    == SOLO);
    ck("outgoing empty -> handoff",     classify(0, 8192)    == HANDOFF);
    ck("one frame each -> mix",         classify(4, 4)       == MIX);

    /* The hole: 1..3 bytes in the outgoing ring. */
    bool all = true;
    for (size_t a = 1; a < PCM_BYTES_PER_FRAME; a++) {
        if (classify(a, 8192) != FELL_THROUGH) all = false;
        if (classify(a, 0)    != FELL_THROUGH) all = false;
    }
    ck("a sub-frame remainder reaches neither mix, solo nor handoff", all);

    /* And it is only the sub-frame case -- nothing else falls through. */
    bool only = true;
    for (size_t a = 0; a <= 64; a++) {
        for (size_t b = 0; b <= 64; b++) {
            const bool sub = (a > 0 && a < PCM_BYTES_PER_FRAME);
            if ((classify(a, b) == FELL_THROUGH) != sub) only = false;
        }
    }
    ck("no other combination falls through", only);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
