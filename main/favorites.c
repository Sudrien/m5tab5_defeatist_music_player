/*
 * favorites.c -- see favorites.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "favmatch.h"
#include "favorites.h"
#include "stations.h"
#include "storage.h"
#include "storage_io.h"

static const char *TAG = "tab5_favs";

static station_t        *s_list;
static int               s_count;
static storage_id_t      s_vol = STORAGE_COUNT;
static SemaphoreHandle_t s_lock;

static void lock_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

/* ------------------------------------------------------------------ */
/* Matching                                                            */
/* ------------------------------------------------------------------ */

bool favorites_url_eq(const char *a, const char *b)
{
    /* The rule itself is in favmatch.h, which is header-only so that
     * texttest can compile it. This is the export -- callers outside
     * this module ask through favorites.h and do not need to know the
     * matching lives in its own header. */
    return favmatch_eq(a, b);
}

bool favorites_contains(const char *url)
{
    if (!url || !url[0]) return false;
    lock_init();

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (favorites_url_eq(s_list[i].url, url)) { found = true; break; }
    }
    xSemaphoreGive(s_lock);
    return found;
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */

static bool path_for(storage_id_t id, char *out, size_t out_size,
                     const char *name)
{
    const char *mount = storage_mount_path(id);
    if (!mount) return false;
    const int n = snprintf(out, out_size, "%s/%s", mount, name);
    return n > 0 && (size_t)n < out_size;
}

/*
 * Which volume a write goes to. See favorites.h: the favourites' own
 * volume, then the station list's, then the first present one.
 *
 * The middle term is the one that earns its place. The first star on a
 * card has no favourites file to belong to, and "the volume the list I
 * am looking at came from" is the only other thing that means anything
 * to the person pressing the button.
 */
static storage_id_t write_volume(void)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const storage_id_t mine = s_vol;
    xSemaphoreGive(s_lock);

    if (mine < STORAGE_COUNT && storage_present(mine)) return mine;

    const storage_id_t theirs = stations_volume();
    if (theirs < STORAGE_COUNT && storage_present(theirs)) return theirs;

    for (int id = 0; id < STORAGE_COUNT; id++) {
        if (storage_present((storage_id_t)id)) return (storage_id_t)id;
    }
    return STORAGE_COUNT;
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */
/* ------------------------------------------------------------------ */

static size_t read_file(storage_id_t id, char *buf, size_t buf_size)
{
    char path[128];
    if (!path_for(id, path, sizeof(path), FAVORITES_FILENAME)) return 0;

    FILE *f = storage_io_open(path, "r");
    if (!f) return 0;

    /* BACKGROUND, per chunk, no lease held across the loop -- the same
     * rule and the same reason as stations.c's read_file(). */
    size_t got = 0;
    while (got < buf_size) {
        const size_t n = storage_io_fread(buf + got, buf_size - got, f,
                                          STORAGE_IO_BACKGROUND);
        if (n == 0) break;
        got += n;
    }
    storage_io_close(f);
    return got;
}

/* Install a list under the lock and free the old one after the pointer
 * has moved. `fresh` may be NULL with `count` 0, which is how a card
 * with no favourites file empties the set. */
static void install(station_t *fresh, int count, storage_id_t from)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    station_t *old = s_list;
    s_list = fresh;
    s_count = count;
    s_vol = from;
    xSemaphoreGive(s_lock);
    free(old);
}

bool favorites_load(void)
{
    char *text = heap_caps_malloc(STATIONS_FILE_MAX, MALLOC_CAP_SPIRAM);
    if (!text) {
        ESP_LOGE(TAG, "no PSRAM to read the favourites");
        return false;
    }

    size_t n = 0;
    storage_id_t from = STORAGE_COUNT;
    for (int id = 0; id < STORAGE_COUNT; id++) {
        if (!storage_present((storage_id_t)id)) continue;
        n = read_file((storage_id_t)id, text, STATIONS_FILE_MAX);
        if (n > 0) { from = (storage_id_t)id; break; }
    }

    if (from == STORAGE_COUNT) {
        /*
         * No file anywhere. Not a warning: it is what every card looks
         * like until somebody stars something. The set is emptied even
         * so -- a card swapped for one with no favourites must not
         * leave the previous card's stars showing.
         */
        free(text);
        install(NULL, 0, STORAGE_COUNT);
        return false;
    }

    station_t *fresh = heap_caps_calloc(STATIONLIST_MAX, sizeof(station_t),
                                        MALLOC_CAP_SPIRAM);
    if (!fresh) {
        ESP_LOGE(TAG, "no PSRAM for %d favourites", STATIONLIST_MAX);
        free(text);
        return false;
    }

    stationlist_stats_t stats;
    const int count = stationlist_parse(text, n, fresh, STATIONLIST_MAX,
                                        &stats);
    free(text);

    if (count <= 0) {
        free(fresh);
        install(NULL, 0, from);
        ESP_LOGW(TAG, "%s parsed to 0 (%d bad scheme, %d too long)",
                 FAVORITES_FILENAME, stats.bad_scheme, stats.too_long);
        return false;
    }

    install(fresh, count, from);
    ESP_LOGI(TAG, "%d favourite%s from %s", count, count == 1 ? "" : "s",
             storage_mount_path(from));
    return true;
}

int favorites_count(void)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = s_count;
    xSemaphoreGive(s_lock);
    return n;
}

storage_id_t favorites_volume(void)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const storage_id_t v = s_vol;
    xSemaphoreGive(s_lock);
    return v;
}

bool favorites_get(int i, station_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = (i >= 0 && i < s_count);
    if (ok) *out = s_list[i];
    xSemaphoreGive(s_lock);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

/* See stations.c's ends_with_newline(): a hand-edited file whose last
 * line has no terminator is common, and appending to one without a
 * separator joins two entries and loses both. */
static bool ends_with_newline(const char *path)
{
    FILE *f = storage_io_open(path, "r");
    if (!f) return true;

    bool ok = true;
    if (fseek(f, -1, SEEK_END) == 0) {
        char last = '\n';
        if (fread(&last, 1, 1, f) == 1) ok = (last == '\n');
    }
    storage_io_close(f);
    return ok;
}

bool favorites_add(const char *name, const char *url)
{
    char entry[STATION_NAME_MAX + STATION_URL_MAX + 32];
    const size_t len = station_entry(name, url, entry, sizeof(entry));
    if (len == 0) {
        ESP_LOGW(TAG, "refusing to star: bad name or URL");
        return false;
    }

    if (favorites_contains(url)) return true;   /* already starred */

    if (favorites_count() >= STATIONLIST_MAX) {
        ESP_LOGW(TAG, "favourites are full (%d); not starring",
                 STATIONLIST_MAX);
        return false;
    }

    const storage_id_t target = write_volume();
    if (target == STORAGE_COUNT) {
        ESP_LOGW(TAG, "no volume to write %s to", FAVORITES_FILENAME);
        return false;
    }

    char path[128];
    if (!path_for(target, path, sizeof(path), FAVORITES_FILENAME)) return false;

    const bool separate = !ends_with_newline(path);

    /* One open, one write, one close, no lease across it -- exactly
     * stations_append(). "a" creates the file, so the first star on a
     * volume that has never had one needs no special case. */
    FILE *f = storage_io_open(path, "a");
    if (!f) {
        ESP_LOGW(TAG, "cannot append to %s (%s)", path, strerror(errno));
        return false;
    }

    bool wrote = true;
    if (separate) wrote = (fwrite("\n", 1, 1, f) == 1);
    if (wrote) wrote = (fwrite(entry, 1, len, f) == len);
    const bool flushed = (fflush(f) == 0);
    const int  flush_errno = errno;
    const bool closed = (storage_io_close(f) == 0);

    if (!wrote || !flushed || !closed) {
        ESP_LOGW(TAG, "starring failed (%s)",
                 strerror(flushed ? errno : flush_errno));
        return false;
    }

    ESP_LOGI(TAG, "starred: %s", url);
    favorites_load();
    /* The star counts only if the parser reads it back -- the same
     * round trip stations_append() insists on. */
    return favorites_contains(url);
}

bool favorites_remove(const char *url)
{
    if (!url || !url[0]) return false;
    if (!favorites_contains(url)) return true;  /* already not starred */

    const storage_id_t target = favorites_volume();
    if (target == STORAGE_COUNT || !storage_present(target)) {
        ESP_LOGW(TAG, "the favourites' volume is gone; not unstarring");
        return false;
    }

    char path[128], tmp[128];
    if (!path_for(target, path, sizeof(path), FAVORITES_FILENAME) ||
        !path_for(target, tmp, sizeof(tmp), FAVORITES_TEMPNAME)) {
        return false;
    }

    /*
     * Written from the PARSED list rather than by filtering the file's
     * bytes. That loses hand-written comments, and it is still the
     * right choice: filtering text means deciding which of a `#EXTINF`
     * and its URL to drop and what to do with a directive between them,
     * which is a second parser. The list is what the file means.
     */
    FILE *f = storage_io_open(tmp, "w");
    if (!f) {
        ESP_LOGW(TAG, "cannot write %s (%s)", tmp, strerror(errno));
        return false;
    }

    bool ok = (fputs("#EXTM3U\n", f) >= 0);
    int kept = 0;
    const int n = favorites_count();
    for (int i = 0; ok && i < n; i++) {
        station_t st;
        if (!favorites_get(i, &st)) continue;
        if (favorites_url_eq(st.url, url)) continue;

        char entry[STATION_NAME_MAX + STATION_URL_MAX + 32];
        const size_t len = station_entry(st.name, st.url, entry, sizeof(entry));
        if (len == 0) continue;     /* unwritable: drop rather than corrupt */
        ok = (fwrite(entry, 1, len, f) == len);
        kept++;
    }

    const bool flushed = (fflush(f) == 0);
    const int  flush_errno = errno;
    const bool closed = (storage_io_close(f) == 0);

    if (!ok || !flushed || !closed) {
        ESP_LOGW(TAG, "unstarring failed (%s); leaving %s alone",
                 strerror(flushed ? errno : flush_errno), FAVORITES_FILENAME);
        remove(tmp);
        return false;
    }

    /*
     * One rename over the original. No .bak: see favorites.h. Power
     * lost before this leaves the original list whole and costs a temp
     * file the next removal overwrites.
     *
     * The lease is taken around the rename alone, so a playback read
     * can get in front of it.
     */
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    const int installed = rename(tmp, path);
    storage_io_release();

    if (installed != 0) {
        ESP_LOGW(TAG, "could not install %s (%s)", path, strerror(errno));
        remove(tmp);
        return false;
    }

    ESP_LOGI(TAG, "unstarred: %s (%d left)", url, kept);
    favorites_load();
    return !favorites_contains(url);
}

bool favorites_toggle(const char *name, const char *url)
{
    if (!url || !url[0]) return false;

    if (favorites_contains(url)) {
        favorites_remove(url);
    } else {
        favorites_add(name, url);
    }
    /* What it IS now, not what was asked for: a failed write leaves the
     * button showing the truth. */
    return favorites_contains(url);
}
