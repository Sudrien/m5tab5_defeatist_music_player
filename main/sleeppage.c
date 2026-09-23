/*
 * sleeppage.c -- see sleeppage.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sleeppage.h"

#include "esp_log.h"
#include "gfx.h"
#include "menuscroll.h"
#include "settings.h"
#include "sleeptimer.h"

#include <stdio.h>

static const char *TAG = "tab5_sleep";

/* panel.c's, copied -- see sleeppage.h. */
#define C_BG        RGB(0x0C, 0x0C, 0x0C)
#define C_ROW       RGB(0x1A, 0x1A, 0x1A)
#define C_TEXT      RGB(0xEE, 0xEE, 0xEE)
#define C_DIM       RGB(0x77, 0x77, 0x77)
#define C_BTN       RGB(0x26, 0x26, 0x26)
#define C_ON        RGB(0x3C, 0xB3, 0x71)
#define C_ACCENT    RGB(0xD1, 0x3B, 0x2C)
#define C_FAINT     RGB(0x55, 0x55, 0x55)
#define C_RULE      RGB(0x33, 0x33, 0x33)

/*
 * The footer, and the CLOSE button inside it.
 *
 * ONE function for the button's rectangle, read by both the drawing and
 * the hit test. browser.c's scrollbar learned this the expensive way --
 * "drawing and hit-testing computed this independently in the first
 * version of this patch and it was wrong within a day" -- and this file
 * had a worse version of the same fault: the button was drawn 180 px
 * wide and centred, and the hit test was the ENTIRE full-width strip.
 * On a 1280-wide landscape screen that made 86% of the bar an invisible
 * close button, which is exactly how it was reported from a board.
 */
#define FOOT_BTN_W      (180)
#define FOOT_BTN_PAD    (16)

#define HEAD_H      (96)            /* where panel.c has its tab strip */
#define LIST_TOP    (HEAD_H + 24)
#define FOOT_H      (120)
#define ROW_H       (64)
#define LABEL_SCALE (2)
#define NAME_SCALE  (3)
#define OPTION_H    (ROW_H + 24)    /* panel.c's AUDIO_SWITCH_H */
#define NOTE_GAP    (14)
#define NOTE_STEP   (GFX_GLYPH_H(LABEL_SCALE) + 12)
#define NOTE_LINES  (2)
#define GAP         (36)            /* panel.c's AUDIO_GAP */
#define SLIDER_H    (ROW_H + 96)    /* panel.c's AUDIO_SLIDER_H */
#define SLIDER_INSET (24)
#define SLIDER_KNOB  (28)

static void close_box(int *x, int *y, int *w, int *h)
{
    *w = FOOT_BTN_W;
    *h = FOOT_H - 2 * FOOT_BTN_PAD;
    *x = gfx_w() / 2 - FOOT_BTN_W / 2;
    *y = gfx_h() - FOOT_H + FOOT_BTN_PAD;
}

static bool s_open;
static bool s_dirty;
static bool s_was_down;
/*
 * What the Screen switch shows. Always ON when the page opens -- a page
 * you can see is a screen that is on -- and OFF from the tap until the
 * caller has faded the backlight out and closed the page. Nothing else
 * can switch it back, so there is no OFF-to-ON path here: waking is a
 * touch on a dark screen, which ui.c owns.
 */
static bool s_screen_on = true;
/*
 * The scroll. In portrait the page fits with room to spare and this
 * stays at zero and draws nothing; rotated, the same content is 860 px
 * tall in a 504 px viewport and the timer row is simply unreachable
 * without it.
 */
static menuscroll_t s_scroll;
static int s_content_h;     /* set by the draw, read by the touch */
/* A drag on the brightness slider, held across polls -- panel.c's
 * crossfade slider, for panel.c's reason. */
static bool s_drag;

/* The timer: what player.c says is running, and a drag's position. */
static int     s_timer_step;
static int64_t s_timer_left = -1;
static int     s_drag_step;
static bool    s_timer_drag;

bool sleeppage_is_open(void) { return s_open; }

void sleeppage_open(void)
{
    s_open = true;
    s_dirty = true;
    s_was_down = false;
    s_screen_on = true;
    s_drag = false;
    s_timer_drag = false;
    s_scroll.off = 0;
    s_scroll.drag = false;
}

void sleeppage_close(void)
{
    s_open = false;
}

/*
 * The options, top to bottom. One today. The sleep timer's rows go below
 * it, which is why this is a list of boxes counted from LIST_TOP and not
 * one box.
 */
/*
 * The band between the header and the footer. Everything below is laid
 * out from list_top() and so moves as one when the scroll changes --
 * the boxes chain off each other precisely so there is one place to
 * subtract it.
 */
static int view_y(void)  { return LIST_TOP; }
static int view_h(void)  { return gfx_h() - FOOT_H - LIST_TOP; }
static int list_top(void)
{
    return view_y() - menuscroll_clamp(s_scroll.off, s_content_h, view_h());
}

static void screen_box(int *x, int *y, int *w, int *h)
{
    *x = 0; *y = list_top(); *w = gfx_w(); *h = OPTION_H;
}

static void brightness_box(int *x, int *y, int *w, int *h)
{
    int sx, sy, sw, sh;
    screen_box(&sx, &sy, &sw, &sh);
    *x = 0;
    *y = sy + sh + NOTE_GAP + NOTE_LINES * NOTE_STEP + GAP;
    *w = gfx_w();
    *h = SLIDER_H;
}

static void slider_track(int *x0, int *x1)
{
    *x0 = SLIDER_INSET + SLIDER_KNOB;
    *x1 = gfx_w() - SLIDER_INSET - SLIDER_KNOB;
}

/*
 * Rotation sits under Brightness rather than under the timer: the three
 * rows above it are what the screen is doing, and the timer is what the
 * music is about to stop doing. Grouping by that is why it is not simply
 * appended to the bottom.
 */
#define ROT_NOTE_LINES  (1)

static void rotation_box(int *x, int *y, int *w, int *h)
{
    int bx, by, bw, bh;
    brightness_box(&bx, &by, &bw, &bh);
    *x = 0;
    *y = by + bh + GAP;
    *w = gfx_w();
    *h = OPTION_H;
}

static void timer_box(int *x, int *y, int *w, int *h)
{
    int rx, ry, rw, rh;
    rotation_box(&rx, &ry, &rw, &rh);
    *x = 0;
    *y = ry + rh + NOTE_GAP + ROT_NOTE_LINES * NOTE_STEP + GAP;
    *w = gfx_w();
    *h = SLIDER_H;
}

/* A little air under the last note, so the end of the content does not
 * sit flush against the footer. */
#define BOT_PAD (24)

/*
 * How tall the content is, measured from list_top(). The scroll cancels
 * -- every box chains off list_top() and this subtracts it again -- so
 * this may be called before s_content_h is right and still be right.
 */
static void layout(void)
{
    int x, y, w, h;
    timer_box(&x, &y, &w, &h);
    s_content_h = (y + h + NOTE_GAP + NOTE_STEP + BOT_PAD) - list_top();
}

void sleeppage_set_timer(int step, int64_t seconds_left)
{
    if (step != s_timer_step || seconds_left != s_timer_left) {
        s_timer_step = step;
        s_timer_left = seconds_left;
        s_dirty = true;
    }
}

int sleeppage_timer_step(void) { return s_drag_step; }

void sleeppage_draw(void)
{
    if (!s_open || !s_dirty) return;
    s_dirty = false;

    layout();
    s_scroll.off = menuscroll_clamp(s_scroll.off, s_content_h, view_h());

    const int w = gfx_w(), h = gfx_h();
    gfx_fill_rect(0, 0, w, h, C_BG);

    int x, y, bw, bh;
    screen_box(&x, &y, &bw, &bh);
    gfx_fill_rect(x, y, bw, bh, C_ROW);
    gfx_draw_text(24, y + (bh - GFX_GLYPH_H(NAME_SCALE)) / 2, "Screen",
                  NAME_SCALE, 400, C_TEXT);
    {
        /* panel.c's pill, same size and place. */
        const int pw = 132, ph = 56;
        const int px = w - 24 - pw, py = y + (bh - ph) / 2;
        const char *text = s_screen_on ? "ON" : "OFF";
        gfx_fill_rect(px, py, pw, ph, s_screen_on ? C_ON : C_BTN);
        const int tw = gfx_text_w(text, NAME_SCALE);
        gfx_draw_text(px + (pw - tw) / 2, py + (ph - GFX_GLYPH_H(NAME_SCALE)) / 2,
                      text, NAME_SCALE, pw - 8, s_screen_on ? C_BG : C_DIM);
    }
    {
        static const char *const note[] = {
            "Off fades the backlight out. Playback carries on.",
            "Touch anywhere to wake it.",
        };
        int ny = y + bh + NOTE_GAP;
        for (size_t i = 0; i < sizeof(note) / sizeof(note[0]); i++) {
            gfx_draw_text(24, ny, note[i], LABEL_SCALE, w - 48, C_DIM);
            ny += NOTE_STEP;
        }
    }

    /* --- Brightness ------------------------------------------------ */
    brightness_box(&x, &y, &bw, &bh);
    gfx_fill_rect(x, y, bw, bh, C_ROW);
    {
        const int b = settings_brightness();
        char head[32];
        snprintf(head, sizeof(head), "Brightness   %d%%", b);
        gfx_draw_text(24, y + 20, head, NAME_SCALE, w - 48, C_TEXT);

        const int span = SETTINGS_BRIGHTNESS_MAX - SETTINGS_BRIGHTNESS_MIN;
        int tx0, tx1;
        slider_track(&tx0, &tx1);
        const int ty = y + bh - 44;
        const int kx = tx0 + ((tx1 - tx0) * (b - SETTINGS_BRIGHTNESS_MIN)) / span;
        gfx_fill_rect(tx0, ty - 3, tx1 - tx0, 6, C_BTN);
        gfx_fill_rect(tx0, ty - 3, kx - tx0, 6, C_ACCENT);
        gfx_fill_circle(kx, ty, SLIDER_KNOB / 2, C_TEXT);

        char lo[8];
        snprintf(lo, sizeof(lo), "%d%%", SETTINGS_BRIGHTNESS_MIN);
        gfx_draw_text(SLIDER_INSET, ty + SLIDER_KNOB, lo, LABEL_SCALE, 80, C_FAINT);
        const int mw = gfx_text_w("100%", LABEL_SCALE);
        gfx_draw_text(w - SLIDER_INSET - mw, ty + SLIDER_KNOB, "100%",
                      LABEL_SCALE, 80, C_FAINT);
    }

    /* --- Rotation --------------------------------------------------- */
    rotation_box(&x, &y, &bw, &bh);
    {
        const int rot = settings_screen_rotation();
        gfx_fill_rect(x, y, bw, bh, C_ROW);
        gfx_draw_text(24, y + (bh - GFX_GLYPH_H(NAME_SCALE)) / 2, "Rotation",
                      NAME_SCALE, 400, C_TEXT);

        /* The Screen row's pill, same size and place. Wider text than
         * ON/OFF, so it is the same box with a smaller scale rather than
         * a box that does not line up with the one above. */
        const int pw = 132, ph = 56;
        const int px = w - 24 - pw, py = y + (bh - ph) / 2;
        /* Lit for any turn, not just 180: the pill says "not as it
         * shipped", and the number beside it says which turn. */
        static const char *const names[4] = { "0", "90", "180", "270" };
        const char *text = names[rot & 3];
        gfx_fill_rect(px, py, pw, ph, rot ? C_ON : C_BTN);
        const int tw = gfx_text_w(text, NAME_SCALE);
        gfx_draw_text(px + (pw - tw) / 2, py + (ph - GFX_GLYPH_H(NAME_SCALE)) / 2,
                      text, NAME_SCALE, pw - 8, rot ? C_BG : C_DIM);
    }
    {
        static const char *const note[] = {
            "Quarter turns. 90 and 270 are landscape; 180 is for when "
            "the cable is at the wrong end.",
        };
        gfx_draw_text(24, y + bh + NOTE_GAP, note[0], LABEL_SCALE, w - 48, C_DIM);
    }

    /* --- Sleep timer ----------------------------------------------- */
    timer_box(&x, &y, &bw, &bh);
    gfx_fill_rect(x, y, bw, bh, C_ROW);
    {
        /* While dragging, the finger's position; otherwise what runs. */
        const int step = s_timer_drag ? s_drag_step : s_timer_step;
        char head[48];
        if (step == 0) {
            snprintf(head, sizeof(head), "Sleep timer   off");
        } else if (!s_timer_drag && s_timer_left > 0) {
            const long m = (long)(s_timer_left / 60), sec = (long)(s_timer_left % 60);
            snprintf(head, sizeof(head), "Sleep timer   %ld:%02ld", m, sec);
        } else {
            snprintf(head, sizeof(head), "Sleep timer   %d min", sleeptimer_minutes(step));
        }
        gfx_draw_text(24, y + 20, head, NAME_SCALE, w - 48, step ? C_TEXT : C_DIM);

        int tx0, tx1;
        slider_track(&tx0, &tx1);
        const int ty = y + bh - 44;
        const int kx = tx0 + ((tx1 - tx0) * step) / SLEEPTIMER_STEPS;
        gfx_fill_rect(tx0, ty - 3, tx1 - tx0, 6, C_BTN);
        /* A tick per step, so the notches are visible before dragging. */
        for (int i = 0; i <= SLEEPTIMER_STEPS; i++) {
            gfx_fill_rect(tx0 + ((tx1 - tx0) * i) / SLEEPTIMER_STEPS - 1, ty - 9, 2, 18, C_BTN);
        }
        if (step) gfx_fill_rect(tx0, ty - 3, kx - tx0, 6, C_ACCENT);
        gfx_fill_circle(kx, ty, SLIDER_KNOB / 2, step ? C_TEXT : C_DIM);

        gfx_draw_text(SLIDER_INSET, ty + SLIDER_KNOB, "off", LABEL_SCALE, 80, C_FAINT);
        const int mw = gfx_text_w("2 h", LABEL_SCALE);
        gfx_draw_text(w - SLIDER_INSET - mw, ty + SLIDER_KNOB, "2 h",
                      LABEL_SCALE, 80, C_FAINT);
    }
    {
        static const char *const note[] = {
            "Fades out, pauses, and turns the screen off.",
        };
        gfx_draw_text(24, y + bh + NOTE_GAP, note[0], LABEL_SCALE, w - 48, C_DIM);
    }

    /*
     * Header, AFTER the rows. There is no clip in gfx, so this is the
     * clip: the content is drawn as though the page were unbounded and
     * the opaque header covers whatever ran off the top of the
     * viewport. The footer does the same at the other end.
     */
    gfx_fill_rect(0, 0, w, LIST_TOP, C_BG);
    gfx_draw_text(24, (HEAD_H - GFX_GLYPH_H(NAME_SCALE)) / 2, "Sleep",
                  NAME_SCALE, w - 48, C_TEXT);
    gfx_fill_rect(0, LIST_TOP - 2, w, 2, C_RULE);

    {
        int bar_y, bar_h;
        if (menuscroll_geom(&s_scroll, s_content_h, view_y(), view_h(),
                            &bar_y, &bar_h)) {
            gfx_fill_rect(w - MENUSCROLL_W, view_y(), MENUSCROLL_W, view_h(),
                          C_ROW);
            gfx_fill_rect(w - MENUSCROLL_W, bar_y, MENUSCROLL_W, bar_h,
                          s_scroll.drag ? C_TEXT : C_DIM);
        }
    }

    /*
     * Footer: panel.c's, one button, the way out.
     *
     * FILLED, not just ruled. This used to draw a 2 px rule and the
     * button, leaving the rest of the strip transparent -- so in
     * landscape, where the content runs 140 px past the bottom, the
     * rotation note rendered straight through the bar and came out
     * either side of the button. Drawn last and opaque, it is a bar.
     */
    const int fy = h - FOOT_H;
    gfx_fill_rect(0, fy, w, FOOT_H, C_BG);
    gfx_fill_rect(0, fy, w, 2, C_RULE);

    int cbx, cby, cbw, cbh;
    close_box(&cbx, &cby, &cbw, &cbh);
    gfx_fill_rect(cbx, cby, cbw, cbh, C_BTN);
    const int cw = gfx_text_w("CLOSE", LABEL_SCALE);
    gfx_draw_text(cbx + (cbw - cw) / 2,
                  cby + (cbh - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                  "CLOSE", LABEL_SCALE, cbw - 8, C_TEXT);

    gfx_blit(0, h);
}

sleeppage_result_t sleeppage_touch(bool down, int x, int y)
{
    const bool tapped = down && !s_was_down;
    s_was_down = down;

    if (!s_open) return SLEEPPAGE_NONE;

    layout();

    /*
     * The scrollbar, before the sliders and outside the tapped test: a
     * drag is a run of downs with one edge at the front. It wins over
     * the rows it overlaps -- that is what the strip is for -- and a
     * press in it never reaches them.
     */
    {
        if (menuscroll_touch(&s_scroll, down, tapped, x, y, gfx_w(),
                             s_content_h, view_y(), view_h())) {
            s_dirty = true;
            return SLEEPPAGE_NONE;
        }
    }

    /*
     * Rows begin at a press inside the viewport and nowhere else. A
     * drag already under way carries on wherever the finger goes --
     * that is what the s_drag / s_timer_drag halves of these tests are
     * -- but a row scrolled under the header or the footer is not a
     * target, and without this a press on the bar would start the
     * slider hidden behind it.
     */
    const bool in_view = (y >= view_y() && y < view_y() + view_h());

    /*
     * The slider first, before the tapped test: a drag is a run of downs
     * with one edge at the front, and applied on every move so the
     * backlight follows the finger. Snapped to whole percent on the way
     * in, so the knob only sits where a setting exists.
     */
    {
        int sx, sy, sw, sh;
        brightness_box(&sx, &sy, &sw, &sh);
        if (s_drag || (tapped && in_view && y >= sy && y < sy + sh)) {
            if (down) {
                int tx0, tx1;
                slider_track(&tx0, &tx1);
                int pos = x - tx0;
                if (pos < 0) pos = 0;
                if (pos > tx1 - tx0) pos = tx1 - tx0;
                const int spanpx = tx1 - tx0;
                const int span = SETTINGS_BRIGHTNESS_MAX - SETTINGS_BRIGHTNESS_MIN;
                const int want = SETTINGS_BRIGHTNESS_MIN +
                                 (spanpx > 0 ? (pos * span + spanpx / 2) / spanpx : 0);
                s_drag = true;
                if (want != settings_brightness()) {
                    settings_set_brightness((uint8_t)want);
                    s_dirty = true;
                    return SLEEPPAGE_BRIGHTNESS;
                }
                return SLEEPPAGE_NONE;
            }
            if (s_drag) {
                s_drag = false;
                /* player.c logs position and duty together. */
                return SLEEPPAGE_BRIGHTNESS_DONE;
            }
        }
    }

    /* The timer slider, the same way: snapped to whole steps as the
     * finger moves, and started only on release, so dragging across
     * "2 h" on the way to "30 min" never runs a two-hour timer. */
    {
        int sx, sy, sw, sh;
        timer_box(&sx, &sy, &sw, &sh);
        if (s_timer_drag || (tapped && in_view && y >= sy && y < sy + sh)) {
            if (down) {
                int tx0, tx1;
                slider_track(&tx0, &tx1);
                int pos = x - tx0;
                if (pos < 0) pos = 0;
                if (pos > tx1 - tx0) pos = tx1 - tx0;
                const int spanpx = tx1 - tx0;
                const int step = spanpx > 0
                    ? (pos * SLEEPTIMER_STEPS + spanpx / 2) / spanpx : 0;
                if (!s_timer_drag || step != s_drag_step) s_dirty = true;
                s_timer_drag = true;
                s_drag_step = step;
                return SLEEPPAGE_NONE;
            }
            if (s_timer_drag) {
                s_timer_drag = false;
                s_dirty = true;
                return SLEEPPAGE_TIMER;
            }
        }
    }

    if (!tapped) return SLEEPPAGE_NONE;

    /*
     * The footer strip SINKS every press in it, and only the button
     * acts. Two separate properties and both were missing:
     *
     *  - the button is the target, not the strip, so the 1100 px of bar
     *    either side of it no longer closes the page;
     *  - and a press anywhere else in the strip stops here rather than
     *    falling through to the rows. That matters in landscape, where
     *    the content overflows: rotation_box() spans 526..614 and the
     *    strip starts at 600, so 14 px of bar sit directly over the
     *    rotation row. Falling through would cycle the screen angle
     *    from a tap on what is visibly a footer.
     */
    if (y >= gfx_h() - FOOT_H) {
        int cbx, cby, cbw, cbh;
        close_box(&cbx, &cby, &cbw, &cbh);
        if (x >= cbx && x < cbx + cbw && y >= cby && y < cby + cbh) {
            ESP_LOGI(TAG, "button: close");
            return SLEEPPAGE_CLOSE;
        }
        return SLEEPPAGE_NONE;
    }

    int bx, by, bw, bh;

    if (!in_view) return SLEEPPAGE_NONE;

    /*
     * Rotation, before the Screen row, because both are whole-row
     * targets and the Screen row's test is the one that turns the
     * backlight off -- a row that fell through to it would be a
     * misplaced tap with a consequence.
     *
     * The page does not turn the screen over itself; it records the
     * setting and reports it, the same way the Screen switch does not
     * fade its own backlight. The caller's repaint is what makes it
     * visible.
     */
    rotation_box(&bx, &by, &bw, &bh);
    if (y >= by && y < by + bh) {
        /* Four angles now, so the tap cycles rather than toggles:
         * 0, 90, 180, 270 and round. A switch with four positions is
         * still a switch -- the pill shows which one it is on. */
        const int want = (settings_screen_rotation() + 1) & 3;
        settings_set_screen_rotation(want);
        ESP_LOGI(TAG, "rotation: %d", want * 90);
        s_dirty = true;
        return SLEEPPAGE_FLIP;
    }

    /* Whole row, not a pill, for panel.c's reason: a row is the target a
     * thumb actually hits. */
    screen_box(&bx, &by, &bw, &bh);
    if (y >= by && y < by + bh && s_screen_on) {
        ESP_LOGI(TAG, "screen off");
        s_screen_on = false;
        s_dirty = true;
        return SLEEPPAGE_SCREEN_OFF;
    }
    return SLEEPPAGE_NONE;
}
