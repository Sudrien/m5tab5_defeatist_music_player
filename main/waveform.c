/*
 * waveform.c
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"

#include "gfx.h"
#include "waveform.h"

static const char *TAG = "tab5_wave";

/* Dim enough not to compete with the cover it surrounds, bright enough to
 * read as deliberate rather than as an artefact. */
#define C_WAVE      RGB(0x9A, 0x2A, 0x20)
#define C_WAVE_HI   RGB(0xD1, 0x3B, 0x2C)

/*
 * Floor on the drawn thickness.
 *
 * global_gain on a real track occupies a narrow slice of its 0-255 range,
 * and the quiet end of a normalised envelope came out as a one-pixel
 * hairline -- present, correct, and invisible from arm's length on a
 * 294 PPI panel. The band reads as a band at 6 px; below that it reads as
 * nothing at all, which is indistinguishable from the feature not
 * working.
 */
#define MIN_DEPTH   (6)

static inline int level_at(const framewalk_t *w, int i, int n)
{
    if (w->columns <= 0 || n <= 0) return 0;
    int c = (int)(((long)i * w->columns) / n);
    if (c < 0) c = 0;
    if (c >= w->columns) c = w->columns - 1;
    return w->level[c];
}

/*
 * The envelope is drawn from a floor rather than from zero.
 *
 * global_gain on a quiet passage is not 0, it is a low number, so a track
 * scaled straight from 0-255 sits as a fat band that never touches the
 * inner edge. Rescaling from the track's own minimum makes the quiet
 * parts actually read as quiet -- and makes two different tracks look
 * different, which is the entire point of drawing this.
 */
static void span(const framewalk_t *w, int *lo, int *hi)
{
    int mn = 255, mx = 0;
    for (int i = 0; i < w->columns; i++) {
        const int v = w->level[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mx <= mn) { mn = 0; mx = 255; }
    *lo = mn;
    *hi = mx;
}

static inline int scaled(int v, int lo, int hi, int depth)
{
    if (hi <= lo) return 0;
    int d = MIN_DEPTH + ((v - lo) * (depth - MIN_DEPTH)) / (hi - lo);
    if (d < MIN_DEPTH) d = MIN_DEPTH;
    if (d > depth) d = depth;
    return d;
}

/*
 * The envelope, mirrored about the middle of the artwork area.
 *
 * When cut is true a square in the centre is left alone -- not painted
 * black here, because albumart.c has already cleared the area and drawn
 * the cover into exactly that square. Skipping it is what leaves the
 * cover visible with a black margin, and it means the envelope never has
 * to know how big the picture is.
 */
static void draw_envelope(const framewalk_t *w, int art_h, bool cut)
{
    const int width = gfx_w();
    const int mid = art_h / 2;
    const int max_half = art_h / 2 - 24;

    const int cx0 = (width - WAVE_INNER) / 2;
    const int cx1 = cx0 + WAVE_INNER;
    const int cy0 = (art_h - WAVE_INNER) / 2;
    const int cy1 = cy0 + WAVE_INNER;

    /*
     * In frame mode the envelope starts at the edge of the cutout rather
     * than at the middle.
     *
     * Scaling from the centre looked right on paper and wrong on screen:
     * only the loudest columns reached past the cover, so instead of a
     * frame there were spikes escaping from behind a square. Basing the
     * bar at the cutout edge and letting it grow outward from there gives
     * a band that surrounds the cover continuously and still breathes
     * with the track -- which is what a frame is.
     */
    const int base = cut ? (WAVE_INNER / 2 + 12) : 0;
    const int reach = max_half - base;

    int lo, hi;
    span(w, &lo, &hi);

    for (int x = 0; x < width; x++) {
        const int v = level_at(w, x, width);
        const int h = base + scaled(v, lo, hi, reach);
        int top = mid - h;
        int bot = mid + h;

        if (!cut || x < cx0 || x >= cx1) {
            gfx_fill_rect(x, top, 1, bot - top, C_WAVE);
            gfx_fill_rect(x, mid - 2, 1, 4, C_WAVE_HI);
            continue;
        }

        /* Inside the cutout's columns: draw only the parts of the bar
         * that fall above and below the square. A column whose envelope
         * never reaches past the cover contributes nothing, which is
         * correct -- the cover is in front of it. */
        if (top < cy0) gfx_fill_rect(x, top, 1, cy0 - top, C_WAVE);
        if (bot > cy1) gfx_fill_rect(x, cy1, 1, bot - cy1, C_WAVE);
    }
}

void waveform_draw(const framewalk_t *w, wave_mode_t mode, int art_h)
{
    if (!w || !w->has_levels || w->columns <= 0) return;
    if (art_h <= 0) return;

    int lo, hi;
    span(w, &lo, &hi);

    /* The geometry, not just the fact of drawing. When the envelope did
     * not appear on screen there was no way to tell from the log whether
     * this ran at all, whether it ran with a degenerate range, or whether
     * it drew somewhere off-panel. */
    ESP_LOGI(TAG, "%s: levels %d..%d, panel %dx%d, cutout %d at (%d,%d)",
             mode == WAVE_FRAME ? "frame" : "fill", lo, hi,
             gfx_w(), art_h, WAVE_INNER,
             (gfx_w() - WAVE_INNER) / 2, (art_h - WAVE_INNER) / 2);

    draw_envelope(w, art_h, mode == WAVE_FRAME);

    gfx_blit(0, art_h);
}
