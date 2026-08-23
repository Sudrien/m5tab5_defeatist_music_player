/*
 * waveform.h -- the loudness envelope, drawn as a frame around the cover
 * or as a fill when there is none.
 *
 * Not animated and not a progress indicator. It is drawn once when the
 * scan finishes and then left alone, which is what lets the scan run at
 * whatever pace the card allows instead of having to keep up with
 * playback.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "framewalk.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One renderer, two shapes.
 *
 * The envelope is always drawn the same way -- mirrored about the middle
 * of the artwork area, one column per pixel of width. WAVE_FRAME differs
 * only in punching a black square out of the middle for the cover to sit
 * in, so the result reads as a frame around it rather than as a separate
 * kind of drawing.
 *
 * This replaced a band that ran round the cover's perimeter, four sides
 * with the song's four quarters. It looked deliberate but it was a second
 * layout to reason about, it needed corner gaps to avoid looking broken,
 * and the time axis ran clockwise -- which nothing else on screen does.
 * Left to right in both cases is one idea instead of two.
 */
typedef enum {
    WAVE_FILL = 0,  /* no cover: the envelope fills the area */
    WAVE_FRAME,     /* cover present: same envelope, black square cut out */
} wave_mode_t;

/*
 * Side of the black square cut out for the cover.
 *
 * Fixed rather than derived from the cover, because albumart.c does not
 * report the rectangle it drew into. 548 leaves a 24 px margin around a
 * 500 px cover; a cover larger than 548 would be clipped by the envelope
 * drawn around it. The fix is for albumart_show() to return its rect, not
 * for this to guess.
 */
#define WAVE_INNER      (548)

/*
 * Draw into the gfx shadow and blit.
 *
 * art_h is the height of the artwork area -- the panel less the transport
 * bar. Must be called from the same task that draws everything else; this
 * writes the shared framebuffer and does not lock.
 */
void waveform_draw(const framewalk_t *w, wave_mode_t mode, int art_h);

#ifdef __cplusplus
}
#endif
