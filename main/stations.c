/*
 * stations.c -- see stations.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "stations.h"
#include "storage.h"
#include "storage_io.h"

static const char *TAG = "tab5_stations";

static station_t         *s_list;
static int                s_count;
static int                s_index;
static storage_id_t       s_vol = STORAGE_COUNT;
static stationlist_stats_t s_stats;
static SemaphoreHandle_t  s_lock;

static void lock_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

/*
 * Read a whole station file into `buf`, returning how many bytes.
 *
 * BACKGROUND class, and that is the point of using storage_io at all
 * here: this can run while a track is playing -- a card inserted
 * mid-album is the case -- and the decode loop's lease must win. 0 on
 * any failure, including the file not existing, which is the normal case
 * on a volume nobody has put a station list on.
 */
static size_t read_file(storage_id_t id, char *buf, size_t buf_size)
{
    const char *mount = storage_mount_path(id);
    if (!mount) return 0;

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", mount, STATIONS_FILENAME);

    FILE *f = storage_io_open(path, "r");
    if (!f) return 0;

    /*
     * No storage_io_acquire() around this. storage_io_fread() takes its
     * own lease per chunk and its header says in terms not to call it
     * while holding one -- wrapping the loop in a lease would hold the
     * card across the whole read and starve the decode loop, which is
     * the exact contention the BACKGROUND class exists to avoid.
     */
    size_t got = 0;
    while (got < buf_size) {
        const size_t n = storage_io_fread(buf + got, buf_size - got, f,
                                          STORAGE_IO_BACKGROUND);
        if (n == 0) break;
        got += n;
    }
    storage_io_close(f);

    ESP_LOGI(TAG, "%s: %u bytes", path, (unsigned)got);
    return got;
}

bool stations_load(void)
{
    lock_init();

    char *text = heap_caps_malloc(STATIONS_FILE_MAX, MALLOC_CAP_SPIRAM);
    if (!text) {
        ESP_LOGE(TAG, "no PSRAM to read a station list");
        return false;
    }

    /*
     * SD first, then USB, and the first one that HAS the file wins --
     * not the first one that parses. A stations.m3u that exists and is
     * empty is a statement, and falling through to the other volume
     * would mean a blank file on the card silently playing the drive's
     * list instead.
     */
    size_t n = 0;
    storage_id_t from = STORAGE_COUNT;
    for (int id = 0; id < STORAGE_COUNT; id++) {
        if (!storage_present((storage_id_t)id)) continue;
        n = read_file((storage_id_t)id, text, STATIONS_FILE_MAX);
        if (n > 0) { from = (storage_id_t)id; break; }
    }
    if (from == STORAGE_COUNT) {
        ESP_LOGI(TAG, "no %s on any volume", STATIONS_FILENAME);
        free(text);
        return false;
    }

    station_t *fresh = heap_caps_calloc(STATIONLIST_MAX, sizeof(station_t),
                                        MALLOC_CAP_SPIRAM);
    if (!fresh) {
        ESP_LOGE(TAG, "no PSRAM for %d stations", STATIONLIST_MAX);
        free(text);
        return false;
    }

    stationlist_stats_t stats;
    const int count = stationlist_parse(text, n, fresh, STATIONLIST_MAX,
                                        &stats);
    free(text);

    if (count <= 0) {
        /*
         * The file was there and produced nothing. Say what it was
         * rejected for rather than just the count: a list of http URLs
         * behind a proxy that only does https is bad_scheme, and a list
         * of 600-character signed URLs is too_long, and those are
         * different things to go and fix.
         */
        ESP_LOGW(TAG, "%s parsed to 0 stations (%d bad scheme, %d too long, "
                      "%d overflowed)", STATIONS_FILENAME, stats.bad_scheme,
                 stats.too_long, stats.overflowed);
        free(fresh);
        return false;
    }

    /*
     * Swapped in whole, under the lock, and the old one freed after the
     * pointer has been replaced. A reader holding the lock sees one list
     * or the other and never a half-built one.
     */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    station_t *old = s_list;
    s_list = fresh;
    s_count = count;
    s_vol = from;
    s_stats = stats;
    /* The index is reset rather than kept. It indexed a different list,
     * and a station list reloaded because a card appeared is a different
     * list even when it happens to be the same length. */
    s_index = 0;
    xSemaphoreGive(s_lock);
    free(old);

    ESP_LOGI(TAG, "%d stations from %s", count, storage_mount_path(from));
    if (stats.bad_scheme || stats.too_long || stats.overflowed) {
        ESP_LOGW(TAG, "skipped: %d bad scheme, %d too long, %d over %d",
                 stats.bad_scheme, stats.too_long, stats.overflowed,
                 STATIONLIST_MAX);
    }
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "  %2d  %s", i + 1, s_list[i].name);
    }
    return true;
}

int stations_count(void)
{
    return s_count;
}

storage_id_t stations_volume(void)
{
    return s_vol;
}

bool stations_get(int i, station_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!s_lock) return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_list && i >= 0 && i < s_count) {
        *out = s_list[i];
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

int stations_index(void)
{
    return s_index;
}

void stations_set_index(int i)
{
    if (s_count <= 0) { s_index = 0; return; }
    if (i < 0 || i >= s_count) return;
    s_index = i;
}

int stations_next(void)
{
    if (s_count <= 0) return 0;
    s_index = (s_index + 1) % s_count;
    return s_index;
}

int stations_prev(void)
{
    if (s_count <= 0) return 0;
    /* + s_count before the modulo: -1 % n is not n-1 in C. */
    s_index = (s_index + s_count - 1) % s_count;
    return s_index;
}

bool stations_have_other(void)
{
    return s_count > 1;
}

void stations_stats(stationlist_stats_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_lock);
}
