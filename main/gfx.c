/*
 * gfx.c -- primitives lifted verbatim out of ui.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"

#include "font8x8_basic.h"

#include "gfx.h"

static const char *TAG = "tab5_gfx";

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static int s_w, s_h;

/*
 * One blit at a time.
 *
 * The claim that there is a single writer to the framebuffer was never
 * quite true: the transport bar is drawn by ui_task and the artwork by
 * the decode loop, which are two tasks, and both end in
 * esp_lcd_panel_draw_bitmap(). The DPI panel takes one transfer at a
 * time and says so:
 *
 *   dpi_panel_draw_bitmap(553): previous draw operation is not finished
 *
 * It was rare while the bar repainted at 10 Hz and stopped being rare the
 * moment a bouncing title raised that to 25.
 *
 * The mutex is necessary and not sufficient. draw_bitmap() from an
 * external buffer goes out over DMA2D and returns before the transfer
 * completes -- the driver takes its own semaphore with a zero timeout and
 * returns ESP_ERR_INVALID_STATE if the last one is still in flight -- so
 * a second caller can lose even after the first has returned. Hence the
 * retry below. The mutex still earns its place: without it the two tasks
 * take turns failing each other's retries.
 *
 * The two writers own disjoint bands of the shadow, rows above the bar
 * and rows below it, so the only thing they contend for is the transfer.
 */
#define BLIT_RETRIES    (20)

/*
 * ...and a way to know when the transfer is actually done.
 *
 * The retry alone worked and was loud: the driver logs an error from
 * inside on every attempt that loses, so a contended blit printed three
 * or four lines of
 *
 *   dpi_panel_draw_bitmap(553): previous draw operation is not finished
 *
 * before succeeding. Retrying an operation that has a completion callback
 * is guessing at a fact the hardware will tell you, so the callback is
 * registered and each blit waits for it before releasing the mutex. The
 * next caller then cannot be early.
 *
 * The retry stays as a fallback. If the callback is ever not delivered --
 * a driver path that skips it, a timeout -- the wait expires and the
 * behaviour degrades to what it was rather than to a stall.
 */
#define BLIT_DONE_MS    (60)

static SemaphoreHandle_t s_blit_done;

/* Must be in IRAM: the driver checks, because it calls this from the DMA
 * completion ISR. Returning true asks for a yield when the give woke a
 * higher-priority task. */
static IRAM_ATTR bool on_blit_done(esp_lcd_panel_handle_t panel,
                                   esp_lcd_dpi_panel_event_data_t *data,
                                   void *ctx)
{
    (void)panel; (void)data; (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_blit_done, &woken);
    return woken == pdTRUE;
}
static SemaphoreHandle_t s_blit_lock;

esp_err_t gfx_init(esp_lcd_panel_handle_t panel, int w, int h)
{
    s_panel = panel;
    s_w = w;
    s_h = h;

    s_blit_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_blit_lock, ESP_ERR_NO_MEM, TAG, "no blit mutex");

    s_blit_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_blit_done, ESP_ERR_NO_MEM, TAG, "no blit semaphore");

    const esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_blit_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(panel, &cbs, NULL),
                        TAG, "blit callback");

    s_fb = heap_caps_malloc((size_t)w * h * sizeof(uint16_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_NO_MEM, TAG, "no room for the shadow buffer");
    memset(s_fb, 0, (size_t)w * h * sizeof(uint16_t));

    /* Clear the panel's own buffer once, so the boot screen is black
     * rather than whatever the DPI peripheral powered up holding. */
    uint16_t *panel_fb = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, (void **)&panel_fb) == ESP_OK) {
        memset(panel_fb, 0, (size_t)w * h * sizeof(uint16_t));
    }
    return ESP_OK;
}

uint16_t *gfx_fb(void) { return s_fb; }
int gfx_w(void) { return s_w; }
int gfx_h(void) { return s_h; }

esp_err_t gfx_blit_err(int y0, int y1)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;
    if (y0 < 0) y0 = 0;
    if (y1 > s_h) y1 = s_h;
    if (y1 <= y0) return ESP_OK;

    /* Full-width band, so the source rows are contiguous and the driver
     * copies the region in one go. Passing the whole-screen base pointer
     * with a y offset would be a different bitmap entirely. */
    xSemaphoreTake(s_blit_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    for (int i = 0; i < BLIT_RETRIES; i++) {
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, y0, s_w, y1,
                                        &s_fb[(size_t)y0 * s_w]);
        if (err != ESP_ERR_INVALID_STATE) break;
        /* One tick, which is longer than a band transfer takes. Sleeping
         * rather than spinning: the task that owns the previous transfer
         * needs the CPU to finish it. */
        vTaskDelay(1);
    }

    /* Wait for the transfer this call started, still holding the mutex,
     * so the next caller finds the panel idle. The synchronous path in
     * the driver invokes the callback before returning, in which case the
     * token is already there and this does not block at all. */
    if (err == ESP_OK) {
        xSemaphoreTake(s_blit_done, pdMS_TO_TICKS(BLIT_DONE_MS));
    }

    xSemaphoreGive(s_blit_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "blit %d..%d failed: %s", y0, y1,
                                esp_err_to_name(err));
    return err;
}

void gfx_blit(int y0, int y1)
{
    (void)gfx_blit_err(y0, y1);
}

/* ------------------------------------------------------------------ */
/* Shapes                                                              */
/* ------------------------------------------------------------------ */

void gfx_px(int x, int y, uint16_t c)
{
    if (!s_fb) return;
    if (x < 0 || x >= s_w || y < 0 || y >= s_h) return;
    s_fb[y * s_w + x] = c;
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    if (!s_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) {
        uint16_t *row = &s_fb[(y + r) * s_w + x];
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

void gfx_fill_circle(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        const int span = (int)(0.5f + __builtin_sqrtf((float)(r * r - dy * dy)));
        gfx_fill_rect(cx - span, cy + dy, 2 * span + 1, 1, c);
    }
}

void gfx_ring(int cx, int cy, int r, int thick, uint16_t c)
{
    const int inner = r - thick;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 >= inner * inner) gfx_px(cx + dx, cy + dy, c);
        }
    }
}

void gfx_ring_arc(int cx, int cy, int r, int thick, int pct, uint16_t c)
{
    if (pct <= 0) return;
    if (pct > 100) pct = 100;
    const int inner = r - thick;
    /* 1024ths of a turn, integer only -- no atan2f per pixel. */
    const int limit = (pct * 1024) / 100;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > r * r || d2 < inner * inner) continue;
            /* Angle from 12 o'clock, clockwise, in 1024ths. Quadrant
             * dispatch plus a linear ramp inside each -- close enough for
             * a 46 px indicator and free of floating point. */
            const int ax = dx, ay = -dy;
            int a;
            const int adx = ax < 0 ? -ax : ax;
            const int ady = ay < 0 ? -ay : ay;
            const int denom = adx + ady;
            if (denom == 0) continue;
            const int frac = (adx * 256) / denom;   /* 0 at vertical */
            if (ax >= 0 && ay >= 0)      a = frac;              /* 0..256   */
            else if (ax >= 0)            a = 512 - frac;        /* 256..512 */
            else if (ay < 0)             a = 512 + frac;        /* 512..768 */
            else                         a = 1024 - frac;       /* 768..1024*/
            if (a <= limit) gfx_px(cx + dx, cy + dy, c);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Seven-segment digits                                                */
/* ------------------------------------------------------------------ */

/* Segment order: a top, b top-right, c bottom-right, d bottom,
 * e bottom-left, f top-left, g middle. */
static const uint8_t k_seg[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

static void seg_digit(int x, int y, int n, int w, int h, int t, uint16_t c)
{
    if (n < 0 || n > 9) return;
    const uint8_t m = k_seg[n];
    const int mid = y + h / 2;

    if (m & 0x01) gfx_fill_rect(x, y, w, t, c);                   /* a */
    if (m & 0x02) gfx_fill_rect(x + w - t, y, t, h / 2, c);       /* b */
    if (m & 0x04) gfx_fill_rect(x + w - t, mid, t, h / 2, c);     /* c */
    if (m & 0x08) gfx_fill_rect(x, y + h - t, w, t, c);           /* d */
    if (m & 0x10) gfx_fill_rect(x, mid, t, h / 2, c);             /* e */
    if (m & 0x20) gfx_fill_rect(x, y, t, h / 2, c);               /* f */
    if (m & 0x40) gfx_fill_rect(x, mid - t / 2, w, t, c);         /* g */
}

void gfx_draw_time(int x, int y, uint32_t sec, uint16_t c)
{
    uint32_t m = sec / 60;
    const uint32_t s2 = sec % 60;
    if (m > 99) m = 99;

    seg_digit(x, y, (int)(m / 10), GFX_DIG_W, GFX_DIG_H, GFX_DIG_T, c);
    x += GFX_DIG_W + GFX_DIG_GAP;
    seg_digit(x, y, (int)(m % 10), GFX_DIG_W, GFX_DIG_H, GFX_DIG_T, c);
    x += GFX_DIG_W + GFX_DIG_GAP;

    gfx_fill_rect(x + 1, y + GFX_DIG_H / 3, GFX_DIG_T, GFX_DIG_T, c);
    gfx_fill_rect(x + 1, y + (2 * GFX_DIG_H) / 3, GFX_DIG_T, GFX_DIG_T, c);
    x += GFX_DIG_W / 2 + GFX_DIG_GAP;

    seg_digit(x, y, (int)(s2 / 10), GFX_DIG_W, GFX_DIG_H, GFX_DIG_T, c);
    x += GFX_DIG_W + GFX_DIG_GAP;
    seg_digit(x, y, (int)(s2 % 10), GFX_DIG_W, GFX_DIG_H, GFX_DIG_T, c);
}

void gfx_draw_time_neg(int x, int y, uint32_t sec, uint16_t c)
{
    /* The minus is a bar the width of a digit at the vertical middle --
     * segment g, drawn on its own. Reusing the segment geometry is what
     * keeps it aligned with the digits beside it at any size. */
    gfx_fill_rect(x, y + GFX_DIG_H / 2 - GFX_DIG_T / 2, GFX_DIG_W, GFX_DIG_T, c);
    gfx_draw_time(x + GFX_DIG_W + GFX_DIG_GAP, y, sec, c);
}

void gfx_draw_small_time_centred(int cx, int y, uint32_t sec, uint16_t c)
{
    uint32_t m = sec / 60;
    const uint32_t s2 = sec % 60;
    if (m > 99) m = 99;

    const int run = 4 * GFX_SDIG_W + 3 * GFX_SDIG_GAP + GFX_SDIG_W / 2 + GFX_SDIG_GAP;
    int x = cx - run / 2;

    seg_digit(x, y, (int)(m / 10), GFX_SDIG_W, GFX_SDIG_H, GFX_SDIG_T, c);
    x += GFX_SDIG_W + GFX_SDIG_GAP;
    seg_digit(x, y, (int)(m % 10), GFX_SDIG_W, GFX_SDIG_H, GFX_SDIG_T, c);
    x += GFX_SDIG_W + GFX_SDIG_GAP;

    gfx_fill_rect(x + 1, y + GFX_SDIG_H / 3, GFX_SDIG_T, GFX_SDIG_T, c);
    gfx_fill_rect(x + 1, y + (2 * GFX_SDIG_H) / 3, GFX_SDIG_T, GFX_SDIG_T, c);
    x += GFX_SDIG_W / 2 + GFX_SDIG_GAP;

    seg_digit(x, y, (int)(s2 / 10), GFX_SDIG_W, GFX_SDIG_H, GFX_SDIG_T, c);
    x += GFX_SDIG_W + GFX_SDIG_GAP;
    seg_digit(x, y, (int)(s2 % 10), GFX_SDIG_W, GFX_SDIG_H, GFX_SDIG_T, c);
}

/* A percentage, centred on cx. 100 needs three digits, 0-99 needs two, and
 * the run is centred either way so the number does not shuffle sideways as
 * it crosses 100. */
void gfx_draw_pct_centred(int cx, int y, int pct, uint16_t c)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    const int digits = (pct >= 100) ? 3 : 2;
    const int run = digits * GFX_SDIG_W + (digits - 1) * GFX_SDIG_GAP;
    int x = cx - run / 2;

    if (digits == 3) {
        seg_digit(x, y, 1, GFX_SDIG_W, GFX_SDIG_H, GFX_SDIG_T, c);
        x += GFX_SDIG_W + GFX_SDIG_GAP;
        seg_digit(x, y, 0, GFX_SDIG_W, GFX_SDIG_H, GFX_SDIG_T, c);
        x += GFX_SDIG_W + GFX_SDIG_GAP;
        seg_digit(x, y, 0, GFX_SDIG_W, GFX_SDIG_H, GFX_SDIG_T, c);
    } else {
        seg_digit(x, y, pct / 10, GFX_SDIG_W, GFX_SDIG_H, GFX_SDIG_T, c);
        x += GFX_SDIG_W + GFX_SDIG_GAP;
        seg_digit(x, y, pct % 10, GFX_SDIG_W, GFX_SDIG_H, GFX_SDIG_T, c);
    }
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

void gfx_draw_char(int x, int y, char ch, int scale, uint16_t c)
{
    const unsigned char u = (unsigned char)ch;
    if (u > 127) return;
    const char *g = font8x8_basic[u];

    for (int row = 0; row < 8; row++) {
        const unsigned char bits = (unsigned char)g[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1 << col))) continue;
            gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, c);
        }
    }
}

void gfx_draw_text_clipped(int x, int y, int win_x, int win_w,
                           const char *s, int scale, uint16_t c)
{
    if (!s || !*s || win_w <= 0) return;
    const int gw = GFX_GLYPH_W(scale);
    const int win_x1 = win_x + win_w;

    for (int i = 0; s[i]; i++) {
        const int gx = x + i * gw;
        /* Wholly left of the window: keep going, the string runs
         * rightward. Wholly right of it: nothing after this can be
         * visible either, so stop -- a 200 character title otherwise
         * costs 200 glyph draws to show forty. */
        if (gx + gw <= win_x) continue;
        if (gx >= win_x1) break;

        const unsigned char u = (unsigned char)s[i];
        if (u > 127) continue;
        const char *g = font8x8_basic[u];

        for (int row = 0; row < 8; row++) {
            const unsigned char bits = (unsigned char)g[row];
            for (int col = 0; col < 8; col++) {
                if (!(bits & (1 << col))) continue;
                int px = gx + col * scale;
                int pw = scale;
                /* Clip the run rather than the glyph. A glyph half out of
                 * the window has to be drawn half, or the text appears to
                 * jump a character at a time at each end. */
                if (px < win_x) { pw -= win_x - px; px = win_x; }
                if (px + pw > win_x1) pw = win_x1 - px;
                if (pw <= 0) continue;
                gfx_fill_rect(px, y + row * scale, pw, scale, c);
            }
        }
    }
}

int gfx_text_w(const char *s, int scale)
{
    if (!s) return 0;
    return (int)strlen(s) * GFX_GLYPH_W(scale);
}

void gfx_draw_text(int x, int y, const char *s, int scale, int max_w, uint16_t c)
{
    if (!s || !*s) return;
    const int gw = GFX_GLYPH_W(scale);
    const int room = max_w / gw;
    const int len = (int)strlen(s);

    if (len <= room) {
        for (int i = 0; i < len; i++) gfx_draw_char(x + i * gw, y, s[i], scale, c);
        return;
    }
    if (room < 4) return;
    for (int i = 0; i < room - 3; i++) gfx_draw_char(x + i * gw, y, s[i], scale, c);
    for (int i = room - 3; i < room; i++) gfx_draw_char(x + i * gw, y, '.', scale, c);
}

/* Keeps the tail. A truncated path with the head kept reads "/sd/Music/Th"
 * for every directory on the card; with the tail kept it reads
 * "...st Album", which is the part that says where you are. */
void gfx_draw_text_tail(int x, int y, const char *s, int scale, int max_w, uint16_t c)
{
    if (!s || !*s) return;
    const int gw = GFX_GLYPH_W(scale);
    const int room = max_w / gw;
    const int len = (int)strlen(s);

    if (len <= room) {
        for (int i = 0; i < len; i++) gfx_draw_char(x + i * gw, y, s[i], scale, c);
        return;
    }
    if (room < 4) return;
    for (int i = 0; i < 3; i++) gfx_draw_char(x + i * gw, y, '.', scale, c);
    const char *tail = s + len - (room - 3);
    for (int i = 0; i < room - 3; i++) {
        gfx_draw_char(x + (i + 3) * gw, y, tail[i], scale, c);
    }
}
