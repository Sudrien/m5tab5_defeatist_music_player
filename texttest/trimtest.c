/*
 * trimtest.c -- the two crossfade length refusals and the trim verdict.
 *
 * The length rules are symmetric and it would be easy to write one of
 * them backwards, so both directions are checked against the boundary
 * value rather than a comfortable example.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum { TRIM_UNKNOWN = 0, TRIM_EXACT, TRIM_NONE } trim_t;

static bool join_exact(trim_t out, trim_t in)
{
    return out == TRIM_EXACT && in == TRIM_EXACT;
}

/* The arming decision's length half. */
static bool arm_ok(uint32_t sec, uint32_t in_len, uint32_t out_len)
{
    if (!sec) return false;
    if (in_len && in_len <= sec * 2) return false;
    if (out_len && out_len <= sec * 2) return false;
    return true;
}

static int fails;
static void ck(const char *what, bool ok)
{
    printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    /* The case as specified: 24 s track, 12 s crossfade. */
    ck("24 s outgoing under a 12 s fade is refused", !arm_ok(12, 200, 24));
    ck("25 s outgoing under a 12 s fade is allowed",  arm_ok(12, 200, 25));
    ck("  exactly 2x is refused, not the first acceptable case", !arm_ok(12, 200, 24));
    ck("48 s outgoing under a 12 s fade is allowed",  arm_ok(12, 200, 48));

    /* The incoming rule still works and is independent. */
    ck("short incoming alone refuses",  !arm_ok(12, 20, 200));
    ck("short outgoing alone refuses",  !arm_ok(12, 200, 20));
    ck("both short refuses",            !arm_ok(12, 20, 20));

    /* Unknown is not short -- a length of 0 must never refuse. */
    ck("unknown incoming length does not refuse", arm_ok(12, 0, 200));
    ck("unknown outgoing length does not refuse", arm_ok(12, 200, 0));
    ck("both unknown does not refuse",            arm_ok(12, 0, 0));

    /* Crossfade off is off regardless. */
    ck("a zero-second setting is always off", !arm_ok(0, 200, 200));

    /* Short fades stay usable on short tracks. */
    ck("a 29 s track takes a 2 s fade",  arm_ok(2, 200, 29));
    ck("a 4 s track does not take a 2 s fade", !arm_ok(2, 200, 4));
    ck("a 3 s track does not take a 2 s fade", !arm_ok(2, 200, 3));

    /* The trim verdict: only exact/exact joins. */
    ck("exact + exact joins",        join_exact(TRIM_EXACT, TRIM_EXACT));
    ck("exact + none does not",     !join_exact(TRIM_EXACT, TRIM_NONE));
    ck("none + exact does not",     !join_exact(TRIM_NONE,  TRIM_EXACT));
    ck("unknown never joins",       !join_exact(TRIM_UNKNOWN, TRIM_EXACT) &&
                                    !join_exact(TRIM_EXACT, TRIM_UNKNOWN) &&
                                    !join_exact(TRIM_UNKNOWN, TRIM_UNKNOWN));
    ck("none + none does not",      !join_exact(TRIM_NONE,  TRIM_NONE));

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
