/*
 * playlist.c
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "decoder.h"
#include "playlist.h"
#include "storage.h"

static const char *TAG = "tab5_playlist";

static char **s_paths;              /* PSRAM, PLAYLIST_MAX pointers */
static uint8_t *s_played;           /* shuffle bitmap, one bit per entry */
static int s_count;
static int s_current = -1;
static char s_dir[512];

static int cmp_path(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

void playlist_clear(void)
{
    for (int i = 0; i < s_count; i++) free(s_paths[i]);
    s_count = 0;
    s_current = -1;
    s_dir[0] = '\0';
    if (s_played) memset(s_played, 0, (PLAYLIST_MAX + 7) / 8);
}

esp_err_t playlist_load_dir(const char *dir)
{
    if (!dir || !*dir) return ESP_ERR_INVALID_ARG;

    if (!s_paths) {
        s_paths = heap_caps_calloc(PLAYLIST_MAX, sizeof(char *),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_played = heap_caps_calloc((PLAYLIST_MAX + 7) / 8, 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_paths || !s_played) {
            ESP_LOGE(TAG, "out of memory for the track list");
            return ESP_ERR_NO_MEM;
        }
    }

    playlist_clear();

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGE(TAG, "cannot open %s", dir);
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *e;
    bool truncated = false;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) continue;
        /* Before decoder_supports(), because the entries this catches
         * would pass it: an AppleDouble sidecar is called ._Track.mp3
         * and holds a resource fork. The chooser has always hidden
         * these; this list did not, so playing a folder written on a Mac
         * queued eighteen entries for nine tracks and every other one
         * decoded to nothing. */
        if (storage_is_hidden(e->d_name)) continue;
        if (!decoder_supports(e->d_name)) continue;
        if (s_count >= PLAYLIST_MAX) { truncated = true; break; }

        char full[512];
        if (!storage_join_path(full, sizeof(full), dir, e->d_name)) {
            ESP_LOGW(TAG, "path too long, skipping: %s/%s", dir, e->d_name);
            continue;
        }
        s_paths[s_count] = strdup(full);
        if (!s_paths[s_count]) break;
        s_count++;
    }
    closedir(d);

    if (truncated) {
        ESP_LOGW(TAG, "%s has more than %d tracks; the rest are ignored",
                 dir, PLAYLIST_MAX);
    }

    /* FatFs hands entries back in directory order, which is creation
     * order on most cards -- so an album copied track by track is roughly
     * right and an album copied by a tool that parallelises is not. Sort
     * rather than trust it. */
    qsort(s_paths, (size_t)s_count, sizeof(char *), cmp_path);

    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    ESP_LOGI(TAG, "%s: %d track%s", dir, s_count, s_count == 1 ? "" : "s");
    return s_count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int playlist_count(void) { return s_count; }

const char *playlist_path(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return s_paths[i];
}

int playlist_index_of(const char *path)
{
    if (!path) return -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_paths[i], path) == 0) return i;
    }
    return -1;
}

int playlist_current(void) { return s_current; }

void playlist_set_current(int i)
{
    s_current = (i >= 0 && i < s_count) ? i : -1;
    if (s_current >= 0 && s_played) {
        s_played[s_current / 8] |= (uint8_t)(1 << (s_current % 8));
    }
}

const char *playlist_dir(void) { return s_dir; }

static bool shuffle_seen(int i)
{
    return s_played && (s_played[i / 8] & (1 << (i % 8)));
}

const char *playlist_peek_next(play_order_t order)
{
    if (s_count <= 0) return NULL;
    if (order != PLAY_ORDER_ALL) return NULL;   /* see the header */

    const int n = s_current + 1;
    if (s_current < 0 || n >= s_count) return NULL;
    return s_paths[n];
}

bool playlist_has_next(play_order_t order)
{
    if (s_count <= 0) return false;
    if (order == PLAY_ORDER_SHUFFLE) return true;   /* see the header */
    return s_current >= 0 && s_current + 1 < s_count;
}

const char *playlist_next(play_order_t order)
{
    if (s_count <= 0) return NULL;

    if (order == PLAY_ORDER_ONE) return NULL;

    if (order == PLAY_ORDER_SHUFFLE) {
        int remaining = 0;
        for (int i = 0; i < s_count; i++) if (!shuffle_seen(i)) remaining++;

        if (remaining == 0) {
            /* Exhausted: start a fresh pass rather than replaying the
             * same order. Everything except the track just played, so the
             * wrap does not repeat it back to back. */
            memset(s_played, 0, (size_t)(PLAYLIST_MAX + 7) / 8);
            if (s_current >= 0 && s_count > 1) {
                s_played[s_current / 8] |= (uint8_t)(1 << (s_current % 8));
                remaining = s_count - 1;
            } else {
                remaining = s_count;
            }
        }

        int pick = (int)(esp_random() % (uint32_t)remaining);
        for (int i = 0; i < s_count; i++) {
            if (shuffle_seen(i)) continue;
            if (pick-- == 0) {
                playlist_set_current(i);
                return s_paths[i];
            }
        }
        return NULL;
    }

    const int next = s_current + 1;
    if (next >= s_count) return NULL;        /* stop at the end of the folder */
    playlist_set_current(next);
    return s_paths[next];
}

const char *playlist_prev(void)
{
    if (s_count <= 0 || s_current <= 0) return NULL;
    playlist_set_current(s_current - 1);
    return s_paths[s_current];
}
