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
    /*
     * A station from the RADIO tab. `index` is its position in the
     * station list and `path` is NULL.
     *
     * The index rather than the URL, deliberately: stations.c owns the
     * list, the name and the URL, and handing back a copy of two of
     * those would give the player a station that could disagree with
     * the one stations_index() reports a moment later. The player sets
     * the index and reads the station back, so there is one answer to
     * "which station is this" and stations.c has it.
     */
    BROWSER_PLAY_STREAM,
    BROWSER_CANCELLED,      /* closed without choosing */
    /*
     * The RADIO tab wants the station list read off the card again.
     *
     * A request and not a result: `path` is NULL and `index` is -1, the
     * chooser stays open, and nothing has happened yet. The caller has
     * to perform it, because stations_load() opens a file and the task
     * that polls touch must not block on the card -- which is the whole
     * reason the chooser could not reload until now.
     *
     * Answered by browser_stations_reloaded(), whenever the load has
     * actually been done.
     */
    BROWSER_RELOAD_STATIONS,
} browser_result_kind_t;

typedef struct {
    browser_result_kind_t kind;
    const char *path;       /* owned by browser.c, valid until the next call */
    int index;              /* BROWSER_PLAY_STREAM only; -1 otherwise */
} browser_result_t;

/*
 * The station list has been re-read; rebuild the rows from it.
 *
 * Called on the task that polls touch, after some other task has done
 * the load, and only that task ever touches the row array. A no-op
 * unless the RADIO tab is the one showing: the rows belong to whichever
 * tab is selected, and rebuilding station rows under a directory
 * listing would replace the listing with stations.
 */
void browser_stations_reloaded(void);

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
