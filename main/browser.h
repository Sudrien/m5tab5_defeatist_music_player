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

    /*
     * A row of the radio menu wants fetching from the directory.
     * `index` is the row, which radiobrowser_menu_kind() turns into a
     * request -- the chooser deliberately does not know what a row
     * means, for the same reason it does not know what a station is:
     * one table, in radiobrowser.h, and two callers that cannot drift.
     *
     * Requested rather than performed, like the reload beside it and
     * more so: this one waits on a network. The chooser stays open and
     * says it is fetching; the rows arrive through
     * browser_stations_reloaded() when the player task has them.
     */
    BROWSER_FETCH_STATIONS,

    /*
     * Open the web form that adds a station to stations.m3u -- the
     * radio menu's "Add a station by phone..." row.
     *
     * Requested rather than performed, like the two above: it starts an
     * HTTP server, and starting one is the player task's business. The
     * chooser closes, because the next thing to look at is the panel,
     * which is where the address to type into a phone is shown.
     */
    BROWSER_ADD_STATION,

    /*
     * The radio menu's "Starred stations" row: load favorites.m3u and
     * show it as the station list.
     *
     * Requested rather than performed, like the three above, and for
     * stations_load()'s reason -- it opens a file on the card and the
     * task that polls touch must not. The rows arrive through
     * browser_stations_reloaded() once the player has them.
     */
    BROWSER_LOAD_FAVORITES,

    /*
     * Star or unstar the station on row `index`.
     *
     * The chooser's half of the star. The panel's button acts on what
     * is PLAYING; this acts on what was tapped, which is not the same
     * station and usually not playing at all -- starring something off
     * a chart without listening to it first is the ordinary case.
     *
     * Requested rather than performed: it writes to the card. The
     * chooser stays open and the row does not change until the player
     * has done it and called browser_stations_reloaded().
     */
    BROWSER_TOGGLE_FAVORITE,

    /*
     * Star or unstar a file or folder on a volume tab. `path` is the
     * row's full path and `index` is 1 for a folder, 0 for a file.
     *
     * Requested, like the station star: it writes starred.m3u, so the
     * player does it and then calls browser_stars_changed(). A mark
     * only, for now -- see starred.h.
     */
    BROWSER_TOGGLE_STAR,
} browser_result_kind_t;

typedef struct {
    browser_result_kind_t kind;
    const char *path;       /* owned by browser.c, valid until the next call */
    int index;              /* BROWSER_PLAY_STREAM, _FETCH_STATIONS and
                             * _TOGGLE_FAVORITE; -1 otherwise */
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

/* Local stars changed; re-resolve the rows' marks. Same task and same
 * rule as browser_stations_reloaded(), for the volume tabs. */
void browser_stars_changed(void);

/*
 * A line for the radio tab's status row, replacing the usual one until
 * the next list arrives: "fetching jazz...", or why it did not.
 *
 * Copied. NULL clears it. Safe from the player task -- it is a string
 * this task writes and ui_task reads, one buffer, and a torn line is a
 * row of text that is briefly wrong rather than anything worse.
 */
void browser_set_radio_status(const char *line);

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

/*
 * Which station is playing, as its index in the station list, or -1 for
 * none. The RADIO tab's marker; the volume tabs ignore it.
 *
 * An index rather than a name or a URL, for the reason
 * BROWSER_PLAY_STREAM is an index: stations.c owns the list and a
 * position in it is the only thing that cannot disagree with itself.
 */
void browser_set_station(int index);

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
