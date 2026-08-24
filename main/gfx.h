/*
 * gfx.h -- the primitives ui.c already had, moved out so browser.c can use
 * them too.
 *
 * This file introduces nothing. Every function here was a static in ui.c
 * and is unchanged apart from the name; the extraction happened because
 * the file chooser needs rectangles, circles, seven-segment digits and
 * clipped font8x8 text, and those are exactly the five things the
 * transport bar needed. A second copy is the thing that drifts -- the
 * same argument the README already makes for id3_read_tags() living in
 * albumart.c.
 *
 * There is one framebuffer and one panel, so this keeps them in statics
 * rather than threading a context through every call. ui_init() is what
 * fills them in.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#include "ark10.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RGB565, because that is what panel_init() configures and what
 * albumart.c writes. */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/*
 * Everything is drawn into a shadow buffer in PSRAM and copied to the
 * panel a region at a time, rather than written into the framebuffer the
 * DPI peripheral is scanning out of.
 *
 * The old way is why the screen flashed. There is one framebuffer and the
 * panel reads it continuously, so every intermediate state was displayed:
 * the bar cleared to grey before its contents arrived, and -- much worse
 * -- albumart's full-screen memset, which blanked the whole panel and
 * then filled it back in over the length of a PNG decode.
 *
 * The cost is one 1.8 MB PSRAM allocation and a memcpy per blit. The
 * memcpy is of the region actually redrawn, not the screen, which is why
 * every blit here is full-width: a full-width band is contiguous in both
 * buffers and copies in one call.
 */
esp_err_t gfx_init(esp_lcd_panel_handle_t panel, int w, int h);

/* The shadow, not the panel's own buffer. Safe to draw into at any time. */
uint16_t *gfx_fb(void);
int gfx_w(void);
int gfx_h(void);

/* Copy rows [y0, y1) of the shadow to the panel. Nothing appears on
 * screen until this is called, which is the point. */
void gfx_blit(int y0, int y1);

/* Same, but returns the panel's error instead of swallowing it. The
 * drawing code does not care; albumart does, because a cover that failed
 * to reach the panel should not be reported as shown. */
esp_err_t gfx_blit_err(int y0, int y1);

void gfx_px(int x, int y, uint16_t c);
void gfx_fill_rect(int x, int y, int w, int h, uint16_t c);
void gfx_fill_circle(int cx, int cy, int r, uint16_t c);
void gfx_ring(int cx, int cy, int r, int thick, uint16_t c);

/* Arc from 12 o'clock, clockwise, covering pct of the circle. */
void gfx_ring_arc(int cx, int cy, int r, int thick, int pct, uint16_t c);

/* Seven-segment metrics, exported because callers lay out around them. */
#define GFX_DIG_W       (20)
#define GFX_DIG_H       (38)
#define GFX_DIG_T       (5)
#define GFX_DIG_GAP     (5)
#define GFX_TIME_W      (4 * GFX_DIG_W + 3 * GFX_DIG_GAP + GFX_DIG_W / 2 + GFX_DIG_GAP)

#define GFX_SDIG_W      (14)
#define GFX_SDIG_H      (26)
#define GFX_SDIG_T      (3)
#define GFX_SDIG_GAP    (4)

void gfx_draw_time(int x, int y, uint32_t sec, uint16_t c);

/* Remaining time, with a leading minus. Its own function rather than a
 * flag on gfx_draw_time() because the minus changes the width, and the
 * caller right-justifies it -- a caller that has to know whether the sign
 * is there in order to place the run may as well ask for it by name. */
#define GFX_TIME_NEG_W  (GFX_DIG_W + GFX_DIG_GAP + GFX_TIME_W)
void gfx_draw_time_neg(int x, int y, uint32_t sec, uint16_t c);
void gfx_draw_small_time_centred(int cx, int y, uint32_t sec, uint16_t c);
void gfx_draw_pct_centred(int cx, int y, int pct, uint16_t c);

/*
 * ark10, integer-scaled. One column of gap per glyph, as font8x8 had.
 *
 * Ark's monospaced halfwidth advance is 5, not 6 -- the glyphs are drawn
 * with their own right bearing inside the cell. The extra column is here
 * anyway because a handful of them do not have it: the tilde on U+00F1
 * and the ogonek on U+0118 reach the fifth column, and at scale 3 a
 * n-tilde touching the next letter is exactly the kind of thing that
 * reads as a rendering bug rather than as a typeface.
 *
 * GFX_GLYPH_H exists because callers were centring text with a literal
 * `8 * scale`. The glyph is 10 tall now. Anything vertical that still
 * says 8 is a layout bug waiting for a European filename.
 */
#define GFX_GLYPH_W(scale)  (ARK10_W * (scale) + (scale))
#define GFX_GLYPH_H(scale)  (ARK10_H * (scale))

/* Takes a Unicode codepoint, not a byte -- the strings the other
 * functions here walk are UTF-8, and a char cannot name 'ł'. */
void gfx_draw_char(int x, int y, uint32_t cp, int scale, uint16_t c);

/* Left-aligned, clipped to max_w with an ellipsis. No reflow. */
void gfx_draw_text(int x, int y, const char *s, int scale, int max_w, uint16_t c);

/* Same, but the tail is kept rather than the head -- for paths, where the
 * end is the part that identifies the directory. */
void gfx_draw_text_tail(int x, int y, const char *s, int scale, int max_w, uint16_t c);

/*
 * Left-aligned at x, clipped to the window [win_x, win_x + win_w) --
 * with no ellipsis, and with x allowed to fall outside the window on
 * either side.
 *
 * That last part is the whole point: it is what a marquee is. The
 * existing gfx_draw_text() truncates to a character boundary and adds
 * dots, which is the right answer for a list of filenames and the wrong
 * one for a title sliding past a fixed opening, where a glyph has to be
 * drawn half in and half out.
 */
void gfx_draw_text_clipped(int x, int y, int win_x, int win_w,
                           const char *s, int scale, uint16_t c);

/* Pixel width the string would occupy unclipped. Counts glyph cells,
 * not bytes: a UTF-8 string is narrower than strlen() suggests. */
int gfx_text_w(const char *s, int scale);

#ifdef __cplusplus
}
#endif
