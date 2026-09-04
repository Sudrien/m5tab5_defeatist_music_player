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

#include "ark12.h"

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

/*
 * A transfer taller than this is split, with a tick between the pieces.
 *
 * The DPI peripheral reads the panel's framebuffer out of PSRAM
 * continuously and cannot wait; a transfer into that same PSRAM is
 * bandwidth taken away from it, and past some size the fetch falls
 * behind, the bridge underruns and the panel goes cyan for a frame. The
 * pixel-clock note in player.c has the arithmetic.
 *
 * The steady load is not the problem: ui_task repaints the transport
 * bar, UI_BAR_H = 560 rows, and the panel holds through it at 50 Hz.
 * What flashed was the one transfer larger than that -- the 720-row art
 * strip, sent whole whenever a cover is drawn or cleared, which is once
 * per track change and which is exactly where the flash was seen.
 *
 * So the threshold sits above the bar and below the strip: the frame the
 * UI sends constantly is untouched, and the occasional big one is spread
 * instead. 240 rows a piece makes the strip three transfers with two
 * one-tick gaps in it -- 2 ms added to a repaint that happens between
 * tracks, against a flash that is visible every time.
 *
 * Splitting is safe because a full-width band is what the driver wants
 * anyway; each piece is contiguous in the shadow buffer exactly as the
 * whole was.
 */
#define BLIT_BAND_ROWS          (240)
#define BLIT_BAND_ABOVE         (600)

esp_err_t gfx_blit_err(int y0, int y1)
{
    if (!s_fb) return ESP_ERR_INVALID_STATE;
    if (y0 < 0) y0 = 0;
    if (y1 > s_h) y1 = s_h;
    if (y1 <= y0) return ESP_OK;

    /* Big region: hand it over in pieces, with the bus free between
     * them. Recursion depth is one -- the pieces are BLIT_BAND_ROWS
     * tall and the test is for more than BLIT_BAND_ABOVE. */
    if (y1 - y0 > BLIT_BAND_ABOVE) {
        for (int y = y0; y < y1; y += BLIT_BAND_ROWS) {
            const int end = (y + BLIT_BAND_ROWS < y1) ? y + BLIT_BAND_ROWS : y1;
            const esp_err_t berr = gfx_blit_err(y, end);
            if (berr != ESP_OK) return berr;
            if (end < y1) vTaskDelay(1);
        }
        return ESP_OK;
    }

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

static void seg_dash(int x, int y, int w, int h, int t, uint16_t c)
{
    gfx_fill_rect(x, y + h / 2 - t / 2, w, t, c);
}

void gfx_draw_time_dashes(int x, int y, uint16_t c)
{
    /* Same cell positions as gfx_draw_time(), so the run does not shift
     * sideways at the moment the real numbers arrive. */
    seg_dash(x, y, GFX_DIG_W, GFX_DIG_H, GFX_DIG_T, c);
    x += GFX_DIG_W + GFX_DIG_GAP;
    seg_dash(x, y, GFX_DIG_W, GFX_DIG_H, GFX_DIG_T, c);
    x += GFX_DIG_W + GFX_DIG_GAP;

    gfx_fill_rect(x + 1, y + GFX_DIG_H / 3, GFX_DIG_T, GFX_DIG_T, c);
    gfx_fill_rect(x + 1, y + (2 * GFX_DIG_H) / 3, GFX_DIG_T, GFX_DIG_T, c);
    x += GFX_DIG_W / 2 + GFX_DIG_GAP;

    seg_dash(x, y, GFX_DIG_W, GFX_DIG_H, GFX_DIG_T, c);
    x += GFX_DIG_W + GFX_DIG_GAP;
    seg_dash(x, y, GFX_DIG_W, GFX_DIG_H, GFX_DIG_T, c);
}

void gfx_draw_time_neg(int x, int y, uint32_t sec, uint16_t c)
{
    /* The minus is a bar the width of a digit at the vertical middle --
     * segment g, drawn on its own. Reusing the segment geometry is what
     * keeps it aligned with the digits beside it at any size. */
    gfx_fill_rect(x, y + GFX_DIG_H / 2 - GFX_DIG_T / 2, GFX_DIG_W, GFX_DIG_T, c);
    gfx_draw_time(x + GFX_DIG_W + GFX_DIG_GAP, y, sec, c);
}

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

/*
 * This section used to index font8x8_basic[] with a byte and return
 * early for anything above 127. It then decoded UTF-8 to a codepoint and
 * looked it up in ark12 for Latin-1 Supplement and Latin Extended-A, so
 * "Björk", "Sigur Rós", "Łódź" and "Beyoncé" rendered as themselves
 * rather than as a row of question marks. ark12 now also carries
 * Hiragana, Katakana, CJK Symbols and Punctuation and a size-appropriate
 * subset of CJK Unified Ideographs, at double the cell width -- so a
 * Japanese or Chinese title stops being a row of identical boxes the
 * same way an accented one stopped being a row of question marks.
 *
 * The strings arriving here are UTF-8 by construction: albumart.c
 * converts every ID3 text encoding to it, and FatFs is configured to
 * hand back UTF-8 filenames. A byte sequence that is not valid UTF-8 is
 * still possible -- a tag can contain anything -- and decodes to one
 * replacement glyph per bad byte rather than being allowed to desync the
 * decoder and eat the rest of the string.
 *
 * EVERY GLYPH HAS ITS OWN WIDTH NOW, NOT A CONSTANT ONE.
 *
 * Before this, a string's pixel width was glyph_count(s) * a compile-time
 * constant -- true as long as every glyph in the subset was the same 5px
 * cell, and it was, because the subset was Latin. It stopped being true
 * the moment ark12 gained fullwidth glyphs: a title mixing "Vol. 2" and
 * a kanji is not N cells of one size, it is some cells of one size and
 * some of another, and no single constant describes it.
 *
 * So every function below that used to multiply a glyph count by
 * GFX_GLYPH_W(scale) now sums each glyph's own advance instead, via
 * glyph_for()'s w field. That is the entire shape of this change --
 * nothing outside this file had to move, because nothing outside this
 * file did its own per-glyph layout math; everything else only ever
 * asked gfx_text_w() for a total (checked by grepping every caller of
 * GFX_GLYPH_W before touching this file: all four uses were already
 * inside gfx.c, and every external caller wanted a sum, not a stride).
 */

/*
 * A glyph's bitmap and its width together, because from ark12_glyph()
 * onward nothing here can assume one without the other any more. bits is
 * NULL for a character that occupies no cell at all (soft hyphen); w is
 * meaningless in that case and the caller must not read it, matching
 * ark12_glyph()'s own contract for a failed lookup.
 */
typedef struct {
    const uint16_t *bits;
    int w;
} glyph_t;

/*
 * Drawn for anything the table does not have. Two of them, not one --
 * the box has to claim a width, and a narrow box in the middle of a run
 * of fullwidth glyphs would misalign everything after it as badly as
 * drawing no box at all. Which one is chosen is decided by cp_is_wide()
 * below, from the codepoint alone, since a glyph that failed the lookup
 * has by definition no bitmap of its own to measure.
 */
static const uint16_t NOTDEF_HALF[ARK12_H] = {
    0x000, 0x000, 0x01F, 0x011, 0x011, 0x011, 0x011, 0x011, 0x011, 0x01F,
    0x000, 0x000
};
static const uint16_t NOTDEF_FULL[ARK12_H] = {
    0x000, 0x000, 0x7FF, 0x401, 0x401, 0x401, 0x401, 0x401, 0x401, 0x7FF,
    0x000, 0x000
};

/*
 * Is a codepoint the kind of thing this font draws fullwidth, for a
 * codepoint that is NOT in the table -- ark12_glyph() already answers
 * this correctly for one that is, from the width it was generated with.
 *
 * This is not the Unicode East Asian Width property in full, which has
 * categories this player has no use for (Ambiguous, in particular, is a
 * whole table of its own and a policy decision, not a fact). It is the
 * blocks a music library's tags can plausibly contain and that ark12's
 * RANGES draws fullwidth when it draws them at all: Hangul (in case a
 * Korean title arrives despite the font having no glyphs for it -- see
 * tools/gen_ark12.py), Hiragana through CJK Compatibility, the two CJK
 * Unified Ideograph blocks in range, CJK Compatibility Ideographs, and
 * the Fullwidth Forms taggers use for fullwidth Latin and punctuation.
 * Getting this wrong for a codepoint outside ark12's subset costs one
 * misjudged notdef box, not a crash and not a misdecoded string.
 */
static bool cp_is_wide(uint32_t cp)
{
    return (cp >= 0x1100  && cp <= 0x115F)  ||  /* Hangul Jamo */
           (cp >= 0x2E80  && cp <= 0x303E)  ||  /* CJK radicals, symbols & punctuation */
           (cp >= 0x3041  && cp <= 0x33FF)  ||  /* Hiragana .. CJK Compatibility */
           (cp >= 0x3400  && cp <= 0x4DBF)  ||  /* CJK Unified Ext-A */
           (cp >= 0x4E00  && cp <= 0x9FFF)  ||  /* CJK Unified Ideographs */
           (cp >= 0xA960  && cp <= 0xA97F)  ||  /* Hangul Jamo Extended-A */
           (cp >= 0xAC00  && cp <= 0xD7A3)  ||  /* Hangul Syllables */
           (cp >= 0xF900  && cp <= 0xFAFF)  ||  /* CJK Compatibility Ideographs */
           (cp >= 0xFF00  && cp <= 0xFF60)  ||  /* Fullwidth Forms */
           (cp >= 0xFFE0  && cp <= 0xFFE6);     /* Fullwidth Signs */
}

/*
 * Decode one codepoint, advancing *p past it. Returns 0 at end of
 * string.
 *
 * Overlong forms, surrogates and continuation bytes appearing where a
 * lead byte should are all rejected as one bad byte each. That is
 * stricter than it needs to be for drawing text, and it is the
 * difference between a corrupt tag costing one glyph and a corrupt tag
 * costing the rest of the row.
 */
static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    const unsigned char b = s[0];

    if (b == 0) return 0;

    int n;
    uint32_t cp;
    if      (b < 0x80)          { *p += 1; return b; }
    else if ((b & 0xE0) == 0xC0) { n = 1; cp = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { n = 2; cp = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { n = 3; cp = b & 0x07; }
    else                         { *p += 1; return 0xFFFD; }

    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += 1; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }

    static const uint32_t min_for[4] = { 0, 0x80, 0x800, 0x10000 };
    if (cp < min_for[n] || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *p += 1;
        return 0xFFFD;
    }

    *p += n + 1;
    return cp;
}

/*
 * Codepoint to bitmap, with the two Latin-1 spacing characters Ark does
 * not draw handled here rather than baked into the table -- they are
 * behaviour, not glyphs, and putting them in the generated file would
 * mean the generator had opinions about rendering.
 *
 * Returns NULL for a character that occupies no cell at all (soft
 * hyphen), which the callers skip without advancing x.
 */
static glyph_t glyph_for(uint32_t cp)
{
    if (cp == 0x00A0) cp = 0x0020;      /* no-break space draws as space */
    if (cp == 0x00AD) return (glyph_t){ NULL, 0 };  /* soft hyphen: not a line break here */

    int w;
    const uint16_t *bits = ark12_glyph(cp, &w);
    if (bits) return (glyph_t){ bits, w };

    return (glyph_t){ cp_is_wide(cp) ? NOTDEF_FULL : NOTDEF_HALF,
                       cp_is_wide(cp) ? ARK12_FULL_W : ARK12_HALF_W };
}

static void blit_glyph(const uint16_t *g, int w, int x, int y, int scale, uint16_t c)
{
    for (int row = 0; row < ARK12_H; row++) {
        const uint16_t bits = g[row];
        for (int col = 0; col < w; col++) {
            if (!(bits & (1u << col))) continue;
            gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, c);
        }
    }
}

void gfx_draw_char(int x, int y, uint32_t cp, int scale, uint16_t c)
{
    glyph_t g = glyph_for(cp);
    if (g.bits) blit_glyph(g.bits, g.w, x, y, scale, c);
}

void gfx_draw_text_clipped(int x, int y, int win_x, int win_w,
                           const char *s, int scale, uint16_t c)
{
    if (!s || !*s || win_w <= 0) return;
    const int win_x1 = win_x + win_w;

    int cx = x;
    uint32_t cp;
    while ((cp = utf8_next(&s)) != 0) {
        glyph_t g = glyph_for(cp);
        if (!g.bits) continue;

        const int adv = (g.w + 1) * scale;
        const int gx = cx;
        cx += adv;

        /* Wholly left of the window: keep going, the string runs
         * rightward. Wholly right of it: nothing after this can be
         * visible either, so stop -- a 200 character title otherwise
         * costs 200 glyph draws to show forty. */
        if (gx + adv <= win_x) continue;
        if (gx >= win_x1) break;

        for (int row = 0; row < ARK12_H; row++) {
            const uint16_t bits = g.bits[row];
            for (int col = 0; col < g.w; col++) {
                if (!(bits & (1u << col))) continue;
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
    int w = 0;
    uint32_t cp;
    while ((cp = utf8_next(&s)) != 0) {
        glyph_t g = glyph_for(cp);
        if (!g.bits) continue;
        w += (g.w + 1) * scale;
    }
    return w;
}

void gfx_draw_text(int x, int y, const char *s, int scale, int max_w, uint16_t c)
{
    if (!s || !*s || max_w <= 0) return;

    if (gfx_text_w(s, scale) <= max_w) {
        int cx = x;
        uint32_t cp;
        while ((cp = utf8_next(&s)) != 0) {
            glyph_t g = glyph_for(cp);
            if (!g.bits) continue;
            blit_glyph(g.bits, g.w, cx, y, scale, c);
            cx += (g.w + 1) * scale;
        }
        return;
    }

    /* Doesn't fit as-is: draw what fits ahead of a three-dot ellipsis.
     * The dots are always Latin regardless of the string's own script,
     * so their width is the narrow advance -- GFX_GLYPH_W, not a
     * per-glyph one -- and that does not change with what surrounds
     * them.
     *
     * The bail-out below is checked against that same narrow advance,
     * same as the original "room < 4" guard was: it is a check that
     * *something* plus the dots can fit, not a guarantee that the
     * specific next glyph will, because a fullwidth glyph can still lose
     * that comparison once real widths are walked below. That leaves the
     * ellipsis drawn on its own in the rare case where budget admits a
     * narrow glyph but the string's last-fitting candidate is fullwidth
     * -- three dots and nothing else is still a more honest answer than
     * silently dropping the ellipsis or overrunning max_w. */
    const int dot_adv = GFX_GLYPH_W(scale);
    const int dots_w = 3 * dot_adv;
    if (max_w < dots_w + dot_adv) return;

    const int budget = max_w - dots_w;
    int cx = x, used = 0;
    uint32_t cp;
    while ((cp = utf8_next(&s)) != 0) {
        glyph_t g = glyph_for(cp);
        if (!g.bits) continue;
        const int adv = (g.w + 1) * scale;
        if (used + adv > budget) break;
        blit_glyph(g.bits, g.w, cx, y, scale, c);
        cx += adv;
        used += adv;
    }
    for (int i = 0; i < 3; i++) {
        gfx_draw_char(cx, y, '.', scale, c);
        cx += dot_adv;
    }
}

/* How many glyphs gfx_draw_text_tail() ever needs to remember at once --
 * bounded independent of the string's length, which matters because the
 * caller here includes browser.c's path row and a path can run to
 * hundreds of bytes (storage_join_path()'s buffers are 512).
 *
 * The bound: the narrowest advance anywhere in this UI is GFX_GLYPH_W at
 * LABEL_SCALE (2), 12 px, and the panel is 720 px, so at most 60 cells
 * could ever be visible regardless of scale or script. 96 leaves margin
 * without being tuned to one caller's constants. Nothing above this
 * count is ever held -- see the ring buffer below -- so a long path
 * costs one pass over its bytes and a fixed 96-entry stack array, not
 * memory proportional to its length. */
#define TAIL_MAX_GLYPHS  (96)

/* Keeps the tail. A truncated path with the head kept reads "/sd/Music/Th"
 * for every directory on the card; with the tail kept it reads
 * "...st Album", which is the part that says where you are. */
void gfx_draw_text_tail(int x, int y, const char *s, int scale, int max_w, uint16_t c)
{
    if (!s || !*s || max_w <= 0) return;

    if (gfx_text_w(s, scale) <= max_w) {
        gfx_draw_text(x, y, s, scale, max_w, c);
        return;
    }

    const int dot_adv = GFX_GLYPH_W(scale);
    const int dots_w = 3 * dot_adv;
    if (max_w < dots_w + dot_adv) return;
    const int budget = max_w - dots_w;

    /* UTF-8 has no shortcut for walking a string backward, so it is
     * decoded once, forward, into a ring of the last TAIL_MAX_GLYPHS
     * glyphs seen -- a safe superset of any tail that could actually fit
     * in budget, per the constant's own comment. head is the index the
     * *next* write would land on, which after at least one wrap is also
     * the oldest surviving entry -- ordinary ring-buffer bookkeeping. */
    struct { glyph_t g; int adv; } ring[TAIL_MAX_GLYPHS];
    int n = 0, head = 0;
    uint32_t cp;
    while ((cp = utf8_next(&s)) != 0) {
        glyph_t g = glyph_for(cp);
        if (!g.bits) continue;
        ring[head].g = g;
        ring[head].adv = (g.w + 1) * scale;
        head = (head + 1) % TAIL_MAX_GLYPHS;
        if (n < TAIL_MAX_GLYPHS) n++;
    }

    /* Walk newest-to-oldest accumulating width until the next entry
     * would exceed budget; keep counts how many trailing glyphs survive
     * that walk, which is exactly the tail this function exists to
     * draw. */
    int acc = 0, keep = 0;
    for (int i = 0; i < n; i++) {
        const int idx = (head - 1 - i + TAIL_MAX_GLYPHS) % TAIL_MAX_GLYPHS;
        if (acc + ring[idx].adv > budget) break;
        acc += ring[idx].adv;
        keep++;
    }

    int cx = x;
    for (int i = 0; i < 3; i++) { gfx_draw_char(cx, y, '.', scale, c); cx += dot_adv; }

    /* keep-1 is the oldest of the retained glyphs (leftmost once drawn)
     * and 0 is the newest (the string's actual last character), so
     * walking that direction draws left to right. */
    for (int i = keep - 1; i >= 0; i--) {
        const int idx = (head - 1 - i + TAIL_MAX_GLYPHS) % TAIL_MAX_GLYPHS;
        blit_glyph(ring[idx].g.bits, ring[idx].g.w, cx, y, scale, c);
        cx += ring[idx].adv;
    }
}
