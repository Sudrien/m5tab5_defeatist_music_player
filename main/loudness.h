/*
 * loudness.h -- ITU-R BS.1770 integrated loudness, measured off the PCM
 * that is already being decoded for the speaker.
 *
 * WHY THIS IS NOT THE FRAME WALK
 *
 * framewalk.c reads global_gain out of each MP3 frame's side info. That
 * is an encoder's quantisation-step choice, and it correlates with how
 * much is going on in a granule because a VBR-ish encoder spends bits
 * where there is something to encode. It is a fine cheap shape for a
 * seek bar and it is NOT loudness: two granules with the same
 * global_gain can sound nothing alike, because the number says how the
 * encoder budgeted and not what the waveform does. Calling it
 * ReplayGain was wrong, and this file is the part that earns the name.
 *
 * Real loudness is a property of the decoded signal, so it needs the
 * decoded signal. There is no header field that shortcuts it.
 *
 * WHY IT IS STILL CHEAP
 *
 * The expensive step -- the decode -- is already happening, once per
 * block, to feed the I2S ring. This taps the same int16 buffer on its
 * way past. What it adds is two biquads and a running sum per sample,
 * which against an MP3 synthesis filterbank is a rounding error. There
 * is no second file read, no second decode and no second task: the
 * accumulator is a local of the decode loop and nobody else touches it,
 * which is the same single-owner rule player.c applies to the ring.
 *
 * WHAT IS MEASURED
 *
 * BS.1770-4 as ReplayGain 2.0 uses it:
 *
 *   1. K-weighting -- a high-shelf then a high-pass, per channel, which
 *      is the standard's model of how a listener weights frequency.
 *   2. Mean square per 400 ms block, 75% overlap (a block every 100 ms).
 *   3. Two-stage gating. An absolute gate drops blocks below -70 LUFS;
 *      a relative gate then drops blocks more than 10 LU below the mean
 *      of what survived. Gating is what stops a track's silence and its
 *      quiet intro dragging the answer down -- it is the difference
 *      between BS.1770 and an average, and it is why this is not a
 *      running RMS with a nicer name.
 *   4. The surviving blocks' mean square becomes LUFS.
 *
 * Channel weights are the standard's: 1.0 for L and R. Mono is measured
 * as a single channel at weight 1.0 rather than being duplicated into
 * two, which would report the same signal 3 dB louder than it is.
 *
 * SAMPLE PEAK, NOT TRUE PEAK
 *
 * The peak reported is max(|sample|) over the decoded samples. BS.1770
 * specifies a true peak, found by oversampling 4x to catch the
 * inter-sample peaks a reconstruction filter can produce above what any
 * sample shows. That needs a polyphase filter per channel per sample
 * and it exists to protect a downstream converter from clipping.
 *
 * This is a documented simplification, not an oversight. It is the same
 * call the Ogg envelope makes in framewalk.c -- state what the number
 * is rather than let a better name imply a measurement that was not
 * taken. If peak-limiting is ever added and needs the real thing, this
 * is where it goes, and it is a LOUDNESS_VERSION bump so every sidecar
 * recomputes.
 *
 * SHORT TRACKS
 *
 * A track shorter than one 400 ms block, or one where every block falls
 * under the absolute gate, still produces an answer -- from whatever
 * blocks there were, or from the ungated mean if gating left nothing.
 * `blocks_gated` is reported alongside so a consumer can tell a
 * confident measurement from a thin one instead of guessing. Refusing
 * to answer would mean a short track is indistinguishable from one
 * nobody has played yet, and the caller would re-measure it on every
 * play for ever.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when the measurement changes meaning -- a different gate, a
 * different weighting, or sample peak becoming true peak. Stored in the
 * sidecar so an old value is recomputed rather than believed. This is
 * deliberately separate from the sidecar's own format version: the
 * record's shape and the number's meaning change for different reasons
 * and should not invalidate each other. */
#define LOUDNESS_VERSION        (1)

/* The same, for what loudness_fade() means by a fade. Separate because
 * retuning a threshold there says nothing about the integrated figure,
 * and should re-examine fades without re-measuring every track. */
#define LOUDNESS_FADE_VERSION   (1)

/* BS.1770 reference: the loudness a track is normalised towards. -18
 * LUFS is the ReplayGain 2.0 convention (EBU R128 broadcast uses -23);
 * the gain to apply is REFERENCE minus the measured integrated value. */
#define LOUDNESS_REFERENCE_LUFS (-18.0f)

/* Up to stereo. A file with more channels is measured on the first two,
 * which is what the player outputs anyway -- audio_out.c is stereo and
 * the decode loop duplicates mono into both slots. */
#define LOUDNESS_MAX_CHANNELS   (2)

/* Columns in the drawable envelope. Matches the panel, which is what
 * will draw it, but the sidecar stores whatever count survived so a
 * short track is not padded -- see replaygain.h. */
#define LOUDNESS_ENV_COLUMNS    (720)

/* Samples per column before any merging: 20 ms at 44.1 kHz. Small
 * enough that a short track still gets a detailed envelope, and the
 * merging handles everything longer. */
#define LOUDNESS_ENV_SPAN0      (882)

typedef struct {
    bool     active;        /* false once invalidated; process() is a no-op */
    bool     started;       /* has any audio been seen at all */

    uint32_t rate;          /* locked on the first block */
    int      channels;      /* locked on the first block */

    /* K-weighting biquad state, per channel. Two sections in series:
     * the high shelf, then the high pass. */
    float    shelf_x1[LOUDNESS_MAX_CHANNELS], shelf_x2[LOUDNESS_MAX_CHANNELS];
    float    shelf_y1[LOUDNESS_MAX_CHANNELS], shelf_y2[LOUDNESS_MAX_CHANNELS];
    float    hp_x1[LOUDNESS_MAX_CHANNELS],    hp_x2[LOUDNESS_MAX_CHANNELS];
    float    hp_y1[LOUDNESS_MAX_CHANNELS],    hp_y2[LOUDNESS_MAX_CHANNELS];

    /* Coefficients, computed for `rate` when it is locked. The standard
     * publishes them for 48 kHz; they are re-derived here for whatever
     * the file actually is, because using the 48 kHz numbers on a
     * 44.1 kHz stream misplaces both corners and biases every result in
     * the same direction, which is the kind of wrong that looks right. */
    float    sb0, sb1, sb2, sa1, sa2;   /* high shelf */
    float    hb0, hb1, hb2, ha1, ha2;   /* high pass */

    /* The 400 ms block being filled, as a sum of squares per channel.
     * 75% overlap means a block boundary every 100 ms, so four partial
     * sums are in flight at once -- kept as a ring of quarter-blocks
     * that are summed when a block completes. */
    double   quarter_sq[LOUDNESS_MAX_CHANNELS][4];
    uint32_t quarter_samples;   /* samples into the current quarter */
    uint32_t quarter_len;       /* samples per quarter (100 ms) */
    int      quarter_idx;       /* which quarter is being filled */
    int      quarters_filled;   /* until the first full block exists */

    /*
     * Gated blocks' loudness, kept as mean-square sums rather than as a
     * list. The relative gate needs a second pass over the blocks, so
     * the blocks that pass the ABSOLUTE gate are accumulated twice: as
     * a running sum (for the relative threshold) and as a histogram
     * (to apply that threshold without keeping every block).
     *
     * The histogram is 0.1 LU buckets from -70 to +5 LUFS, which is 750
     * buckets and 3 KB. Keeping the blocks themselves would be 10 bytes
     * per 100 ms -- a megabyte for a long track, in PSRAM, for a number
     * that is two passes over a histogram instead.
     */
#define LOUDNESS_HIST_BUCKETS   (750)
#define LOUDNESS_HIST_MIN_LUFS  (-70.0f)
#define LOUDNESS_HIST_STEP_LU   (0.1f)
    uint32_t hist[LOUDNESS_HIST_BUCKETS];
    double   abs_gated_sum;     /* sum of mean-square of blocks past the
                                 * absolute gate */
    uint32_t abs_gated_blocks;

    uint32_t blocks_total;      /* every complete block, gated or not */

    /*
     * The last LOUDNESS_TAIL_BLOCKS blocks' momentary loudness, as a
     * ring indexed by block number, for loudness_fade(). Centi-LU in an
     * int16 and floored at LOUDNESS_TAIL_FLOOR_CLU, so silence is a
     * number rather than -inf. Every block goes in, gated or not: the
     * gate is exactly the part of a fade that matters.
     *
     * `tail_smooth` is loudness_fade()'s scratch, kept here rather than
     * on the stack because the decode loop's stack is not the place for
     * another 1.2 KB and this struct is already a static.
     */
#define LOUDNESS_TAIL_BLOCKS    (600)       /* 60 s at one block per 100 ms */
#define LOUDNESS_TAIL_FLOOR_CLU (-8000)     /* -80 LUFS */
    int16_t  tail_clu[LOUDNESS_TAIL_BLOCKS];
    int16_t  tail_smooth[LOUDNESS_TAIL_BLOCKS];
    uint64_t frames_seen;       /* for the track length the fade is placed in */

    /* Sample peak, as a normalised magnitude 0..1 over all channels. */
    float    peak;

    /*
     * The drawable envelope: peak magnitude per column, which is what
     * a waveform IS. framewalk.c's envelope was global_gain out of the
     * frame headers -- an encoder's bit budget, which correlates with
     * loudness and is not it. This is the decoded signal, so the shape
     * on the seek bar is the shape of the audio.
     *
     * The duration is not known here -- the accumulator sees blocks,
     * not a file -- so columns cannot be sized up front. Instead a
     * column covers a fixed span, and when the array fills, adjacent
     * pairs are merged (max, not mean) and the span doubles. A track of
     * any length lands between LOUDNESS_ENV_COLUMNS/2 and
     * LOUDNESS_ENV_COLUMNS columns with at most one pass over the array
     * per doubling, which for a ten-minute track is nine merges of 720
     * bytes in total.
     *
     * Max rather than mean at every stage, for the reason framewalk.c
     * gave: a mean turns a transient into a bump, and the transient is
     * what makes a track's shape recognisable.
     */
    uint8_t  env[LOUDNESS_ENV_COLUMNS];
    int      env_used;          /* columns filled so far */
    uint32_t env_span;          /* samples per column, doubles on merge */
    uint32_t env_pos;           /* samples into the column being filled */
    float    env_peak;          /* running peak within that column */
} loudness_t;

/* Start (or restart) a measurement. Called once per play attempt. */
void loudness_reset(loudness_t *l);

/*
 * Abandon this measurement.
 *
 * Called on a seek. BS.1770 integrates over the whole programme, so a
 * measurement that skipped a section is not a slightly worse answer, it
 * is an answer about a different piece of audio -- and the caller has
 * no way to tell from the number that it happened. Cheaper to stop and
 * let the next uninterrupted play produce a real one.
 *
 * process() becomes a no-op afterwards and finish() reports false.
 */
void loudness_invalidate(loudness_t *l);

/*
 * Feed one decoded block.
 *
 * `pcm` is interleaved int16 as it comes out of decoder_read(), `n` is
 * the total number of int16 values across all channels (the same units
 * decoder_read() returns), and `channels` and `rate` describe it.
 *
 * The rate and channel count are locked on the first block. A file that
 * changes either mid-stream invalidates the measurement rather than
 * being stitched together: the block structure is defined in seconds,
 * so a rate change moves what a block means.
 */
void loudness_process(loudness_t *l, const int16_t *pcm, int n,
                      int channels, uint32_t rate);

/*
 * Finish and report.
 *
 * Returns false if the measurement was invalidated or nothing was ever
 * fed to it. Returns true with a usable answer otherwise, including for
 * a track too short to fill a 400 ms block -- see "SHORT TRACKS" above.
 *
 * `out_lufs` is the integrated loudness. `out_peak_dbfs` is the sample
 * peak in dBFS (0.0 is full scale, negative below it). `out_blocks` is
 * how many blocks survived gating, which is the confidence figure.
 */
bool loudness_finish(loudness_t *l, float *out_lufs, float *out_peak_dbfs,
                     uint32_t *out_blocks);

/*
 * The envelope built during the pass: `*out_cols` columns of 0-255 peak
 * magnitude, written into `dst` (which must hold LOUDNESS_ENV_COLUMNS).
 *
 * Valid after loudness_process() has been fed anything at all, and
 * unlike loudness_finish() it does not care whether the measurement was
 * invalidated -- a seek ruins an integrated loudness figure but the
 * columns either side of it are still the audio that was there. The
 * caller decides whether a partial envelope is worth storing.
 */
void loudness_envelope(const loudness_t *l, uint8_t *dst, int *out_cols);

/*
 * A FADE THAT IS ALREADY IN THE FILE
 *
 * The crossfade ramps the outgoing track down whatever the track is
 * doing, so a song mastered with its own fade gets faded twice: the
 * software ramp is laid over a recording that is already going quiet,
 * and the end of the song drops out early. Before anything can decline
 * to do that, something has to know which tracks fade and where. This
 * is that something, and it answers from the same pass as the loudness.
 *
 * What counts as a fade, all measured in LU against this track's own
 * integrated loudness, so the answer does not move with ReplayGain or
 * with how loud the file was mastered:
 *
 *   1. The end of the audio is the last block above
 *      max(-70 LUFS, integrated - LOUDNESS_FADE_AUDIBLE_LU). Digital
 *      silence or dither after that is trailing silence, not fade.
 *   2. The level is smoothed over +-1 s of blocks, averaged in LU
 *      rather than in power. A power mean over a decay that is straight
 *      in dB is dominated by its loud edge and reads the fade as
 *      starting late; a mean of LU values does not.
 *   3. The start is found by walking back from the end for as long as
 *      the level keeps climbing -- while the LOUDNESS_FADE_LOOK_BLOCKS
 *      before a point hold something LOUDNESS_FADE_RISE_LU louder than
 *      anything after it. Walking from the end rather than from where
 *      the song was last at its own level is what lets a quiet outro
 *      fade from the outro, and what finds the start of a
 *      linear-amplitude fade, whose first seconds fall by barely a dB.
 *   4. A walk that is still climbing when it runs out of kept blocks
 *      has not found the start, and reports no fade.
 *   5. It is a fade if it drops at least LOUDNESS_FADE_MIN_DEPTH_LU and
 *      lasts at least LOUDNESS_FADE_MIN_MS.
 *
 * There is no "downhill all the way" test, and that is not an omission.
 * The walk in 3 already stops at anything the song climbs back up from,
 * so a quiet bridge followed by more song never reaches 5. What a
 * running-minimum check would add is refusing a fade with a late drum
 * hit in it -- which is still a recording going quiet on its own, and
 * exactly what should not be faded a second time.
 *
 * A long natural decay -- a held chord ringing out -- passes all five
 * and is reported as a fade. That is deliberate: for anything deciding
 * whether to ramp a track down, a recording that is already decaying to
 * nothing is the same problem whether a fader or a piano did it.
 *
 * Only the last LOUDNESS_TAIL_BLOCKS are kept, so a fade that starts
 * more than a minute before the end is not seen, and is reported as no
 * fade rather than as a fade with a wrong start.
 */
typedef struct {
    bool     has_fade;
    uint32_t start_ms;      /* where the level starts falling */
    uint32_t end_ms;        /* where it is no longer audible */
    uint32_t total_ms;      /* decoded length */
    /*
     * Where the audio ends, fade or not: the end of the last block that
     * passes the audible test in 1 above. total_ms - audio_end_ms is the
     * silence recorded after it. Equal to total_ms when that is unknown
     * -- nothing audible in the kept tail -- so an unknown reads as no
     * silence rather than as a minute of it. With a fade, end_ms is the
     * same number.
     */
    uint32_t audio_end_ms;
    float    depth_lu;      /* how far it falls, start to end */
} loudness_fade_t;

#define LOUDNESS_FADE_AUDIBLE_LU     (40.0f)
#define LOUDNESS_FADE_RISE_LU        (0.5f)
#define LOUDNESS_FADE_LOOK_BLOCKS    (20)       /* 2 s */
#define LOUDNESS_FADE_MIN_DEPTH_LU   (10.0f)
#define LOUDNESS_FADE_MIN_MS         (1500u)
#define LOUDNESS_FADE_SMOOTH_BLOCKS  (10)       /* either side: +-1 s */

/*
 * Look for a fade at the end of what was fed.
 *
 * Returns false when there is no answer to give -- invalidated, never
 * started, or nothing past the absolute gate -- and true when it looked,
 * with out->has_fade saying what it found. A track too short to judge
 * is true with has_fade false: that is an answer, and recording it stops
 * the next play looking again.
 *
 * `integrated_lufs` is loudness_finish()'s answer for the same pass.
 * Uses the struct's scratch, so like everything else here it belongs to
 * the one task feeding the measurement.
 */
bool loudness_fade(loudness_t *l, float integrated_lufs, loudness_fade_t *out);

#ifdef __cplusplus
}
#endif
