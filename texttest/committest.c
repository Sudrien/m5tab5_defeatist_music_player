/*
 * committest.c -- when the screen is allowed to change.
 *
 * The release rule extracted from player.c: s_visuals_released is
 * lowered by track_change_begin(), raised by the writer at the crossfade
 * midpoint or when the outgoing ring runs dry, and read by
 * track_commit_due(). What is being checked is ORDERING against the
 * audio, which is the only property that matters and the one a board log
 * shows last.
 *
 * The lines marked EXACT are the ones that must not move: an ordinary
 * track change has to fire at the same instant it always did.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- the extracted rule -------------------------------------------- */

static bool     g_released;
static uint32_t g_xfade_pos, g_xfade_frames;
static bool     g_xfade_active, g_tail_pending;

static void track_change_begin_model(bool tail_playing)
{
    g_released = !tail_playing;
}

/* xfade_mix()'s tail, verbatim in shape. */
static void xfade_advance(uint32_t frames)
{
    const uint32_t total = g_xfade_frames;
    uint32_t p1 = g_xfade_pos + frames;
    if (p1 > total) p1 = total;
    g_xfade_pos = p1;
    if (!g_released && p1 * 2u >= total) g_released = true;
}

/* the writer's tail-dry check */
static void tail_dry(void)
{
    if (g_tail_pending) { g_tail_pending = false; g_released = true; }
}

static bool track_commit_due(void) { return g_released; }

/* ---- cases ---------------------------------------------------------- */

static int fails;
static void ck(const char *what, int ok)
{
    printf("%-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    /* 1. No tail at all: play from stopped. EXACT -- fires immediately. */
    g_tail_pending = false; g_xfade_active = false;
    track_change_begin_model(false);
    ck("play from stopped commits at once", track_commit_due());

    /* 2. Tail, no crossfade. EXACT -- fires when the ring runs dry and
     *    not one pass earlier. */
    g_tail_pending = true;
    track_change_begin_model(true);
    ck("tail playing: held while the old track is audible", !track_commit_due());
    tail_dry();
    ck("  and released the instant the ring runs dry", track_commit_due());

    /* 3. Crossfade: the case this change exists for. 5 s at 44100,
     *    delivered in 1024-frame chunks. */
    g_tail_pending = true; g_xfade_active = true;
    g_xfade_pos = 0; g_xfade_frames = 5 * 44100;
    track_change_begin_model(true);

    bool at_start = track_commit_due();
    int  released_at = -1;
    for (uint32_t n = 0; g_xfade_pos < g_xfade_frames; n++) {
        xfade_advance(1024);
        if (released_at < 0 && track_commit_due()) released_at = (int)g_xfade_pos;
    }
    ck("crossfade: not released at the start of the overlap", !at_start);
    ck("  released at the midpoint, not the end",
       released_at >= 0 && released_at >= (int)g_xfade_frames / 2 &&
       released_at < (int)g_xfade_frames / 2 + 1024);
    ck("  which is well before the overlap finishes",
       released_at < (int)g_xfade_frames - 1024);

    /* 4. A crossfade shorter than one chunk still releases. This is why
     *    the test is on the advanced position and not the previous one. */
    g_xfade_pos = 0; g_xfade_frames = 500;
    track_change_begin_model(true);
    xfade_advance(1024);
    ck("an overlap shorter than one chunk still releases", track_commit_due());

    /* 5. Cut short: the outgoing ring empties before the midpoint. The
     *    backstop has to fire or the screen never changes. */
    g_tail_pending = true;
    g_xfade_pos = 0; g_xfade_frames = 5 * 44100;
    track_change_begin_model(true);
    xfade_advance(1024);                 /* nowhere near halfway */
    ck("cut-short crossfade is not released by the overlap",
       !track_commit_due());
    tail_dry();                          /* ring emptied early */
    ck("  but the tail-dry backstop releases it", track_commit_due());

    /* 6. It does not survive into the next track. A crossfade raises the
     *    flag while the PREVIOUS track's decode loop is still running. */
    g_released = true;
    track_change_begin_model(true);
    ck("a raised release does not survive the next track change",
       !track_commit_due());

    /* 7. Monotonic within one overlap -- no flicker. */
    g_xfade_pos = 0; g_xfade_frames = 4096;
    track_change_begin_model(true);
    bool seen = false, flicker = false;
    while (g_xfade_pos < g_xfade_frames) {
        xfade_advance(256);
        if (track_commit_due()) seen = true;
        else if (seen) flicker = true;
    }
    ck("release never drops once raised inside an overlap", seen && !flicker);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
