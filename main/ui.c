/*
 * ui.c -- the transport bar.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "gfx.h"
#include "ui.h"

static const char *TAG = "tab5_ui";

/* RGB() and every drawing primitive now live in gfx.c, so the chooser can
 * use the same ones. Nothing here changed except the names. */

#define C_BG        RGB(0x11, 0x11, 0x11)
#define C_TRACK     RGB(0x3A, 0x3A, 0x3A)   /* unplayed / unfilled */
#define C_FILL      RGB(0xD1, 0x3B, 0x2C)   /* played / set volume */
#define C_THUMB     RGB(0xFF, 0xFF, 0xFF)
#define C_ICON      RGB(0xCC, 0xCC, 0xCC)
/* Album row. Dimmer than artist, but its own value rather than reusing
 * C_TRACK -- that is 0x3A, chosen to be a slider groove that does not
 * compete with the fill, and it is too dark to read as text. */
#define C_ALBUM     RGB(0x88, 0x88, 0x88)
#define C_BUBBLE_BG RGB(0x1A, 0x1A, 0x1A)
#define C_BUBBLE_ED RGB(0x44, 0x44, 0x44)

/* Two thicknesses, and the asymmetry is the point: the filled part is
 * thick so progress reads from arm's length, the remainder is thin so it
 * does not compete with it. */
#define TRACK_THIN  (4)
#define TRACK_THICK (13)
#define THUMB_R     (16)

/* Drawn geometry, relative to the top of the bar. */
/*
 * Sizes here are set for a 5" 720x1280 panel -- about 294 PPI, where an
 * 8 px font glyph is 1.4 mm tall and unreadable at arm's length. Scale 5
 * puts the title at roughly 3.5 mm, which is about a phone's body text.
 * Everything else is sized to match rather than left at the values that
 * looked fine in a mockup rendered at 96 PPI.
 */
/*
 * Three text rows now, not two.
 *
 * Artist and album shared a line because three stacked rows made the bar
 * taller than the artwork could spare. They no longer share it: an album
 * title of any length pushed the artist out of the joined string, and the
 * artist is the part people read. The bar absorbed the extra row instead
 * -- see UI_BAR_H.
 *
 * The rows are 50 px, 32 px and 32 px apart rather than evenly spaced.
 * The title is scale 5 (40 px tall) and needs the clearance; artist and
 * album are both scale 3 (24 px) and sit closer to each other than either
 * does to the title, so they read as a pair belonging to it.
 */
#define TEXT_Y      (18)   /* title, top of the bar */
#define ARTIST_Y    (68)
#define ALBUM_Y     (100)
#define SEEK_Y      (174)
#define SEEK_X0     (142)  /* clears the MM:SS run at either end */
#define ROW_Y       (254)
#define BTN_R       (46)    /* play/pause circle */
#define ICON_HALF   (26)

/* Hit targets are padded well beyond the drawn shapes. A 22 px slider on
 * a 5" panel is a small thing to hit with a thumb, and there is nothing
 * adjacent to steal the press from -- the same reasoning as the map
 * project's BTN_PAD_TOP. */
#define HIT_PAD_Y   (30)
#define HIT_PAD_X   (14)

/* Finger bubble.
 *
 * It tracks the finger horizontally only. Following the finger vertically
 * as well meant it sat at a different height depending on where in the
 * padded hit box the press landed, which reads as the indicator jumping
 * around rather than as a value changing. Fixed height, and the height is
 * measured from the seek bar so both sliders raise it to the same place.
 *
 * It also has to be erased. Everything else here is inside the bar, which
 * is cleared wholesale every frame; the bubble deliberately reaches above
 * the bar onto the cover art, which is not. Hence s_bubble_bg, a saved
 * strip of the art captured once, restored before each repaint. Without
 * it a drag leaves a trail of previous bubbles across the artwork. */
#define BUBBLE_R    (64)
/* Measured from the seek bar down to the bubble's bottom edge, so it must
 * exceed SEEK_Y or the bubble overlaps the bar. That overlap is not
 * cosmetic: the bar is blitted before the bubble is drawn, so any part of
 * the bubble inside it is written to the framebuffer and never pushed,
 * showing stale pixels until the next frame clears them. */
#define BUBBLE_ABOVE (SEEK_Y + 36)

static uint16_t *s_fb;
static int s_w, s_h;
static int s_bar_top;

/* Live drag state. -1 = nothing being dragged. */
static int s_drag = -1;         /* 0 = seek, 1 = volume */
static int s_drag_x, s_drag_y;
static int s_drag_pct;
static bool s_was_down;

/* Saved cover-art strip behind the bubble, and whether a bubble was drawn
 * last frame (so the restore only costs anything while dragging). */
static uint16_t *s_bubble_bg;
static int s_bubble_top, s_bubble_h;
static bool s_bubble_shown;

static int bubble_cy(void)
{
    return s_bar_top + SEEK_Y - BUBBLE_ABOVE - BUBBLE_R;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/* Seek runs nearly the full width; volume sits between the folder icon
 * and the play button. Returned in absolute screen coordinates. */
static void seek_bounds(int *x0, int *x1, int *y)
{
    *x0 = SEEK_X0;
    *x1 = s_w - SEEK_X0;
    *y  = s_bar_top + SEEK_Y;
}

static void vol_bounds(int *x0, int *x1, int *y)
{
    *x0 = 196;
    *x1 = 402;
    *y  = s_bar_top + ROW_Y;
}

static void play_centre(int *cx, int *cy)
{
    *cx = 524;
    *cy = s_bar_top + ROW_Y;
}

static void folder_centre(int *cx, int *cy)
{
    *cx = 84;
    *cy = s_bar_top + ROW_Y;
}

static void moon_centre(int *cx, int *cy)
{
    *cx = s_w - 84;
    *cy = s_bar_top + ROW_Y;
}

/* ------------------------------------------------------------------ */
/* Widgets                                                             */
/* ------------------------------------------------------------------ */

static void draw_slider(int x0, int x1, int y, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const int w = x1 - x0;
    const int split = x0 + (w * pct) / 100;

    /* Thin remainder first, then the thick fill over its left end, so the
     * two never disagree by a pixel at the join. */
    gfx_fill_rect(x0, y - TRACK_THIN / 2, w, TRACK_THIN, C_TRACK);
    gfx_fill_rect(x0, y - TRACK_THICK / 2, split - x0, TRACK_THICK, C_FILL);
    gfx_fill_circle(split, y, THUMB_R, C_THUMB);
}

static void draw_play_pause(bool playing)
{
    int cx, cy;
    play_centre(&cx, &cy);
    gfx_fill_circle(cx, cy, BTN_R, C_THUMB);

    if (playing) {
        /* Pause: two bars. */
        gfx_fill_rect(cx - 15, cy - 20, 10, 40, C_BG);
        gfx_fill_rect(cx + 5, cy - 20, 10, 40, C_BG);
    } else {
        /* Play: a triangle, nudged right so it looks centred rather than
         * measuring centred. */
        for (int dy = -20; dy <= 20; dy++) {
            const int a = dy < 0 ? -dy : dy;
            gfx_fill_rect(cx - 11, cy + dy, 34 - (a * 34) / 20, 1, C_BG);
        }
    }
}

static void draw_folder(void)
{
    int cx, cy;
    folder_centre(&cx, &cy);
    gfx_fill_rect(cx - ICON_HALF, cy - 18, 21, 7, C_ICON);          /* tab */
    gfx_fill_rect(cx - ICON_HALF, cy - 12, 2 * ICON_HALF, 32, C_ICON);
    gfx_fill_rect(cx - ICON_HALF + 4, cy - 7, 2 * ICON_HALF - 8, 23, C_BG);
}

static void draw_moon(void)
{
    int cx, cy;
    moon_centre(&cx, &cy);
    gfx_fill_circle(cx, cy, ICON_HALF, C_ICON);
    gfx_fill_circle(cx + 13, cy - 10, ICON_HALF, C_BG); /* bite out the crescent */
}

/* A speaker is a small rectangle (the body) with a cone flaring out to the
 * right. The first version drew the cone as a triangle whose height shrank
 * with x, which is an arrow pointing right -- the flare has to grow with
 * x, not shrink. */
static void draw_speaker(void)
{
    int x0, x1, y;
    vol_bounds(&x0, &x1, &y);
    const int cx = x0 - 42, cy = y;

    gfx_fill_rect(cx - 13, cy - 6, 9, 13, C_ICON);      /* body */
    for (int dx = 0; dx <= 15; dx++) {              /* cone, flaring right */
        const int half = 4 + dx;
        gfx_fill_rect(cx - 4 + dx, cy - half, 1, 2 * half + 1, C_ICON);
    }
}

/* ------------------------------------------------------------------ */

/* Copy the band the bubble can occupy out of the framebuffer. Called once
 * the cover art is on screen -- before that there is nothing worth
 * saving. */
void ui_capture_background(void)
{
    if (!s_fb) return;
    free(s_bubble_bg);
    s_bubble_top = bubble_cy() - BUBBLE_R - 2;
    if (s_bubble_top < 0) s_bubble_top = 0;
    s_bubble_h = (bubble_cy() + BUBBLE_R + 2) - s_bubble_top;
    if (s_bubble_top + s_bubble_h > s_bar_top) s_bubble_h = s_bar_top - s_bubble_top;
    if (s_bubble_h <= 0) { s_bubble_bg = NULL; return; }

    s_bubble_bg = heap_caps_malloc((size_t)s_w * s_bubble_h * sizeof(uint16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bubble_bg) {
        ESP_LOGW(TAG, "no room to save the bubble background; drags will smear");
        return;
    }
    memcpy(s_bubble_bg, &s_fb[s_bubble_top * s_w],
           (size_t)s_w * s_bubble_h * sizeof(uint16_t));
}

void ui_clear_art(void)
{
    if (!s_fb) return;
    gfx_fill_rect(0, 0, s_w, s_bar_top, C_BG);
    gfx_blit(0, s_bar_top);
    /* The saved strip is now a strip of black, which is exactly what the
     * bubble should put back until the next cover is drawn over it. */
    s_bubble_shown = false;
}

static void bubble_restore(void)
{
    if (!s_bubble_bg || !s_bubble_shown) return;
    memcpy(&s_fb[s_bubble_top * s_w], s_bubble_bg,
           (size_t)s_w * s_bubble_h * sizeof(uint16_t));
    gfx_blit(s_bubble_top, s_bubble_top + s_bubble_h);
    s_bubble_shown = false;
}

esp_err_t ui_init(esp_lcd_panel_handle_t panel, int w, int h)
{
    /* gfx owns the framebuffer lookup now; this file keeps its own copies
     * of the geometry because every layout function reads them. */
    ESP_RETURN_ON_ERROR(gfx_init(panel, w, h), TAG, "gfx");
    s_w = w;
    s_h = h;
    s_bar_top = h - UI_BAR_H;
    s_fb = gfx_fb();
    return ESP_OK;
}

void ui_draw(const ui_state_t *st)
{
    if (!s_fb) return;

    if (st->screen_off) {
        /* Nothing at all -- the backlight is off, and drawing into a dark
         * panel just burns PSRAM bandwidth the decoder wants. */
        return;
    }

    /* Put the artwork back before anything else, or the previous frame's
     * bubble stays on it. */
    if (s_drag < 0) bubble_restore();

    gfx_fill_rect(0, s_bar_top, s_w, UI_BAR_H, C_BG);

    int x0, x1, y;
    seek_bounds(&x0, &x1, &y);
    const int pos_pct = (st->len_sec > 0)
                      ? (int)((st->pos_sec * 100) / st->len_sec)
                      : 0;
    /*
     * Three states, not two:
     *   - seekable: full slider with a thumb, draggable
     *   - known length, not seekable: progress fills, no thumb. Honest --
     *     the position is real -- and the missing thumb is what says not
     *     to try dragging it.
     *   - no length: a bare groove.
     */
    if (st->can_seek) {
        draw_slider(x0, x1, y, s_drag == 0 ? s_drag_pct : pos_pct);
    } else if (st->len_sec > 0) {
        gfx_fill_rect(x0, y - 3, x1 - x0, 6, C_TRACK);
        gfx_fill_rect(x0, y - 3, ((x1 - x0) * pos_pct) / 100, 6, C_FILL);
    } else {
        gfx_fill_rect(x0, y - 3, x1 - x0, 6, C_TRACK);
    }

    /* Elapsed left of the bar, total right of it. Total reads 00:00 when
     * the backend could not supply a duration, which is the honest
     * rendering of "unknown" and matches the empty bar next to it. */
    gfx_draw_time(10, y - GFX_DIG_H / 2, st->pos_sec, C_ICON);
    gfx_draw_time(s_w - GFX_TIME_W - 10, y - GFX_DIG_H / 2, st->len_sec, C_TRACK);

    /* Title big, then artist, then album -- a row each.
     *
     * The joined "artist  -  album" string is gone, and with it the 96
     * byte buffer it was built in: the two tag fields are 64 bytes each,
     * so anything approaching full length was silently truncated, and it
     * truncated the album first only by luck of ordering.
     *
     * Album is dimmer than artist rather than the same grey. Three rows
     * of equal weight read as a paragraph; the hierarchy is what makes it
     * scannable at arm's length. */
    gfx_draw_text(16, s_bar_top + TEXT_Y, st->title, 4, s_w - 32, C_THUMB);
    if (st->artist && *st->artist) {
        gfx_draw_text(16, s_bar_top + ARTIST_Y, st->artist, 3, s_w - 32, C_ICON);
    }
    if (st->album && *st->album) {
        gfx_draw_text(16, s_bar_top + ALBUM_Y, st->album, 3, s_w - 32, C_ALBUM);
    }

    vol_bounds(&x0, &x1, &y);
    draw_slider(x0, x1, y, s_drag == 1 ? s_drag_pct : st->volume);

    draw_speaker();
    draw_folder();
    draw_moon();
    draw_play_pause(st->playing);

    gfx_blit(s_bar_top, s_h);

    /* Bubble last, and in its own band above the bar. Restoring the saved
     * strip first is what erases the previous position -- the alternative,
     * redrawing the whole cover, costs a full-screen blit per drag poll. */
    if (s_drag >= 0 && s_bubble_bg) {
        memcpy(&s_fb[s_bubble_top * s_w], s_bubble_bg,
               (size_t)s_w * s_bubble_h * sizeof(uint16_t));

        int bx = s_drag_x;
        if (bx < BUBBLE_R + 2) bx = BUBBLE_R + 2;
        if (bx > s_w - BUBBLE_R - 2) bx = s_w - BUBBLE_R - 2;
        const int by = bubble_cy();

        gfx_fill_circle(bx, by, BUBBLE_R, C_BUBBLE_BG);
        gfx_ring(bx, by, BUBBLE_R, 3, C_BUBBLE_ED);
        gfx_ring(bx, by, BUBBLE_R - 16, 7, C_TRACK);
        gfx_ring_arc(bx, by, BUBBLE_R - 16, 7, s_drag_pct, C_FILL);
        /* Seek reads as a time, volume as a percentage. A seek bubble
         * showing "46" is a number with no units on a bar whose two ends
         * are already clocks. */
        if (s_drag == 0 && st->len_sec > 0) {
            const uint32_t t = (uint32_t)((uint64_t)st->len_sec * s_drag_pct / 100);
            gfx_draw_small_time_centred(bx, by - GFX_SDIG_H / 2, t, C_THUMB);
        } else {
            gfx_draw_pct_centred(bx, by - GFX_SDIG_H / 2, s_drag_pct, C_THUMB);
        }

        gfx_blit(s_bubble_top, s_bubble_top + s_bubble_h);
        s_bubble_shown = true;
    }
}

/* ------------------------------------------------------------------ */

static bool in_box(int x, int y, int cx, int cy, int half)
{
    return x >= cx - half - HIT_PAD_X && x <= cx + half + HIT_PAD_X &&
           y >= cy - half - HIT_PAD_Y && y <= cy + half + HIT_PAD_Y;
}

static int pct_from_x(int x, int x0, int x1)
{
    if (x <= x0) return 0;
    if (x >= x1) return 100;
    return ((x - x0) * 100) / (x1 - x0);
}

ui_action_t ui_touch(const ui_state_t *st, bool down, int x, int y)
{
    ui_action_t act = { UI_ACTION_NONE, 0 };
    const bool tapped = down && !s_was_down;
    const bool released = !down && s_was_down;
    s_was_down = down;

    /* Screen off: the only thing a touch does is wake, and it does not
     * also press whatever is under it. Waking straight into a button
     * would let one tap turn the screen off again. */
    if (st->screen_off) {
        if (tapped) act.kind = UI_ACTION_SCREEN_ON;
        return act;
    }

    if (s_drag >= 0) {
        int x0, x1, sy;
        if (s_drag == 0) seek_bounds(&x0, &x1, &sy);
        else             vol_bounds(&x0, &x1, &sy);

        if (down) {
            s_drag_x = x;
            s_drag_y = y;
            s_drag_pct = pct_from_x(x, x0, x1);
            /* Volume tracks live -- you want to hear it while moving.
             * Seek does not: decoding to a new position on every poll
             * would thrash the SD card, so it fires once on release. */
            if (s_drag == 1) {
                act.kind = UI_ACTION_VOLUME;
                act.value = s_drag_pct;
            }
            return act;
        }

        if (released) {
            act.kind = (s_drag == 0) ? UI_ACTION_SEEK : UI_ACTION_VOLUME;
            act.value = s_drag_pct;
            s_drag = -1;
            return act;
        }
        s_drag = -1;
        return act;
    }

    if (!tapped) return act;

    int cx, cy;

    play_centre(&cx, &cy);
    if (in_box(x, y, cx, cy, BTN_R)) {
        act.kind = UI_ACTION_PLAY_PAUSE;
        return act;
    }

    folder_centre(&cx, &cy);
    if (in_box(x, y, cx, cy, ICON_HALF)) {
        act.kind = UI_ACTION_CHOOSE_FILE;
        return act;
    }

    moon_centre(&cx, &cy);
    if (in_box(x, y, cx, cy, ICON_HALF)) {
        act.kind = UI_ACTION_SCREEN_OFF;
        return act;
    }

    /* Sliders last: their padded hit boxes are wide, so a button landing
     * inside one has to win first. */
    int x0, x1, sy;
    vol_bounds(&x0, &x1, &sy);
    if (x >= x0 - HIT_PAD_X && x <= x1 + HIT_PAD_X &&
        y >= sy - HIT_PAD_Y && y <= sy + HIT_PAD_Y) {
        s_drag = 1;
        s_drag_x = x;
        s_drag_y = y;
        s_drag_pct = pct_from_x(x, x0, x1);
        act.kind = UI_ACTION_VOLUME;
        act.value = s_drag_pct;
        return act;
    }

    /*
     * Seek, but only when there is something to seek within.
     *
     * With no duration the bar has no scale, so a drag has nothing to
     * mean: the bubble showed a bare percentage, the thumb followed the
     * finger, and on release the player logged "seek ignored" and the
     * thumb snapped back. That is a control that looks live and is not,
     * which is worse than one that plainly does nothing.
     *
     * The test is seekability, not length. Those came apart the moment
     * duration.c started reading lengths out of containers: an Ogg has a
     * full, correct, moving seek bar and no way to seek within it, and
     * gating on len_sec let the drag through to a player that logged
     * "seek ignored" and snapped the thumb back.
     */
    if (!st->can_seek) return act;

    seek_bounds(&x0, &x1, &sy);
    if (x >= x0 - HIT_PAD_X && x <= x1 + HIT_PAD_X &&
        y >= sy - HIT_PAD_Y && y <= sy + HIT_PAD_Y) {
        s_drag = 0;
        s_drag_x = x;
        s_drag_y = y;
        s_drag_pct = pct_from_x(x, x0, x1);
        return act;
    }

    return act;
}
