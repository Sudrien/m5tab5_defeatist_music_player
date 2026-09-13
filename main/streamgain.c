/*
 * streamgain.c -- see streamgain.h.
 *
 * The two functions the header declared and did not define. Everything
 * else about the carrier is inline up there, because it is bookkeeping
 * the compiler should fold into the caller's loop; these two are
 * arithmetic that runs once per chunk and has no business being
 * inlined into a writer's hot path.
 *
 * NOTHING HERE LOCKS, BLOCKS OR ALLOCATES. Both are called from the
 * writer task -- see the ownership split in the header -- and both read
 * records the decode loop has already finished writing.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>

#include "streamgain.h"

/*
 * Mean square -> LUFS, with the standard's -0.691 offset.
 *
 * The same expression loudness.c's ms_to_lufs() uses, and a second copy
 * of it, which wants justifying rather than glossing. It is three terms
 * of the standard rather than a policy: the offset is published, the
 * scale factor is published, and neither can drift without BS.1770
 * changing under both files at once. What must NOT be copied is the
 * gate, which is a decision about which blocks count -- and it is not:
 * loudness.c gates over a histogram of a whole track and this gates
 * over a window of at most 256 blocks, for the reason the header gives.
 */
static float ms_to_lufs(double ms)
{
    if (ms <= 0.0) return -HUGE_VALF;
    return (float)(-0.691 + 10.0 * log10(ms));
}

bool streamgain_window_db(const streamgain_t *g, float *out_db)
{
    if (!g || !out_db) return false;

    const int n = streamgain_count(g);
    if (n <= 0) return false;

    /*
     * Absolute gate, at -70 LUFS. A block below it is silence as far as
     * the standard is concerned and takes no part in the answer -- but
     * it was still pushed, and still counted toward the window length,
     * because the carrier has to stay in step with audio that is going
     * to be played whether it is silent or not.
     */
    double abs_sum = 0.0;
    int abs_n = 0;
    const double abs_gate_ms = pow(10.0, (-70.0 + 0.691) / 10.0);

    const uint32_t tail = g->tail;
    for (int i = 0; i < n; i++) {
        const double ms = (double)g->rec[(tail + (uint32_t)i)
                                         & (STREAMGAIN_RECORDS - 1)].msq;
        if (!(ms > abs_gate_ms)) continue;
        abs_sum += ms;
        abs_n++;
    }
    if (!abs_n) return false;       /* the window is silence; hold */

    /*
     * Relative gate, 10 LU below the mean of what survived the absolute
     * one. Over a window this usually discards nothing, which the
     * header says at length and which is not a reason to skip it: the
     * case it does catch -- a genuine silence inside an otherwise loud
     * twenty seconds, which is every ad break's leading edge -- is
     * exactly the case that would otherwise drag the gain up just
     * before the audio that does not need it arrives.
     */
    const double abs_mean = abs_sum / (double)abs_n;
    const double rel_gate_ms =
        pow(10.0, ((double)(ms_to_lufs(abs_mean) - 10.0f) + 0.691) / 10.0);

    double sum = 0.0;
    int used = 0;
    for (int i = 0; i < n; i++) {
        const double ms = (double)g->rec[(tail + (uint32_t)i)
                                         & (STREAMGAIN_RECORDS - 1)].msq;
        if (!(ms > abs_gate_ms)) continue;
        if (!(ms >= rel_gate_ms)) continue;
        sum += ms;
        used++;
    }

    /* The relative gate cannot empty a set its own threshold was
     * computed from -- the mean of a set has members at or above it --
     * but a NaN in a record could, and a NaN reaching the multiplier is
     * silence in both channels for as long as it is applied. */
    if (!used) { sum = abs_sum; used = abs_n; }

    const float lufs = ms_to_lufs(sum / (double)used);
    if (!isfinite(lufs)) return false;

    *out_db = STREAMGAIN_TARGET_LUFS - lufs;
    return true;
}

void streamgain_step(streamgain_t *g, int elapsed_ms)
{
    if (!g || elapsed_ms <= 0) return;

    /*
     * A thin window holds rather than measures.
     *
     * WNZK has been observed running with a 1.4 s reserve for minutes,
     * and a 1.4 s window is not a loudness measurement -- gating it is
     * meaningless and the number it produces moves with the programme
     * rather than with the station. Holding is what the header calls
     * for and it costs nothing: the last confident answer was about
     * this station a few seconds ago.
     *
     * Before any of the arithmetic, because an unconfident window
     * should not even be asked.
     */
    float want;
    if (!streamgain_confident(g)) return;
    if (!streamgain_window_db(g, &want)) return;

    /*
     * The peak clamp, which is what makes the boost bound real.
     *
     * +6 dB is allowed and +6 dB on a window whose peak is already
     * -3 dBFS is not, so the smaller of the two wins. The peak is the
     * loudest in the whole window rather than the next block's: the
     * gain being chosen now will still be close to this one when that
     * peak is reached, since the slew is a decibel a second, and a
     * clamp computed against a quiet block that a loud one follows is a
     * clamp that does not hold.
     */
    int peak = 0;
    const int n = streamgain_count(g);
    const uint32_t tail = g->tail;
    for (int i = 0; i < n; i++) {
        const int p = g->rec[(tail + (uint32_t)i)
                             & (STREAMGAIN_RECORDS - 1)].peak;
        if (p > peak) peak = p;
    }

    float ceiling = STREAMGAIN_MAX_BOOST_DB;
    if (peak > 0) {
        /* Headroom to full scale, less the same 1 dB of margin the file
         * path keeps, so a boost cannot land a sample on the rail. */
        const float head_db = 20.0f * log10f(32767.0f / (float)peak) - 1.0f;
        if (head_db < ceiling) ceiling = head_db;
    }
    if (ceiling < 0.0f) ceiling = 0.0f;     /* never a clamp into a cut */

    if (want > ceiling) want = ceiling;
    if (want < -STREAMGAIN_MAX_CUT_DB) want = -STREAMGAIN_MAX_CUT_DB;

    /*
     * And the slew, which the header calls the whole design and which
     * still has no measurement behind it. Nothing here chooses it; this
     * only enforces it.
     *
     * The first confident answer is taken whole rather than slewed up
     * to from 0 dB. Slewing there would mean every station starts at
     * the wrong level and walks to the right one over several seconds,
     * audibly, on exactly the audio a listener is paying most attention
     * to -- and unlike every later move, there is no previous answer
     * being departed from, so there is nothing for the ramp to protect.
     */
    if (!g->have_gain) {
        g->gain_db = want;
        g->have_gain = true;
        return;
    }

    const float limit = STREAMGAIN_SLEW_DB_S * (float)elapsed_ms / 1000.0f;
    const float delta = want - g->gain_db;
    if (delta > limit)       g->gain_db += limit;
    else if (delta < -limit) g->gain_db -= limit;
    else                     g->gain_db = want;
}
