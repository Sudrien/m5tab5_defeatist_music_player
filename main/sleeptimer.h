/*
 * sleeptimer.h -- the sleep timer's arithmetic, with nothing to run.
 *
 * Set from the Sleep page in 15-minute steps, up to two hours. When it
 * runs out, the volume ramps to nothing over SLEEPTIMER_FADE_MS, the
 * track pauses, the volume is put back where it was -- paused, so
 * nothing is heard -- and the screen fades off. The next press of play is
 * at the old level.
 *
 * The ramp ENDS at the deadline rather than starting there, so "45
 * minutes" is when the sound has gone, not when it starts to go.
 *
 * Relative only, and on the monotonic clock: the deadline is an
 * esp_timer reading, so an NTP step cannot move it. Not saved: a sleep
 * timer that came back after a reboot would be a surprise, not a
 * setting. "Until 07:00" needs the wall clock and is not this.
 *
 * Header-only and pure, so texttest compiles this file.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLEEPTIMER_STEP_MIN     (15)
#define SLEEPTIMER_STEPS        (8)         /* 15 min .. 2 h; 0 is off */
/* ESTIMATE. Long enough not to be an audible cut, short enough that the
 * last track does not go on quietly for a minute. */
#define SLEEPTIMER_FADE_MS      (20000)

static inline int sleeptimer_minutes(int step)
{
    if (step <= 0) return 0;
    if (step > SLEEPTIMER_STEPS) step = SLEEPTIMER_STEPS;
    return step * SLEEPTIMER_STEP_MIN;
}

/* Whole seconds left before the sound is gone, rounded up; 0 when off or
 * past. `deadline_us` 0 is off. */
static inline int64_t sleeptimer_seconds_left(int64_t now_us, int64_t deadline_us)
{
    if (deadline_us <= 0 || now_us >= deadline_us) return 0;
    return (deadline_us - now_us + 999999) / 1000000;
}

/*
 * The volume to play at now, given the level before the ramp began.
 * `from` until the ramp, then linear down to 0 at the deadline -- linear
 * in the player's volume percent, which is already a perceptual scale.
 * -1 when the timer is off.
 */
static inline int sleeptimer_volume(int64_t now_us, int64_t deadline_us, int from)
{
    if (deadline_us <= 0) return -1;
    const int64_t fade_us = (int64_t)SLEEPTIMER_FADE_MS * 1000;
    const int64_t start = deadline_us - fade_us;
    if (now_us <= start) return from;
    if (now_us >= deadline_us) return 0;
    const int64_t left = deadline_us - now_us;
    return (int)((from * left + fade_us / 2) / fade_us);
}

#ifdef __cplusplus
}
#endif
