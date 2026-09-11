/*
 * sleeppage.c -- see sleeppage.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sleeppage.h"

#include "esp_log.h"
#include "gfx.h"
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
static void screen_box(int *x, int *y, int *w, int *h)
{
    *x = 0; *y = LIST_TOP; *w = gfx_w(); *h = OPTION_H;
}

static void brightness_box(int *x, int *y, int *w, int *h)
{
    *x = 0;
    *y = LIST_TOP + OPTION_H + NOTE_GAP + NOTE_LINES * NOTE_STEP + GAP;
    *w = gfx_w();
    *h = SLIDER_H;
}

static void slider_track(int *x0, int *x1)
{
    *x0 = SLIDER_INSET + SLIDER_KNOB;
    *x1 = gfx_w() - SLIDER_INSET - SLIDER_KNOB;
}

static void timer_box(int *x, int *y, int *w, int *h)
{
    int bx, by, bw, bh;
    brightness_box(&bx, &by, &bw, &bh);
    *x = 0;
    *y = by + bh + GAP;
    *w = gfx_w();
    *h = SLIDER_H;
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

    const int w = gfx_w(), h = gfx_h();
    gfx_fill_rect(0, 0, w, h, C_BG);

    gfx_draw_text(24, (HEAD_H - GFX_GLYPH_H(NAME_SCALE)) / 2, "Sleep",
                  NAME_SCALE, w - 48, C_TEXT);
    gfx_fill_rect(0, LIST_TOP - 2, w, 2, C_RULE);

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

    /* Footer: panel.c's, one button, the way out. */
    const int fy = h - FOOT_H;
    gfx_fill_rect(0, fy, w, 2, C_RULE);
    gfx_fill_rect(w / 2 - 90, fy + 16, 180, FOOT_H - 32, C_BTN);
    const int cw = gfx_text_w("CLOSE", LABEL_SCALE);
    gfx_draw_text(w / 2 - cw / 2,
                  fy + 16 + (FOOT_H - 32 - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                  "CLOSE", LABEL_SCALE, 172, C_TEXT);

    gfx_blit(0, h);
}

sleeppage_result_t sleeppage_touch(bool down, int x, int y)
{
    const bool tapped = down && !s_was_down;
    s_was_down = down;

    if (!s_open) return SLEEPPAGE_NONE;

    /*
     * The slider first, before the tapped test: a drag is a run of downs
     * with one edge at the front, and applied on every move so the
     * backlight follows the finger. Snapped to whole percent on the way
     * in, so the knob only sits where a setting exists.
     */
    {
        int sx, sy, sw, sh;
        brightness_box(&sx, &sy, &sw, &sh);
        if (s_drag || (tapped && y >= sy && y < sy + sh)) {
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
        if (s_timer_drag || (tapped && y >= sy && y < sy + sh)) {
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

    if (y >= gfx_h() - FOOT_H) {
        ESP_LOGI(TAG, "button: close");
        return SLEEPPAGE_CLOSE;
    }

    int bx, by, bw, bh;
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
