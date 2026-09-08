/*
 * tailtest.c -- the tail slot, and the two ways it used to strand.
 *
 * s_tail_pending and s_tail_ring are ONE slot. Everything here is about
 * what happens when a second tail wants it, or when the ring it names is
 * taken for the next track. Both need a track shorter than the ring
 * depth, which is why they only showed up around a 20 s track on a ring
 * that holds 20 s.
 */
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define RINGS 2

static bool  pending;
static int   tail_ring;
static bool  released;
static int   lost;                 /* XEXIT_TAIL_LOST: audio discarded */
static int   slot;                 /* XEXIT_TAIL_SLOT: bookkeeping only */
static int   dry;                  /* XEXIT_TAIL_DRY  */
static int   ring_bytes[RINGS];
static int   play, fill;

static void tail_retire(bool audio_discarded)
{
    if (!pending) return;
    pending = false;
    if (audio_discarded) lost++; else slot++;
    released = true;
}

/* decode loop: take the other ring for the next track */
static void advance_fill(void)
{
    fill = (fill + 1) % RINGS;
    if (pending && tail_ring == fill) tail_retire(true);
    ring_bytes[fill] = 0;                    /* the reset */
}

/* decode loop: this track ended with audio still queued */
static void latch_tail(void)
{
    if (ring_bytes[fill] == 0) return;
    if (pending && tail_ring != fill) tail_retire(false);
    tail_ring = fill;
    pending = true;
}

/* The next track starting. This -- not the latch -- is the only place
 * the release is lowered, which is what makes tail_retire()'s raise
 * meaningful: it belongs to the boundary being abandoned, not to the
 * one after it. */
static void track_change_begin(void)
{
    released = !pending;
}

/* writer: the tail ring ran dry */
static void writer_drain(void)
{
    if (ring_bytes[play] > 0) { ring_bytes[play] = 0; }
    if (play != fill) play = fill;
    if (pending && ring_bytes[tail_ring] == 0) {
        pending = false; dry++; released = true;
    }
}

static void reset_all(void)
{
    memset(ring_bytes, 0, sizeof ring_bytes);
    pending = false; released = false; lost = slot = dry = 0; play = fill = 0;
}

static int fails;
static void ck(const char *what, bool ok)
{ printf("%-60s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

int main(void)
{
    /* Ordinary boundary: one tail, drains, no loss. */
    reset_all();
    ring_bytes[fill] = 3500; latch_tail();
    ck("an ordinary tail latches", pending && !released);
    track_change_begin();
    advance_fill(); ring_bytes[fill] = 3500;   /* next track fills the other */
    writer_drain();
    ck("  drains normally", !pending && dry == 1 && lost == 0);
    ck("  and releases the screen", released);

    /* A track shorter than the ring: its own tail latches while the
     * previous one is still queued. The old slot must not vanish
     * silently. */
    reset_all();
    ring_bytes[0] = 3500; fill = 0; latch_tail();       /* track A tail */
    track_change_begin();
    advance_fill(); ring_bytes[1] = 3500;               /* short track B */
    latch_tail();                                       /* B's tail */
    ck("a second tail takes the slot, without discarding audio",
       slot == 1 && lost == 0);
    ck("  the slot now names the newer ring", pending && tail_ring == 1);
    ck("  and the screen was released for the lost one", released);

    /* The next track needs the ring the tail is in. */
    reset_all();
    ring_bytes[1] = 3500; fill = 1; latch_tail();
    ck("tail is on ring 1", pending && tail_ring == 1);
    fill = 0;                       /* pretend we are on the other one */
    advance_fill();                 /* -> back to ring 1, the tail's */
    ck("advancing onto the tail's ring discards it", !pending && lost == 1);
    ck("  and releases the screen", released);

    /* Never two pendings at once, over a long run of short tracks. */
    reset_all();
    bool ok = true;
    for (int i = 0; i < 50; i++) {
        track_change_begin();
        advance_fill();
        ring_bytes[fill] = 3500;
        latch_tail();
        if (!pending) ok = false;               /* exactly one, always */
    }
    ck("fifty short tracks never strand a tail", ok);
    /* The count is one per boundary that had a tail already pending;
     * what matters is that NONE of them discarded audio. */
    ck("  every takeover was bookkeeping, and none discarded audio",
       slot > 0 && lost == 0);

    /* A retire is idempotent and safe with nothing pending. */
    reset_all();
    tail_retire(false);
    tail_retire(true);
    ck("retiring nothing does nothing",
       lost == 0 && slot == 0 && !released);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
