#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pull the first JPEG APIC frame out of an MP3's ID3v2 tag.
 * Returns ESP_ERR_NOT_FOUND when there is no tag or no picture frame.
 * On success the caller owns *out and must free() it. */
esp_err_t albumart_extract(FILE *f, uint8_t **out, size_t *out_len);

/* Decode and blit centred, cropping anything larger than the panel.
 * JPEG goes through the P4's hardware codec; PNG is streamed by pngle
 * straight into the framebuffer. albumart_show() picks by magic bytes. */
esp_err_t albumart_show(esp_lcd_panel_handle_t panel, int screen_w, int screen_h,
                        const uint8_t *img, size_t img_len);

esp_err_t albumart_draw(esp_lcd_panel_handle_t panel, int screen_w, int screen_h,
                        const uint8_t *jpeg, size_t jpeg_len);

#ifdef __cplusplus
}
#endif
