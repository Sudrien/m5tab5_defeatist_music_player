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

static esp_lcd_touch_handle_t s_touch;

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

bool touch_get(int *x, int *y)
{
    if (!s_touch) return false;

    /* esp_lcd_touch_get_coordinates() is deprecated and goes away in the
     * component's 2.0.0. get_data() is the replacement and returns a
     * status rather than a bool, so "no finger" is ESP_OK with a zero
     * count -- not an error. */
    esp_lcd_touch_point_data_t pt[1] = { 0 };
    uint8_t count = 0;

    if (esp_lcd_touch_read_data(s_touch) != ESP_OK) return false;
    if (esp_lcd_touch_get_data(s_touch, pt, &count, 1) != ESP_OK || count == 0) {
        return false;
    }

    *x = pt[0].x;
    *y = pt[0].y;
    return true;
}
