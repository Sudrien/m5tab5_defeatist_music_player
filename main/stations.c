/*
 * stations.c -- see stations.h.
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

#include "stations.h"
#include "radiokeep.h"
#include "storage.h"
#include "storage_io.h"

static const char *TAG = "tab5_stations";

static station_t         *s_list;
static int                s_count;
static int                s_index;
static storage_id_t       s_vol = STORAGE_COUNT;
static stationlist_stats_t s_stats;
/* Where a list that is not on a volume came from. Written under the
 * lock with the list it describes, so the two cannot disagree. */
static char               s_label[48];
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
        free(text);
        /* 5048: no volume at all, rather than volumes without the file.
         * Then the stations kept in flash are the list -- see
         * radiokeep.h. With any volume present its list stands, even an
         * absent one. */
        bool any = false;
        for (int id = 0; id < STORAGE_COUNT; id++) {
            if (storage_present((storage_id_t)id)) any = true;
        }
        if (!any) {
            static station_t kept[2];   /* 1.2 KB: not on the caller's stack */
            const int k = radiokeep_list(kept);
            if (k > 0) return stations_set_remote(kept, k, RADIOKEEP_LABEL);
        }
        ESP_LOGI(TAG, "no %s on any volume", STATIONS_FILENAME);
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
    s_label[0] = '\0';   /* a volume names itself */
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

/*
 * Does this file end in a newline?
 *
 * Asked before appending, because a hand-edited stations.m3u whose last
 * line has no terminator is common -- editors differ, and the parser
 * does not care, so nothing has ever forced the issue. Appending to
 * such a file without checking would join the new entry to the old last
 * line and lose both.
 *
 * True for a file that does not exist or is empty: nothing to separate
 * from, so no separator wanted.
 */
static bool ends_with_newline(const char *path)
{
    FILE *f = storage_io_open(path, "r");
    if (!f) return true;

    bool ok = true;
    if (fseek(f, -1, SEEK_END) == 0) {
        char last = '\n';
        /* One byte, and the file position is already where it belongs,
         * so this is a plain fread rather than storage_io_read_at(). */
        if (fread(&last, 1, 1, f) == 1) ok = (last == '\n');
    }
    /* fseek failing means the file is empty (or unseekable), and an
     * empty file needs no separator. */
    storage_io_close(f);
    return ok;
}

bool stations_append(const char *name, const char *url)
{
    lock_init();

    char entry[STATION_NAME_MAX + STATION_URL_MAX + 32];
    const size_t len = station_entry(name, url, entry, sizeof(entry));
    if (len == 0) {
        ESP_LOGW(TAG, "refusing to add a station: bad name or URL");
        return false;
    }

    /*
     * The cap, checked against the list rather than the file. They agree
     * -- the list was parsed from the file -- and the list is the thing
     * already in memory. Refused rather than written and silently
     * dropped by the next parse, which is what stats.overflowed counts
     * and which would look like the write having failed anyway.
     */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int have = s_count;
    const storage_id_t listed = s_vol;
    xSemaphoreGive(s_lock);
    if (have >= STATIONLIST_MAX) {
        ESP_LOGW(TAG, "station list is full (%d); not adding", STATIONLIST_MAX);
        return false;
    }

    /* The list's own volume, or the first one there. See stations.h. */
    storage_id_t target = STORAGE_COUNT;
    if (listed < STORAGE_COUNT && storage_present(listed)) {
        target = listed;
    } else {
        for (int id = 0; id < STORAGE_COUNT; id++) {
            if (storage_present((storage_id_t)id)) {
                target = (storage_id_t)id;
                break;
            }
        }
    }
    if (target == STORAGE_COUNT) {
        ESP_LOGW(TAG, "no volume to write %s to", STATIONS_FILENAME);
        return false;
    }

    const char *mount = storage_mount_path(target);
    if (!mount) return false;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", mount, STATIONS_FILENAME);

    const bool separate = !ends_with_newline(path);

    /*
     * Append, the way settings.c appends: one open, one write, one
     * close, no lease held across it. "a" creates the file, so the first
     * station added to a volume that has never had a list works without
     * a special case.
     */
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
        /*
         * A partial line may now be on the card. That is survivable in
         * exactly the way a partial settings record is: stationlist_parse()
         * skips a line it cannot read, and every entry before it was
         * whole when it was written.
         */
        ESP_LOGW(TAG, "append to %s failed (%s)", path,
                 strerror(flushed ? errno : flush_errno));
        return false;
    }

    ESP_LOGI(TAG, "added a station to %s: %s", path, url);

    /* Read it back. The entry counts as added only if the parser agrees
     * -- see stations.h. */
    return stations_load();
}

bool stations_set_remote(const station_t *list, int count, const char *label)
{
    lock_init();
    if (!list || count <= 0) return false;
    if (count > STATIONLIST_MAX) count = STATIONLIST_MAX;

    station_t *fresh = heap_caps_calloc(STATIONLIST_MAX, sizeof(station_t),
                                        MALLOC_CAP_SPIRAM);
    if (!fresh) {
        ESP_LOGE(TAG, "no PSRAM for %d stations", count);
        return false;
    }
    memcpy(fresh, list, (size_t)count * sizeof(station_t));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    station_t *old = s_list;
    s_list = fresh;
    s_count = count;
    /*
     * STORAGE_COUNT is the truth here rather than a placeholder: this
     * list is on no volume, and stations_volume() answering SD would
     * have the status line name a card that does not hold it.
     */
    s_vol = STORAGE_COUNT;
    memset(&s_stats, 0, sizeof(s_stats));
    snprintf(s_label, sizeof(s_label), "%s", label ? label : "the directory");
    /* Reset for the same reason a reload resets it: this is a different
     * list even when it happens to be the same length. */
    s_index = 0;
    xSemaphoreGive(s_lock);
    free(old);

    ESP_LOGI(TAG, "%d stations from %s", count, s_label);
    return true;
}

const char *stations_source(void)
{
    if (s_vol != STORAGE_COUNT) {
        const char *mount = storage_mount_path(s_vol);
        if (mount) return mount;
    }
    return s_label[0] ? s_label : "?";
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
