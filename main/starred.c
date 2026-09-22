/*
 * starred.c -- see starred.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "starred.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "storage.h"
#include "storage_io.h"

static const char *TAG = "tab5_star";

/* One volume's stars: relative paths, strdup'd into PSRAM. */
typedef struct {
    bool     loaded;
    uint32_t gen;           /* storage_generation() when loaded */
    int      n;
    char   **item;          /* STARRED_MAX slots, allocated on first load */
} vol_t;

static vol_t             s_vol[STORAGE_COUNT];
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static void lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }

/* The key: path relative to its volume, '/' appended for a folder.
 * STORAGE_COUNT when the path is on no volume or does not fit. */
static storage_id_t key_of(const char *path, bool is_dir, char *out, size_t n)
{
    const storage_id_t id = path ? storage_of_path(path) : STORAGE_COUNT;
    if (id >= STORAGE_COUNT) return STORAGE_COUNT;
    const char *rel = path + strlen(storage_mount_path(id));
    while (*rel == '/') rel++;
    if (!*rel) return STORAGE_COUNT;            /* the root: nothing to star */
    const int w = snprintf(out, n, "%s%s", rel, is_dir ? "/" : "");
    if (w <= 0 || (size_t)w >= n) return STORAGE_COUNT;
    return id;
}

static bool file_path(storage_id_t id, const char *name, char *out, size_t n)
{
    const int w = snprintf(out, n, "%s/%s", storage_mount_path(id), name);
    return w > 0 && (size_t)w < n;
}

static void vol_clear(vol_t *v)
{
    for (int i = 0; i < v->n; i++) free(v->item[i]);
    v->n = 0;
}

/* Read the volume's file into v. Held under the lock. Also adopts a
 * temp file orphaned between remove and rename -- favorites.c's rule,
 * and the rename is its own test: it only succeeds with no list. */
static void vol_load(storage_id_t id, vol_t *v)
{
    if (!v->item) {
        v->item = heap_caps_calloc(STARRED_MAX, sizeof(char *), MALLOC_CAP_SPIRAM);
        if (!v->item) return;
    }
    vol_clear(v);
    v->loaded = true;
    v->gen = storage_generation();

    char path[64], tmp[64];
    if (!file_path(id, STARRED_FILENAME, path, sizeof(path)) ||
        !file_path(id, STARRED_TEMPNAME, tmp, sizeof(tmp))) return;
    if (rename(tmp, path) == 0) {
        ESP_LOGW(TAG, "recovered %s from an interrupted write", path);
    }

    FILE *f = storage_io_open(path, "r");
    if (!f) return;                             /* no stars: ordinary */

    static char line[520];                      /* not the stack */
    while (fgets(line, sizeof(line), f) && v->n < STARRED_MAX) {
        size_t len = strcspn(line, "\r\n");
        line[len] = '\0';
        if (!len || line[0] == '#') continue;
        char *s = strdup(line);
        if (!s) break;
        v->item[v->n++] = s;
    }
    storage_io_close(f);
    ESP_LOGI(TAG, "%s: %d starred", path, v->n);
}

static vol_t *vol_get(storage_id_t id)
{
    vol_t *v = &s_vol[id];
    if (!v->loaded || v->gen != storage_generation()) vol_load(id, v);
    return v;
}

static int find(const vol_t *v, const char *key)
{
    for (int i = 0; i < v->n; i++) {
        if (strcmp(v->item[i], key) == 0) return i;
    }
    return -1;
}

bool starred_contains(const char *path, bool is_dir)
{
    char key[520];
    const storage_id_t id = key_of(path, is_dir, key, sizeof(key));
    if (id >= STORAGE_COUNT) return false;
    lock();
    const bool yes = find(vol_get(id), key) >= 0;
    unlock();
    return yes;
}

/* Write v whole to the temp, then remove and rename -- FATFS will not
 * rename over an existing file. See favorites_remove(). */
static bool vol_write(storage_id_t id, const vol_t *v)
{
    char path[64], tmp[64];
    if (!file_path(id, STARRED_FILENAME, path, sizeof(path)) ||
        !file_path(id, STARRED_TEMPNAME, tmp, sizeof(tmp))) return false;

    FILE *f = storage_io_open(tmp, "w");
    if (!f) {
        ESP_LOGW(TAG, "cannot write %s (%s)", tmp, strerror(errno));
        return false;
    }
    bool ok = fputs("#EXTM3U\n", f) >= 0;
    for (int i = 0; ok && i < v->n; i++) {
        ok = fputs(v->item[i], f) >= 0 && fputc('\n', f) != EOF;
    }
    const bool flushed = fflush(f) == 0;
    const bool closed = storage_io_close(f) == 0;
    if (!ok || !flushed || !closed) {
        ESP_LOGW(TAG, "writing %s failed; leaving %s alone", tmp, path);
        remove(tmp);
        return false;
    }

    storage_io_acquire(STORAGE_IO_BACKGROUND);
    remove(path);
    storage_io_release();
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    const int moved = rename(tmp, path);
    storage_io_release();
    if (moved != 0) {
        ESP_LOGW(TAG, "could not install %s (%s)", path, strerror(errno));
        return false;
    }
    return true;
}

bool starred_toggle(const char *path, bool is_dir)
{
    char key[520];
    const storage_id_t id = key_of(path, is_dir, key, sizeof(key));
    if (id >= STORAGE_COUNT) return false;

    lock();
    vol_t *v = vol_get(id);
    if (!v->item) { unlock(); return false; }

    const int at = find(v, key);
    bool changed = false;
    if (at >= 0) {
        free(v->item[at]);
        memmove(&v->item[at], &v->item[at + 1],
                (size_t)(v->n - at - 1) * sizeof(char *));
        v->n--;
        changed = true;
    } else if (v->n < STARRED_MAX) {
        char *s = strdup(key);
        if (s) { v->item[v->n++] = s; changed = true; }
    } else {
        ESP_LOGW(TAG, "%d starred already; not starring %s", STARRED_MAX, key);
    }

    if (changed && !vol_write(id, v)) changed = false;

    /* Read back either way: the file is the answer, not the list we
     * just edited -- after a failed write they disagree. */
    vol_load(id, v);
    const bool now = find(v, key) >= 0;
    unlock();

    if (changed) ESP_LOGI(TAG, "%s: %s", now ? "starred" : "unstarred", key);
    return now;
}
