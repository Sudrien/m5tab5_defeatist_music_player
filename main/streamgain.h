/*
 * streamgain.h -- slow loudness levelling for a live stream.
 *
 * NOT REPLAYGAIN, AND THE NAME MATTERS. An earlier draft of this file
 * called it "ReplayGain for something that never ends", which is wrong
 * in a way that would have set the wrong expectation for everything
 * built on top of it. See "WHAT THIS ACTUALLY IS" below, which is the
 * first thing to read.
 *
 * NO LONGER A DRAFT as of 0400: streamgain.c implements the two
 * functions this file used to declare and not define, player.c pushes
 * from the stream decode loop and applies from the writer, and
 * texttest/streamgaintest.c is the host test. The reasoning below is
 * unchanged and was written before any of it, which is the point of
 * having written it down first.
 *
 * THE PROBLEM
 *
 * loudness.c already measures BS.1770-4 properly -- K-weighting, 400 ms
 * blocks at 75% overlap, two-stage gating -- off the decoded PCM on its
 * way to the speaker. That is the real thing and it is not the hard
 * part.
 *
 * The hard part is that ReplayGain for a FILE works by cheating on
 * time: scan the whole thing, decide one number, then go back to the
 * start and apply it. loudness_finish() is that cheat. A stream has no
 * finish. The gain has to be chosen while the audio is playing, and the
 * obvious ways of doing that are all bad:
 *
 *   - A running average is an AGC. It pumps on anything with dynamics,
 *     and BS.1770's gating -- the thing that stops a quiet intro
 *     dragging the answer down -- does not work on a window that keeps
 *     moving underneath it.
 *   - Per-track, using the ICY title as a boundary, works on exactly
 *     one of the three stations this project has ever tested. SomaFM
 *     sends titles, WUOM has never sent one, WNZK sends " - " forever.
 *   - Measure once at connect and hold it: honest, simple, and wrong
 *     across the first ad break.
 *
 * THE THING THIS PLAYER HAS THAT A PC PLAYER DOES NOT
 *
 * PCM_RING_BYTES is about twenty seconds. The decode loop runs that far
 * ahead of the speaker, and on SomaFM the board has been observed
 * holding `audio 17.6s` steadily for minutes. **So the audio that is
 * about to be heard has already been decoded, and can already have been
 * measured.**
 *
 * That turns the problem inside out. Instead of guessing a gain from
 * the past and applying it to the future, a block can be measured when
 * it is decoded and have ITS OWN measurement applied when it plays,
 * with a window of real blocks either side of it to gate against. That
 * is not an AGC with a nicer name: the gain applied to a moment of
 * audio is derived from a window centred on that moment, which is what
 * ReplayGain does for a file, at a smaller scale.
 *
 * The cost is that it needs a carrier: a measurement taken in the
 * decode loop has to reach the writer, twenty seconds later, still
 * lined up with the samples it describes.
 *
 * WHAT THIS ACTUALLY IS
 *
 * A gated mean over the blocks in a twenty-second window. Which is to
 * say: **yes, essentially a twenty-second average**, with BS.1770's
 * gating and weighting on top, and the gating does much less work here
 * than it does for a file.
 *
 * That is worth being blunt about, because the difference from
 * ReplayGain is not a detail:
 *
 *   - Over a five-minute track, the relative gate throws away the quiet
 *     intro so the loud body sets the number. That is the whole point
 *     of gating and it is why ReplayGain is not an average.
 *   - Over twenty seconds, the window can be ENTIRELY quiet intro.
 *     There is nothing louder in it to gate against, so the relative
 *     gate has almost nothing to discard and the answer degrades toward
 *     a plain windowed mean.
 *
 * So the gain WILL move within a track: a quiet verse lifts, a loud
 * chorus cuts. That is dynamic range compression with a twenty-second
 * time constant, and it is precisely what ReplayGain exists not to do.
 *
 * AND THERE IS NO WAY AROUND IT. Per-track ReplayGain needs the whole
 * track before the first sample plays. Twenty seconds of lookahead
 * gives that only for tracks shorter than twenty seconds. The ICY title
 * marks a boundary on SomaFM and never on WUOM or WNZK, and even with
 * a boundary the measurement still finishes after the audio it
 * describes has been heard. Live radio cannot have per-track gain. It
 * can have levelling, and this is levelling.
 *
 * WHICH MAKES THE SLEW RATE THE WHOLE DESIGN. Slow enough and the gain
 * effectively only moves at track and ad boundaries, where stations
 * change level anyway, and within-track pumping stays below notice.
 * Too slow and an ad break is over before it responds. Everything else
 * in this file is bookkeeping in service of choosing one number, and
 * that number cannot be chosen from a header.
 *
 * The honest comparison is not ReplayGain. It is what broadcast does --
 * R128 with a slow follower -- and it should be judged as that.
 *
 * WHAT A CARRIER HAS TO SURVIVE
 *
 * FreeRTOS stream buffers cannot be peeked. There is no read that does
 * not consume, and the writer may be blocked inside
 * xStreamBufferReceive() on the same buffer -- reaching into its
 * storage behind its back is precisely what 0507 spent two patches
 * learning not to do. So the records travel in their own ring beside
 * the PCM, and they are kept in step by counting bytes rather than by
 * assuming the two rings advance together.
 *
 * Byte accounting, not index accounting, because the writer's chunk
 * size and the decoder's block size have no relationship: a chunk can
 * straddle two blocks and a block can span several chunks. Every record
 * carries how many PCM bytes it describes, and the writer draws down
 * that count as it consumes. This is the part most likely to be got
 * wrong and it is the part a host can test completely.
 *
 * NOT A BYTE OF PEAK. The level strip's carrier would be one byte per
 * column and that is the wrong record for this, so the two share this
 * one rather than running in parallel -- a second queue alongside the
 * first is the outcome to avoid, and it is obvious now and invisible in
 * three months. A record holds the block's mean square and its peak,
 * which is what gating and clip protection need, and the strip can take
 * the peak out of the same record.
 *
 * WHAT IS STILL OPEN, AND SHOULD BE SETTLED BEFORE ANY OF THIS IS
 * WRITTEN
 *
 *  1. STREAMGAIN_SLEW_DB_S. It is the only thing standing between
 *     "levelling" and "compression", there is no measurement anywhere
 *     in this project that says what it should be, and no amount of
 *     further reasoning in this file will produce one. It needs a board
 *     and ears, on a station with real dynamics -- which of the three
 *     tested so far means SomaFM, since WUOM is talk and WNZK has been
 *     " - " for its entire recorded history.
 *  2. The window is bounded by the ring, so it shrinks when the reserve
 *     does. WNZK has been observed at 1.4 s. A 1.4 s window is not a
 *     loudness measurement and gating it is meaningless -- see
 *     streamgain_confident() -- so the behaviour when the reserve is
 *     thin has to be "hold the last good gain", not "measure harder".
 *  3. Whether this applies to files too. It should not: files have
 *     loudness_finish() and a sidecar, which is strictly better because
 *     it sees the whole track. Two gain paths on one writer is a way to
 *     apply both.
 *
 * NOTHING HERE DECODES, LOCKS OR DRAWS. Pure arithmetic over a ring of
 * records, testable on a host, in the same split as bufferplan.h.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many block records the carrier holds.
 *
 * A block every 100 ms (400 ms windows at 75% overlap, as BS.1770 and
 * loudness.c use), and the PCM ring is about 20 s, so 200 records
 * covers a full ring. 256 rounds up and leaves room for the decode loop
 * to run ahead of its own accounting by a few blocks without the ring
 * being the thing that stops it.
 *
 * Each record is 12 bytes, so this is 3 KB. Internal RAM is the scarce
 * thing on this board -- the AAC decoder costs 14860 bytes of it -- so
 * this belongs in PSRAM beside the ring it describes, not beside the
 * decoder.
 */
#define STREAMGAIN_RECORDS      (256)
#define STREAMGAIN_BLOCK_MS     (100)

/*
 * The reference, matching ReplayGain 2.0 and what rg.c uses for files,
 * so a station and a file sound the same loudness. Any other value here
 * would make the radio quieter or louder than the card for no reason
 * the listener could discover.
 */
#define STREAMGAIN_TARGET_LUFS  (-18.0f)

/*
 * Bounds on the gain. Not symmetric, and deliberately.
 *
 * Boosting a quiet station is where the risk is: +12 dB on something
 * already near full scale clips, and a limiter is a different project.
 * Cutting a loud one is safe, so the cut is allowed to go further.
 */
#define STREAMGAIN_MAX_BOOST_DB (6.0f)
#define STREAMGAIN_MAX_CUT_DB   (15.0f)

/*
 * How fast the gain may move, in dB per second.
 *
 * THE KNOB WITH NO MEASUREMENT BEHIND IT. 1.0 dB/s means a 6 dB
 * correction takes six seconds, which is slow enough not to be heard as
 * pumping and fast enough to catch an ad break within a sentence. That
 * is reasoning, not evidence, and it is the first thing to change once
 * somebody has listened.
 */
#define STREAMGAIN_SLEW_DB_S    (1.0f)

/*
 * The least measured audio that makes a gain worth believing.
 *
 * Three seconds. BS.1770's relative gate compares blocks against the
 * mean of the blocks that passed the absolute gate, and with a handful
 * of blocks that mean is one loud passage. Below this the answer is
 * arithmetic rather than a measurement, and the caller holds the last
 * good gain instead -- which is why streamgain_confident() exists as a
 * separate question from streamgain_db().
 *
 * WNZK has been observed running with a 1.4 s reserve for minutes at a
 * time. That station would spend most of its life below this line, and
 * holding a gain is the right answer there.
 */
#define STREAMGAIN_MIN_WINDOW_MS (3000)

/*
 * One block's worth of measurement, as the decode loop produces it.
 *
 * `msq` is the K-weighted mean square from loudness.c's filters -- the
 * quantity its gating works on -- and not a level in dB, because gating
 * sums and compares mean squares and converting to dB and back at every
 * step loses the ability to do that.
 *
 * `peak` is the sample peak, 0..32767, so the gain can be clamped
 * against clipping without a second pass, and so the level strip can
 * draw the reserve from the same record rather than needing its own.
 *
 * `bytes` is how much PCM this block describes. It is what keeps the
 * carrier in step with a ring that neither side indexes.
 */
typedef struct {
    float    msq;
    uint16_t peak;
    uint16_t bytes_hi;      /* bytes >> 16, so the pair packs into 12 */
    uint16_t bytes_lo;
} streamgain_rec_t;

/*
 * The carrier.
 *
 * ONE PRODUCER AND ONE CONSUMER, AND THE FIELDS ARE SPLIT BETWEEN THEM.
 * The draft had `count` and `queued_bytes` written from both ends, which
 * on two tasks is a lost update rather than a race that merely reorders
 * -- `count++` on the decode loop against `count--` on the writer loses
 * one of them, and the carrier then drifts against the audio for the
 * rest of the session with nothing to say it happened. There is no lock
 * here and there should not be one: the writer must not wait on the
 * decode loop for a bookkeeping ring.
 *
 * So every field has exactly one writer:
 *
 *   decode loop  head, pushed_bytes, dropped
 *   writer       tail, tail_bytes_left, played_bytes, gain_db, have_gain
 *
 * `head` and `tail` are free-running and masked on use, so the count is
 * their difference and neither side has to write a shared total.
 * STREAMGAIN_RECORDS is a power of two for that reason. Unsigned
 * subtraction is defined on wrap, which is what makes a free-running
 * pair legal rather than merely usual.
 *
 * A record is written before `head` moves and is not touched again, so
 * the reader either does not see it yet or sees it whole.
 */
typedef struct {
    streamgain_rec_t rec[STREAMGAIN_RECORDS];

    volatile uint32_t head;             /* producer */
    volatile uint32_t pushed_bytes;
    volatile uint32_t dropped;          /* records the carrier could not take */

    volatile uint32_t tail;             /* consumer */
    uint32_t tail_bytes_left;           /* of rec[tail], still unplayed */
    volatile uint32_t played_bytes;

    float    gain_db;                   /* what is being applied now */
    bool     have_gain;                 /* has a confident answer been had */
} streamgain_t;

static inline void streamgain_reset(streamgain_t *g)
{
    if (!g) return;
    memset(g, 0, sizeof(*g));
}

/* How many records are in hand. Safe from either side: each index has
 * one writer and the difference is correct whichever one moved last --
 * it is a snapshot, and a snapshot is all either caller wants. */
static inline int streamgain_count(const streamgain_t *g)
{
    if (!g) return 0;
    const uint32_t n = g->head - g->tail;
    return (int)(n > STREAMGAIN_RECORDS ? STREAMGAIN_RECORDS : n);
}

/* PCM the carrier describes and the writer has not yet played. */
static inline uint32_t streamgain_queued_bytes(const streamgain_t *g)
{
    return g ? (uint32_t)(g->pushed_bytes - g->played_bytes) : 0;
}

static inline uint32_t streamgain_rec_bytes(const streamgain_rec_t *r)
{
    return ((uint32_t)r->bytes_hi << 16) | r->bytes_lo;
}

/*
 * The decode loop hands over one block, with the PCM it describes.
 *
 * Dropped when the carrier is full, and that is correct rather than
 * lazy: full means the decoder is further ahead than twenty seconds,
 * the window is already longer than any measurement needs, and the
 * alternative -- stalling the decode loop on a bookkeeping ring --
 * would make loudness measurement a reason for audio to stop.
 *
 * The bytes still count. A dropped record's PCM is in the ring and will
 * be played, so `pushed_bytes` has to include it or the carrier drifts
 * behind the audio by exactly what was dropped. That is the failure
 * this signature exists to make impossible to write by accident.
 */
static inline bool streamgain_push(streamgain_t *g, float msq, int peak,
                                   uint32_t bytes)
{
    if (!g || bytes == 0) return false;
    g->pushed_bytes += bytes;

    if (streamgain_count(g) >= STREAMGAIN_RECORDS) { g->dropped++; return false; }

    if (peak < 0) peak = 0;
    if (peak > 32767) peak = 32767;

    streamgain_rec_t *r = &g->rec[g->head & (STREAMGAIN_RECORDS - 1)];
    r->msq      = msq;
    r->peak     = (uint16_t)peak;
    r->bytes_hi = (uint16_t)(bytes >> 16);
    r->bytes_lo = (uint16_t)(bytes & 0xFFFF);

    /* The record is complete before the reader can see it. */
    g->head++;
    return true;
}

/*
 * The writer has played `bytes` of PCM; retire whatever that covers.
 *
 * Draws down the oldest record first and only retires it when it is
 * fully consumed, because a chunk can straddle two blocks and a block
 * can span several chunks -- the writer's chunk size and the decoder's
 * block size have no relationship at all.
 *
 * `played_bytes` moves by the whole amount even when the carrier has no
 * records left to retire, which is the dropped-record case above: the
 * audio was played, so the accounting has to say so.
 */
static inline void streamgain_consume(streamgain_t *g, uint32_t bytes)
{
    if (!g) return;

    /*
     * Clamped to what has been pushed, and that is not defensive
     * programming -- it is the ordinary case at the start of a stream.
     * The first PCM reaches the ring before the first 400 ms block
     * closes, so the writer legitimately plays audio the carrier never
     * described. Without the clamp `played_bytes` passes `pushed_bytes`
     * and the unsigned difference becomes about four billion, which
     * reads as a window that will never be thin again.
     */
    const uint32_t queued = streamgain_queued_bytes(g);
    if (bytes > queued) bytes = queued;
    g->played_bytes += bytes;

    while (bytes > 0 && streamgain_count(g) > 0) {
        if (g->tail_bytes_left == 0) {
            g->tail_bytes_left =
                streamgain_rec_bytes(&g->rec[g->tail & (STREAMGAIN_RECORDS - 1)]);
            if (g->tail_bytes_left == 0) { g->tail++; continue; }
        }
        if (bytes < g->tail_bytes_left) {
            g->tail_bytes_left -= bytes;
            return;
        }
        bytes -= g->tail_bytes_left;
        g->tail_bytes_left = 0;
        g->tail++;
    }
}

/*
 * Retire everything, because the PCM it described has been discarded.
 *
 * A flush -- a pause, a station change, a rate change -- empties the
 * ring, and records that survived it would be drawn down by the NEXT
 * stream's audio: the carrier would run a whole window behind the
 * sound for as long as the session lasted, which is the one failure
 * mode of a byte-counted carrier that produces no error and no log
 * line.
 *
 * Deliberately NOT streamgain_reset(). `gain_db` is a fact about the
 * station and survives a pause; clearing it would make every resume
 * start at unity and walk back, audibly, to where it already was.
 *
 * Consumer side, like everything else it touches.
 */
static inline void streamgain_drain(streamgain_t *g)
{
    if (!g) return;
    g->tail = g->head;
    g->tail_bytes_left = 0;
    g->played_bytes = g->pushed_bytes;
}

/*
 * How much measured audio is in hand, in milliseconds.
 *
 * From the record count and not from the byte count, because that is
 * what the gate is about: a window is worth gating when it has enough
 * BLOCKS in it, and bytes only say how long the audio is. A carrier
 * that dropped records has fewer blocks than its bytes suggest, and the
 * gate should notice.
 */
static inline int streamgain_window_ms(const streamgain_t *g)
{
    return g ? streamgain_count(g) * STREAMGAIN_BLOCK_MS : 0;
}

static inline bool streamgain_confident(const streamgain_t *g)
{
    return streamgain_window_ms(g) >= STREAMGAIN_MIN_WINDOW_MS;
}

/*
 * The gain the queued window asks for, before slewing and clamping.
 *
 * Two-stage gating over the records, as BS.1770 specifies and
 * loudness.c implements for a file: drop blocks below the absolute
 * gate, take the mean of what is left, drop blocks more than 10 LU
 * below that mean, and report the mean of the survivors.
 *
 * Gating over a window rather than a whole track is the liberty taken
 * here, and the header's opening note says plainly what it costs: over
 * twenty seconds the relative gate often has nothing to discard, so
 * this is closer to a windowed mean than the same code is when
 * loudness.c runs it over a whole file. It is still worth running --
 * it correctly discards a genuine silence in the window, which a plain
 * mean would let drag the answer down -- but it is not doing the work
 * it does for a track, and code that assumes otherwise will be
 * surprised.
 *
 * Over the mean squares directly rather than through a histogram. That
 * is the one place this deliberately does NOT reuse loudness.c's shape:
 * the histogram is there because a track has tens of thousands of
 * blocks and keeping them all would be a megabyte, and this window has
 * at most 256. Bucketing 256 floats to avoid keeping 256 floats would
 * be quantisation bought with nothing.
 *
 * Returns false when there is nothing to say, which is the case the
 * caller must handle by holding rather than by using zero.
 */
bool streamgain_window_db(const streamgain_t *g, float *out_db);

/*
 * Move the applied gain toward the target, at most SLEW dB per second,
 * and clamp it so the loudest peak in the window cannot clip.
 *
 * The peak clamp is what makes the boost bound real rather than
 * decorative: +6 dB is allowed, and +6 dB on a window whose peak is
 * already -3 dBFS is not, so the smaller of the two wins.
 *
 * Called by the consumer, since the elapsed time that bounds a slew is
 * the time the audio took to play and nothing else.
 */
void streamgain_step(streamgain_t *g, int elapsed_ms);

/* What to apply right now. Valid whether or not the window is
 * confident: an unconfident window holds the last good answer, and
 * before there has ever been one this is 0 dB, which is the only honest
 * starting point. */
static inline float streamgain_db(const streamgain_t *g)
{
    return (g && g->have_gain) ? g->gain_db : 0.0f;
}

/*
 * The gain as a 16.16 multiplier, which is what a sample loop wants.
 *
 * Here rather than at the call site so that the one conversion from dB
 * to a multiplier in this program is in the file that owns the dB.
 * Saturating application is the caller's, because only the caller knows
 * the sample width.
 */
static inline int32_t streamgain_q16(float db)
{
    if (db > STREAMGAIN_MAX_BOOST_DB) db = STREAMGAIN_MAX_BOOST_DB;
    if (db < -STREAMGAIN_MAX_CUT_DB)  db = -STREAMGAIN_MAX_CUT_DB;
    /* powf() rather than a table: this runs once per chunk, not once
     * per sample, and a table would be a second place for the curve to
     * live. */
    return (int32_t)(powf(10.0f, db / 20.0f) * 65536.0f + 0.5f);
}

#ifdef __cplusplus
}
#endif
