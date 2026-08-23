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

bool touch_present(void);

#ifdef __cplusplus
}
#endif
