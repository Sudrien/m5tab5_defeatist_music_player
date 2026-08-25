/*
 * mediacache.c -- see mediacache.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "mediacache.h"

static const char *TAG = "tab5_cache";

typedef struct {
    char        path[512];
    bool        used;
    bool        pinned;
    uint32_t    stamp;          /* MRU ordering; 0 means never touched */

    uint8_t    *art;
    size_t      art_len;

    framewalk_t *walk;          /* PSRAM; ~1 KB */
} entry_t;

static entry_t  s_e[MEDIACACHE_ENTRIES];
static uint32_t s_clock;

static void release(entry_t *e);

/*
 * Safe to call on a populated cache: it releases first.
 *
 * A plain memset here would abandon every buffer the entries point at,
 * which is a leak of up to three covers -- a third of a megabyte -- and
 * the kind that only shows up if init() is ever called twice. Cheaper to
 * make the function correct than to document that it must not be.
 */
void mediacache_init(void)
{
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        if (s_e[i].used) release(&s_e[i]);
    }
    memset(s_e, 0, sizeof(s_e));
    s_clock = 0;
}

static entry_t *find(const char *path)
{
    if (!path || !*path) return NULL;
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        if (s_e[i].used && strcmp(s_e[i].path, path) == 0) return &s_e[i];
    }
    return NULL;
}

static void release(entry_t *e)
{
    free(e->art);
    heap_caps_free(e->walk);
    memset(e, 0, sizeof(*e));
}

/*
 * A free slot, or the least recently used unpinned one.
 *
 * Returns NULL when every slot is pinned, which the caller must treat as
 * "do not cache this" rather than as a reason to evict something pinned.
 * Dropping a pinned entry would mean the prefetch of the next track
 * throwing away the previous track that the back button is for -- the
 * exact thing the pins exist to stop.
 */
static entry_t *slot_for(const char *path)
{
    entry_t *e = find(path);
    if (e) return e;

    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        if (!s_e[i].used) return &s_e[i];
    }

    entry_t *victim = NULL;
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        if (s_e[i].pinned) continue;
        if (!victim || s_e[i].stamp < victim->stamp) victim = &s_e[i];
    }
    if (!victim) {
        ESP_LOGD(TAG, "every slot pinned; not caching %s", path);
        return NULL;
    }

    ESP_LOGD(TAG, "evicting %s", victim->path);
    release(victim);
    return victim;
}

static void touch(entry_t *e)
{
    e->stamp = ++s_clock;
}

const uint8_t *mediacache_art(const char *path, size_t *len)
{
    entry_t *e = find(path);
    if (!e || !e->art) return NULL;
    touch(e);
    *len = e->art_len;
    return e->art;
}

void mediacache_put_art(const char *path, uint8_t *img, size_t len)
{
    if (!img) return;

    entry_t *e = slot_for(path);
    if (!e) { free(img); return; }       /* ownership was taken; honour it */

    if (!e->used) {
        e->used = true;
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    /* Replacing rather than adding: a second cover for the same path
     * means the first was fetched before something changed, and keeping
     * both would leak the older one. */
    free(e->art);
    e->art = img;
    e->art_len = len;
    touch(e);
}

const framewalk_t *mediacache_walk(const char *path)
{
    entry_t *e = find(path);
    if (!e || !e->walk) return NULL;
    touch(e);
    return e->walk;
}

void mediacache_put_walk(const char *path, const framewalk_t *w)
{
    if (!w) return;

    entry_t *e = slot_for(path);
    if (!e) return;

    if (!e->used) {
        e->used = true;
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    if (!e->walk) {
        e->walk = heap_caps_malloc(sizeof(*e->walk),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!e->walk) return;            /* entry stays valid, just art-only */
    }
    memcpy(e->walk, w, sizeof(*e->walk));
    touch(e);
}

void mediacache_pin(const char *path)
{
    entry_t *e = find(path);
    if (e) { e->pinned = true; touch(e); }
}

void mediacache_unpin_all(void)
{
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) s_e[i].pinned = false;
}

void mediacache_clear(void)
{
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        if (s_e[i].used) release(&s_e[i]);
    }
}

void mediacache_stats(int *entries, size_t *bytes)
{
    int n = 0;
    size_t b = 0;
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        if (!s_e[i].used) continue;
        n++;
        b += s_e[i].art_len;
        if (s_e[i].walk) b += sizeof(*s_e[i].walk);
    }
    if (entries) *entries = n;
    if (bytes) *bytes = b;
}
