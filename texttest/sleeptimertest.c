/*
 * sleeptimertest.c -- main/sleeptimer.h, compiled from the header.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "sleeptimer.h"

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

#define S (1000000LL)

int main(void)
{
    printf("steps are 15 minutes, off at 0, two hours at most\n");
    CHECK(sleeptimer_minutes(0) == 0, "off");
    CHECK(sleeptimer_minutes(-3) == 0, "negative");
    CHECK(sleeptimer_minutes(1) == 15, "one step");
    CHECK(sleeptimer_minutes(3) == 45, "three steps");
    CHECK(sleeptimer_minutes(SLEEPTIMER_STEPS) == 120, "top");
    CHECK(sleeptimer_minutes(99) == 120, "clamped");

    printf("time left rounds up and stops at zero\n");
    CHECK(sleeptimer_seconds_left(0, 0) == 0, "off");
    CHECK(sleeptimer_seconds_left(100 * S, 100 * S) == 0, "at the deadline");
    CHECK(sleeptimer_seconds_left(101 * S, 100 * S) == 0, "past it");
    CHECK(sleeptimer_seconds_left(99 * S + 1, 100 * S) == 1, "a microsecond short is a second");
    CHECK(sleeptimer_seconds_left(0, 45 * 60 * S) == 2700, "45 min");

    printf("the ramp ends at the deadline, and only then is it silent\n");
    const int64_t d = 3600 * S;
    const int64_t f = (int64_t)SLEEPTIMER_FADE_MS * 1000;
    CHECK(sleeptimer_volume(0, 0, 62) == -1, "off is -1");
    CHECK(sleeptimer_volume(0, d, 62) == 62, "long before");
    CHECK(sleeptimer_volume(d - f, d, 62) == 62, "at the start of the ramp");
    CHECK(sleeptimer_volume(d - f / 2, d, 62) == 31, "halfway: %d", sleeptimer_volume(d - f / 2, d, 62));
    CHECK(sleeptimer_volume(d, d, 62) == 0, "at the deadline");
    CHECK(sleeptimer_volume(d + S, d, 62) == 0, "after");
    CHECK(sleeptimer_volume(d - S / 1000, d, 62) == 0, "a millisecond before rounds to 0");
    CHECK(sleeptimer_volume(d - f / 4, d, 100) > 0, "a quarter from the end is not silent yet");
    {
        int prev = 101, rises = 0, early_zero = 0;
        for (int64_t t = d - f - S; t <= d + S; t += S / 50) {
            const int v = sleeptimer_volume(t, d, 100);
            if (v > prev) rises++;
            if (v == 0 && t < d - f / 50) early_zero++;
            prev = v;
        }
        CHECK(rises == 0, "volume rose %d times", rises);
        CHECK(early_zero == 0, "silent %d samples before the last 2%%", early_zero);
    }
    CHECK(sleeptimer_volume(d - f / 2, d, 0) == 0, "from 0 stays 0");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
