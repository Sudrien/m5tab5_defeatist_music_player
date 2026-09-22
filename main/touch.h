/*
 * Capacitive touch.
 *
 * A thin wrapper over esp_lcd_touch: the driver selection and the
 * revision-specific INT handling are lifted verbatim from
 * m5tab5_esp_idf_display_example, which is confirmed on hardware. This
 * file exists only to give ui.c a one-point API and to keep the probe
 * logic out of player.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Must run after the panel is up: TP_RST is expander 1 P5, driven high by
 * the same PI4IOE1_OUT_SET write that releases LCD_RST, so before
 * io_expanders_init() there is nothing on the bus to probe.
 *
 * ESP_ERR_NOT_FOUND is not fatal -- the player runs without touch, it
 * just cannot be controlled. */
esp_err_t touch_init(i2c_master_bus_handle_t bus, int panel_w, int panel_h);

/* One point, or false when nothing is down. Coordinates are panel-native
 * portrait: 0..719 across, 0..1279 down, matching the framebuffer that
 * albumart.c and ui.c write into. */
bool touch_get(int *x, int *y);

/*
 * Discard the press that is currently down, reporting "up" until the
 * finger actually lifts.
 *
 * For the moment a tap changes which screen is up. One physical press is
 * one intent, but ui.c and browser.c each detect taps by their own
 * rising edge, and the screen that opens starts with its edge detector
 * saying "nothing was down" while the finger is still on the glass. The
 * next poll is then a fresh tap, at the coordinates of the control that
 * did the opening, delivered to a screen that means something entirely
 * different by them.
 *
 * That was not theoretical: the folder icon sits at y=1100, the chooser
 * puts list row 10 there, and tapping the folder icon played the
 * eleventh track in the directory. Repeatably, because the geometry is
 * fixed.
 *
 * Swallowing belongs here rather than in either screen because the thing
 * being consumed is the physical press, and both sides of every
 * transition need it. Fixing it in browser.c alone leaves the mirror
 * image: choosing a track closes the chooser, and the still-down finger
 * becomes a tap on the transport bar underneath.
 *
 * The swallow lifts when the finger has lifted AND a short settle window
 * has passed. The window is the second half of the same problem: a
 * capacitive panel drops and reacquires a point when a finger rolls, and
 * at the moment a new screen appears that reacquisition is
 * indistinguishable from a deliberate tap on whatever is now underneath.
 */
void touch_swallow(void);

/*
 * Turn the coordinates with the screen. 0..3 quarter turns clockwise,
 * the same numbers gfx_set_rotation() takes and necessarily the same
 * value, or every control is somewhere other than where it was drawn.
 *
 * The controller reports in panel-native portrait space whatever the
 * picture is doing, so this undoes what gfx did. Given a panel point
 * (px, py) and the panel extent touch_init() was handed:
 *
 *   0    (px, py)
 *   90   (py, w-1-px)          -- logical x runs down the glass
 *   180  (w-1-px, h-1-py)
 *   270  (h-1-py, px)
 *
 * which is the inverse of the forward mapping in gfx.c's comment. At 90
 * and 270 the returned point is in the LANDSCAPE space the screens drew
 * in, so its x may be up to h-1 and its y up to w-1.
 *
 * Kept here rather than in the caller because the caller is four
 * screens' worth of hit tests -- ui.c, browser.c, panel.c, sleeppage.c
 * -- and they all want the point in the space they drew in. One turn at
 * the source is one place to be wrong.
 */
void touch_set_rotation(int quarter_turns);

bool touch_present(void);

#ifdef __cplusplus
}
#endif
