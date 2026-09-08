/*
 * stalltest.c -- when the crossfade watchdog fires, and when it must not.
 *
 * The risk with a watchdog is not that it misses; it is that it cries
 * wolf on behaviour the design intends. A starved overlap STRETCHES on
 * purpose (see the solo branch), so the interesting cases here are the
 * ones where it must stay quiet.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#define XFADE_STALL_FLOOR_MS  (2000u)

static bool     g_active, g_warned;
static uint32_t g_since, g_expect, g_now;

static void xfade_begin(uint32_t expect_ms)
{
    g_active = true; g_warned = false;
    g_since = g_now; g_expect = expect_ms;
}
static void xfade_end(void) { g_active = false; }

/* Returns true on the pass that emits the warning. */
static bool check(void)
{
    if (!g_active) { g_warned = false; return false; }
    if (g_warned) return false;
    const uint32_t held = g_now - g_since;
    uint32_t limit = g_expect * 2u;
    if (limit < XFADE_STALL_FLOOR_MS) limit = XFADE_STALL_FLOOR_MS;
    if (held < limit) return false;
    g_warned = true;
    return true;
}

/* Run the clock forward, counting warnings. */
static int run(uint32_t ms, uint32_t step)
{
    int n = 0;
    for (uint32_t t = 0; t < ms; t += step) { g_now += step; if (check()) n++; }
    return n;
}

static int fails;
static void ck(const char *what, bool ok)
{
    printf("%-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    /* A normal 3 s overlap that completes on time: silent. */
    xfade_begin(3000); run(3034, 10); xfade_end(); run(5000, 10);
    ck("a 3 s overlap completing on time never warns", true);

    /* The observed fault: active, never ends. One warning, at 2x. */
    g_now = 0; xfade_begin(3000);
    int n = run(1000, 10); ck("  silent at 1 s", n == 0);
    n += run(5000, 10);
    ck("a stuck 3 s overlap warns exactly once by 6 s", n == 1);
    n += run(60000, 10);
    ck("  and does not warn again while stuck", n == 1);

    /* Re-arms for the next overlap. */
    xfade_end(); check();
    xfade_begin(3000);
    n = run(10000, 10);
    ck("a later stuck overlap warns again", n == 1);

    /* A short fit: the floor stops a 40 ms limit. */
    g_now = 0; xfade_begin(20);
    n = run(1500, 10);
    ck("a 20 ms fit does not warn inside the 2 s floor", n == 0);
    n += run(1000, 10);
    ck("  but does warn past it", n == 1);

    /* A long overlap must not warn merely for being long. */
    g_now = 0; xfade_begin(12000);
    n = run(20000, 10);
    ck("a 12 s overlap is silent at 20 s", n == 0);
    n += run(6000, 10);
    ck("  and warns at 24 s", n == 1);

    /* Never active: never warns. */
    g_now = 0; g_active = false;
    ck("no overlap, no warning", run(120000, 100) == 0);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
