/*
 * sleeppage.h -- the page behind the moon on the transport bar.
 *
 * The moon used to turn the screen off in one tap. It now opens this
 * page, whose first row is a switch, "Screen [ON]", in the settings
 * panel's shape. Switching it off is exactly what the moon used to do, because the sleep timer
 * needs somewhere to live and the moon is where somebody going to sleep
 * already reaches. A tap more for the screen, and a place for the timer
 * that does not add an icon to a bar that has run out of room.
 *
 * Shaped like panel.c, on purpose and for panel.c's reason: one writer
 * to the framebuffer, so the same open/draw/touch triple, driven from
 * ui_task, with the same footer and the same edge detection. The palette
 * is copied rather than shared, as panel.c copies browser.c's.
 *
 * The page does not act. It reports what was chosen and player.c does
 * it, so the screen state keeps exactly one owner.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SLEEPPAGE_NONE = 0,     /* nothing for the caller to do */
    SLEEPPAGE_CLOSE,        /* CLOSE pressed */
    SLEEPPAGE_BRIGHTNESS,   /* the slider moved settings_brightness();
                               the caller applies it to the backlight,
                               and the page stays open */
    SLEEPPAGE_BRIGHTNESS_DONE,  /* the drag ended; nothing to apply, but
                               the caller logs the duty it chose */
    SLEEPPAGE_TIMER,        /* the timer slider was released:
                               sleeppage_timer_step() says where, 0 off;
                               the caller (re)starts the timer from now */
    SLEEPPAGE_SCREEN_OFF,   /* the Screen switch went to OFF. The page has
                               already drawn OFF; the caller fades the
                               backlight, so the switch is seen to move,
                               then closes the page */
} sleeppage_result_t;

void sleeppage_open(void);
void sleeppage_close(void);
bool sleeppage_is_open(void);

/* Repaint if a touch dirtied it. Cheap every frame. */
void sleeppage_draw(void);

/* A press, same edge detection as panel_touch(). */
sleeppage_result_t sleeppage_touch(bool down, int x, int y);

/*
 * The timer, told to the page. The page does not own the timer -- player.c
 * does, because it is what fades and pauses -- so the caller hands over
 * the step it is running and the seconds left, every frame the page is
 * up. The page redraws when either changes, which is once a second while
 * a timer runs.
 */
void sleeppage_set_timer(int step, int64_t seconds_left);

/* Where the slider was released, for SLEEPPAGE_TIMER. */
int sleeppage_timer_step(void);

#ifdef __cplusplus
}
#endif
