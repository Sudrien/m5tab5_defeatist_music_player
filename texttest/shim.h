/*
 * shim.h -- enough of ESP-IDF to compile main/gfx.c on a host.
 *
 * This exists for the reason seektest/ exists, and the reason CLAUDE.md
 * gives for it: "Generated test files verify the logic against the format
 * as understood; they cannot verify the understanding." The same trap is
 * open here in a different shape. The variable-width text layout could be
 * reimplemented in the harness and tested against itself, and it would
 * pass, and it would prove nothing -- a reimplementation shares the
 * author's misunderstanding by construction.
 *
 * So gfx.c is compiled, verbatim, by texttest/Makefile. Nothing in this
 * directory reimplements glyph_for(), gfx_text_w() or the tail walk; they
 * are the shipping functions, reached through the shipping header, with
 * only the panel underneath them replaced. If a bound is wrong here, it is
 * wrong on the device.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --- FreeRTOS ------------------------------------------------------ */
/* gfx.c takes a mutex around blits. On the host there is one thread and
 * no preemption, so these are no-ops rather than pthread calls: the
 * harness is testing layout arithmetic, and a real mutex would only add
 * a way for the test itself to deadlock. */
typedef void *SemaphoreHandle_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE                    1
#define pdFALSE                   0
#define portMAX_DELAY             0xFFFFFFFFu
#define pdMS_TO_TICKS(ms)         (ms)
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{ (void)s; (void)t; return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)1; }
static inline BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken)
{ (void)s; if (woken) *woken = pdFALSE; return pdTRUE; }
#define portYIELD_FROM_ISR(x)     do { (void)(x); } while (0)
static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline TickType_t xTaskGetTickCount(void) { return 0; }

/* --- esp_err / logging --------------------------------------------- */
typedef int esp_err_t;
#define ESP_OK                    0
#define ESP_FAIL                  -1
#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103

#define ESP_LOGE(tag, ...)        do { (void)(tag); } while (0)
#define ESP_LOGW(tag, ...)        do { (void)(tag); } while (0)
#define ESP_LOGI(tag, ...)        do { (void)(tag); } while (0)
#define ESP_LOGD(tag, ...)        do { (void)(tag); } while (0)
#define ESP_LOGV(tag, ...)        do { (void)(tag); } while (0)

#define ESP_RETURN_ON_ERROR(x, tag, ...) \
    do { esp_err_t e_ = (x); if (e_ != ESP_OK) return e_; } while (0)
#define ESP_RETURN_ON_FALSE(a, err, tag, ...) \
    do { if (!(a)) return (err); } while (0)
#define ESP_GOTO_ON_ERROR(x, lbl, tag, ...) \
    do { esp_err_t e_ = (x); if (e_ != ESP_OK) goto lbl; } while (0)

/* --- attributes ----------------------------------------------------- */
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR

/* --- heap ----------------------------------------------------------- */
#define MALLOC_CAP_8BIT           (1 << 2)
#define MALLOC_CAP_DMA            (1 << 3)
#define MALLOC_CAP_SPIRAM         (1 << 10)
#define MALLOC_CAP_INTERNAL       (1 << 11)
void *heap_caps_malloc(size_t sz, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps);
void  heap_caps_free(void *p);

/* --- LCD panel ------------------------------------------------------ */
/* gfx_init() is never called by the harness -- the tests drive the text
 * functions directly and the panel stays NULL -- but the types have to
 * exist for gfx.c to compile. */
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int x0, int y0,
                                    int x1, int y1, const void *data);

typedef struct { int dummy; } esp_lcd_dpi_panel_event_data_t;

typedef struct {
    bool (*on_color_trans_done)(esp_lcd_panel_handle_t,
                                esp_lcd_dpi_panel_event_data_t *, void *);
} esp_lcd_dpi_panel_event_callbacks_t;

esp_err_t esp_lcd_dpi_panel_register_event_callbacks(
    esp_lcd_panel_handle_t p, const esp_lcd_dpi_panel_event_callbacks_t *cbs, void *arg);
esp_err_t esp_lcd_dpi_panel_get_frame_buffer(esp_lcd_panel_handle_t p, uint32_t n, void **fb);
const char *esp_err_to_name(esp_err_t e);
