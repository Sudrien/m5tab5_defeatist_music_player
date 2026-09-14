/*
 * bufferplantest.c -- the watermark machine, including the edges that
 * only happen on a bad evening.
 *
 * The two properties worth more than any single case:
 *
 *   1. **It never chops.** Once the writer stops it stays stopped until
 *      RESUME_MS, whatever the buffer does in between. A ring sitting on
 *      the threshold is the normal condition of a 0.97x link, and a
 *      single-threshold version sounds like chopping rather than like a
 *      pause.
 *
 *   2. **It never claims audio it does not have.** `audible` is never
 *      true with an empty buffer, in any phase, on any input sequence --
 *      checked on 20000 random walks including hostile ones.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bufferplan.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do {                           \
    checks++;                                           \
    if (!(cond)) {                                      \
        failures++;                                     \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
    }                                                   \
} while (0)

static bufplan_t b;
static bufplan_out_t out;

/*
 * The ring the cases below are played on.
 *
 * 5334 ms of capacity puts the 75% target at exactly 4000, which is the
 * fixed threshold every case in this file was written against. So the
 * older cases are unchanged in meaning and still say what they said:
 * what moved is where the number comes from, not what it is. The cases
 * that exercise a DIFFERENT ring set this for themselves.
 */
#define CAPACITY_4000   (5334)

static int g_capacity = CAPACITY_4000;
static int START = 4000;

static void set_capacity(int capacity_ms)
{
    g_capacity = capacity_ms;
    START = bufplan_start_target_ms(capacity_ms);
}

/* One step at a given time with a given buffer level. */
static void step(int64_t now, int buffered, bool done, bool stop)
{
    bufplan_in_t in = {
        .now_ms = now, .buffered_ms = buffered,
        .capacity_ms = g_capacity,
        .source_done = done, .stop_requested = stop,
    };
    bufplan_step(&b, &in, &out);
}

int main(void)
{
    printf("bufferplantest\n");
    set_capacity(CAPACITY_4000);
    CHECK(START == 4000, "the shim ring's target is %d, not 4000", START);

    /* ---------------------------------------------------------------- */
    /* The ordinary life of a stream                                     */
    /* ---------------------------------------------------------------- */
    {
        bufplan_init(&b, 0);
        step(0, 0, false, false);
        CHECK(!out.audible && out.show_buffering,
              "silent at the start: audible=%d buffering=%d",
              out.audible, out.show_buffering);

        /* Filling, but not there yet. Sound must not start early: the
         * whole point of the watermark is the dip that comes after. */
        step(1000, 1500, false, false);
        CHECK(!out.audible, "sound started at 1.5 s of buffer");
        step(2000, START - 1, false, false);
        CHECK(!out.audible, "sound started one millisecond early");

        step(3000, START, false, false);
        CHECK(out.audible && !out.show_buffering,
              "sound did not start at the watermark");
        CHECK(b.phase == BUFPLAN_PLAYING, "phase is %s",
              bufplan_phase_name(b.phase));

        /* A dip that stays above LOW_MS is not a rebuffer. This is the
         * common case on a 0.97x link and it must be inaudible. */
        step(8000, 2000, false, false);
        CHECK(out.audible, "a dip to 2 s stopped the writer");
        step(9000, BUFPLAN_LOW_MS, false, false);
        CHECK(out.audible, "a dip to exactly LOW_MS stopped the writer");
        CHECK(b.rebuffers == 0, "%u rebuffers counted during a healthy dip",
              b.rebuffers);
    }

    /* ---------------------------------------------------------------- */
    /* Hysteresis: the property that keeps it from chopping              */
    /* ---------------------------------------------------------------- */
    {
        bufplan_init(&b, 0);
        step(0, START, false, false);
        CHECK(out.audible, "setup: not playing");

        step(1000, BUFPLAN_LOW_MS - 1, false, false);
        CHECK(!out.audible && out.show_buffering,
              "did not stop below LOW_MS");
        CHECK(b.rebuffers == 1, "%u rebuffers counted", b.rebuffers);

        /* Everything between LOW and RESUME must stay silent. A
         * single-threshold version restarts here, once per step. */
        for (int ms = BUFPLAN_LOW_MS; ms < BUFPLAN_RESUME_MS; ms += 100) {
            step(2000 + ms, ms, false, false);
            CHECK(!out.audible,
                  "restarted at %d ms, below RESUME_MS -- this is the chop", ms);
        }
        CHECK(b.rebuffers == 1,
              "%u rebuffers after one dry-out -- it re-entered the phase",
              b.rebuffers);

        step(20000, BUFPLAN_RESUME_MS, false, false);
        CHECK(out.audible, "did not resume at RESUME_MS");
        CHECK(b.phase == BUFPLAN_PLAYING, "phase is %s",
              bufplan_phase_name(b.phase));

        /* And a second dry-out counts again. */
        step(21000, 0, false, false);
        CHECK(b.rebuffers == 2, "second dry-out counted %u", b.rebuffers);
    }

    /* ---------------------------------------------------------------- */
    /* A stream that ends with audio still buffered                      */
    /* ---------------------------------------------------------------- */
    {
        bufplan_init(&b, 0);
        step(0, START, false, false);
        CHECK(out.audible, "setup: not playing");

        /* The socket closes with eight seconds in the ring. Those bytes
         * are real audio; going silent now throws them away. */
        step(1000, 8000, true, false);
        CHECK(out.audible, "went silent with 8 s of audio still buffered");
        CHECK(b.phase == BUFPLAN_DRAINING, "phase is %s",
              bufplan_phase_name(b.phase));

        /* Draining does not rebuffer, even below LOW_MS: there is
         * nothing to refill from, and a pause before the end of
         * something already over is worse than the end. */
        step(9000, 500, true, false);
        CHECK(out.audible, "draining rebuffered below LOW_MS");
        CHECK(b.phase == BUFPLAN_DRAINING, "phase is %s",
              bufplan_phase_name(b.phase));
        CHECK(b.rebuffers == 0, "draining counted %u rebuffers", b.rebuffers);

        step(9500, 0, true, false);
        CHECK(!out.audible && out.finished, "did not end when the buffer emptied");
        CHECK(b.phase == BUFPLAN_ENDED, "phase is %s", bufplan_phase_name(b.phase));
    }

    /* Ending during a rebuffer drains rather than waiting for a
     * RESUME_MS that will never arrive. */
    {
        bufplan_init(&b, 0);
        step(0, START, false, false);
        step(1000, 0, false, false);
        CHECK(b.phase == BUFPLAN_REBUFFERING, "setup: phase is %s",
              bufplan_phase_name(b.phase));
        step(2000, 2000, true, false);
        CHECK(b.phase == BUFPLAN_DRAINING,
              "a dead source left it rebuffering behind a message that is a lie");
        CHECK(out.audible, "did not play out what was buffered");
    }

    /* Ending during a rebuffer with nothing buffered ends outright. */
    {
        bufplan_init(&b, 0);
        step(0, START, false, false);
        step(1000, 0, false, false);
        step(2000, 0, true, false);
        CHECK(b.phase == BUFPLAN_ENDED, "phase is %s", bufplan_phase_name(b.phase));
        CHECK(out.finished && !out.audible, "ended but not finished/silent");
    }

    /* A source that dies during preroll still plays what it got. */
    {
        bufplan_init(&b, 0);
        step(0, 2000, false, false);
        CHECK(!out.audible, "setup: audible during preroll");
        step(1000, 2000, true, false);
        CHECK(b.phase == BUFPLAN_DRAINING && out.audible,
              "two seconds decoded before the source died were thrown away");
    }
    {
        bufplan_init(&b, 0);
        step(1000, 0, true, false);
        CHECK(b.phase == BUFPLAN_ENDED, "a dead source with nothing buffered: %s",
              bufplan_phase_name(b.phase));
    }

    /* ---------------------------------------------------------------- */
    /* Giving up                                                         */
    /* ---------------------------------------------------------------- */

    /* A station too slow for this link must not sit on "Buffering" all
     * evening. */
    {
        bufplan_init(&b, 0);
        for (int64_t t = 0; t < BUFPLAN_PREROLL_GIVEUP_MS; t += 1000) {
            step(t, 500, false, false);
            CHECK(b.phase == BUFPLAN_PREROLL, "gave up early at %lld ms", (long long)t);
        }
        step(BUFPLAN_PREROLL_GIVEUP_MS, 500, false, false);
        CHECK(b.phase == BUFPLAN_ENDED, "never gave up on preroll: %s",
              bufplan_phase_name(b.phase));
    }

    /* A rebuffer that gets nowhere. The limit is longer than netplan's
     * whole backoff schedule, so a source still working through its
     * retries is never cut off by this. */
    {
        CHECK(BUFPLAN_STALL_GIVEUP_MS > 15000,
              "the stall limit (%d ms) is shorter than netplan's backoff "
              "schedule, so a source still retrying would be cut off",
              BUFPLAN_STALL_GIVEUP_MS);

        bufplan_init(&b, 0);
        step(0, START, false, false);
        step(100, 0, false, false);
        CHECK(b.phase == BUFPLAN_REBUFFERING, "setup: %s",
              bufplan_phase_name(b.phase));
        for (int64_t t = 200; t < 100 + BUFPLAN_STALL_GIVEUP_MS; t += 1000) {
            step(t, 500, false, false);
            CHECK(b.phase == BUFPLAN_REBUFFERING, "gave up early at %lld", (long long)t);
        }
        step(100 + BUFPLAN_STALL_GIVEUP_MS, 500, false, false);
        CHECK(b.phase == BUFPLAN_ENDED, "never gave up on a stalled rebuffer: %s",
              bufplan_phase_name(b.phase));
    }

    /* A rebuffer that recovers just before the limit is not punished. */
    {
        bufplan_init(&b, 0);
        step(0, START, false, false);
        step(100, 0, false, false);
        step(100 + BUFPLAN_STALL_GIVEUP_MS - 1, BUFPLAN_RESUME_MS, false, false);
        CHECK(b.phase == BUFPLAN_PLAYING, "a recovery one ms early was refused: %s",
              bufplan_phase_name(b.phase));
    }

    /* ---------------------------------------------------------------- */
    /* Stop, and ENDED being terminal                                    */
    /* ---------------------------------------------------------------- */
    {
        bufplan_init(&b, 0);
        step(0, START, false, false);
        /* Nobody wants four more seconds of a station they just left. */
        step(1000, 10000, false, true);
        CHECK(!out.audible && out.finished,
              "a stop kept playing buffered audio");
        CHECK(b.phase == BUFPLAN_ENDED, "phase is %s", bufplan_phase_name(b.phase));

        /* Terminal: no input revives it. */
        step(2000, 20000, false, false);
        CHECK(b.phase == BUFPLAN_ENDED && !out.audible && out.finished,
              "ENDED was left by a full buffer");
        step(3000, 20000, false, false);
        CHECK(b.phase == BUFPLAN_ENDED, "ENDED was left on a later step");
    }

    /* A stop during preroll, and during a rebuffer. */
    {
        bufplan_init(&b, 0);
        step(0, 1000, false, false);
        step(500, 1000, false, true);
        CHECK(b.phase == BUFPLAN_ENDED, "stop during preroll: %s",
              bufplan_phase_name(b.phase));

        bufplan_init(&b, 0);
        step(0, START, false, false);
        step(100, 0, false, false);
        step(200, 0, false, true);
        CHECK(b.phase == BUFPLAN_ENDED, "stop during rebuffer: %s",
              bufplan_phase_name(b.phase));
    }

    /* ---------------------------------------------------------------- */
    /* Level-triggered: skipped readings cannot strand it                */
    /* ---------------------------------------------------------------- */
    {
        /* A caller that misses every reading between empty and full must
         * end up in the same place as one that saw them all. */
        bufplan_init(&b, 0);
        step(0, 0, false, false);
        step(60000, 20000, false, false);
        CHECK(b.phase == BUFPLAN_PLAYING,
              "a single large jump stranded it in %s", bufplan_phase_name(b.phase));
    }

    /* Repeated identical steps change nothing (idempotent at rest). */
    {
        bufplan_init(&b, 0);
        step(0, START, false, false);
        const uint32_t r = b.rebuffers;
        for (int i = 0; i < 50; i++) step(1000, START, false, false);
        CHECK(b.phase == BUFPLAN_PLAYING && b.rebuffers == r,
              "repeated identical steps moved it to %s (%u rebuffers)",
              bufplan_phase_name(b.phase), b.rebuffers);
    }

    /* Negative and absurd buffer readings are clamped, not trusted. */
    {
        bufplan_init(&b, 0);
        step(0, START, false, false);
        step(1000, -5000, false, false);
        CHECK(!out.audible, "a negative buffer reading stayed audible");
        bufplan_init(&b, 0);
        step(0, 1 << 30, false, false);
        CHECK(out.audible, "an absurd buffer reading refused to play");
    }

    /* Time going backwards (an NTP step) must not accumulate nonsense or
     * trip a timeout early. */
    {
        bufplan_init(&b, 0);
        step(10000, 0, false, false);
        step(5000, 0, false, false);
        step(6000, 0, false, false);
        CHECK(b.silent_ms >= 0, "silent_ms went negative: %lld",
              (long long)b.silent_ms);
        CHECK(b.phase == BUFPLAN_PREROLL, "a clock step changed phase to %s",
              bufplan_phase_name(b.phase));
    }

    /* ---------------------------------------------------------------- */
    /* The start target as a share of the ring                           */
    /* ---------------------------------------------------------------- */

    /* A 44.1 kHz station on the real ring: 3520 KB is 20.4 s, so the
     * target is a little over fifteen seconds. A link with headroom
     * must wait for it and must NOT start at the old four. */
    {
        set_capacity(20400);
        CHECK(START == 15300, "target on a 20.4 s ring is %d", START);

        bufplan_init(&b, 0);
        step(0, 0, false, false);
        /* Climbing fast enough that the stall detector never fires:
         * three seconds of audio per five of wall clock is 1.6x. */
        for (int64_t t = 1000; t <= 26000; t += 1000) {
            const int level = (int)(t * 3 / 5);
            step(t, level < 20400 ? level : 20400, false, false);
            if (level < START) {
                CHECK(!out.audible, "sound started at %d of %d", level, START);
            }
        }
        CHECK(out.audible, "sound never started on a fast link");
    }

    /* The same ring, a station delivering at almost exactly its own
     * bitrate. The reserve stops growing around four seconds and the
     * target is unreachable forever. This is the case that decides
     * whether the feature is usable: without the floor it is a healthy
     * station declared dead at PREROLL_GIVEUP_MS. */
    {
        set_capacity(20400);
        bufplan_init(&b, 0);
        step(0, 0, false, false);

        step(2000, 3000, false, false);
        CHECK(!out.audible, "started below the floor");
        step(4000, 4100, false, false);
        CHECK(!out.audible, "started at the floor without waiting a window");

        /* Flat from here. The first window closes at 5000 and finds
         * 4100 against a mark of 0 -- that is growth, so it does not
         * fire. The second closes at 10000 with no growth at all. */
        step(5000, 4150, false, false);
        CHECK(!out.audible, "the first window mistook the ramp for a stall");
        step(8000, 4180, false, false);
        CHECK(!out.audible, "started before a window had closed flat");
        step(10000, 4200, false, false);
        CHECK(out.audible, "a flat link never started at all");
        CHECK(b.phase == BUFPLAN_PLAYING, "phase is %s",
              bufplan_phase_name(b.phase));
    }

    /* Flat but BELOW the floor is not a start. There is a level below
     * which four seconds of audio is not a reserve, and a link stuck
     * there is the one PREROLL_GIVEUP_MS is for. */
    {
        set_capacity(20400);
        bufplan_init(&b, 0);
        step(0, 0, false, false);
        for (int64_t t = 1000; t < BUFPLAN_PREROLL_GIVEUP_MS; t += 1000) {
            step(t, 1200, false, false);
            CHECK(!out.audible, "started on 1.2 s at t=%lld", (long long)t);
        }
        step(BUFPLAN_PREROLL_GIVEUP_MS, 1200, false, false);
        CHECK(b.phase == BUFPLAN_ENDED, "a stuck link did not give up: %s",
              bufplan_phase_name(b.phase));
    }

    /* A ring smaller than the floor. Neither number may name a level
     * that cannot exist: the failure this guards is the ring
     * saturating, the decode loop parking in the send, and the station
     * dying of a threshold rather than of the network. */
    {
        set_capacity(3000);
        CHECK(START == 2250, "target on a 3 s ring is %d", START);
        CHECK(bufplan_start_floor_ms(3000) == 2250,
              "the floor was not clamped to the ring");

        bufplan_init(&b, 0);
        step(0, 0, false, false);
        step(1000, 2249, false, false);
        CHECK(!out.audible, "started one millisecond early on a small ring");
        step(2000, 2250, false, false);
        CHECK(out.audible, "a small ring never reached its own target");
    }

    /* Capacity unknown -- the rate is zero until the first frame
     * decodes, and the caller says so rather than guessing. The floor
     * alone applies, which is what this did before the share existed. */
    {
        set_capacity(0);
        CHECK(START == BUFPLAN_START_MIN_MS, "unknown capacity gave %d", START);

        bufplan_init(&b, 0);
        step(0, 0, false, false);
        step(1000, BUFPLAN_START_MIN_MS - 1, false, false);
        CHECK(!out.audible, "started early with no capacity known");
        step(2000, BUFPLAN_START_MIN_MS, false, false);
        CHECK(out.audible, "never started with no capacity known");
    }

    /* The window belongs to the phase, not to the machine: a second
     * station must not inherit the first one's mark and start on a
     * stall it never observed. */
    {
        set_capacity(20400);
        bufplan_init(&b, 0);
        step(0, 0, false, false);
        for (int64_t t = 1000; t <= 12000; t += 1000) step(t, 4200, false, false);
        CHECK(out.audible, "the first station did not start");

        /* A new station, well past the first one's window boundaries. */
        bufplan_init(&b, 20000);
        step(20000, 0, false, false);
        step(20500, 4200, false, false);
        CHECK(!out.audible,
              "the second station inherited the first one's stall window");
        /* Its own first window closes at 25000 and sees the ramp from
         * zero, which is growth. The one after it is the flat one. */
        step(26000, 4250, false, false);
        CHECK(!out.audible, "the second station started on its ramp window");
        step(31500, 4260, false, false);
        CHECK(out.audible, "the second station never started");
    }

    set_capacity(CAPACITY_4000);

    /* ---------------------------------------------------------------- */
    /* Random walks: the invariants, on 20000 hostile sequences          */
    /* ---------------------------------------------------------------- */
    {
        srand(20260911);
        for (int iter = 0; iter < 20000; iter++) {
            bufplan_init(&b, 0);
            int64_t now = 0;
            int buffered = 0;
            bool done = false;
            bufplan_phase_t prev = b.phase;
            bool was_audible = false;
            int64_t stopped_at = -1;

            for (int s = 0; s < 60; s++) {
                now += rand() % 3000;
                /* A walk that goes everywhere, including straight to the
                 * thresholds and straight past them. */
                switch (rand() % 5) {
                case 0: buffered = 0; break;
                case 1: buffered = BUFPLAN_LOW_MS; break;
                case 2: buffered = BUFPLAN_RESUME_MS; break;
                case 3: buffered += (rand() % 4000) - 2000; break;
                default: buffered = rand() % 20000; break;
                }
                if (buffered < 0) buffered = 0;
                if (!done && rand() % 40 == 0) done = true;
                const bool stop = (rand() % 200 == 0);

                step(now, buffered, done, stop);

                /* Invariant 1: never audible with nothing to play. */
                if (out.audible && buffered <= 0) {
                    CHECK(0, "iter %d step %d: audible with an empty buffer in %s",
                          iter, s, bufplan_phase_name(b.phase));
                    goto done_walk;
                }
                /* Invariant 2: no chopping. Going from silent to audible
                 * mid-stream only ever happens at RESUME_MS or better. */
                /* DRAINING is deliberately exempt: a source that died
                 * mid-rebuffer plays out what is left, and that is the
                 * honest end of the stream rather than a restart. The
                 * chop this guards against is a return to PLAYING. */
                if (out.audible && !was_audible &&
                    prev == BUFPLAN_REBUFFERING &&
                    b.phase == BUFPLAN_PLAYING &&
                    buffered < BUFPLAN_RESUME_MS) {
                    CHECK(0, "iter %d step %d: resumed at %d ms, below RESUME_MS",
                          iter, s, buffered);
                    goto done_walk;
                }
                /* And the exemption is not a hole: leaving REBUFFERING
                 * while audible below RESUME_MS is only ever DRAINING,
                 * and only ever with a dead source. */
                if (out.audible && !was_audible &&
                    prev == BUFPLAN_REBUFFERING && buffered < BUFPLAN_RESUME_MS &&
                    !(b.phase == BUFPLAN_DRAINING && done)) {
                    CHECK(0, "iter %d step %d: became audible at %d ms in %s "
                             "with source_done=%d", iter, s, buffered,
                          bufplan_phase_name(b.phase), (int)done);
                    goto done_walk;
                }
                /* Invariant 3: ENDED is terminal. */
                if (stopped_at >= 0 && b.phase != BUFPLAN_ENDED) {
                    CHECK(0, "iter %d step %d: left ENDED for %s",
                          iter, s, bufplan_phase_name(b.phase));
                    goto done_walk;
                }
                if (b.phase == BUFPLAN_ENDED && stopped_at < 0) stopped_at = now;
                /* Invariant 4: finished and audible are exclusive. */
                if (out.finished && out.audible) {
                    CHECK(0, "iter %d step %d: finished and audible at once", iter, s);
                    goto done_walk;
                }
                /* Invariant 5: exactly one of the three outputs, or none
                 * only when draining an empty ring is impossible. */
                if (out.audible && out.show_buffering) {
                    CHECK(0, "iter %d step %d: audible and buffering at once", iter, s);
                    goto done_walk;
                }

                prev = b.phase;
                was_audible = out.audible;
            }
            checks++;
        done_walk:;
        }
    }

    /* Every phase has a distinct name. */
    {
        const bufplan_phase_t all[] = {
            BUFPLAN_PREROLL, BUFPLAN_PLAYING, BUFPLAN_REBUFFERING,
            BUFPLAN_DRAINING, BUFPLAN_ENDED,
        };
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
            const char *n = bufplan_phase_name(all[i]);
            CHECK(n && n[0] && strcmp(n, "?") != 0, "phase %d has no name", (int)all[i]);
            for (size_t j = 0; j < i; j++) {
                CHECK(strcmp(n, bufplan_phase_name(all[j])) != 0,
                      "phases %d and %d share \"%s\"", (int)all[i], (int)all[j], n);
            }
        }
    }


    /* ---------------------------------------------------------------- */
    /* bufplan_note_attempt(): the stall clock is not wall clock         */
    /* ---------------------------------------------------------------- */
    {
        /*
         * The board case. A resume rebuffers; the source spends six
         * seconds per attempt against a server that handshakes and then
         * times out; five of those is thirty seconds and the stall rule
         * ended the stream 25 ms after the fifth attempt succeeded.
         */
        bufplan_t b;
        bufplan_init(&b, 0);
        bufplan_in_t in = { 0 };
        bufplan_out_t out;
        in.buffered_ms = 0;

        /* Into REBUFFERING, the silent phase the giveup applies to. */
        bufplan_enter(&b, BUFPLAN_REBUFFERING, 1000);
        CHECK(b.phase == BUFPLAN_REBUFFERING, "rebuffering");

        /* Without a note, thirty seconds of silence ends it -- which is
         * the rule and is correct for a source doing nothing. */
        in.now_ms = 1000 + BUFPLAN_STALL_GIVEUP_MS;
        bufplan_step(&b, &in, &out);
        CHECK(b.phase == BUFPLAN_ENDED, "no attempts: 30 s ends it");

        /* With an attempt noted every six seconds, it does not. This is
         * the fault: five attempts of ~6 s cross 30 s of wall clock
         * while the source is plainly still working. */
        bufplan_init(&b, 0);
        bufplan_enter(&b, BUFPLAN_REBUFFERING, 1000);
        int64_t t = 1000;
        for (int attempt = 0; attempt < 5; attempt++) {
            t += 6000;
            bufplan_note_attempt(&b, t);
            in.now_ms = t;
            bufplan_step(&b, &in, &out);
            CHECK(b.phase == BUFPLAN_REBUFFERING,
                  "attempt %d at %lld ms still rebuffering, not %s",
                  attempt + 1, (long long)t, bufplan_phase_name(b.phase));
        }
        CHECK(t > BUFPLAN_STALL_GIVEUP_MS,
              "the test must actually pass 30 s of wall clock (%lld)",
              (long long)t);

        /* And the rule still bites once the attempts stop. Thirty
         * seconds after the LAST one, not after the first. */
        in.now_ms = t + BUFPLAN_STALL_GIVEUP_MS;
        bufplan_step(&b, &in, &out);
        CHECK(b.phase == BUFPLAN_ENDED, "30 s after the last attempt ends it");

        /* PREROLL too: a first connect can be just as slow. */
        bufplan_init(&b, 0);
        CHECK(b.phase == BUFPLAN_PREROLL, "starts in preroll");
        bufplan_note_attempt(&b, 9000);
        CHECK(b.phase_since_ms == 9000, "preroll clock restarts");

        /* But not in a phase where the clock means something else. A
         * note while playing must not move anything. */
        bufplan_enter(&b, BUFPLAN_PLAYING, 10000);
        bufplan_note_attempt(&b, 20000);
        CHECK(b.phase_since_ms == 10000,
              "playing is untouched, got %lld", (long long)b.phase_since_ms);

        bufplan_note_attempt(NULL, 0);  /* must not crash */
    }

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
