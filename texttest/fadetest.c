/*
 * fadetest.c -- loudness_fade(), against the real main/loudness.c.
 *
 * Not a transcription: this links the file the firmware builds, so a
 * threshold changed there is a threshold tested here. The signals are
 * noise shaped by a gain curve in dB, which is what a fader does, fed
 * through loudness_process() in decoder-sized blocks exactly as the
 * decode loop feeds it.
 *
 * What it cannot say is whether the thresholds suit real music. Noise
 * has no bars, no bridges and no reverb tails; the cases with a pulse
 * and a quiet bridge are the nearest this gets, and the board log's
 * "fade:" line on a real library is the check that matters.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "loudness.h"

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

/* A gain curve: dB at time t seconds, or -inf for digital silence. */
typedef float (*shape_fn)(double t);

static uint32_t rng = 0x12345678u;
static float noise(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)((int32_t)rng) / 2147483648.0f;
}

static loudness_t L;   /* static for the same reason player.c's is */

/* Feed `sec` seconds of noise under `shape`; report the fade, or false. */
static bool run(shape_fn shape, double sec, uint32_t rate, int channels,
                float offset_db, bool seek_midway, loudness_fade_t *out)
{
    loudness_reset(&L);
    rng = 0x12345678u;

    const int chunk = 1024;
    int16_t pcm[1024 * 2];
    const uint64_t frames_total = (uint64_t)(sec * rate);

    for (uint64_t f = 0; f < frames_total; f += (uint64_t)chunk) {
        const int nf = (frames_total - f) < (uint64_t)chunk
                     ? (int)(frames_total - f) : chunk;
        for (int i = 0; i < nf; i++) {
            const double t = (double)(f + (uint64_t)i) / rate;
            const float db = shape(t) + offset_db;
            const float g = isinf(db) ? 0.0f : 0.25f * powf(10.0f, db / 20.0f);
            for (int c = 0; c < channels; c++) {
                const long v = lrintf(noise() * g * 32767.0f);
                pcm[i * channels + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        loudness_process(&L, pcm, nf * channels, channels, rate);
        if (seek_midway && f < frames_total / 2 && f + (uint64_t)chunk >= frames_total / 2) {
            loudness_invalidate(&L);
        }
    }

    float lufs = 0.0f, peak = 0.0f;
    uint32_t gated = 0;
    if (!loudness_finish(&L, &lufs, &peak, &gated)) return false;
    return loudness_fade(&L, lufs, out);
}

static bool near_ms(uint32_t got, double want_sec, double tol_sec)
{
    return fabs((double)got / 1000.0 - want_sec) <= tol_sec;
}

/* ---- shapes ---- */

static float ramp(double t, double t0, double t1, float db0, float db1)
{
    if (t <= t0) return db0;
    if (t >= t1) return db1;
    return db0 + (db1 - db0) * (float)((t - t0) / (t1 - t0));
}

/* 60 s of song, 8 s straight-in-dB fade of 40 dB, 2 s digital silence. */
static float s_classic(double t)
{
    if (t >= 68.0) return -INFINITY;
    return ramp(t, 60.0, 68.0, 0.0f, -40.0f);
}

/* 60 s of song, then digital silence. A hard ending. */
static float s_hard(double t) { return t >= 60.0 ? -INFINITY : 0.0f; }

/* A soft outro held level for 20 s, then stops dead. */
static float s_soft_outro(double t)
{
    if (t >= 80.0) return -INFINITY;
    return t >= 60.0 ? -8.0f : 0.0f;
}

/* 60 s song, 15 s outro 6 dB down, then a 6 s fade from there. */
static float s_outro_then_fade(double t)
{
    if (t >= 81.0) return -INFINITY;
    if (t < 60.0) return 0.0f;
    return ramp(t, 75.0, 81.0, -6.0f, -46.0f);
}

/* A fade to -30, and then the song comes back and ends hard. */
static float s_false_fade(double t)
{
    if (t >= 75.0) return -INFINITY;
    if (t >= 65.0) return 0.0f;
    return ramp(t, 60.0, 65.0, 0.0f, -30.0f);
}

/* A 20 s fade of 40 dB, cut off by the edit halfway down. */
static float s_cut_fade(double t)
{
    if (t >= 70.0) return -INFINITY;
    return ramp(t, 60.0, 80.0, 0.0f, -40.0f);
}

/* A 30 s fade, the long DAW kind. */
static float s_long(double t)
{
    if (t >= 90.0) return -INFINITY;
    return ramp(t, 60.0, 90.0, 0.0f, -45.0f);
}

/* The classic fade under a 2 Hz beat that swings 12 dB. */
static float s_beat(double t)
{
    const float beat = fmod(t * 2.0, 1.0) < 0.25 ? 0.0f : -12.0f;
    if (t >= 68.0) return -INFINITY;
    return beat + ramp(t, 60.0, 68.0, 0.0f, -40.0f);
}

/* A fade that starts two minutes before the end: out of reach. */
static float s_too_long(double t)
{
    if (t >= 180.0) return -INFINITY;
    return ramp(t, 60.0, 180.0, 0.0f, -45.0f);
}

/* A 6 dB decrescendo into a hard ending: quieter, not a fade. */
static float s_shallow(double t)
{
    if (t >= 65.0) return -INFINITY;
    return ramp(t, 60.0, 65.0, 0.0f, -6.0f);
}

/* The classic fade with a hit 6 LU above it two thirds of the way down. */
static float s_hit(double t)
{
    if (t >= 68.0) return -INFINITY;
    const float f = ramp(t, 60.0, 68.0, 0.0f, -40.0f);
    return (t >= 65.0 && t < 65.6) ? f + 6.0f : f;
}

/* A song, then 5 s of room tone 45 dB under it. */
static float s_room_tone(double t) { return t >= 60.0 ? -45.0f : 0.0f; }

/* A song, then 75 s of digital silence before the file ends. */
static float s_long_silence(double t) { return t >= 60.0 ? -INFINITY : 0.0f; }

/* Five seconds of anything. */
static float s_short(double t) { return ramp(t, 2.0, 5.0, 0.0f, -40.0f); }

/* A quiet song fading into dither-level noise rather than silence. */
static float s_noise_floor(double t)
{
    return ramp(t, 60.0, 68.0, 0.0f, -60.0f);
}

int main(void)
{
    loudness_fade_t fd;

    printf("a mastered fade is found where it starts and ends\n");
    CHECK(run(s_classic, 70.0, 44100, 2, 0.0f, false, &fd), "no answer");
    CHECK(fd.has_fade, "missed");
    CHECK(near_ms(fd.start_ms, 60.0, 1.0), "start %u ms", fd.start_ms);
    CHECK(near_ms(fd.end_ms, 68.0, 0.5), "end %u ms", fd.end_ms);
    CHECK(near_ms(fd.total_ms, 70.0, 0.05), "total %u ms", fd.total_ms);
    CHECK(fd.depth_lu >= 20.0f && fd.depth_lu <= 45.0f, "depth %.1f", (double)fd.depth_lu);
    const loudness_fade_t ref = fd;

    printf("the answer does not move with the level it was mastered at\n");
    CHECK(run(s_classic, 70.0, 44100, 2, -15.0f, false, &fd) && fd.has_fade, "missed at -15 dB");
    CHECK(fabs((double)fd.start_ms - ref.start_ms) <= 300.0, "start moved %u -> %u", ref.start_ms, fd.start_ms);
    CHECK(fabs((double)fd.end_ms - ref.end_ms) <= 300.0, "end moved %u -> %u", ref.end_ms, fd.end_ms);

    printf("the times are seconds at any rate, and mono is the same song\n");
    CHECK(run(s_classic, 70.0, 48000, 2, 0.0f, false, &fd) && fd.has_fade, "missed at 48 kHz");
    CHECK(near_ms(fd.start_ms, 60.0, 1.0) && near_ms(fd.end_ms, 68.0, 0.5),
          "48 kHz %u..%u", fd.start_ms, fd.end_ms);
    CHECK(run(s_classic, 70.0, 22050, 1, 0.0f, false, &fd) && fd.has_fade, "missed in mono");
    CHECK(near_ms(fd.start_ms, 60.0, 1.0) && near_ms(fd.end_ms, 68.0, 0.5),
          "mono %u..%u", fd.start_ms, fd.end_ms);

    printf("a hard ending is not a fade\n");
    CHECK(run(s_hard, 62.0, 44100, 2, 0.0f, false, &fd), "no answer");
    CHECK(!fd.has_fade, "found %u..%u", fd.start_ms, fd.end_ms);

    printf("a quiet outro that stops dead is not a fade\n");
    CHECK(run(s_soft_outro, 82.0, 44100, 2, 0.0f, false, &fd), "no answer");
    CHECK(!fd.has_fade, "found %u..%u depth %.1f", fd.start_ms, fd.end_ms, (double)fd.depth_lu);

    printf("an outro that then fades starts where the level falls\n");
    CHECK(run(s_outro_then_fade, 83.0, 44100, 2, 0.0f, false, &fd) && fd.has_fade, "missed");
    CHECK(near_ms(fd.start_ms, 75.0, 1.5), "start %u ms, want ~75000", fd.start_ms);

    printf("a dip the song comes back from is not a fade\n");
    CHECK(run(s_false_fade, 77.0, 44100, 2, 0.0f, false, &fd), "no answer");
    CHECK(!fd.has_fade, "found %u..%u", fd.start_ms, fd.end_ms);

    printf("a decrescendo into a hard ending is not deep enough to be a fade\n");
    CHECK(run(s_shallow, 67.0, 22050, 2, 0.0f, false, &fd), "no answer");
    CHECK(!fd.has_fade, "found %u..%u depth %.1f", fd.start_ms, fd.end_ms, (double)fd.depth_lu);

    printf("a hit inside a fade does not stop it being one\n");
    CHECK(run(s_hit, 70.0, 22050, 2, 0.0f, false, &fd) && fd.has_fade, "missed");
    CHECK(near_ms(fd.start_ms, 60.0, 1.0) && near_ms(fd.end_ms, 68.0, 0.5),
          "%u..%u", fd.start_ms, fd.end_ms);

    printf("a fade cut short by the edit is a fade that ends at the cut\n");
    CHECK(run(s_cut_fade, 72.0, 44100, 2, 0.0f, false, &fd) && fd.has_fade, "missed");
    CHECK(near_ms(fd.start_ms, 60.0, 1.0) && near_ms(fd.end_ms, 70.0, 0.5),
          "%u..%u", fd.start_ms, fd.end_ms);
    CHECK(fd.depth_lu >= 10.0f && fd.depth_lu <= 22.0f, "depth %.1f", (double)fd.depth_lu);

    /* It ends where it passes 40 LU under the song, 86.7 s, not where
     * the fader reached the bottom: the last 5 dB are not audible as
     * anything and are what "end" deliberately leaves out. */
    printf("a long fade is found from its start\n");
    CHECK(run(s_long, 92.0, 22050, 2, 0.0f, false, &fd) && fd.has_fade, "missed");
    CHECK(near_ms(fd.start_ms, 60.0, 1.0) && near_ms(fd.end_ms, 86.7, 0.7),
          "%u..%u", fd.start_ms, fd.end_ms);

    printf("a beat under the fade does not break it up\n");
    CHECK(run(s_beat, 70.0, 22050, 2, 0.0f, false, &fd) && fd.has_fade, "missed");
    CHECK(near_ms(fd.start_ms, 60.0, 1.5) && near_ms(fd.end_ms, 68.0, 0.7),
          "%u..%u", fd.start_ms, fd.end_ms);

    printf("a fade that fades into noise rather than silence\n");
    CHECK(run(s_noise_floor, 75.0, 22050, 2, 0.0f, false, &fd) && fd.has_fade, "missed");
    CHECK(near_ms(fd.start_ms, 60.0, 1.0), "start %u ms", fd.start_ms);
    CHECK(fd.end_ms < 69000u, "end %u ms, past where it went inaudible", fd.end_ms);

    printf("a fade longer than the tail is not reported with a wrong start\n");
    CHECK(run(s_too_long, 182.0, 22050, 2, 0.0f, false, &fd), "no answer");
    CHECK(!fd.has_fade, "found %u..%u", fd.start_ms, fd.end_ms);

    printf("the end of the audio is kept whether or not there is a fade\n");
    CHECK(run(s_hard, 64.0, 22050, 2, 0.0f, false, &fd) && !fd.has_fade, "hard end");
    CHECK(near_ms(fd.audio_end_ms, 60.0, 0.5), "hard end: audio ends %u ms", fd.audio_end_ms);
    CHECK(near_ms(fd.total_ms, 64.0, 0.05), "hard end: total %u ms", fd.total_ms);
    CHECK(run(s_classic, 70.0, 22050, 2, 0.0f, false, &fd) && fd.has_fade, "fade");
    CHECK(fd.audio_end_ms == fd.end_ms, "fade: audio_end %u != end %u", fd.audio_end_ms, fd.end_ms);

    printf("a quiet room tone after a loud master is silence, relative to the song\n");
    CHECK(run(s_room_tone, 65.0, 22050, 2, 0.0f, false, &fd), "no answer");
    CHECK(near_ms(fd.audio_end_ms, 60.0, 0.5), "audio ends %u ms", fd.audio_end_ms);

    printf("silence longer than the kept tail is unknown, not a minute of silence\n");
    CHECK(run(s_long_silence, 135.0, 22050, 2, 0.0f, false, &fd), "no answer");
    CHECK(fd.audio_end_ms == fd.total_ms, "audio_end %u of %u", fd.audio_end_ms, fd.total_ms);

    printf("a short track is an answer, and the answer is no\n");
    CHECK(run(s_short, 5.0, 44100, 2, 0.0f, false, &fd), "no answer for a short track");
    CHECK(!fd.has_fade && near_ms(fd.total_ms, 5.0, 0.05), "has %d total %u", fd.has_fade, fd.total_ms);

    printf("a seek leaves nothing to answer with\n");
    CHECK(!run(s_classic, 70.0, 22050, 2, 0.0f, true, &fd), "answered after a seek");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
