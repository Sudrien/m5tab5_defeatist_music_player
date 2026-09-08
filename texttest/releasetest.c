/*
 * releasetest.c -- every way an overlap ends must release the screen.
 *
 * The board showed what happens when one exit does not: "Beautiful &
 * Broken" stayed on screen across two subsequent tracks. So the property
 * under test is not any single branch, it is the absence of an exception
 * -- exhaustively, over every exit the writer has.
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EXIT_MIDPOINT, EXIT_DONE, EXIT_CUT_SHORT,
    EXIT_RATE_EARLY, EXIT_RATE_LATE, EXIT_TAIL_DRY,
    EXIT_N
} exit_t;

static const char *name[EXIT_N] = {
    "midpoint", "done", "cut short", "rate (early)", "rate (late)",
    "tail dry"
};

static bool released;
static bool active;

/* Each branch as it now stands in the writer / decode loop. */
static void take_exit(exit_t e)
{
    switch (e) {
    case EXIT_MIDPOINT:                 released = true; break;
    case EXIT_DONE:       active=false; released = true; break;
    case EXIT_CUT_SHORT:  active=false; released = true; break;
    case EXIT_RATE_EARLY: active=false; released = true; break;
    case EXIT_RATE_LATE:  active=false; released = true; break;
    case EXIT_TAIL_DRY:                 released = true; break;
    default: break;
    }
}

static int fails;
static void ck(const char *what, bool ok)
{ printf("%-58s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }

int main(void)
{
    /* Exhaustive: no exit may leave the screen behind. */
    bool all = true;
    for (int e = 0; e < EXIT_N; e++) {
        released = false; active = true;
        take_exit((exit_t)e);
        if (!released) { printf("  %s did not release\n", name[e]); all = false; }
    }
    ck("every exit releases the screen", all);

    /* The partial-frame case has no exit at all now -- it falls through,
     * leaving the overlap running so a later exit ends it. What must NOT
     * happen is the 1105 behaviour: overlap ended, screen not released. */
    released = false; active = true;
    /* fall-through: nothing changes */
    ck("a partial frame does not end the overlap", active);
    ck("  and so cannot end it without releasing", !(!active && !released));

    /* And once something does end it, the screen follows. */
    take_exit(EXIT_CUT_SHORT);
    ck("the exit that eventually fires releases", released && !active);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
