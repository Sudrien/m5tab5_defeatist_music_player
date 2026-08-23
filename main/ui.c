/*
 * ui.c -- the transport bar.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "font8x8_basic.h"

#include "ui.h"

static const char *TAG = "tab5_ui";

/* RGB565. The framebuffer is RGB565 because that is what panel_init()
 * configures and what albumart.c writes. */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BG        RGB(0x11, 0x11, 0x11)
#define C_TRACK     RGB(0x3A, 0x3A, 0x3A)   /* unplayed / unfilled */
#define C_FILL      RGB(0xD1, 0x3B, 0x2C)   /* played / set volume */
#define C_THUMB     RGB(0xFF, 0xFF, 0xFF)
#define C_ICON      RGB(0xCC, 0xCC, 0xCC)
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
#define TEXT_Y      (18)   /* title row, top of the bar */
#define SUB_Y       (68)   /* artist / album */
#define SEEK_Y      (134)
#define SEEK_X0     (132)  /* clears the MM:SS run at either end */
#define ROW_Y       (214)
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

static esp_lcd_panel_handle_t s_panel;
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
/* Primitives                                                          */
/* ------------------------------------------------------------------ */

static inline void px(int x, int y, uint16_t c)
{
    if (x < 0 || x >= s_w || y < 0 || y >= s_h) return;
    s_fb[y * s_w + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint16_t *row = &s_fb[(y + r) * s_w + x];
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void fill_circle(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        const int span = (int)(0.5f + __builtin_sqrtf((float)(r * r - dy * dy)));
        fill_rect(cx - span, cy + dy, 2 * span + 1, 1, c);
    }
}

static void ring(int cx, int cy, int r, int thick, uint16_t c)
{
    const int inner = r - thick;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 >= inner * inner) px(cx + dx, cy + dy, c);
        }
    }
}

/* Arc from 12 o'clock, clockwise, covering pct of the circle. Used only
 * by the finger bubble, where it shows the value being dragged without
 * needing a font. */
static void ring_arc(int cx, int cy, int r, int thick, int pct, uint16_t c)
{
    if (pct <= 0) return;
    if (pct > 100) pct = 100;
    const int inner = r - thick;
    /* 1024ths of a turn, integer only -- no atan2f per pixel. */
    const int limit = (pct * 1024) / 100;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > r * r || d2 < inner * inner) continue;
            /* Angle from 12 o'clock, clockwise, in 1024ths. Quadrant
             * dispatch plus a linear ramp inside each -- close enough for
             * a 46 px indicator and free of floating point. */
            const int ax = dx, ay = -dy;
            int a;
            const int adx = ax < 0 ? -ax : ax;
            const int ady = ay < 0 ? -ay : ay;
            const int denom = adx + ady;
            if (denom == 0) continue;
            const int frac = (adx * 256) / denom;   /* 0 at vertical */
            if (ax >= 0 && ay >= 0)      a = frac;              /* 0..256   */
            else if (ax >= 0)            a = 512 - frac;        /* 256..512 */
            else if (ay < 0)             a = 512 + frac;        /* 512..768 */
            else                         a = 1024 - frac;       /* 768..1024*/
            if (a <= limit) px(cx + dx, cy + dy, c);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Seven-segment digits                                                */
/* ------------------------------------------------------------------ */

/*
 * No font is linked, and vendoring one for two timestamps is not worth
 * it. Seven segments cover 0-9 and a colon, which is the whole of MM:SS.
 *
 * Segment order: a top, b top-right, c bottom-right, d bottom,
 * e bottom-left, f top-left, g middle.
 */
static const uint8_t k_seg[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

#define DIG_W       (20)
#define DIG_H       (38)
#define DIG_T       (5)     /* segment thickness */
#define DIG_GAP     (5)

static void draw_digit(int x, int y, int n, uint16_t c)
{
    if (n < 0 || n > 9) return;
    const uint8_t m = k_seg[n];
    const int w = DIG_W, h = DIG_H, t = DIG_T;
    const int mid = y + h / 2;

    if (m & 0x01) fill_rect(x, y, w, t, c);                       /* a */
    if (m & 0x02) fill_rect(x + w - t, y, t, h / 2, c);           /* b */
    if (m & 0x04) fill_rect(x + w - t, mid, t, h / 2, c);         /* c */
    if (m & 0x08) fill_rect(x, y + h - t, w, t, c);               /* d */
    if (m & 0x10) fill_rect(x, mid, t, h / 2, c);                 /* e */
    if (m & 0x20) fill_rect(x, y, t, h / 2, c);                   /* f */
    if (m & 0x40) fill_rect(x, mid - t / 2, w, t, c);             /* g */
}

/* Smaller variant for the finger bubble, where a 24 px digit will not fit
 * inside the ring. Same segment table, different geometry. */
#define SDIG_W      (14)
#define SDIG_H      (26)
#define SDIG_T      (3)
#define SDIG_GAP    (4)

static void draw_small_digit(int x, int y, int n, uint16_t c)
{
    if (n < 0 || n > 9) return;
    const uint8_t m = k_seg[n];
    const int w = SDIG_W, h = SDIG_H, t = SDIG_T;
    const int mid = y + h / 2;

    if (m & 0x01) fill_rect(x, y, w, t, c);
    if (m & 0x02) fill_rect(x + w - t, y, t, h / 2, c);
    if (m & 0x04) fill_rect(x + w - t, mid, t, h / 2, c);
    if (m & 0x08) fill_rect(x, y + h - t, w, t, c);
    if (m & 0x10) fill_rect(x, mid, t, h / 2, c);
    if (m & 0x20) fill_rect(x, y, t, h / 2, c);
    if (m & 0x40) fill_rect(x, mid - t / 2, w, t, c);
}

/* A percentage, centred on cx. 100 needs three digits, 0-99 needs two, and
 * the run is centred either way so the number does not shuffle sideways as
 * it crosses 100. */
static void draw_pct_centred(int cx, int y, int pct, uint16_t c)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    const int digits = (pct >= 100) ? 3 : 2;
    const int run = digits * SDIG_W + (digits - 1) * SDIG_GAP;
    int x = cx - run / 2;

    if (digits == 3) {
        draw_small_digit(x, y, 1, c);
        x += SDIG_W + SDIG_GAP;
        draw_small_digit(x, y, 0, c);
        x += SDIG_W + SDIG_GAP;
        draw_small_digit(x, y, 0, c);
    } else {
        draw_small_digit(x, y, pct / 10, c);
        x += SDIG_W + SDIG_GAP;
        draw_small_digit(x, y, pct % 10, c);
    }
}

/* MM:SS in the small digits, centred. Used by the seek bubble. */
static void draw_small_time_centred(int cx, int y, uint32_t sec, uint16_t c)
{
    uint32_t m = sec / 60;
    const uint32_t s2 = sec % 60;
    if (m > 99) m = 99;

    const int run = 4 * SDIG_W + 3 * SDIG_GAP + SDIG_W / 2 + SDIG_GAP;
    int x = cx - run / 2;

    draw_small_digit(x, y, (int)(m / 10), c);
    x += SDIG_W + SDIG_GAP;
    draw_small_digit(x, y, (int)(m % 10), c);
    x += SDIG_W + SDIG_GAP;

    fill_rect(x + 1, y + SDIG_H / 3, SDIG_T, SDIG_T, c);
    fill_rect(x + 1, y + (2 * SDIG_H) / 3, SDIG_T, SDIG_T, c);
    x += SDIG_W / 2 + SDIG_GAP;

    draw_small_digit(x, y, (int)(s2 / 10), c);
    x += SDIG_W + SDIG_GAP;
    draw_small_digit(x, y, (int)(s2 % 10), c);
}

/* Width of an MM:SS run, so callers can right-align without guessing. */
#define TIME_W  (4 * DIG_W + 3 * DIG_GAP + DIG_W / 2 + DIG_GAP)

static void draw_time(int x, int y, uint32_t sec, uint16_t c)
{
    uint32_t m = sec / 60;
    const uint32_t s2 = sec % 60;
    if (m > 99) m = 99;

    draw_digit(x, y, (int)(m / 10), c);
    x += DIG_W + DIG_GAP;
    draw_digit(x, y, (int)(m % 10), c);
    x += DIG_W + DIG_GAP;

    fill_rect(x + 1, y + DIG_H / 3, DIG_T, DIG_T, c);
    fill_rect(x + 1, y + (2 * DIG_H) / 3, DIG_T, DIG_T, c);
    x += DIG_W / 2 + DIG_GAP;

    draw_digit(x, y, (int)(s2 / 10), c);
    x += DIG_W + DIG_GAP;
    draw_digit(x, y, (int)(s2 % 10), c);
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

/*
 * font8x8_basic is ASCII 0-127, one byte per row, LSB leftmost. Scaled by
 * an integer factor because 8 px is unreadable on a 5" 720x1280 panel --
 * scale 2 is the title row, scale 1 is artist and album.
 */
#define GLYPH_W(scale)  (8 * (scale) + (scale))     /* one column of gap */

static void draw_char(int x, int y, char ch, int scale, uint16_t c)
{
    const unsigned char u = (unsigned char)ch;
    if (u > 127) return;
    const char *g = font8x8_basic[u];

    for (int row = 0; row < 8; row++) {
        const unsigned char bits = (unsigned char)g[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1 << col))) continue;
            fill_rect(x + col * scale, y + row * scale, scale, scale, c);
        }
    }
}

/* Draws left-aligned, clipped to max_w, with an ellipsis when it does not
 * fit. Returns nothing: there is no reflow and no second line, because a
 * title long enough to need one is a title the user already knows. */
static void draw_text(int x, int y, const char *s, int scale, int max_w, uint16_t c)
{
    if (!s || !*s) return;
    const int gw = GLYPH_W(scale);
    const int room = max_w / gw;
    const int len = (int)strlen(s);

    if (len <= room) {
        for (int i = 0; i < len; i++) draw_char(x + i * gw, y, s[i], scale, c);
        return;
    }
    if (room < 4) return;
    for (int i = 0; i < room - 3; i++) draw_char(x + i * gw, y, s[i], scale, c);
    for (int i = room - 3; i < room; i++) draw_char(x + i * gw, y, '.', scale, c);
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
    fill_rect(x0, y - TRACK_THIN / 2, w, TRACK_THIN, C_TRACK);
    fill_rect(x0, y - TRACK_THICK / 2, split - x0, TRACK_THICK, C_FILL);
    fill_circle(split, y, THUMB_R, C_THUMB);
}

static void draw_play_pause(bool playing)
{
    int cx, cy;
    play_centre(&cx, &cy);
    fill_circle(cx, cy, BTN_R, C_THUMB);

    if (playing) {
        /* Pause: two bars. */
        fill_rect(cx - 15, cy - 20, 10, 40, C_BG);
        fill_rect(cx + 5, cy - 20, 10, 40, C_BG);
    } else {
        /* Play: a triangle, nudged right so it looks centred rather than
         * measuring centred. */
        for (int dy = -20; dy <= 20; dy++) {
            const int a = dy < 0 ? -dy : dy;
            fill_rect(cx - 11, cy + dy, 34 - (a * 34) / 20, 1, C_BG);
        }
    }
}

static void draw_folder(void)
{
    int cx, cy;
    folder_centre(&cx, &cy);
    fill_rect(cx - ICON_HALF, cy - 18, 21, 7, C_ICON);          /* tab */
    fill_rect(cx - ICON_HALF, cy - 12, 2 * ICON_HALF, 32, C_ICON);
    fill_rect(cx - ICON_HALF + 4, cy - 7, 2 * ICON_HALF - 8, 23, C_BG);
}

static void draw_moon(void)
{
    int cx, cy;
    moon_centre(&cx, &cy);
    fill_circle(cx, cy, ICON_HALF, C_ICON);
    fill_circle(cx + 13, cy - 10, ICON_HALF, C_BG); /* bite out the crescent */
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

    fill_rect(cx - 13, cy - 6, 9, 13, C_ICON);      /* body */
    for (int dx = 0; dx <= 15; dx++) {              /* cone, flaring right */
        const int half = 4 + dx;
        fill_rect(cx - 4 + dx, cy - half, 1, 2 * half + 1, C_ICON);
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

static void bubble_restore(void)
{
    if (!s_bubble_bg || !s_bubble_shown) return;
    memcpy(&s_fb[s_bubble_top * s_w], s_bubble_bg,
           (size_t)s_w * s_bubble_h * sizeof(uint16_t));
    esp_lcd_panel_draw_bitmap(s_panel, 0, s_bubble_top,
                              s_w, s_bubble_top + s_bubble_h, s_fb);
    s_bubble_shown = false;
}

esp_err_t ui_init(esp_lcd_panel_handle_t panel, int w, int h)
{
    s_panel = panel;
    s_w = w;
    s_h = h;
    s_bar_top = h - UI_BAR_H;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, (void **)&s_fb),
                        TAG, "get fb");
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

    fill_rect(0, s_bar_top, s_w, UI_BAR_H, C_BG);

    int x0, x1, y;
    seek_bounds(&x0, &x1, &y);
    const int pos_pct = (st->len_sec > 0)
                      ? (int)((st->pos_sec * 100) / st->len_sec)
                      : 0;
    draw_slider(x0, x1, y, s_drag == 0 ? s_drag_pct : pos_pct);

    /* Elapsed left of the bar, total right of it. Total reads 00:00 when
     * the backend could not supply a duration, which is the honest
     * rendering of "unknown" and matches the empty bar next to it. */
    draw_time(10, y - DIG_H / 2, st->pos_sec, C_ICON);
    draw_time(s_w - TIME_W - 10, y - DIG_H / 2, st->len_sec, C_TRACK);

    /* Title big, artist and album small underneath on one line. The two
     * are joined here rather than given a row each: three stacked rows
     * pushed the bar over 200 px, and artist alone is the part people
     * actually read. */
    draw_text(16, s_bar_top + TEXT_Y, st->title, 5, s_w - 32, C_THUMB);
    if (st->artist && *st->artist) {
        char sub[96];
        if (st->album && *st->album) {
            snprintf(sub, sizeof(sub), "%s  -  %s", st->artist, st->album);
        } else {
            snprintf(sub, sizeof(sub), "%s", st->artist);
        }
        draw_text(16, s_bar_top + SUB_Y, sub, 3, s_w - 32, C_ICON);
    }

    vol_bounds(&x0, &x1, &y);
    draw_slider(x0, x1, y, s_drag == 1 ? s_drag_pct : st->volume);

    draw_speaker();
    draw_folder();
    draw_moon();
    draw_play_pause(st->playing);

    esp_lcd_panel_draw_bitmap(s_panel, 0, s_bar_top, s_w, s_h, s_fb);

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

        fill_circle(bx, by, BUBBLE_R, C_BUBBLE_BG);
        ring(bx, by, BUBBLE_R, 3, C_BUBBLE_ED);
        ring(bx, by, BUBBLE_R - 16, 7, C_TRACK);
        ring_arc(bx, by, BUBBLE_R - 16, 7, s_drag_pct, C_FILL);
        /* Seek reads as a time, volume as a percentage. A seek bubble
         * showing "46" is a number with no units on a bar whose two ends
         * are already clocks. */
        if (s_drag == 0 && st->len_sec > 0) {
            const uint32_t t = (uint32_t)((uint64_t)st->len_sec * s_drag_pct / 100);
            draw_small_time_centred(bx, by - SDIG_H / 2, t, C_THUMB);
        } else {
            draw_pct_centred(bx, by - SDIG_H / 2, s_drag_pct, C_THUMB);
        }

        esp_lcd_panel_draw_bitmap(s_panel, 0, s_bubble_top,
                                  s_w, s_bubble_top + s_bubble_h, s_fb);
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
