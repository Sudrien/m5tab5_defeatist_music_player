/*
 * waveform.c
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "esp_log.h"

#include "gfx.h"
#include "waveform.h"

static const char *TAG = "tab5_wave";

/*
 * Floor on the drawn height.
 *
 * global_gain on a real track occupies a narrow slice of its 0-255 range,
 * and the quiet end of a normalised envelope came out as a one-pixel
 * hairline -- present, correct, and invisible from arm's length on a
 * 294 PPI panel. At 5 px a quiet passage still reads as part of the bar
 * rather than as a gap in it, which matters more now that the bar is also
 * the seek control: a gap in a slider looks broken.
 */
#define MIN_H       (5)

/* The walk, copied. See waveform_set(). */
static uint8_t s_level[FRAMEWALK_MAX_COLUMNS];
static int     s_columns;
static int     s_lo, s_hi;

static inline int level_at(int i, int n)
{
    if (s_columns <= 0 || n <= 0) return 0;
    int c = (int)(((long)i * s_columns) / n);
    if (c < 0) c = 0;
    if (c >= s_columns) c = s_columns - 1;
    return s_level[c];
}

/*
 * The envelope is drawn from the track's own minimum rather than from
 * zero.
 *
 * global_gain on a quiet passage is not 0, it is a low number, so a track
 * scaled straight from 0-255 sits as a fat slab that never comes near the
 * baseline. Rescaling from the track's own range makes the quiet parts
 * actually read as quiet -- and makes two different tracks look
 * different, which is the entire point of drawing this.
 */
static void span(const framewalk_t *w, int *lo, int *hi)
{
    int mn = 255, mx = 0;
    for (int i = 0; i < w->columns; i++) {
        const int v = w->level[i];
        /*
         * Zero is framewalk's flag for a granule with no main data --
         * silence, not a quiet passage. It must not set the floor: a
         * track that opens with a second of encoder padding would put lo
         * at 0, and the music, which lives between about 150 and 255,
         * would then be drawn as a slab occupying the top third of the
         * bar with no shape in it. That is the failure span() exists to
         * prevent, arriving through the front door.
         */
        if (v == 0) continue;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mx <= mn) { mn = 0; mx = 255; }
    *lo = mn;
    *hi = mx;
}

void waveform_set(const framewalk_t *w)
{
    if (!w || !w->has_levels || w->columns <= 0) {
        s_columns = 0;
        return;
    }

    s_columns = w->columns;
    if (s_columns > FRAMEWALK_MAX_COLUMNS) s_columns = FRAMEWALK_MAX_COLUMNS;
    memcpy(s_level, w->level, (size_t)s_columns);
    span(w, &s_lo, &s_hi);

    ESP_LOGI(TAG, "envelope: %d columns, levels %d..%d",
             s_columns, s_lo, s_hi);
}

bool waveform_ready(void)
{
    return s_columns > 0;
}


void waveform_draw_bar(int x0, int x1, int base_y, int height, int pct,
                       uint16_t past, uint16_t future)
{
    if (s_columns <= 0 || x1 <= x0 || height <= MIN_H) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    const int width = x1 - x0;
    const int split = x0 + (width * pct) / 100;
    const int reach = height - MIN_H;

    for (int i = 0; i < width; i++) {
        const int x = x0 + i;
        const int v = level_at(i, width);

        /* v below s_lo is a silent column, which span() left out of the
         * range; it clamps to the floor rather than going negative. */
        int h = MIN_H + ((v - s_lo) * reach) / (s_hi - s_lo);
        if (h < MIN_H) h = MIN_H;
        if (h > height) h = height;

        /*
         * One column, one colour, decided by which side of the playhead
         * it is on. Splitting a column down the middle would be more
         * accurate and would cost a second fill per column for a
         * distinction narrower than the pixel it is drawn in.
         */
        gfx_fill_rect(x, base_y - h, 1, h, x < split ? past : future);
    }
}
