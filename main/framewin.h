/*
 * framewin.h -- a window of bytes over a stream, for a decoder that
 * needs whole frames, with nothing to run.
 *
 * Phase 1 puts compressed bytes in a ring. A decoder cannot use a ring
 * directly: `mp3dec_decode_frame()` and `esp_audio_simple_dec_process()`
 * both want a contiguous buffer, both report how much of it they
 * consumed, and both may consume nothing because the buffer does not yet
 * hold a whole frame. So between the ring and the decoder there is a
 * window that is refilled, partially consumed, and slid along -- and
 * that bookkeeping is where a decode loop goes wrong.
 *
 * The failure it exists to prevent is not a crash. A window that loses a
 * few bytes at each refill boundary desynchronises the decoder, which
 * then resynchronises on the next frame header and carries on. The
 * result is a click every few seconds and a stream that otherwise works
 * perfectly. On a live stream, with no file to compare against and no
 * seek bar to re-run a passage, that is close to undiagnosable from the
 * outside -- it would present as "internet radio is a bit crackly".
 *
 * THE INVARIANT
 *
 * **Every byte written in comes out exactly once, in order.** The test
 * asserts it directly, across random chunk sizes and random consume
 * amounts: the concatenation of everything the decoder was shown and
 * consumed equals the byte stream that went in.
 *
 * THE WINDOW MUST BE BIGGER THAN THE LARGEST FRAME
 *
 * If it is not, a frame can arrive that never fits, the decoder consumes
 * nothing every time, the window stays full, and the loop spins forever
 * making no progress. That is a hang, not a glitch. The constant below
 * is sized against the worst case of both backends and asserted at
 * compile time; `framewin_stuck()` is the runtime answer for the case
 * where the stream is simply not what it claimed to be.
 *
 *   MPEG1 Layer II, 384 kbit/s, 32 kHz, padded   1441 bytes
 *   ADTS, 13-bit frame length field              8191 bytes
 *
 * So 8191 is the real bound, and a window has to hold one whole frame
 * plus whatever partial frame precedes it -- twice that is 16382. A
 * 16 KB window would satisfy that by two bytes, which is luck rather
 * than design, so this is 24 KB: two maximum frames and half of a third.
 * In PSRAM that is nothing; a hang is not.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAMEWIN_MAX_FRAME  (8191)          /* ADTS, 13-bit length field */
#define FRAMEWIN_BYTES      (24 * 1024)

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;       /* bytes valid in buf */
    size_t   pos;       /* bytes of those already consumed */
    /* Refills since the last time the decoder consumed anything. A
     * decoder that keeps asking for more without ever taking any is
     * either being starved of a whole frame or looking at something that
     * is not frames at all, and those need telling apart. */
    unsigned dry;
    uint64_t in, out;   /* totals, for the invariant and the log */
} framewin_t;

static inline void framewin_init(framewin_t *w, uint8_t *buf, size_t cap)
{
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
}

/* Unconsumed bytes, and where they start. What the decoder is shown. */
static inline size_t framewin_avail(const framewin_t *w)
{
    return w ? w->len - w->pos : 0;
}

static inline const uint8_t *framewin_data(const framewin_t *w)
{
    return (w && w->buf) ? w->buf + w->pos : NULL;
}

/*
 * The decoder took `n` bytes. Consuming more than is there is refused
 * rather than clamped: a decoder reporting that is a decoder whose
 * return value has been misread, and silently believing it would slide
 * the window past bytes that were never shown to anything.
 */
static inline bool framewin_consume(framewin_t *w, size_t n)
{
    if (!w) return false;
    if (n > framewin_avail(w)) return false;
    w->pos += n;
    w->out += n;
    if (n) w->dry = 0;
    return true;
}

/*
 * Slide the unconsumed tail to the front and report the room left at the
 * end. Called before every refill; cheap when the window is empty, which
 * is the common case once a frame has been consumed.
 */
static inline size_t framewin_space(framewin_t *w)
{
    if (!w || !w->buf) return 0;
    const size_t keep = framewin_avail(w);
    if (w->pos) {
        /* memmove, not memcpy: the regions overlap whenever less than
         * half the window was consumed, which is most of the time. */
        if (keep) memmove(w->buf, w->buf + w->pos, keep);
        w->len = keep;
        w->pos = 0;
    }
    return w->cap - w->len;
}

/* Where a refill should write, after framewin_space(). */
static inline uint8_t *framewin_tail(framewin_t *w)
{
    return (w && w->buf) ? w->buf + w->len : NULL;
}

/*
 * `n` bytes were written at framewin_tail(). Refuses an overrun rather
 * than accepting it, for the same reason framewin_consume() does.
 */
static inline bool framewin_commit(framewin_t *w, size_t n)
{
    if (!w || w->len + n > w->cap) return false;
    w->len += n;
    w->in += n;
    w->dry++;
    return true;
}

/*
 * Is this loop making progress?
 *
 * True when the window is full and the decoder has taken nothing across
 * several refills -- the shape of a stream that is not what it claimed
 * to be, or a frame longer than the window. The caller's answer is to
 * discard a byte and let the decoder resynchronise, or to give up on the
 * stream; either is better than spinning.
 *
 * The threshold is deliberately not 1. A decoder legitimately consumes
 * nothing while a frame is still arriving, and on a 64 kbit/s station
 * that is most refills at the start.
 */
#define FRAMEWIN_DRY_LIMIT  (8)

static inline bool framewin_stuck(const framewin_t *w)
{
    return w && w->dry >= FRAMEWIN_DRY_LIMIT && w->len == w->cap;
}

/*
 * Drop one byte from the front, for the stuck case: it is what lets a
 * decoder that is staring at a false sync find the real one. Returns
 * false when there is nothing to drop.
 */
static inline bool framewin_skip_byte(framewin_t *w)
{
    if (!w || framewin_avail(w) == 0) return false;
    w->pos++;
    w->out++;
    w->dry = 0;
    return true;
}

/* Everything shown to the decoder has been consumed. */
static inline bool framewin_empty(const framewin_t *w)
{
    return framewin_avail(w) == 0;
}

/*
 * Forget the contents without forgetting the totals. For a reconnect:
 * the bytes in the window belong to a body that has ended, and the next
 * one starts at its own frame boundary.
 */
static inline void framewin_reset(framewin_t *w)
{
    if (!w) return;
    w->len = 0;
    w->pos = 0;
    w->dry = 0;
}

#ifdef __cplusplus
}
#endif
