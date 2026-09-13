/*
 * levelhist.h -- a minute of output level, bucketed into columns, with
 * the cached-but-unheard part measured in the same units.
 *
 * WHAT THIS IS FOR
 *
 * The bar can say "Buffering" and it can say how many seconds are in
 * hand, and neither tells anyone whether the reserve is healthy or on
 * its way to zero. Eleven board runs and a PC comparison went into
 * working out that WNZK sawtooths from 4.1s to 1.4s every thirty-five
 * seconds, and every one of them needed a log. A minute of history on
 * screen would have shown it at a glance.
 *
 * So: a strip where the left is a minute ago, a mark is now, and the
 * grey to the right of the mark is audio that has arrived and not been
 * heard yet. Level behind, reserve ahead, one timeline, same scale.
 * A reserve that shrinks run by run is then a shape rather than a
 * column of numbers.
 *
 * NO DRAWING HERE, AND NO FREERTOS
 *
 * The same split stationlist.h, netplan.h, streamplan.h and bufferplan.h
 * use, for the same reason: this is arithmetic over time and a ring, a
 * host can test all of it, and none of it needs a panel. `ui.c` will
 * turn columns into rectangles in a later patch and will have no
 * opinions about time.
 *
 * WHY PEAK AND NOT RMS
 *
 * The writer already computes a peak per track for the `output peak`
 * log line, so peak is free and RMS is a square root per bucket. More
 * to the point, peak is the right answer for this display: the question
 * a listener asks of a level strip is "was it playing", and a peak
 * answers that at any bucket size, while an RMS over 250 ms of a quiet
 * passage can read as silence. This is not a meter for setting gain.
 *
 * WHY ONE BYTE A COLUMN
 *
 * 240 columns is 240 bytes, which fits anywhere and can be published to
 * ui_task by copy. int16 sample peaks are shifted down to 0..255. The
 * bar is 72 px tall, so a byte is already more resolution than the
 * strip can draw, and a 16-bit column would double the copy for nothing
 * visible.
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

/*
 * A minute, in 250 ms columns.
 *
 * 240 columns against a strip about 700 px wide is just under 3 px a
 * column, which is a sensible minimum for something drawn as a filled
 * rectangle -- below about 2 px a column the gaps matter more than the
 * heights.
 *
 * 250 ms is also comfortably longer than the writer's chunk, so a column
 * is always the peak of several chunks rather than a single one sampled
 * at an arbitrary phase.
 */
#define LEVELHIST_COLUMNS    (240)
#define LEVELHIST_SPAN_MS    (60000)
#define LEVELHIST_BUCKET_MS  (LEVELHIST_SPAN_MS / LEVELHIST_COLUMNS)  /* 250 */

/*
 * Where "now" sits in the strip.
 *
 * Two thirds along, leaving 40 s of history and 20 s of room for the
 * reserve. Not centred: the history is the part that is always there,
 * and a reserve is usually a few seconds -- SomaFM holds about 21 s on a
 * good link, WNZK under 4 s -- so giving the future half the strip would
 * leave most of it empty most of the time.
 *
 * 20 s of headroom does clip: WUOM's front-loaded burst reached 50 s on
 * a PC. Clipping is the right failure -- the difference between 20 s and
 * 50 s of reserve does not change any decision, while the difference
 * between 4 s and 1 s does, and that end is drawn at full resolution.
 */
#define LEVELHIST_NOW_COLUMN (LEVELHIST_COLUMNS * 2 / 3)     /* 160 */
#define LEVELHIST_AHEAD_COLUMNS (LEVELHIST_COLUMNS - LEVELHIST_NOW_COLUMN)

/*
 * WHERE THE NEWEST COLUMN IS, WHICH IS NOT THE END OF THE ARRAY.
 *
 * levelhist_read() returns the whole ring -- a full minute, 240 columns,
 * oldest first -- so the NEWEST sample is out[LEVELHIST_COLUMNS - 1].
 * But the mark sits at LEVELHIST_NOW_COLUMN, 160, with the 80 columns
 * past it reserved for the buffer. A drawing that took out[0..159] as
 * the history would be showing the OLDEST forty seconds of the minute
 * and putting the most recent twenty seconds off the end of the strip,
 * behind the reserve.
 *
 * Which is what 0332 did, and on the board it looked like this: the red
 * stayed empty for the first twenty seconds, because out[0..159] was
 * still the zeroed part of the ring, and then crept in from the right.
 * The grey filled normally the whole time, so the two looked like they
 * were taking turns.
 *
 * So the history to draw is the LAST LEVELHIST_NOW_COLUMN columns of
 * the read, and this is where that offset lives rather than as an
 * expression in a draw loop -- it is a property of how the ring and the
 * strip line up, and getting it wrong is invisible until somebody
 * watches the screen for a minute.
 *
 * The oldest 20 s of the ring is therefore never drawn. Keeping a full
 * minute and showing 40 s of it is deliberate: LEVELHIST_NOW_COLUMN is
 * the one number here most likely to be revised after somebody looks at
 * the panel, and a ring that already holds the wider span means moving
 * the mark is a one-line change rather than a resize.
 */
#define LEVELHIST_HISTORY_OFFSET \
    (LEVELHIST_COLUMNS - LEVELHIST_NOW_COLUMN)      /* 80 */

typedef struct {
    uint8_t  col[LEVELHIST_COLUMNS]; /* ring; `head` is the NEXT to write */
    int      head;
    uint8_t  pending;                /* peak of the bucket being filled */
    int      pending_ms;             /* how much of it has elapsed */
    bool     wrapped;                /* has the ring filled at least once */
    int      filled;                 /* columns written, capped at COLUMNS */
} levelhist_t;

static inline void levelhist_reset(levelhist_t *h)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
}

/*
 * Fold a block of audio in.
 *
 * `peak` is the largest absolute sample in the block, 0..32767, and
 * `ms` is how long the block will take to play -- frames divided by the
 * OUTPUT rate, not the stream's, for the reason fade_out_begin() gives:
 * the strip is a picture of what the hardware clocked out.
 *
 * A block longer than one bucket closes several. That is not the normal
 * path -- the writer's chunk is well under 250 ms -- but a resume after
 * a pause can hand over a large block at once, and dropping the extra
 * columns would make the strip narrower than the time it claims to
 * cover. The whole block gets the same peak, which is the best that can
 * be said about it without keeping the samples.
 */
static inline void levelhist_push(levelhist_t *h, int peak, int ms)
{
    if (!h || ms <= 0) return;

    if (peak < 0) peak = 0;
    if (peak > 32767) peak = 32767;
    const uint8_t p8 = (uint8_t)(peak >> 7);      /* 32767 -> 255 */

    if (p8 > h->pending) h->pending = p8;
    h->pending_ms += ms;

    while (h->pending_ms >= LEVELHIST_BUCKET_MS) {
        h->col[h->head] = h->pending;
        h->head = (h->head + 1) % LEVELHIST_COLUMNS;
        if (h->filled < LEVELHIST_COLUMNS) h->filled++;
        else h->wrapped = true;
        h->pending_ms -= LEVELHIST_BUCKET_MS;
        /* The peak carries into the next column only if this block is
         * still spilling into it. A block spanning three buckets really
         * was that loud for all three. */
        h->pending = (h->pending_ms > 0) ? p8 : 0;
    }
}

/*
 * Silence, for a stretch when nothing is being written.
 *
 * A rebuffer is not a gap in the record: it is a stretch during which
 * the level was zero, and it has to occupy its own width on the strip or
 * the history compresses and the dropouts vanish. That is the single
 * most important thing this display has to show, so it gets its own call
 * rather than relying on the caller remembering to push zeros.
 */
static inline void levelhist_silence(levelhist_t *h, int ms)
{
    levelhist_push(h, 0, ms);
}

/*
 * Read the strip out, oldest first, into `out[LEVELHIST_COLUMNS]`.
 *
 * Always the full width. Before a minute has elapsed the unfilled part
 * is zero, which draws as silence -- correct, because before the player
 * started there WAS silence, and a strip that grew from the left would
 * move the "now" mark for the first minute of every station.
 */
static inline void levelhist_read(const levelhist_t *h, uint8_t *out)
{
    if (!out) return;
    memset(out, 0, LEVELHIST_COLUMNS);
    if (!h) return;

    /* `head` is the next slot to write, so it is also the oldest. Before
     * the ring has filled, the oldest is slot 0 and the newest is
     * head-1; the copy below is right either way because the unwritten
     * slots are zero. */
    for (int i = 0; i < LEVELHIST_COLUMNS; i++) {
        out[i] = h->col[(h->head + i) % LEVELHIST_COLUMNS];
    }
}

/*
 * How many columns of reserve to draw ahead of the mark.
 *
 * `buffered_ms` is bufplan's figure -- the same one netstream's
 * statistics line reports as `audio Xs` -- so the grey ahead of the mark
 * and the number in the log are the same quantity, and a screenshot can
 * be checked against a log.
 *
 * Rounded UP, and the reason is the one case that matters: anything
 * above zero must draw at least one column. A reserve of 200 ms is the
 * difference between playing and a dropout, and rounding it to no
 * columns would draw the healthiest possible picture at the worst
 * possible moment.
 */
static inline int levelhist_ahead_columns(int buffered_ms)
{
    if (buffered_ms <= 0) return 0;
    int cols = (buffered_ms + LEVELHIST_BUCKET_MS - 1) / LEVELHIST_BUCKET_MS;
    if (cols > LEVELHIST_AHEAD_COLUMNS) cols = LEVELHIST_AHEAD_COLUMNS;
    return cols;
}

/*
 * How many measured blocks fold into one column of the strip.
 *
 * Here rather than at the call site because it is a property of the two
 * time bases meeting -- a 250 ms column and a 100 ms measurement block
 * -- and a drawing loop that inlined `250 / 100` would silently draw
 * the wrong span the day either constant moved.
 *
 * Rounded UP, so a column is never built from less audio than it
 * claims to cover. The bias is toward showing a loud column slightly
 * early, which on a reserve display is the safe direction: the
 * question the strip answers is "what is coming", and being a quarter
 * second eager about a peak is better than missing it.
 */
static inline int levelhist_blocks_per_column(int block_ms)
{
    if (block_ms <= 0) return 1;
    const int n = (LEVELHIST_BUCKET_MS + block_ms - 1) / block_ms;
    return n < 1 ? 1 : n;
}

/*
 * An int16 sample peak as a column byte.
 *
 * The one conversion between the units the writer measures in and the
 * units the strip stores, in one place so the history and the reserve
 * cannot disagree about it. levelhist_push() does the same shift on the
 * way in; this is for callers building the ahead columns themselves,
 * which cannot go through push() because that would put future audio
 * into the history ring.
 *
 * Anything above zero returns at least 1, for the reason the column
 * height clamp in ui.c exists: quiet is not silent, and the difference
 * is the difference between playing and not.
 */
static inline uint8_t levelhist_peak_to_column(int peak)
{
    if (peak <= 0) return 0;
    int v = peak >> 7;                  /* 0..32767 -> 0..255 */
    if (v > 255) v = 255;
    if (v < 1) v = 1;
    return (uint8_t)v;
}

/*
 * Is the reserve being clipped by the strip's width?
 *
 * So the drawing can mark it rather than silently lying. A strip that
 * shows 20 s when there are 50 s is not wrong about the part it draws,
 * but it is wrong about the edge, and the edge is exactly where a
 * listener looks to see whether the reserve is still growing.
 */
static inline bool levelhist_ahead_clipped(int buffered_ms)
{
    return levelhist_ahead_columns(buffered_ms) >= LEVELHIST_AHEAD_COLUMNS &&
           buffered_ms > LEVELHIST_AHEAD_COLUMNS * LEVELHIST_BUCKET_MS;
}

#ifdef __cplusplus
}
#endif
