/*
 * streamgaintest.c -- the levelling carrier, its gate, and its slew.
 *
 * Links the real main/streamgain.c against the real main/streamgain.h.
 * No FreeRTOS, no decoder, no ring: the whole file is arithmetic over
 * records, which is the split streamgain.h was written in and the
 * reason it could be tested before the decode loop was touched.
 *
 * WHAT IS WORTH TESTING HERE, AND IT IS NOT THE GAIN
 *
 * The number the gate produces is checked against a hand-computed case
 * and otherwise left alone -- it is BS.1770's arithmetic and
 * loudness.c's tests cover the same ground more thoroughly.
 *
 * What this is really for is the BYTE ACCOUNTING, which streamgain.h
 * names as the part most likely to be got wrong: a chunk can straddle
 * two blocks, a block can span several chunks, records are dropped when
 * the carrier fills and their bytes must still be counted, and the two
 * indices are written from different tasks. Every one of those is a
 * silent failure on the board -- the gain drifts behind the audio and
 * nothing says so -- so every one of them is a case below.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "streamgain.h"

static int failures;
static int checks;

#define CHECK(cond, what) do {                                          \
    checks++;                                                           \
    if (!(cond)) {                                                      \
        printf("  FAIL %s (%s:%d)\n", (what), __FILE__, __LINE__);      \
        failures++;                                                     \
    }                                                                   \
} while (0)

/* A mean square for a given LUFS, so cases can be written in the units
 * the standard uses and the header talks in. */
static float ms_for(float lufs)
{
    return (float)pow(10.0, ((double)lufs + 0.691) / 10.0);
}

static void push_n(streamgain_t *g, int n, float lufs, int peak,
                   uint32_t bytes)
{
    for (int i = 0; i < n; i++) streamgain_push(g, ms_for(lufs), peak, bytes);
}

/* ------------------------------------------------------------------ */

static void test_accounting(void)
{
    printf("byte accounting\n");
    static streamgain_t g;
    streamgain_reset(&g);

    /* Ten blocks of 17640 bytes: 100 ms of 44.1 kHz stereo. */
    push_n(&g, 10, -20.0f, 1000, 17640);
    CHECK(streamgain_count(&g) == 10, "ten records in");
    CHECK(streamgain_queued_bytes(&g) == 176400, "and their bytes");
    CHECK(streamgain_window_ms(&g) == 1000, "one second of window");

    /* A chunk smaller than a block retires nothing. */
    streamgain_consume(&g, 4096);
    CHECK(streamgain_count(&g) == 10, "a part-block retires nothing");
    CHECK(streamgain_queued_bytes(&g) == 176400 - 4096, "but is counted");

    /* The rest of that block, exactly. */
    streamgain_consume(&g, 17640 - 4096);
    CHECK(streamgain_count(&g) == 9, "the block completes");

    /* A chunk spanning several blocks. */
    streamgain_consume(&g, 17640 * 3 + 10);
    CHECK(streamgain_count(&g) == 6, "a chunk over three blocks");
    CHECK(streamgain_queued_bytes(&g) == 176400 - (17640 * 4 + 4096 + 10 - 4096),
          "bytes still agree");

    /* Everything, and past the end: the carrier must not go negative or
     * wrap, because both are a window that never becomes confident
     * again for the rest of the session. */
    streamgain_consume(&g, 1000000);
    CHECK(streamgain_count(&g) == 0, "drained");
    CHECK(streamgain_queued_bytes(&g) == 0, "and reads empty rather than huge");
}

static void test_drop_keeps_bytes(void)
{
    printf("dropped records still count their bytes\n");
    static streamgain_t g;
    streamgain_reset(&g);

    /* One more than the carrier holds. The last is refused and its PCM
     * is in the ring regardless -- this is the case the header says the
     * push signature exists to make impossible to get wrong. */
    push_n(&g, STREAMGAIN_RECORDS + 5, -20.0f, 1000, 1000);
    CHECK(streamgain_count(&g) == STREAMGAIN_RECORDS, "full, not over");
    CHECK(g.dropped == 5, "and the drops are counted");
    CHECK(streamgain_queued_bytes(&g) == (uint32_t)(STREAMGAIN_RECORDS + 5) * 1000,
          "dropped bytes are still queued bytes");

    /* Playing all of it, including what was dropped, must leave the
     * carrier empty and in step rather than one window behind. */
    streamgain_consume(&g, (uint32_t)(STREAMGAIN_RECORDS + 5) * 1000);
    CHECK(streamgain_count(&g) == 0, "empty after the audio played");
    CHECK(streamgain_queued_bytes(&g) == 0, "and level with the ring");
}

static void test_drain(void)
{
    printf("drain\n");
    static streamgain_t g;
    streamgain_reset(&g);

    push_n(&g, 40, -14.0f, 4000, 17640);
    streamgain_step(&g, 100000);            /* long enough to reach it */
    const float held = streamgain_db(&g);
    CHECK(g.have_gain, "a confident window gives an answer");

    streamgain_drain(&g);
    CHECK(streamgain_count(&g) == 0, "drained of records");
    CHECK(streamgain_queued_bytes(&g) == 0, "and of bytes");
    CHECK(streamgain_db(&g) == held, "but the station's gain is kept");

    /* And the accounting still works afterwards, which is the thing a
     * drain implemented as "consume a huge number" would break. */
    push_n(&g, 3, -14.0f, 4000, 100);
    streamgain_consume(&g, 300);
    CHECK(streamgain_count(&g) == 0, "in step after a drain");
    CHECK(streamgain_queued_bytes(&g) == 0, "bytes too");
}

static void test_gate(void)
{
    printf("the gate\n");
    static streamgain_t g;
    float db;

    streamgain_reset(&g);
    CHECK(!streamgain_window_db(&g, &db), "an empty window says nothing");

    /* A uniform window: the answer is the target minus the level, and
     * the gating has nothing to do. */
    streamgain_reset(&g);
    push_n(&g, 50, -23.0f, 1000, 100);
    CHECK(streamgain_window_db(&g, &db), "a full window answers");
    CHECK(fabsf(db - (STREAMGAIN_TARGET_LUFS - (-23.0f))) < 0.05f,
          "target minus level");

    /* Digital silence takes no part: half the window at -100 LUFS must
     * not drag the answer down. This is the case the header says the
     * relative gate is still worth running for. */
    streamgain_reset(&g);
    push_n(&g, 25, -23.0f, 1000, 100);
    push_n(&g, 25, -100.0f, 0, 100);
    CHECK(streamgain_window_db(&g, &db), "half silence still answers");
    CHECK(fabsf(db - (STREAMGAIN_TARGET_LUFS - (-23.0f))) < 0.05f,
          "and the silence is gated out");

    /* An all-silent window says nothing rather than asking for +82 dB,
     * which is what an ungated mean would do. */
    streamgain_reset(&g);
    push_n(&g, 50, -100.0f, 0, 100);
    CHECK(!streamgain_window_db(&g, &db), "silence is not a measurement");
}

static void test_slew_and_clamp(void)
{
    printf("slew and clamp\n");
    static streamgain_t g;

    /* The first confident answer is taken whole. Every later move is
     * rate-limited. */
    streamgain_reset(&g);
    push_n(&g, 50, -30.0f, 100, 100);       /* quiet, and a low peak */
    streamgain_step(&g, 100);
    CHECK(g.have_gain, "the first window is believed");
    const float first = streamgain_db(&g);
    CHECK(first > 5.0f, "a quiet station is boosted");
    CHECK(first <= STREAMGAIN_MAX_BOOST_DB + 0.001f, "but not past the bound");

    /* Now ask for a large cut and check it takes the slew's time. */
    streamgain_drain(&g);
    push_n(&g, 50, -5.0f, 30000, 100);
    const float before = streamgain_db(&g);
    streamgain_step(&g, 1000);
    CHECK(streamgain_db(&g) < before, "it moves toward the cut");
    CHECK(before - streamgain_db(&g) <= STREAMGAIN_SLEW_DB_S + 0.001f,
          "by no more than a second's worth");

    /* Ten more seconds gets there, and stops there. */
    for (int i = 0; i < 20; i++) streamgain_step(&g, 1000);
    const float settled = streamgain_db(&g);
    streamgain_step(&g, 1000);
    CHECK(fabsf(settled - streamgain_db(&g)) < 0.001f, "and then holds");
    CHECK(settled >= -STREAMGAIN_MAX_CUT_DB - 0.001f, "inside the cut bound");

    /*
     * The peak clamp, which is what makes the boost bound real. A quiet
     * measurement whose peak is already at full scale cannot be boosted
     * at all, however much the gate asks for.
     */
    streamgain_reset(&g);
    push_n(&g, 50, -30.0f, 32767, 100);
    streamgain_step(&g, 100);
    CHECK(streamgain_db(&g) <= 0.001f,
          "a window peaking at full scale gets no boost");
}

static void test_thin_window_holds(void)
{
    printf("a thin window holds\n");
    static streamgain_t g;
    streamgain_reset(&g);

    /* Under STREAMGAIN_MIN_WINDOW_MS there is nothing worth gating, and
     * the answer must be to hold rather than to measure harder. Before
     * there has ever been an answer, holding is 0 dB. */
    push_n(&g, 5, -30.0f, 100, 100);        /* 500 ms */
    CHECK(!streamgain_confident(&g), "500 ms is not a measurement");
    streamgain_step(&g, 100);
    CHECK(!g.have_gain, "and produces no answer");
    CHECK(streamgain_db(&g) == 0.0f, "unity until there is one");

    /* Get a real answer, then let the window go thin: the answer stays. */
    push_n(&g, 45, -30.0f, 100, 100);
    streamgain_step(&g, 100);
    const float held = streamgain_db(&g);
    CHECK(g.have_gain, "a full window answers");
    streamgain_consume(&g, 45 * 100);       /* back under the floor */
    CHECK(!streamgain_confident(&g), "thin again");
    streamgain_step(&g, 1000);
    CHECK(streamgain_db(&g) == held, "and the last good answer is held");
}

static void test_q16(void)
{
    printf("the multiplier\n");
    CHECK(streamgain_q16(0.0f) == 65536, "unity is unity");
    CHECK(streamgain_q16(6.0f) > 65536, "a boost multiplies up");
    CHECK(streamgain_q16(-6.0f) < 65536, "a cut multiplies down");
    /* -6.02 dB is a half, and landing on it says the curve is amplitude
     * and not power -- the mistake that makes every gain half of what
     * was asked for. */
    CHECK(abs(streamgain_q16(-6.0206f) - 32768) < 40, "-6 dB is a half");
    /* The bounds are enforced here too, so a caller that skipped the
     * step cannot hand the sample loop a multiplier of 40. */
    CHECK(streamgain_q16(60.0f) == streamgain_q16(STREAMGAIN_MAX_BOOST_DB),
          "clamped at the boost bound");
    CHECK(streamgain_q16(-60.0f) == streamgain_q16(-STREAMGAIN_MAX_CUT_DB),
          "and at the cut bound");
}

/*
 * The whole point, stated over the AUDIO rather than over the fields:
 * after any sequence of pushes and consumes, the carrier's idea of what
 * is queued equals the ring's. Driven with block and chunk sizes that
 * have no common factor, because equal sizes are the one case that
 * cannot expose a straddle.
 */
static void test_in_step(void)
{
    printf("in step with the ring, over a long session\n");
    static streamgain_t g;
    streamgain_reset(&g);

    uint32_t written = 0, played = 0;
    unsigned seed = 12345;
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1103515245u + 12345u;
        /* 100 ms of 44.1 kHz stereo, jittered, against chunks that
         * share no factor with it. */
        const uint32_t block = 17640 + (seed >> 28);
        seed = seed * 1103515245u + 12345u;
        const uint32_t chunk = 4000 + ((seed >> 20) & 0x7FF);

        streamgain_push(&g, ms_for(-20.0f), 1000, block);
        written += block;

        if (written - played > 40960) {
            const uint32_t take = chunk < (written - played) ? chunk
                                                             : (written - played);
            streamgain_consume(&g, take);
            played += take;
        }

        if (streamgain_queued_bytes(&g) != written - played) {
            CHECK(false, "carrier and ring agree");
            break;
        }
    }
    CHECK(streamgain_queued_bytes(&g) == written - played, "still in step");

    /* And the window never claims more audio than the ring holds, which
     * is the reading the confidence gate is made from. */
    /* A block is 100 ms and 17640-odd bytes, so the window in
     * milliseconds must not claim more audio than the reserve holds at
     * 176.4 bytes per millisecond. Checked because the confidence gate
     * is read as a duration, and a window that overstates is a gain
     * believed on evidence that is not there. */
    CHECK((uint64_t)streamgain_window_ms(&g) * 1764u / 10u <= (written - played),
          "the window is not longer than the reserve");
}

int main(void)
{
    printf("streamgaintest\n");

    test_accounting();
    test_drop_keeps_bytes();
    test_drain();
    test_gate();
    test_slew_and_clamp();
    test_thin_window_holds();
    test_q16();
    test_in_step();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
