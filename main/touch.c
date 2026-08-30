/*
 * touch.c
 *
 * Two Tab5 revisions exist and they carry different touch controllers:
 *
 *   rev 1: ILI9881C panel + GT911 touch, at the GT911 backup address
 *   rev 2: ST7121/ST7123 panel + ST7123 touch, at 0x55
 *
 * The controller is not named after the panel and the pairing is not
 * guessable from either name, so it is probed rather than assumed --
 * ST7123 first, then GT911, the same order M5's BSP uses.
 *
 * On rev 1 there is a pull-up to 3V3 on the INT line that stops the GT911
 * responding, so that path drives GPIO 23 LOW and polls instead of using
 * the interrupt. Both facts come from m5tab5_esp_idf_display_example.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "touch.h"

static const char *TAG = "tab5_touch";

#define TOUCH_INT_GPIO      (GPIO_NUM_23)

/*
 * How long after a screen transition a new press is ignored, on top of
 * waiting for the finger to lift.
 *
 * Release-then-press is not always a second intent. Capacitive panels
 * report a brief loss of contact when a finger rolls or the pressure
 * eases, and the GT911 in particular will drop a point for one poll and
 * pick it back up -- which, at the moment a new screen has just
 * appeared under it, is indistinguishable from a deliberate second tap
 * on whatever landed there.
 *
 * 250 ms is above the panel's jitter and below what reads as
 * unresponsive: a deliberate second tap takes longer than that to
 * arrive, because the hand has to see the new screen first.
 *
 * If the chooser still feels like it selects something on opening,
 * raise this before reaching for anything else -- it is the only number
 * involved. If it starts feeling sticky when paging quickly through a
 * long directory, lower it. Note that the swallow is armed only on
 * screen transitions, so this never delays an ordinary tap.
 */
#ifndef TOUCH_SETTLE_MS
#define TOUCH_SETTLE_MS     (250)
#endif

static esp_lcd_touch_handle_t s_touch;

/*
 * Set by touch_swallow(). Cleared only when BOTH are true: the finger
 * has lifted, and the settle window has expired.
 *
 * Two conditions because they cover different failures. Waiting for the
 * release stops the press that caused the transition from being read
 * again by the screen it opened. Waiting out the window stops the
 * panel's own release-and-reacquire from becoming a fresh tap on that
 * screen a few milliseconds later. Either alone leaves a real way to
 * select something nobody aimed at.
 */
static bool s_swallow;
static TickType_t s_swallow_until;

esp_err_t touch_init(i2c_master_bus_handle_t bus, int panel_w, int panel_h)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;

    /* Coordinates come out in panel-native portrait space, which is
     * exactly the framebuffer layout, so nothing is swapped or mirrored. */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = panel_w,
        .y_max = panel_h,
        .rst_gpio_num = GPIO_NUM_NC,    /* shared with LCD_RST on the expander */
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
    };

    /* The controller needs a moment after TP_RST goes high. */
    vTaskDelay(pdMS_TO_TICKS(200));

    if (i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS, 100) == ESP_OK) {
        ESP_LOGI(TAG, "ST7123 at 0x%02X", ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS);

        const esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &tp_io), TAG, "touch io");
        ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_st7123(tp_io, &tp_cfg, &s_touch),
                            TAG, "st7123 touch");

    } else if (i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 at 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);

        /* Hold INT low: on this revision a pull-up to 3V3 otherwise blocks
         * the GT911. Poll rather than use the interrupt. */
        const gpio_config_t int_cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .intr_type = GPIO_INTR_DISABLE,
            .pull_up_en = 1,
            .pin_bit_mask = BIT64(TOUCH_INT_GPIO),
        };
        ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "int gpio");
        ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_INT_GPIO, 0), TAG, "int low");
        tp_cfg.int_gpio_num = GPIO_NUM_NC;

        esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &tp_io), TAG, "touch io");
        ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch),
                            TAG, "gt911 touch");

    } else {
        ESP_LOGE(TAG, "no touch controller on the bus");
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

bool touch_present(void)
{
    return s_touch != NULL;
}

void touch_swallow(void)
{
    s_swallow = true;
    s_swallow_until = xTaskGetTickCount() + pdMS_TO_TICKS(TOUCH_SETTLE_MS);
}

/* True while the swallow is still in force. Kept in one place because
 * the release test and the timer test have to agree, and an inverted
 * comparison in one of them is a lock that never lifts. */
static bool swallow_active(bool finger_down)
{
    if (!s_swallow) return false;

    /* Signed subtraction, so this stays correct across the tick counter
     * wrapping -- which it does every 49 days at 1 kHz, and which a
     * plain `now < until` gets wrong exactly once per wrap, for 250 ms,
     * on a device people leave running. */
    const bool settled =
        (TickType_t)(xTaskGetTickCount() - s_swallow_until) < (TickType_t)(1u << 31);

    if (!finger_down && settled) {
        s_swallow = false;
        return false;
    }
    return true;
}

/*
 * DIAGNOSTIC, added by 0503. Not a feature.
 *
 * The volume moved seven times unbidden in one capture -- 75, 24, 8, 29,
 * 63, 63, 15 -- and a `prev` arrived 1.3 s after another `prev`. Neither
 * was pressed. Presses that nobody made are the first explanation for
 * the cyan flash that does not require the bandwidth story that 0501
 * disproved, and they also make every seek measurement in this
 * investigation suspect: "fewer flashes with SEEK_NOOP set" may have
 * been counting touches rather than seeks.
 *
 * So log the edges. Every down with its coordinates, every up with how
 * long the press lasted, and every failed read, because a controller
 * that is dropping or corrupting reports is the mechanism this is
 * looking for. Edges only -- a poll at 50 Hz logged per poll would be
 * its own problem, and the buffer loss that costs is documented in
 * sdkconfig.defaults.
 *
 * Read it against the actions in player.c. A `button: seek` with no
 * `touch: down` before it is a phantom, and a `touch: read failed` next
 * to a flash is the ST7123 misbehaving on a bus it shares with the
 * codec at 0x10 and both expanders at 0x43 and 0x44 -- one of which
 * holds LCD_RST.
 */
#define TOUCH_TRACE             (1)

#if TOUCH_TRACE
static bool       s_trace_down;
static TickType_t s_trace_since;
static uint32_t   s_trace_fails;
#endif

bool touch_get(int *x, int *y)
{
    if (!s_touch) return false;

    /* esp_lcd_touch_get_coordinates() is deprecated and goes away in the
     * component's 2.0.0. get_data() is the replacement and returns a
     * status rather than a bool, so "no finger" is ESP_OK with a zero
     * count -- not an error. */
    esp_lcd_touch_point_data_t pt[1] = { 0 };
    uint8_t count = 0;

    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) {
#if TOUCH_TRACE
        /* Counted rather than logged per occurrence: a controller that
         * has stopped answering would otherwise fill the console with
         * the same line at 50 Hz and lose everything around it. */
        if ((++s_trace_fails % 50) == 1) {
            ESP_LOGW(TAG, "touch: read failed (%" PRIu32 " total)", s_trace_fails);
        }
#endif
        swallow_active(false);
        return false;
    }
    if (esp_lcd_touch_get_data(s_touch, pt, &count, 1) != ESP_OK || count == 0) {
        /* Finger up. Not the end of the swallow on its own: the settle
         * window still has to expire, or the panel dropping a point for
         * one poll counts as the release and the reacquisition counts as
         * a new tap. */
        swallow_active(false);
#if TOUCH_TRACE
        if (s_trace_down) {
            s_trace_down = false;
            ESP_LOGI(TAG, "touch: up after %" PRIu32 " ms",
                     (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - s_trace_since));
        }
#endif
        return false;
    }

#if TOUCH_TRACE
    /* Before the swallow test, deliberately: a swallowed press is still
     * a press the glass reported, and a phantom that arrives during a
     * settle window would otherwise be invisible here. */
    if (!s_trace_down) {
        s_trace_down = true;
        s_trace_since = xTaskGetTickCount();
        ESP_LOGI(TAG, "touch: down at %d,%d (%u point%s)%s",
                 (int)pt[0].x, (int)pt[0].y, (unsigned)count,
                 count == 1 ? "" : "s",
                 s_swallow ? ", swallowed" : "");
    }
#endif

    if (swallow_active(true)) return false;

    *x = pt[0].x;
    *y = pt[0].y;
    return true;
}
