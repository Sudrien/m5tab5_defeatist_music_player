/*
 * cardtimetest -- the filter that stands between a card's timestamps and
 * a permanent, one-way clock floor.
 *
 * cardtime_filter() is four comparisons, and getting any of them wrong
 * is either useless or unrecoverable:
 *
 *   - too permissive at the top, and one file dated 2107 -- which FAT
 *     can represent and bad tools do produce -- latches the floor eighty
 *     years into the future, refusing every real NTP reply for the life
 *     of the device;
 *   - wrong sign on the timezone margin, and a card written east of UTC
 *     pushes the floor hours ahead of the truth, refusing correct NTP
 *     until wall-clock catches up;
 *   - too strict, and the whole thing never raises the floor at all and
 *     is dead code that looks like it works.
 *
 * None of those are visible on the board without waiting out the
 * failure, and the last one is invisible entirely. They are all pure
 * integer arithmetic, so they are checked here.
 *
 * Header-only, like tailplantest and favmatchtest: this compiles
 * cardtime.h itself, so there is nothing to drift from.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>

#include "cardtime.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                            \
    checks++;                                                            \
    if (!(cond)) {                                                       \
        failures++;                                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                    \
        printf(__VA_ARGS__);                                             \
        printf("\n");                                                    \
    }                                                                    \
} while (0)

#define DAY     (86400)
#define YEAR    (365 * DAY)

/* A plausible build stamp: 2026-09-22T00:00:00Z. */
#define BUILD   ((int64_t)1789084800)

int main(void)
{
    printf("cardtimetest\n");

    /* --- the ceiling, which is the one that cannot be got wrong ----- */
    printf("  the ceiling\n");
    {
        /* FAT's maximum representable date is 2107-12-31. A file
         * carrying it must not reach the floor. */
        const int64_t fat_max = (int64_t)4354560000;   /* ~2107-12-31Z */
        CHECK(cardtime_filter(fat_max, BUILD) == 0,
              "a 2107 timestamp was accepted: %lld",
              (long long)cardtime_filter(fat_max, BUILD));

        /* Just inside and just outside, so the bound is the bound and
         * not an approximation of one. */
        CHECK(cardtime_filter(BUILD + CARDTIME_MAX_AHEAD_S, BUILD) != 0,
              "exactly at the ceiling should be allowed");
        CHECK(cardtime_filter(BUILD + CARDTIME_MAX_AHEAD_S + 1, BUILD) == 0,
              "one second past the ceiling should be refused");

        /*
         * The margin must not be able to duck a poisoned value under the
         * bar. A value a few hours past the ceiling is still past it,
         * even though subtracting a day would bring it back inside --
         * which is why the ceiling is tested against the raw mtime.
         */
        const int64_t sneaky = BUILD + CARDTIME_MAX_AHEAD_S + (DAY / 2);
        CHECK(cardtime_filter(sneaky, BUILD) == 0,
              "a value within a day of the ceiling ducked under it");
    }

    /* --- the timezone margin, and its direction -------------------- */
    printf("  the margin runs the safe way\n");
    {
        const int64_t m = BUILD + 30 * DAY;         /* a month fresher */
        const int64_t got = cardtime_filter(m, BUILD);

        CHECK(got == m - CARDTIME_TZ_MARGIN_S,
              "expected %lld, got %lld", (long long)(m - CARDTIME_TZ_MARGIN_S),
              (long long)got);

        /* Subtracted, never added: the result is always BELOW the raw
         * reading. A floor that is low is harmless; one that is high
         * refuses real time. */
        CHECK(got < m, "the margin was not subtracted");

        /* Wider than any offset on Earth, UTC-12 to UTC+14. */
        CHECK(CARDTIME_TZ_MARGIN_S >= 14 * 3600,
              "the margin is narrower than UTC+14");

        /*
         * The case this exists for: a card written at UTC+14 reads 14
         * hours ahead. After the margin it must still be below the true
         * time it was written at, not above it.
         */
        const int64_t truth = BUILD + 30 * DAY;
        const int64_t as_read = truth + 14 * 3600;   /* VFS read it as UTC */
        CHECK(cardtime_filter(as_read, BUILD) < truth,
              "a UTC+14 card still landed ahead of the truth");
    }

    /* --- things that cannot raise the floor ------------------------ */
    printf("  refusals\n");
    {
        CHECK(cardtime_filter(BUILD - YEAR, BUILD) == 0,
              "a card older than the firmware was accepted");
        CHECK(cardtime_filter(BUILD, BUILD) == 0,
              "a card exactly at the build stamp was accepted");

        /* Inside the margin of the build: after subtracting a day it is
         * at or below the build, so it has nothing to add. */
        CHECK(cardtime_filter(BUILD + DAY, BUILD) == 0,
              "a value within the margin of the build was accepted");
        CHECK(cardtime_filter(BUILD + DAY + 1, BUILD) == 1 + BUILD - 0,
              "one second past the margin should yield build+1, got %lld",
              (long long)cardtime_filter(BUILD + DAY + 1, BUILD));

        /* Degenerate inputs. A build stamp that failed to parse means no
         * ceiling, and no ceiling means no guard -- so nothing is
         * accepted at all. */
        CHECK(cardtime_filter(BUILD + YEAR, 0) == 0,
              "accepted a candidate with no build reference");
        CHECK(cardtime_filter(0, BUILD) == 0, "accepted a zero mtime");
        CHECK(cardtime_filter(-1, BUILD) == 0, "accepted a negative mtime");
        CHECK(cardtime_filter(BUILD + YEAR, -1) == 0,
              "accepted a negative build reference");
    }

    /* --- the useful case actually works ---------------------------- */
    printf("  it does its job\n");
    {
        /*
         * The whole point: a firmware six months old, a card with an
         * album copied on last week. The floor should move most of the
         * way there rather than sitting at the build stamp.
         */
        const int64_t build = BUILD;
        const int64_t copied = BUILD + 180 * DAY;
        const int64_t got = cardtime_filter(copied, build);

        CHECK(got > build, "a fresh card did not raise the floor at all");
        CHECK(copied - got <= CARDTIME_TZ_MARGIN_S,
              "lost more than the margin: %lld seconds",
              (long long)(copied - got));
        CHECK(got - build >= 179 * DAY,
              "recovered only %lld days of the 180", (long long)((got - build) / DAY));
    }

    /* --- monotonicity: a later file never yields an earlier floor --- */
    printf("  monotone in mtime\n");
    {
        int64_t prev = 0;
        int bad = 0;
        for (int64_t t = BUILD; t < BUILD + CARDTIME_MAX_AHEAD_S; t += 7 * DAY) {
            const int64_t got = cardtime_filter(t, BUILD);
            if (got != 0) {
                if (got < prev) bad++;
                prev = got;
            }
        }
        CHECK(bad == 0, "%d non-monotone steps", bad);
    }

    /*
     * 5112: corroboration. One entry dated 2028-12-02 put the floor there
     * on every boot without NTP, and after 5110 two recordings were named
     * for it. A candidate now needs another at or below it within two
     * days; the latest such one wins.
     */
    {
        const int64_t now = BUILD + 30 * DAY;
        const int64_t bad = BUILD + 800 * DAY;          /* ~2028-12 */
        int at;

        /* An album copied on: many entries minutes apart, and one stray. */
        int64_t c1[] = { now, now - 600, now - 1200, bad, 0, now - 5 * DAY };
        CHECK(cardtime_pick(c1, 6, &at) == now && at == 0,
              "a lone future entry beat a corroborated one: got %lld", (long long)cardtime_pick(c1, 6, &at));

        /* The stray alone, with nothing near it: nothing is taken. */
        int64_t c2[] = { bad, now - 90 * DAY };
        CHECK(cardtime_pick(c2, 2, &at) == 0 && at == -1, "a lone entry was taken");

        /* Two strays together do corroborate each other -- the rule is
         * about agreement, not about what date is plausible. The ceiling
         * is what guards the far future. */
        int64_t c3[] = { bad, bad - DAY, now };
        CHECK(cardtime_pick(c3, 3, &at) == bad, "two agreeing entries were not taken");

        /* Exactly the window, and one second past it. */
        int64_t c4[] = { now, now - CARDTIME_CORROB_S };
        CHECK(cardtime_pick(c4, 2, &at) == now, "an entry exactly two days below did not corroborate");
        int64_t c5[] = { now, now - CARDTIME_CORROB_S - 1 };
        CHECK(cardtime_pick(c5, 2, &at) == 0, "an entry two days and a second below corroborated");

        /* A later entry does not corroborate an earlier one's claim to
         * be the latest, but the earlier one is corroborated by it only
         * if at or below: the later is the one taken. */
        int64_t c6[] = { now - DAY, now };
        CHECK(cardtime_pick(c6, 2, &at) == now && at == 1, "the pair did not give its later member");

        /* Discards (0) never corroborate, and an empty scan gives 0. */
        int64_t c7[] = { now, 0, 0 };
        CHECK(cardtime_pick(c7, 3, &at) == 0, "a discard corroborated");
        CHECK(cardtime_pick(c7, 0, &at) == 0 && at == -1, "an empty scan gave something");

        /* The player's own root files are not evidence since 5110. */
        CHECK(cardtime_own("stations.m3u") && cardtime_own("Recordings") &&
              cardtime_own("STARRED.M3U") && cardtime_own("favorites.m3u"),
              "the player's own files were not recognised");
        CHECK(!cardtime_own("Boa") && !cardtime_own("stations.m3u.bak") &&
              !cardtime_own("Recordings2"), "a foreign entry was taken for the player's own");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
