/*
 * mediacache.c -- see mediacache.h.
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

#include "mediacache.h"

static const char *TAG = "tab5_cache";

typedef struct {
    char        path[512];
    bool        used;
    bool        pinned;
    uint32_t    stamp;          /* MRU ordering; 0 means never touched */

    uint8_t    *art;
    size_t      art_len;
    bool        no_art;         /* read, and there was none */

    bool        has_tags;
    id3_tags_t  tags;           /* 192 bytes; stored inline, not pointed at */

    framewalk_t *walk;          /* PSRAM; ~1 KB */
} entry_t;

static entry_t  s_e[MEDIACACHE_ENTRIES];
static uint32_t s_clock;

/*
 * The lock this file spent its first version not needing.
 *
 * It was safe without one because every caller was media_task, and the
 * borrowed pointers were safe for the same reason: the only task that
 * could evict an entry was the task holding the pointer. That stopped
 * being true when the decode loop started asking for tags and the
 * envelope at the moment of a track change -- which it has to do,
 * because the whole point of prefetching them is that they are on screen
 * before anything slow has happened.
 *
 * So: a mutex around every entry access, and a split in the contract
 * that the header spells out. Copy-out accessors (mediacache_tags,
 * mediacache_walk_copy) are safe from any task. Borrowing accessors
 * (mediacache_art, mediacache_walk) and anything that stores are still
 * media_task's alone, because a borrowed pointer outlives the lock and
 * nothing but the borrower's own eviction can be reasoned about.
 *
 * A recursive mutex would let the two kinds nest; they do not nest, and
 * a plain one that deadlocks if they ever start is the more useful of
 * the two.
 */
static SemaphoreHandle_t s_lock;

static void lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

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
    /* Before the first lock() can matter: init runs in app_main() with no
     * other task yet created. */
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    lock();
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        if (s_e[i].used) release(&s_e[i]);
    }
    memset(s_e, 0, sizeof(s_e));
    s_clock = 0;
    unlock();
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
    lock();
    entry_t *e = find(path);
    if (!e || !e->art) { unlock(); return NULL; }
    touch(e);
    /*
     * `len` is optional, because sidecar_prime() calls this purely to
     * ask whether art is cached and has nothing to do with its size.
     * Writing through it unconditionally stored to address zero, and
     * only when the answer was yes -- so the fault needed a file with
     * embedded art already in the cache, which is why it survived every
     * run until a test suite included one.
     */
    if (len) *len = e->art_len;
    const uint8_t *p = e->art;
    unlock();
    /* Borrowed past the lock -- media_task only. See the note above. */
    return p;
}

void mediacache_put_art(const char *path, uint8_t *img, size_t len)
{
    if (!img) return;

    lock();
    entry_t *e = slot_for(path);
    if (!e) { unlock(); free(img); return; }  /* ownership was taken; honour it */

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
    e->no_art = false;
    touch(e);
    unlock();
}

bool mediacache_tags(const char *path, id3_tags_t *out)
{
    lock();
    entry_t *e = find(path);
    if (!e || !e->has_tags) { unlock(); return false; }
    touch(e);
    if (out) *out = e->tags;
    unlock();
    return true;
}

void mediacache_put_tags(const char *path, const id3_tags_t *t)
{
    if (!t) return;

    lock();
    entry_t *e = slot_for(path);
    if (!e) { unlock(); return; }

    if (!e->used) {
        e->used = true;
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    e->tags = *t;
    e->has_tags = true;
    touch(e);
    unlock();
}

bool mediacache_no_art(const char *path)
{
    lock();
    entry_t *e = find(path);
    const bool r = e && e->no_art;
    unlock();
    return r;
}

void mediacache_put_no_art(const char *path)
{
    lock();
    entry_t *e = slot_for(path);
    if (!e) { unlock(); return; }

    if (!e->used) {
        e->used = true;
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    e->no_art = true;
    touch(e);
    unlock();
}

const framewalk_t *mediacache_walk(const char *path)
{
    lock();
    entry_t *e = find(path);
    if (!e || !e->walk) { unlock(); return NULL; }
    touch(e);
    const framewalk_t *w = e->walk;
    unlock();
    /* Borrowed past the lock -- media_task only. */
    return w;
}

bool mediacache_walk_copy(const char *path, framewalk_t *out)
{
    if (!out) return false;

    lock();
    entry_t *e = find(path);
    if (!e || !e->walk) { unlock(); return false; }
    touch(e);
    memcpy(out, e->walk, sizeof(*out));
    unlock();
    return true;
}

void mediacache_put_walk(const char *path, const framewalk_t *w)
{
    if (!w) return;

    lock();
    entry_t *e = slot_for(path);
    if (!e) { unlock(); return; }

    if (!e->used) {
        e->used = true;
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    if (!e->walk) {
        e->walk = heap_caps_malloc(sizeof(*e->walk),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        /* Entry stays valid, just art-only. */
        if (!e->walk) { unlock(); return; }
    }
    memcpy(e->walk, w, sizeof(*e->walk));
    touch(e);
    unlock();
}

void mediacache_pin(const char *path)
{
    lock();
    entry_t *e = find(path);
    if (e) { e->pinned = true; touch(e); }
    unlock();
}

void mediacache_unpin_all(void)
{
    lock();
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) s_e[i].pinned = false;
    unlock();
}

void mediacache_clear(void)
{
    lock();
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        if (s_e[i].used) release(&s_e[i]);
    }
    unlock();
}

void mediacache_stats(int *entries, size_t *bytes)
{
    int n = 0;
    size_t b = 0;
    lock();
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        if (!s_e[i].used) continue;
        n++;
        b += s_e[i].art_len;
        if (s_e[i].has_tags) b += sizeof(s_e[i].tags);
        if (s_e[i].walk) b += sizeof(*s_e[i].walk);
    }
    unlock();
    if (entries) *entries = n;
    if (bytes) *bytes = b;
}
