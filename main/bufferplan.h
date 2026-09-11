/*
 * bufferplan.h -- when a stream is allowed to make a sound, with nothing
 * to run.
 *
 * Phase 3 owns the writer, and the only question it has that is not a
 * measurement is this one: given how much decoded audio is sitting in
 * the PCM ring, does the writer run? A file answers it by never asking
 * -- the card is faster than playback and the ring is always full. A
 * live stream at 0.97x answers it several times an evening.
 *
 * WHY THIS IS A MACHINE AND NOT AN IF
 *
 * `if (buffered < 1s) stop;` flaps. The ring sits at the threshold, the
 * writer stops and starts within a few milliseconds of each other, and
 * the listener hears chopping rather than a pause. So there are two
 * thresholds and not one -- stop below LOW_MS, do not start again until
 * RESUME_MS -- and once there are two thresholds and a "have we started
 * yet" distinction and a stream that can end while the buffer still has
 * audio in it, it is a state machine whether or not it is written as
 * one.
 *
 * The cases that are easy to get wrong are all at the edges:
 *
 *   - **A stream that ends with audio still buffered.** Those bytes are
 *     real audio that was paid for; they play out. Going silent the
 *     instant the socket closes throws away up to eighteen seconds.
 *   - **A rebuffer that will never finish.** If the source has given up,
 *     waiting for RESUME_MS is waiting forever with a "Buffering"
 *     message that is a lie. Draining is the honest response.
 *   - **A stream that never reaches the first watermark at all.** A
 *     station too slow for this link would otherwise sit on "Buffering"
 *     silently for the rest of the evening.
 *
 * THE NUMBERS ARE GUESSES; THE MACHINE IS NOT
 *
 * 4 s to start, 1 s to stop, 4 s to resume. These come from the probe's
 * five-second windows falling to 0.83x -- a five-second window at 0.83x
 * is 0.85 s of shortfall, so a second of slack is about one bad window
 * and four is about four. **They will move after a flash and the machine
 * will not**, which is the whole reason they are named constants in a
 * file with a test rather than numbers in `player.c`.
 *
 * WHAT THIS DOES NOT DECIDE
 *
 * It does not know about the compressed ring -- that is netstream's, and
 * a decoder sitting between them means the two fill levels are not the
 * same fact. It does not stop, start or reconnect anything; it answers a
 * question and phase 3 acts. And it has no opinion on pause, because a
 * live stream has no pause: that is a stop and a fresh connect.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Milliseconds of decoded audio in the PCM ring. */
#define BUFPLAN_START_MS        (4000)
#define BUFPLAN_LOW_MS          (1000)
#define BUFPLAN_RESUME_MS       (4000)

/*
 * How long a rebuffer is allowed to get nowhere before the stream is
 * declared over. Longer than netplan's whole backoff schedule (15 s), so
 * a source that is still working through its retries is never cut off by
 * this -- it fires only when bytes are arriving too slowly to ever
 * reach RESUME_MS, which no retry will fix.
 */
#define BUFPLAN_STALL_GIVEUP_MS (30000)

/* The same patience before the first sound. A station this link cannot
 * carry must say so rather than showing "Buffering" all evening. */
#define BUFPLAN_PREROLL_GIVEUP_MS (30000)

typedef enum {
    BUFPLAN_PREROLL = 0,    /* connected, no sound yet, filling */
    BUFPLAN_PLAYING,        /* writer runs */
    BUFPLAN_REBUFFERING,    /* ran dry mid-stream; silent, refilling */
    BUFPLAN_DRAINING,       /* no more bytes will come; play out the rest */
    BUFPLAN_ENDED,          /* terminal */
} bufplan_phase_t;

static inline const char *bufplan_phase_name(bufplan_phase_t p)
{
    switch (p) {
    case BUFPLAN_PREROLL:     return "preroll";
    case BUFPLAN_PLAYING:     return "playing";
    case BUFPLAN_REBUFFERING: return "rebuffering";
    case BUFPLAN_DRAINING:    return "draining";
    case BUFPLAN_ENDED:       return "ended";
    default:                  return "?";
    }
}

typedef struct {
    bufplan_phase_t phase;
    int64_t         phase_since_ms;
    /* For the log and the screen. A stream that rebuffered eleven times
     * in an hour is a different report from one that played through, and
     * neither is visible from a single reading. */
    uint32_t rebuffers;
    int64_t  silent_ms;         /* total time the writer was held off */
    int64_t  last_now_ms;
} bufplan_t;

typedef struct {
    int64_t now_ms;
    int     buffered_ms;    /* decoded audio in the PCM ring */
    /* The source will produce no more bytes, ever: netstream is FAILED,
     * or it is IDLE because it was stopped. Not the same as "currently
     * retrying", which is still live. */
    bool    source_done;
    /* The listener asked for this to end. Ends it now, buffered audio
     * included -- nobody wants four more seconds of a station they just
     * left. */
    bool    stop_requested;
} bufplan_in_t;

typedef struct {
    bool audible;           /* the writer should run */
    bool show_buffering;    /* the screen should say so */
    bool finished;          /* phase 3 may tear down */
} bufplan_out_t;

static inline void bufplan_init(bufplan_t *b, int64_t now_ms)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->phase = BUFPLAN_PREROLL;
    b->phase_since_ms = now_ms;
    b->last_now_ms = now_ms;
}

static inline void bufplan_enter(bufplan_t *b, bufplan_phase_t p, int64_t now_ms)
{
    if (b->phase == p) return;
    if (p == BUFPLAN_REBUFFERING) b->rebuffers++;
    b->phase = p;
    b->phase_since_ms = now_ms;
}

/*
 * One step. Call as often as convenient -- this is level-triggered on
 * `buffered_ms`, not edge-triggered, so a caller that misses a reading
 * cannot leave it in the wrong phase.
 */
static inline void bufplan_step(bufplan_t *b, const bufplan_in_t *in,
                                bufplan_out_t *out)
{
    if (!b || !in || !out) return;
    memset(out, 0, sizeof(*out));

    /* Time that passed while the writer was held off, accumulated before
     * any transition so a phase change on this step is not counted in
     * the phase it is leaving for. */
    const int64_t dt = in->now_ms > b->last_now_ms ? in->now_ms - b->last_now_ms : 0;
    if (b->phase == BUFPLAN_PREROLL || b->phase == BUFPLAN_REBUFFERING) {
        b->silent_ms += dt;
    }
    b->last_now_ms = in->now_ms;

    if (b->phase == BUFPLAN_ENDED) {
        out->finished = true;
        return;
    }
    if (in->stop_requested) {
        bufplan_enter(b, BUFPLAN_ENDED, in->now_ms);
        out->finished = true;
        return;
    }

    const int buffered = in->buffered_ms > 0 ? in->buffered_ms : 0;

    switch (b->phase) {
    case BUFPLAN_PREROLL:
        if (buffered >= BUFPLAN_START_MS) {
            bufplan_enter(b, BUFPLAN_PLAYING, in->now_ms);
        } else if (in->source_done) {
            /* Never reached the watermark and nothing more is coming.
             * Whatever was decoded is still audio; play it. */
            bufplan_enter(b, buffered > 0 ? BUFPLAN_DRAINING : BUFPLAN_ENDED,
                          in->now_ms);
        } else if (in->now_ms - b->phase_since_ms >= BUFPLAN_PREROLL_GIVEUP_MS) {
            bufplan_enter(b, BUFPLAN_ENDED, in->now_ms);
        }
        break;

    case BUFPLAN_PLAYING:
        if (in->source_done) {
            /* The moment no more bytes are coming, this is a drain --
             * not when the buffer happens to run low. The audible
             * behaviour is the same either way (both keep the writer
             * running), but the phase is read for the screen and for
             * the log, and reporting "playing" for eighteen seconds
             * after the stream ended is reporting the wrong thing.
             * Entering here also means the no-rebuffer rule is already
             * in force when the level does fall. */
            bufplan_enter(b, buffered > 0 ? BUFPLAN_DRAINING : BUFPLAN_ENDED,
                          in->now_ms);
        } else if (buffered < BUFPLAN_LOW_MS) {
            bufplan_enter(b, BUFPLAN_REBUFFERING, in->now_ms);
        }
        break;

    case BUFPLAN_REBUFFERING:
        if (in->source_done) {
            /* Waiting for RESUME_MS from a source that has given up is
             * waiting forever behind a message that is a lie. */
            bufplan_enter(b, buffered > 0 ? BUFPLAN_DRAINING : BUFPLAN_ENDED,
                          in->now_ms);
        } else if (buffered >= BUFPLAN_RESUME_MS) {
            bufplan_enter(b, BUFPLAN_PLAYING, in->now_ms);
        } else if (in->now_ms - b->phase_since_ms >= BUFPLAN_STALL_GIVEUP_MS) {
            bufplan_enter(b, BUFPLAN_ENDED, in->now_ms);
        }
        break;

    case BUFPLAN_DRAINING:
        /* No rebuffer from here: there is nothing to refill from, and
         * stopping to wait would be a pause before the end of a track
         * that is already over. */
        if (buffered <= 0) bufplan_enter(b, BUFPLAN_ENDED, in->now_ms);
        break;

    default:
        bufplan_enter(b, BUFPLAN_ENDED, in->now_ms);
        break;
    }

    switch (b->phase) {
    case BUFPLAN_PLAYING:
    case BUFPLAN_DRAINING:
        out->audible = true;
        break;
    case BUFPLAN_PREROLL:
    case BUFPLAN_REBUFFERING:
        out->show_buffering = true;
        break;
    case BUFPLAN_ENDED:
    default:
        out->finished = true;
        break;
    }
}

#ifdef __cplusplus
}
#endif
