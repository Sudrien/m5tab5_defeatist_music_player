/*
 * waveform.h -- the loudness envelope, drawn as the seek bar.
 *
 * It used to be a frame around the cover art (or a fill behind it) in the
 * artwork area, mirrored about a centre line, and it was not a progress
 * indicator. It is both now: one shape, low down in the transport bar,
 * standing on the seek bar's baseline and coloured by how much of the
 * track has played.
 *
 * Three changes, and they are one idea:
 *
 *   1. Upper sideband only. The mirrored envelope spent half its pixels
 *      restating the other half -- global_gain is a magnitude, so the
 *      lower lobe carried no information the upper one did not. Dropping
 *      it buys the same detail in half the height, which is what makes it
 *      fit in the bar at all.
 *   2. Down into the bar. Up in the artwork it competed with the cover
 *      for the same rectangle, and needed a cutout to avoid it.
 *   3. Combined with the seek bar. There were two horizontal, left to
 *      right, time-axis things on screen, one of which showed where you
 *      are and one of which showed what the song looks like. They are the
 *      same axis, so they are now the same object.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "framewalk.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Take a copy of a completed walk, or clear it with NULL.
 *
 * A copy rather than a pointer: the walk lives in the scan task's
 * framewalk_t, which is overwritten the moment the next track starts
 * scanning, and the UI reads its levels on every repaint. The old code
 * drew once from a handoff flag and never looked again, so aliasing was
 * safe; a bar redrawn ten times a second is a live reader and needs its
 * own copy. It is a kilobyte.
 *
 * The normalisation range is computed here, once, instead of on every
 * repaint.
 */
void waveform_set(const framewalk_t *w);

/* Whether there is an envelope to draw. The caller falls back to a plain
 * slider when there is not -- a track still has to be seekable while the
 * scan is running. */
bool waveform_ready(void);

/*
 * Draw the envelope standing on base_y, growing upward, spanning
 * x0..x1 (exclusive), at most `height` tall.
 *
 * pct 0..100 splits it: columns left of the split get `past`, the rest
 * get `future`. The split is a position in the bar, not in the envelope
 * -- passing the drag percentage rather than the playback percentage is
 * what makes a drag preview itself.
 *
 * Draws into the gfx shadow and does not blit; the caller owns the blit,
 * because this is one element of a bar that is pushed in one go.
 */
/*
 * The same bar with every column at full height, for before the
 * envelope exists.
 *
 * Needs no scan and no data -- only the position -- so it works from the
 * first frame of a track, and shares waveform_draw_bar()'s geometry
 * exactly so the row does not change shape when the real envelope
 * replaces it.
 */
void waveform_draw_flat(int x0, int x1, int base_y, int height, int pct,
                        uint16_t past, uint16_t future);

void waveform_draw_bar(int x0, int x1, int base_y, int height, int pct,
                       uint16_t past, uint16_t future);

#ifdef __cplusplus
}
#endif
