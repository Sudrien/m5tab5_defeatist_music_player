/*
 * browser.h -- the file chooser the folder button opens.
 *
 * Full screen rather than a panel over the artwork. The bar is 276 px of
 * a 1280 px panel and the rest is a picture; a chooser that respected
 * that would get eleven rows in the gap and need scrolling twice as
 * often, and the artwork is not information while you are picking a
 * different track anyway.
 *
 * One tab per volume, and the empty one is drawn greyed rather than
 * hidden: a tab that disappears when the card is out and reappears when
 * it is in moves the other tab under the finger.
 *
 * The chooser owns no task. ui_task drives it -- browser_touch() then
 * browser_draw() -- exactly as it drives the transport bar, so there is
 * one thread writing the framebuffer and no lock.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "playlist.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BROWSER_NONE = 0,
    BROWSER_PLAY_FILE,      /* path is a track; its folder becomes the list */
    BROWSER_PLAY_FOLDER,    /* path is a directory; play it from the top */
    BROWSER_CANCELLED,      /* closed without choosing */
} browser_result_kind_t;

typedef struct {
    browser_result_kind_t kind;
    const char *path;       /* owned by browser.c, valid until the next call */
} browser_result_t;

/* Open on the folder of `start` when it is on a mounted volume, otherwise
 * on the first volume that is. Safe to call when nothing is mounted: the
 * chooser opens with both tabs greyed and an empty list, which is a
 * truthful screen and a place to plug something in. */
/*
 * The track that is being HEARD, told to the chooser by the player.
 *
 * The list marks the playing row in the accent colour, and it used to
 * work that out itself from playlist_current(). That is the track the
 * decoder has moved to, which at a boundary is a ring -- twenty seconds
 * -- ahead of the one coming out of the speaker, so the marker jumped a
 * row while the previous song was still playing.
 *
 * The player publishes this at the same handoff that changes the title
 * and the transport bar, so all three move together.
 *
 * NULL or an empty string means nothing is playing, and no row is
 * marked. Calling this is what makes the list redraw; the chooser does
 * not poll for it.
 */
void browser_set_playing(const char *path);

void browser_open(const char *start);

void browser_close(void);
bool browser_is_open(void);

/* One poll of the touch controller, same contract as ui_touch(). */
browser_result_t browser_touch(bool down, int x, int y);

/* Repaint if anything changed. Cheap to call at the UI rate: a full-screen
 * blit only happens when the list, the tabs or the selection moved. */
void browser_draw(void);

/* The repeat mode, cycled from the chooser's footer. The player reads it
 * at the end of every track. */
play_order_t browser_order(void);

#ifdef __cplusplus
}
#endif
