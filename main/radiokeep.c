/*
 * radiokeep.c -- see radiokeep.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "radiokeep.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "favorites.h"      /* favorites_url_eq() */
#include "stations.h"       /* 5056: the uuid of the station being kept */

static const char *TAG = "tab5_keep";

#define NVS_NAMESPACE   "radiokeep"
#define KEY_LAST        "last"
#define KEY_STAR        "star"

/* "name\0url\0uuid\0". Module scope, not a local: at 614 bytes it is past
 * what belongs on a caller's stack (CLAUDE.md), and the lock below
 * serialises every use of it. */
#define BLOB_MAX        (STATION_NAME_MAX + STATION_URL_MAX + STATION_UUID_MAX)
static char s_blob[BLOB_MAX];
static char s_last_err[64];     /* 5051: why the last read went as it did */

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static portMUX_TYPE      s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void lock(void)
{
    taskENTER_CRITICAL(&s_init_mux);
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    taskEXIT_CRITICAL(&s_init_mux);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGive(s_lock); }

/* Under the lock. False when absent or malformed; `out` zeroed then. */
static bool slot_read(const char *key, station_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    const esp_err_t oe = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (oe != ESP_OK) {
        snprintf(s_last_err, sizeof(s_last_err), "open: %s", esp_err_to_name(oe));
        return false;
    }
    size_t len = sizeof(s_blob);
    const esp_err_t err = nvs_get_blob(h, key, s_blob, &len);
    nvs_close(h);
    snprintf(s_last_err, sizeof(s_last_err), "%s: %s, %u bytes",
             key, esp_err_to_name(err), (unsigned)len);
    if (err != ESP_OK || len < 3 || s_blob[len - 1] != '\0') return false;

    const char *name = s_blob;
    const char *url = name + strlen(name) + 1;
    if (url >= s_blob + len) return false;
    const char *uuid = url + strlen(url) + 1;
    if (uuid > s_blob + len) return false;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }
    if (strlen(name) >= sizeof(out->name) || strlen(url) >= sizeof(out->url)) {
        return false;
    }
    strcpy(out->name, name);
    strcpy(out->url, url);
    if (uuid < s_blob + len && strlen(uuid) < sizeof(out->uuid)) {
        strcpy(out->uuid, uuid);
    }
    return true;
}

/* Under the lock. A NULL station erases the slot. */
static bool slot_write(const char *key, const station_t *st)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    if (st) {
        const int n = snprintf(s_blob, sizeof(s_blob), "%s%c%s%c%s",
                               st->name, 0, st->url, 0, st->uuid);
        size_t len = (n > 0 && (size_t)n < sizeof(s_blob)) ? (size_t)n + 1 : 0;
        err = len ? nvs_set_blob(h, key, s_blob, len) : ESP_ERR_INVALID_SIZE;
    } else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: write failed (%s)", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

/*
 * 5056: and the directory's uuid, when the current station is this one.
 * A kept station without it plays but has no artwork: the logo is looked
 * up by uuid (radiobrowser.c), and 5048 stored name and URL only, so a
 * station replayed from the kept list came up with a blank square.
 * Under the lock; `look` is static for its size.
 */
static void fill(station_t *st, const char *name, const char *url)
{
    static station_t look;
    memset(st, 0, sizeof(*st));
    snprintf(st->name, sizeof(st->name), "%s", name ? name : "");
    snprintf(st->url, sizeof(st->url), "%s", url);
    if (stations_get(stations_index(), &look) && favorites_url_eq(look.url, url)) {
        memcpy(st->uuid, look.uuid, sizeof(st->uuid));
    }
}

void radiokeep_note_played(const char *name, const char *url)
{
    if (!url || !url[0]) return;
    static station_t cur, fresh;    /* static for size; used under the lock */
    lock();
    fill(&fresh, name, url);
    if (!slot_read(KEY_LAST, &cur) || !favorites_url_eq(cur.url, url) ||
        strcmp(cur.name, fresh.name) != 0 ||
        (fresh.uuid[0] && strcmp(cur.uuid, fresh.uuid) != 0)) {
        if (slot_write(KEY_LAST, &fresh)) {
            ESP_LOGI(TAG, "last played kept: %.60s", fresh.name);
        }
    }
    unlock();
}

bool radiokeep_star_is(const char *url)
{
    if (!url || !url[0]) return false;
    static station_t cur;
    lock();
    const bool is = slot_read(KEY_STAR, &cur) && favorites_url_eq(cur.url, url);
    unlock();
    return is;
}

bool radiokeep_star_toggle(const char *name, const char *url)
{
    if (!url || !url[0]) return false;
    static station_t cur, fresh;
    bool now;
    lock();
    if (slot_read(KEY_STAR, &cur) && favorites_url_eq(cur.url, url)) {
        now = !slot_write(KEY_STAR, NULL);
        if (!now) ESP_LOGI(TAG, "star cleared");
    } else {
        fill(&fresh, name, url);
        now = slot_write(KEY_STAR, &fresh);
        if (now) ESP_LOGI(TAG, "starred on this Tab5: %.60s", fresh.name);
    }
    unlock();
    return now;
}

int radiokeep_list(station_t out[2])
{
    int n = 0;
    lock();
    const bool star = slot_read(KEY_STAR, &out[0]);
    if (star) n = 1;
    const bool last = slot_read(KEY_LAST, &out[n]);
    if (last && !(n == 1 && favorites_url_eq(out[0].url, out[1].url))) {
        n++;
    }
    unlock();
    /* 5051: said once per load. A card-less boot once came up with an
     * empty kept list after a station had been kept, and the log could
     * not say which slot was missing or why. */
    ESP_LOGI(TAG, "kept list: star %s, last %s (%s)",
             star ? "yes" : "no", last ? "yes" : "no", s_last_err);
    return n;
}
