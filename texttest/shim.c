/*
 * shim.c -- host implementations of the ESP-IDF surface gfx.c touches.
 *
 * heap_caps_* are plain malloc so ASan sees every allocation and every
 * overrun of the shadow framebuffer. That is the point of the harness:
 * gfx_fill_rect() writes into s_fb, so a layout bug that computes a
 * too-large x or w becomes a heap-buffer-overflow with a stack trace
 * rather than a corrupted pixel nobody notices.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "shim.h"

/* The harness needs to read the pixels gfx.c drew, and gfx.c keeps its
 * shadow buffer private -- correctly. Rather than add a test-only
 * accessor to shipping code, the shim remembers the largest allocation
 * it handed out, which is the framebuffer: gfx_init() asks for w*h*2
 * bytes and nothing else here allocates at all. */
void *shim_last_big_alloc;
static size_t shim_last_big_size;

void *heap_caps_malloc(size_t sz, uint32_t caps)
{
    (void)caps;
    void *p = malloc(sz);
    if (p && sz > shim_last_big_size) { shim_last_big_alloc = p; shim_last_big_size = sz; }
    return p;
}
void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps) { (void)caps; return calloc(n, sz); }
void  heap_caps_free(void *p) { free(p); }

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0,
                                    int x1, int y1, const void *data)
{ (void)p; (void)x0; (void)y0; (void)x1; (void)y1; (void)data; return ESP_OK; }

esp_err_t esp_lcd_dpi_panel_register_event_callbacks(
    esp_lcd_panel_handle_t p, const esp_lcd_dpi_panel_event_callbacks_t *cbs, void *arg)
{ (void)p; (void)cbs; (void)arg; return ESP_OK; }

/* Returns failure, which gfx_init() handles: it skips clearing the
 * panel's own buffer. There is no panel here to clear. */
esp_err_t esp_lcd_dpi_panel_get_frame_buffer(esp_lcd_panel_handle_t p, uint32_t n, void **fb)
{ (void)p; (void)n; (void)fb; return ESP_FAIL; }

const char *esp_err_to_name(esp_err_t e) { (void)e; return "ESP_ERR"; }
