/*
 * sleeppage.c -- see sleeppage.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sleeppage.h"

#include "esp_log.h"
#include "gfx.h"

static const char *TAG = "tab5_sleep";

/* panel.c's, copied -- see sleeppage.h. */
#define C_BG        RGB(0x0C, 0x0C, 0x0C)
#define C_ROW       RGB(0x1A, 0x1A, 0x1A)
#define C_TEXT      RGB(0xEE, 0xEE, 0xEE)
#define C_DIM       RGB(0x77, 0x77, 0x77)
#define C_BTN       RGB(0x26, 0x26, 0x26)
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

static bool s_open;
static bool s_dirty;
static bool s_was_down;

bool sleeppage_is_open(void) { return s_open; }

void sleeppage_open(void)
{
    s_open = true;
    s_dirty = true;
    s_was_down = false;
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
static void screen_off_box(int *x, int *y, int *w, int *h)
{
    *x = 0; *y = LIST_TOP; *w = gfx_w(); *h = OPTION_H;
}

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
    screen_off_box(&x, &y, &bw, &bh);
    gfx_fill_rect(x, y, bw, bh, C_ROW);
    gfx_draw_text(24, y + (bh - GFX_GLYPH_H(NAME_SCALE)) / 2, "Screen off",
                  NAME_SCALE, w - 48, C_TEXT);
    {
        static const char *const note[] = {
            "Turns the backlight off. Playback carries on.",
            "Touch anywhere to wake it.",
        };
        int ny = y + bh + NOTE_GAP;
        for (size_t i = 0; i < sizeof(note) / sizeof(note[0]); i++) {
            gfx_draw_text(24, ny, note[i], LABEL_SCALE, w - 48, C_DIM);
            ny += NOTE_STEP;
        }
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
    (void)x;

    if (!s_open || !tapped) return SLEEPPAGE_NONE;

    if (y >= gfx_h() - FOOT_H) {
        ESP_LOGI(TAG, "button: close");
        return SLEEPPAGE_CLOSE;
    }

    int bx, by, bw, bh;
    /* Whole row, not a pill, for panel.c's reason: a row is the target a
     * thumb actually hits. */
    screen_off_box(&bx, &by, &bw, &bh);
    if (y >= by && y < by + bh) {
        ESP_LOGI(TAG, "button: screen off");
        return SLEEPPAGE_SCREEN_OFF;
    }
    return SLEEPPAGE_NONE;
}
