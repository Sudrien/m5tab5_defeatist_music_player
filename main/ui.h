/*
 * Transport bar drawn straight into the panel's scan buffer.
 *
 * No LVGL, no M5Canvas. albumart.c already establishes the pattern --
 * esp_lcd_dpi_panel_get_frame_buffer(), write pixels, hand the same
 * pointer back to draw_bitmap -- and the whole UI is five controls, so a
 * widget toolkit would be more code than the thing it draws.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the UI reads. The player owns this; ui_draw() never writes it. */
typedef struct {
    const char *title;      /* falls back to the filename when no tag */
    const char *artist;     /* NULL or "" hides the row */
    const char *album;
    bool     playing;
    int      volume;        /* 0..100 */
    uint32_t pos_sec;
    uint32_t len_sec;       /* 0 when unknown -- seek bar renders empty */

    /* Whether a drag would do anything. A track can have a known length
     * and still not be seekable: duration.c reads Ogg lengths out of the
     * container, but nothing can seek an Ogg yet. */
    bool can_seek;
    bool     screen_off;
} ui_state_t;

/* What a touch produced. The player acts on these; the UI never acts. */
typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_PLAY_PAUSE,
    UI_ACTION_CHOOSE_FILE,   /* folder button: open the chooser */
    UI_ACTION_SCREEN_OFF,
    UI_ACTION_SCREEN_ON,
    UI_ACTION_SEEK,         /* value = target percent 0..100 */
    UI_ACTION_VOLUME,       /* value = target percent 0..100 */
} ui_action_kind_t;

typedef struct {
    ui_action_kind_t kind;
    int value;
} ui_action_t;

esp_err_t ui_init(esp_lcd_panel_handle_t panel, int w, int h);

/* Save the strip of cover art the finger bubble draws over. Call once the
 * artwork is on screen; without it a drag smears bubbles across the
 * cover, because the bubble is the one thing here that draws outside the
 * bar. */
void ui_capture_background(void);

/* Blank the area above the bar.
 *
 * Needed once tracks follow one another: the cover is drawn by
 * albumart_show() and never cleared, so a track with no art inherited the
 * previous track's cover -- which reads as the player having ignored the
 * choice rather than as the file having no picture in it. */
void ui_clear_art(void);

/* Repaint the bar. Cheap enough to call at 20 Hz: it touches only the
 * bottom UI_BAR_H rows, never the cover art above them. */
void ui_draw(const ui_state_t *st);

/* Feed one poll of the touch controller. down=false means no finger.
 * Returns the action this poll produced, if any.
 *
 * Drags are tracked across calls, which is why this takes raw state
 * rather than taps: the sliders need continuous movement, and the finger
 * bubble has to follow it. */
ui_action_t ui_touch(const ui_state_t *st, bool down, int x, int y);

/* Height of the bar at the bottom of the screen. Everything above it
 * belongs to the cover art. */
/*
 * Grew by 40 px when album moved off the artist line onto its own. That
 * is one scale-3 row (24 px) plus the gap that keeps the three rows from
 * reading as a block of text.
 *
 * It comes out of the artwork, which is the only place it can come from.
 * At 1280 the cover had 1004 rows and now has 964; a 500x500 cover is
 * unaffected either way, and one large enough to be cropped loses 20
 * rows top and bottom.
 */
#define UI_BAR_H    (316)

#ifdef __cplusplus
}
#endif
