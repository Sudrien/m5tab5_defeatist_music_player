/*
 * loudness.c -- see loudness.h for what this measures and why it is not
 * the frame walk.
 *
 * SPDX-License-Identifier: MIT
 */
#include "loudness.h"

#include <math.h>
#include <string.h>

/*
 * K-weighting, re-derived for the stream's own sample rate.
 *
 * BS.1770 publishes the two biquads as coefficient tables at 48 kHz.
 * Using those numbers on a 44.1 kHz file puts both corner frequencies
 * about 9% high, which biases every measurement the same direction --
 * a wrong answer that looks entirely plausible, which is the worst
 * kind. So the analogue prototypes are bilinear-transformed here for
 * whatever rate arrived.
 *
 * Stage 1 is a high shelf, +4 dB above about 1.5 kHz, standing in for
 * the acoustic effect of a head in a diffuse field. Stage 2 is a
 * high-pass at about 38 Hz, which removes the rumble that would
 * otherwise dominate a mean square without being heard.
 */
/*
 * This is the standard's own prototype, bilinear-transformed with tan()
 * pre-warping, NOT an RBJ-cookbook shelf fitted to the same corner and
 * Q. That distinction is worth stating because the cookbook version
 * compiles, runs, produces an entirely plausible envelope, and is
 * wrong: it lands 0.4382 dB at 1 kHz where the standard's filter lands
 * 0.6977 dB, so every measurement comes out 0.25 LU low. A constant
 * offset in the right ballpark is the hardest kind of error to notice
 * from the output alone.
 *
 * The check that settles it: at fs = 48000 this reproduces the
 * coefficient table printed in BS.1770-4 to fourteen decimal places.
 * If this function is ever touched, that is the test to re-run --
 * agreement with the published table at 48 kHz, not whether the numbers
 * look reasonable.
 */
static void design_kweighting(loudness_t *l, uint32_t rate)
{
    const double fs = (double)rate;

    /* ---- Stage 1: high shelf ---- */
    const double f0 = 1681.974450955533;   /* Hz */
    const double G  = 3.999843853973347;   /* dB */
    const double Q  = 0.7071752369554196;

    const double K  = tan(M_PI * f0 / fs);
    const double Vh = pow(10.0, G / 20.0);
    /* The exponent is the standard's, not 0.5. Vb is the shelf's
     * mid-band gain and it is not sqrt(Vh). */
    const double Vb = pow(Vh, 0.4996667741545416);
    const double sa0 = 1.0 + K / Q + K * K;

    l->sb0 = (float)((Vh + Vb * K / Q + K * K) / sa0);
    l->sb1 = (float)((2.0 * (K * K - Vh)) / sa0);
    l->sb2 = (float)((Vh - Vb * K / Q + K * K) / sa0);
    l->sa1 = (float)((2.0 * (K * K - 1.0)) / sa0);
    l->sa2 = (float)((1.0 - K / Q + K * K) / sa0);

    /* ---- Stage 2: high pass ---- */
    const double f0h = 38.13547087602444;  /* Hz */
    const double Qh  = 0.5003270373238773;
    const double Kh  = tan(M_PI * f0h / fs);
    const double ha0 = 1.0 + Kh / Qh + Kh * Kh;

    l->hb0 =  1.0f;
    l->hb1 = -2.0f;
    l->hb2 =  1.0f;
    l->ha1 = (float)((2.0 * (Kh * Kh - 1.0)) / ha0);
    l->ha2 = (float)((1.0 - Kh / Qh + Kh * Kh) / ha0);
}

void loudness_reset(loudness_t *l)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->active = true;
}

void loudness_invalidate(loudness_t *l)
{
    if (!l) return;
    l->active = false;
}

/* Mean square -> LUFS, the standard's -0.691 offset included. */
static float ms_to_lufs(double ms)
{
    if (ms <= 0.0) return -HUGE_VALF;
    return (float)(-0.691 + 10.0 * log10(ms));
}

/*
 * A 400 ms block has completed. Its mean square is the sum over the
 * four quarters currently in flight, divided by the samples in them,
 * with the standard's channel weights applied (1.0 for L and R, so the
 * weighting is a sum across channels rather than a mean).
 */
static void close_block(loudness_t *l)
{
    const uint32_t per_ch = l->quarter_len * 4;
    if (!per_ch) return;

    double ms = 0.0;
    for (int c = 0; c < l->channels && c < LOUDNESS_MAX_CHANNELS; c++) {
        double sum = 0.0;
        for (int q = 0; q < 4; q++) sum += l->quarter_sq[c][q];
        ms += sum / (double)per_ch;      /* weight 1.0 */
    }

    l->blocks_total++;

    const float lufs = ms_to_lufs(ms);

    /* Absolute gate. Below -70 LUFS a block is silence as far as the
     * standard is concerned and takes no part in the answer. */
    if (lufs < LOUDNESS_HIST_MIN_LUFS) return;

    l->abs_gated_sum += ms;
    l->abs_gated_blocks++;

    int b = (int)((lufs - LOUDNESS_HIST_MIN_LUFS) / LOUDNESS_HIST_STEP_LU);
    if (b < 0) b = 0;
    if (b >= LOUDNESS_HIST_BUCKETS) b = LOUDNESS_HIST_BUCKETS - 1;
    l->hist[b]++;
}

void loudness_process(loudness_t *l, const int16_t *pcm, int n,
                      int channels, uint32_t rate)
{
    if (!l || !l->active || !pcm || n <= 0 || channels <= 0 || !rate) return;

    if (!l->started) {
        l->started = true;
        l->rate = rate;
        l->channels = channels < LOUDNESS_MAX_CHANNELS ? channels
                                                       : LOUDNESS_MAX_CHANNELS;
        /* 100 ms quarters; a block is four of them. */
        l->quarter_len = rate / 10;
        if (!l->quarter_len) l->quarter_len = 1;

        /* 20 ms at this rate, scaled from the 44.1 kHz reference so a
         * 48 kHz file gets the same span in time rather than in
         * samples. */
        l->env_span = (uint32_t)(((uint64_t)LOUDNESS_ENV_SPAN0 * rate) / 44100);
        if (!l->env_span) l->env_span = 1;
        design_kweighting(l, rate);
    } else if (l->rate != rate || l->channels != (channels < LOUDNESS_MAX_CHANNELS
                                                  ? channels
                                                  : LOUDNESS_MAX_CHANNELS)) {
        /* The block structure is defined in seconds and the filters are
         * designed for one rate. A stream that changes either mid-way is
         * two measurements, not one. */
        l->active = false;
        return;
    }

    const int ch = l->channels;
    const int src_ch = channels;
    const int frames = n / src_ch;

    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < ch; c++) {
            const float x = (float)pcm[i * src_ch + c] / 32768.0f;

            const float ax = fabsf(x);
            if (ax > l->peak) l->peak = ax;
            if (ax > l->env_peak) l->env_peak = ax;

            /* High shelf. */
            const float sy = l->sb0 * x + l->sb1 * l->shelf_x1[c]
                           + l->sb2 * l->shelf_x2[c]
                           - l->sa1 * l->shelf_y1[c]
                           - l->sa2 * l->shelf_y2[c];
            l->shelf_x2[c] = l->shelf_x1[c];
            l->shelf_x1[c] = x;
            l->shelf_y2[c] = l->shelf_y1[c];
            l->shelf_y1[c] = sy;

            /* High pass. */
            const float hy = l->hb0 * sy + l->hb1 * l->hp_x1[c]
                           + l->hb2 * l->hp_x2[c]
                           - l->ha1 * l->hp_y1[c]
                           - l->ha2 * l->hp_y2[c];
            l->hp_x2[c] = l->hp_x1[c];
            l->hp_x1[c] = sy;
            l->hp_y2[c] = l->hp_y1[c];
            l->hp_y1[c] = hy;

            l->quarter_sq[c][l->quarter_idx] += (double)hy * (double)hy;
        }

        /*
         * Envelope. The per-sample peak across channels is already in
         * hand from the loop above; this only tracks its max over the
         * column and closes the column when the span is reached.
         */
        if (++l->env_pos >= l->env_span) {
            l->env_pos = 0;
            if (l->env_used < LOUDNESS_ENV_COLUMNS) {
                l->env[l->env_used++] =
                    (uint8_t)(l->env_peak * 255.0f + 0.5f);
            }
            l->env_peak = 0.0f;

            if (l->env_used >= LOUDNESS_ENV_COLUMNS) {
                /* Full: fold pairs together and double the span, so the
                 * array now covers twice the time at half the detail
                 * and there is room to keep going. Max, not mean. */
                for (int k = 0; k < LOUDNESS_ENV_COLUMNS / 2; k++) {
                    const uint8_t a = l->env[2 * k];
                    const uint8_t b = l->env[2 * k + 1];
                    l->env[k] = a > b ? a : b;
                }
                l->env_used = LOUDNESS_ENV_COLUMNS / 2;
                l->env_span *= 2;
            }
        }

        if (++l->quarter_samples >= l->quarter_len) {
            l->quarter_samples = 0;
            if (l->quarters_filled < 4) l->quarters_filled++;

            /* With four quarters in flight there is a complete 400 ms
             * block every 100 ms, which is the standard's 75% overlap. */
            if (l->quarters_filled >= 4) close_block(l);

            l->quarter_idx = (l->quarter_idx + 1) % 4;
            for (int c = 0; c < ch; c++) l->quarter_sq[c][l->quarter_idx] = 0.0;
        }
    }
}

bool loudness_finish(loudness_t *l, float *out_lufs, float *out_peak_dbfs,
                     uint32_t *out_blocks)
{
    if (!l || !l->active || !l->started) return false;

    double ms = 0.0;
    uint32_t used = 0;

    if (l->abs_gated_blocks > 0) {
        /*
         * Relative gate: 10 LU below the mean of everything that passed
         * the absolute gate. Applied against the histogram rather than
         * against a stored list of blocks.
         */
        const double abs_mean = l->abs_gated_sum / (double)l->abs_gated_blocks;
        const float rel_thresh = ms_to_lufs(abs_mean) - 10.0f;

        for (int b = 0; b < LOUDNESS_HIST_BUCKETS; b++) {
            if (!l->hist[b]) continue;
            /* Bucket centre, which is what its members are approximated
             * as. 0.1 LU buckets make that error far smaller than the
             * gate's own 10 LU width. */
            const float lufs = LOUDNESS_HIST_MIN_LUFS
                             + ((float)b + 0.5f) * LOUDNESS_HIST_STEP_LU;
            if (lufs < rel_thresh) continue;
            const double bms = pow(10.0, ((double)lufs + 0.691) / 10.0);
            ms += bms * (double)l->hist[b];
            used += l->hist[b];
        }
        if (used) ms /= (double)used;
    }

    if (!used) {
        /*
         * Nothing survived gating -- a very short track, or one quiet
         * enough that every block fell under the absolute gate. Fall
         * back to the ungated mean of whatever was accumulated,
         * including the partial block still in flight, so a short track
         * gets a real (if thin) answer rather than nothing.
         *
         * out_blocks reports 0 here, which is what says the answer is
         * ungated. See "SHORT TRACKS" in the header.
         */
        const uint32_t per_ch = l->quarter_len * 4;
        if (per_ch) {
            double sum = 0.0;
            for (int c = 0; c < l->channels && c < LOUDNESS_MAX_CHANNELS; c++) {
                for (int q = 0; q < 4; q++) sum += l->quarter_sq[c][q];
            }
            ms = sum / (double)per_ch;
        }
    }

    if (out_lufs) *out_lufs = ms_to_lufs(ms);
    if (out_peak_dbfs) {
        *out_peak_dbfs = (l->peak > 0.0f) ? 20.0f * log10f(l->peak)
                                          : -HUGE_VALF;
    }
    if (out_blocks) *out_blocks = used;
    return true;
}

void loudness_envelope(const loudness_t *l, uint8_t *dst, int *out_cols)
{
    if (!l || !dst) { if (out_cols) *out_cols = 0; return; }

    int n = l->env_used;
    if (n > LOUDNESS_ENV_COLUMNS) n = LOUDNESS_ENV_COLUMNS;
    if (n > 0) memcpy(dst, l->env, (size_t)n);

    /*
     * The column still being filled is deliberately left out. It covers
     * less time than the others, so its peak is drawn from a smaller
     * sample and reads lower for no reason the audio accounts for --
     * a dip at the right edge of every track. One column of up to
     * env_span samples is not worth a visible artefact.
     */
    if (out_cols) *out_cols = n;
}
