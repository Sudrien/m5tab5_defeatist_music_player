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

/*
 * A cover, and the entries that point at it.
 *
 * Refcounted because previous/current/next on one album are three paths
 * with one picture, and 1006 measured what not knowing that costs. The
 * hash is kept so the next store can ask "have I got this already"
 * without a memcmp against every held image, and so mediacache_art_hash()
 * can answer the question 1011 left open.
 *
 * `data` is the buffer the caller handed over, unmodified and unmoved --
 * ordinary heap, from covertag_extract_art(). The blob header itself is
 * a few words and goes wherever malloc puts it.
 */
typedef struct {
    uint8_t    *data;
    size_t      len;
    uint32_t    hash;
    int         rc;             /* entries pointing here; blob dies at 0 */
} artblob_t;

typedef struct {
    char        path[512];
    bool        used;
    bool        pinned;
    uint32_t    stamp;          /* MRU ordering; 0 means never touched */

    artblob_t  *art;            /* shared; see artblob_t */
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

/*
 * Drop this entry's reference to its cover. The bytes go only when the
 * last entry lets go of them.
 *
 * The old release() free()d the buffer outright, which was right when an
 * entry owned its cover. Under sharing that would leave the other two
 * entries on an album pointing into freed memory the moment one of them
 * was evicted -- and they would keep answering mediacache_art() with it,
 * because nothing else says the pointer is dead.
 */
static void art_unref(artblob_t *b)
{
    if (!b) return;
    if (--b->rc > 0) return;
    free(b->data);
    free(b);
}

static void release(entry_t *e)
{
    art_unref(e->art);
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
    if (len) *len = e->art->len;
    const uint8_t *p = e->art->data;
    unlock();
    /* Borrowed past the lock -- media_task only. See the note above. */
    return p;
}

/*
 * An entry already holding these exact bytes, or NULL.
 *
 * Hash and length first, memcmp only when both agree -- so the compare
 * runs on a hit and not on a miss, which is the case that matters
 * because a miss is what happens on every track that starts a new album.
 *
 * THE MEMCMP IS NOT OPTIONAL, and the reason is a change of stakes.
 * 1011 wrote that two covers of equal length hashing the same are the
 * same image "for every purpose this program has", and for a diagnostic
 * log line that is true. This is not that purpose: a collision here puts
 * one album's picture on another album's screen and nothing downstream
 * would ever notice. 32 bits is a fine filter and a poor proof, so it is
 * used as a filter.
 *
 * Caller holds the lock.
 */
static artblob_t *blob_find(uint32_t hash, size_t len, const uint8_t *img)
{
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        artblob_t *b = s_e[i].art;
        if (!b || b->hash != hash || b->len != len) continue;
        if (memcmp(b->data, img, len) == 0) return b;
    }
    return NULL;
}

void mediacache_put_art(const char *path, uint8_t *img, size_t len)
{
    if (!img) return;
    if (!len) { free(img); return; }

    /*
     * Hashed BEFORE the lock, deliberately.
     *
     * This is megabytes of Murmur2 -- tens of milliseconds on a cover
     * the size 1006 measured -- and the decode loop takes this same
     * mutex through mediacache_tags() at the instant of a track change.
     * Hashing inside it would hand the one latency-critical caller a
     * stall proportional to the size of somebody else's album art. The
     * bytes are the caller's own and no other task can see them yet, so
     * there is nothing to protect here.
     *
     * The memcmp inside blob_find() does run under the lock. It happens
     * only on a hit, which is the path that is about to save a whole
     * copy, and shortening the hold further would mean the two-phase
     * lookup that the borrow contract makes unnecessary.
     */
    const uint32_t hash = albumart_cover_hash(img, len);

    lock();
    entry_t *e = slot_for(path);
    if (!e) { unlock(); free(img); return; }  /* ownership was taken; honour it */

    if (!e->used) {
        e->used = true;
        snprintf(e->path, sizeof(e->path), "%s", path);
    }

    artblob_t *shared = blob_find(hash, len, img);

    if (shared && shared == e->art) {
        /* Already pointing at it. Re-storing the same cover for the same
         * path is what a prefetch racing a play does. */
        free(img);
        e->no_art = false;
        touch(e);
        unlock();
        return;
    }

    artblob_t *b;
    if (shared) {
        shared->rc++;
        free(img);                  /* ownership was taken; honour it */
        b = shared;
        ESP_LOGD(TAG, "cover shared (%u bytes, hash %08x, rc %d)",
                 (unsigned)len, (unsigned)hash, shared->rc);
    } else {
        b = malloc(sizeof(*b));
        if (!b) { unlock(); free(img); return; }
        b->data = img;
        b->len  = len;
        b->hash = hash;
        b->rc   = 1;
    }

    /* Replacing rather than adding: a second cover for the same path
     * means the first was fetched before something changed, and keeping
     * both would leak the older one. Unref rather than free -- the one
     * being replaced may be another entry's too. */
    art_unref(e->art);
    e->art = b;
    e->no_art = false;
    touch(e);
    unlock();
}

uint32_t mediacache_art_hash(const char *path)
{
    lock();
    entry_t *e = find(path);
    const uint32_t h = (e && e->art) ? e->art->hash : 0u;
    unlock();
    return h;
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

void mediacache_stats(int *entries, size_t *bytes, size_t *saved)
{
    int n = 0;
    size_t b = 0, dup = 0;

    lock();
    for (int i = 0; i < MEDIACACHE_ENTRIES; i++) {
        if (!s_e[i].used) continue;
        n++;
        if (s_e[i].has_tags) b += sizeof(s_e[i].tags);
        if (s_e[i].walk) b += sizeof(*s_e[i].walk);

        if (!s_e[i].art) continue;
        /* Charge a blob to the first entry that names it and count the
         * rest as saved. Three slots means the scan back is three
         * comparisons; a hash set here would be machinery for a loop
         * that cannot exceed nine iterations. */
        bool first = true;
        for (int j = 0; j < i; j++) {
            if (s_e[j].used && s_e[j].art == s_e[i].art) { first = false; break; }
        }
        if (first) b += s_e[i].art->len;
        else       dup += s_e[i].art->len;
    }
    unlock();

    if (entries) *entries = n;
    if (bytes) *bytes = b;
    if (saved) *saved = dup;
}
