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

    /* Whether the next button would do anything. False greys it out --
     * the last track of a folder in list order, or an empty playlist.
     * True under shuffle even though the next track is unknown: it
     * exists, which is what the button is promising. */
    bool     has_next;
    bool     screen_off;

    /*
     * Whether pos_sec, len_sec and can_seek describe the track named
     * above them.
     *
     * False from the moment a track change is decided until the new
     * track's duration is known -- which on a Xing-less MP3 is a whole-
     * file scan away. The three numbers are stale for all of it, and a
     * seek bar that keeps filling and a clock that keeps counting are
     * the most convincing part of the illusion that nothing has
     * happened. While this is false the clocks read as dashes and the
     * bar is a bare groove.
     */
    bool stats_valid;
} ui_state_t;

/* What a touch produced. The player acts on these; the UI never acts. */
typedef enum {
    UI_ACTION_NONE = 0,
    UI_ACTION_PLAY_PAUSE,
    UI_ACTION_CHOOSE_FILE,   /* folder button: open the chooser */
    UI_ACTION_SCREEN_OFF,
    UI_ACTION_SCREEN_ON,
    UI_ACTION_PREV,         /* start of track, or the track before it */
    UI_ACTION_PREV_AGAIN,   /* double tap: back through play history */
    UI_ACTION_NEXT,
    UI_ACTION_SEEK,         /* value = target percent 0..100 */
    UI_ACTION_VOLUME,       /* value = target percent 0..100 */
} ui_action_kind_t;

/* Name of an action, for logging. Never NULL. Lives beside the enum so a
 * new action cannot be added without a name -- the switch has no default,
 * so the compiler asks. */
const char *ui_action_name(ui_action_kind_t k);

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

/*
 * Fill the artwork square with lines of text, centred.
 *
 * For a file with no picture in it. The alternative -- and what this
 * replaces -- is 720x720 of black, which looks like a cover that has not
 * arrived yet rather than one that does not exist. What goes there
 * instead is the thing the file can always say about itself: its format.
 *
 * lines[0] is drawn largest and the rest step down, so the container
 * name reads as a heading and the details under it as detail. Draws and
 * blits immediately; not part of ui_draw().
 */
void ui_show_art_info(const char *const *lines, int n);

/* Repaint the bar. Cheap enough to call at 20 Hz: it touches only the
 * bottom UI_BAR_H rows, never the cover art above them. */
void ui_draw(const ui_state_t *st);

/* Height of the envelope standing on the seek baseline.
 *
 * 64 px of upper sideband. The mirrored version needed twice this and
 * could only get it in the artwork area; half the shape carries all of
 * the information, so it fits here. */
#define UI_WAVE_H   (72)

/*
 * Whether anything on the bar is mid-animation.
 *
 * Exactly one thing is: a title too long for the panel, sliding. The UI
 * task polls at 10 Hz when no finger is down, which is right for a
 * seconds-resolution clock and visibly wrong for something moving --
 * 10 Hz reads as a title jumping in steps rather than travelling. Asking
 * costs nothing and means the faster rate is paid for only while there is
 * something to see.
 */
bool ui_animating(void);

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
/*
 * The artwork is a square now -- 720x720, the full width of the panel --
 * and the bar is what is left: 1280 - 720.
 *
 * Derived rather than chosen, which is the reverse of every version
 * before this one. The bar used to be sized to its contents and the
 * artwork got the remainder, so the cover was 964 rows in a 720 px column
 * and albumart.c letterboxed it into the middle with 122 rows of black
 * above and below. Those rows were doing nothing. Making the cover square
 * hands them to the controls, which had eight rows to fit into 356 px and
 * now have 560.
 */
#define UI_ART_H    (720)
#define UI_BAR_H    (1280 - UI_ART_H)

#ifdef __cplusplus
}
#endif
