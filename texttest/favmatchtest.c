/*
 * favmatchtest.c -- the "same station?" rule in main/favmatch.h,
 * compiled from that header rather than transcribed.
 *
 * What favorites.c does around it -- reading the file, the temp-file
 * rename, the volume precedence -- runs on the board and is not covered
 * here. This is the decision that puts a gold star on a row, and it is
 * pure.
 *
 * THE PROPERTY WORTH ASSERTING is asymmetric: a missed match costs a
 * duplicate entry in favorites.m3u, which is visible and harmless; an
 * extra match puts a star on a station nobody starred, or unstars the
 * wrong one. So the cases below lean hard on things that must NOT be
 * equal, which is the direction the rule is deliberately conservative
 * in.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "favmatch.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

#define SAME(a, b)  CHECK(favmatch_eq((a), (b)), "expected same: %s == %s", (a), (b))
#define DIFF(a, b)  CHECK(!favmatch_eq((a), (b)), "expected different: %s != %s", (a), (b))

int main(void)
{
    /* Identical is identical. */
    SAME("https://stream.zeno.fm/erunhwj5lekvv",
         "https://stream.zeno.fm/erunhwj5lekvv");

    /* Scheme and host fold. This is the case that actually happens:
     * radio-browser and a hand-typed line disagreeing on capitals. */
    SAME("HTTPS://Stream.Zeno.FM/erunhwj5lekvv",
         "https://stream.zeno.fm/erunhwj5lekvv");
    SAME("http://EXAMPLE.COM/x", "http://example.com/x");

    /* The path does not fold -- two streams on one host. */
    DIFF("https://example.com/Jazz", "https://example.com/jazz");

    /* Nor does the query, which is where stream tokens live. */
    DIFF("https://example.com/s?id=A", "https://example.com/s?id=a");

    /* http and https are different stations. A station that moved is a
     * new line in the file; guessing they are the same would unstar the
     * wrong one. */
    DIFF("http://example.com/x", "https://example.com/x");

    /* Trailing slashes are not stripped. Servers disagree about whether
     * these are the same resource, so this does not decide for them. */
    DIFF("https://example.com/x", "https://example.com/x/");
    DIFF("https://example.com", "https://example.com/");

    /* Default ports are not dropped, for the same reason. */
    DIFF("https://example.com:443/x", "https://example.com/x");
    SAME("https://example.com:8000/x", "https://example.com:8000/x");

    /* The port IS part of the authority, so it folds with the host --
     * which costs nothing, digits having no case. What matters is that
     * the authority ends at the port and not before it. */
    SAME("http://EXAMPLE.com:8000/x", "http://example.com:8000/x");

    /* Userinfo is inside the authority too and folds with it. Rare and
     * not worth a special case; asserted so a future change to
     * authority_end() cannot move it silently. */
    SAME("http://User@Example.com/x", "http://user@example.com/x");

    /* Different hosts, same path. */
    DIFF("https://a.example.com/x", "https://b.example.com/x");

    /* A prefix is not a match: the authority lengths differ, which is
     * the early-out, and this pins it. */
    DIFF("https://example.com/x", "https://example.community/x");

    /* Fragments end the authority as surely as a slash does. */
    SAME("https://EXAMPLE.com#a", "https://example.com#a");
    DIFF("https://example.com#a", "https://example.com#A");

    /* No scheme: byte-exact, and NOT folded. station_url_ok() refuses
     * these upstream, so this is the branch that should never run --
     * asserted so that if it ever does, it does not quietly start
     * treating case-different strings as one station. */
    DIFF("example.com/x", "EXAMPLE.com/x");
    SAME("example.com/x", "example.com/x");

    /* NULLs are not equal to anything, including each other. A NULL URL
     * is a station that failed to load, and two of them are not the
     * same station. */
    CHECK(!favmatch_eq(NULL, "https://example.com/"), "NULL matched");
    CHECK(!favmatch_eq("https://example.com/", NULL), "NULL matched");
    CHECK(!favmatch_eq(NULL, NULL), "NULL matched NULL");

    /* Empty strings have no authority, so they take the byte compare. */
    SAME("", "");
    DIFF("", "https://example.com/");

    /* A scheme with nothing after it. station_url_ok() refuses this
     * too; the point is that authority_end() does not run off the end
     * looking for a terminator that is not there. */
    SAME("https://", "https://");
    DIFF("https://", "https://a");

    printf("%s: %d checks, %d failures\n",
           failures ? "FAILURES" : "all passed", checks, failures);
    return failures ? 1 : 0;
}
