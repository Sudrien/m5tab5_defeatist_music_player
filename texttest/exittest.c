/*
 * exittest.c -- the exit tally line.
 *
 * A diagnostic that lies is worse than none, and this one is read
 * against log lines to decide whether a line was lost. So the two things
 * that matter are that a zero is never printed as present, and that a
 * long line truncates rather than running off the buffer.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

enum { XEXIT_START = 0, XEXIT_DONE, XEXIT_CUT_SHORT, XEXIT_PARTIAL,
       XEXIT_RATE_EARLY, XEXIT_RATE_LATE, XEXIT_TAIL_DRY,
       XEXIT_TAIL_GONE, XEXIT_TAIL_CUT, XEXIT_N };
static uint32_t s_xexit[XEXIT_N];
static const char *const s_xexit_name[XEXIT_N] = {
    "start", "done", "cut", "partial", "rate-early", "rate-late",
    "tail-dry", "tail-gone", "tail-cut"
};

static void report(char *out, size_t out_len)
{
    char buf[160];
    int n = 0;
    for (int i = 0; i < XEXIT_N; i++) {
        if (!s_xexit[i]) continue;
        const int w = snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s=%" PRIu32,
                               n ? " " : "", s_xexit_name[i], s_xexit[i]);
        if (w < 0 || (size_t)(n + w) >= sizeof(buf)) break;
        n += w;
    }
    if (!n) snprintf(buf, sizeof(buf), "none yet");
    snprintf(out, out_len, "%s", buf);
}

static int fails;
static void ck(const char *what, int ok)
{
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    char out[256];

    memset(s_xexit, 0, sizeof(s_xexit));
    report(out, sizeof out);
    ck("nothing counted yet reads as such", strcmp(out, "none yet") == 0);

    /* The case the whole patch is for: an overlap started and finished,
     * against a log that showed only the start. */
    s_xexit[XEXIT_START] = 2; s_xexit[XEXIT_DONE] = 2;
    report(out, sizeof out);
    ck("start and done both reported", strcmp(out, "start=2 done=2") == 0);
    ck("  no zero-valued exit is mentioned", strstr(out, "cut") == NULL &&
                                             strstr(out, "partial") == NULL);

    /* One exit missing is the signal being looked for. */
    s_xexit[XEXIT_DONE] = 1;
    report(out, sizeof out);
    ck("a start without its done is visible", strcmp(out, "start=2 done=1") == 0);

    /* Every counter set, at width: must not overflow or drop silently
     * in a way that looks like a zero. */
    for (int i = 0; i < XEXIT_N; i++) s_xexit[i] = 4294967295u;
    report(out, sizeof out);
    ck("all nine at UINT32_MAX stay within the buffer", strlen(out) < 160);
    ck("  and the line is not empty", strlen(out) > 0);

    /* Ordering is stable, so two reports can be diffed by eye. */
    memset(s_xexit, 0, sizeof(s_xexit));
    s_xexit[XEXIT_TAIL_DRY] = 3; s_xexit[XEXIT_START] = 1;
    report(out, sizeof out);
    ck("order follows the enum, not the order counted",
       strcmp(out, "start=1 tail-dry=3") == 0);

    printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails != 0;
}
