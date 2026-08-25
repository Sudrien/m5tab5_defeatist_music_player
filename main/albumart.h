#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Title, artist and album from the ID3v2 tag. Fields are empty strings
 * when the tag does not carry them; the caller falls back to the
 * filename.
 *
 * This lives here rather than in its own file because the frame walker it
 * needs is the same one albumart_extract() already has -- version-
 * dependent frame sizes, the padding check, the tag-end bound. A second
 * copy would be the thing that drifts. */
typedef struct {
    char title[64];
    char artist[64];
    char album[64];
} id3_tags_t;

esp_err_t id3_read_tags(FILE *f, id3_tags_t *out);

/* The same, for a tag that is not at offset 0 -- a WAV's 'id3 ' chunk,
 * or a tagger's ID3 bolted onto the front of a FLAC. covertag.c finds
 * the offset; this reads what is there. */
esp_err_t id3_read_tags_at(FILE *f, long base, id3_tags_t *out);

/* Pull the first JPEG APIC frame out of an MP3's ID3v2 tag.
 * Returns ESP_ERR_NOT_FOUND when there is no tag or no picture frame.
 * On success the caller owns *out and must free() it. */
esp_err_t albumart_extract(FILE *f, uint8_t **out, size_t *out_len);
esp_err_t albumart_extract_at(FILE *f, long base, uint8_t **out, size_t *out_len);

/* JPEG or PNG by magic bytes -- the two things albumart_show() can
 * decode. Every container's picture block ends in this question, and
 * each of them answering it separately is how one ends up accepting a
 * BMP and failing later, where the error is about the decoder rather
 * than the file. */
bool albumart_is_supported_image(const uint8_t *p, size_t len);

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
