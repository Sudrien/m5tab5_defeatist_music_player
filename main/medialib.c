/*
 * medialib.c -- see medialib.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "medialib.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "covertag.h"
#include "cuedir.h"
#include "cuesheet.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mediacat.h"
#include "mediawalk.h"
#include "settings.h"
#include "storage_io.h"
#include "wifi.h"

static const char *TAG = "medialib";

#define CLS     STORAGE_IO_BACKGROUND

typedef struct {
    storage_id_t vol;
    const char  *mount;
    mwalk_fn     fn;        /* the engine's, wrapped by guarded() */
    void        *wctx;
} ctx_t;

/* The volume's absolute path for a relative one. Static: one reconcile
 * at a time, and 512 bytes is not a stack local. */
static char s_abs[16 + MIDX_PATH_MAX + 2];

static const char *absolute(const ctx_t *c, const char *rel)
{
    if (!storage_join_path(s_abs, sizeof(s_abs), c->mount, rel)) return NULL;
    return s_abs;
}

/*
 * Where a run's time goes, so the log can say instead of leaving it to
 * be inferred: tag reads, catalog work, and -- by subtraction -- the
 * walk and the index. Microseconds, for this run only.
 */
static int64_t s_tag_us, s_cat_us;

static bool cat_append(void *ctx, const mediacat_rec_t *r, uint32_t *off)
{
    const ctx_t *c = ctx;
    const int64_t t0 = esp_timer_get_time();
    const bool ok = mediacat_append(c->vol, r, off);
    s_cat_us += esp_timer_get_time() - t0;
    return ok;
}

/* The engine's hook: the session's lines onto the card, before the new
 * index that points at them is installed. */
static bool cat_flush(void *ctx)
{
    (void)ctx;
    const int64_t t0 = esp_timer_get_time();
    const bool ok = mediacat_session_close();
    s_cat_us += esp_timer_get_time() - t0;
    return ok;
}

/*
 * Through the run's session: the same handle the appends go through, so
 * there is one view of the file. (5014 opened the catalog per read to
 * avoid a second, staler view beside the appender; with one handle for
 * both there is no second view to avoid, and a rerun of 1192 tracks
 * stops paying 103 opens for its long paths.)
 */
static bool cat_read(void *ctx, uint32_t off, mediacat_rec_t *out)
{
    (void)ctx;
    const int64_t t0 = esp_timer_get_time();
    const bool ok = mediacat_session_read(off, out);
    s_cat_us += esp_timer_get_time() - t0;
    return ok;
}

/*
 * Tags the way the player gets them for the screen: a cue track's from
 * its sheet (an image's own tags describe the whole disc), anything
 * else's from the file. A track with no tags is still indexed; MPD
 * shows it by name.
 */
static void tags_read(const ctx_t *c, const char *rel, mediacat_rec_t *r);

static void tags(void *ctx, const char *rel, mediacat_rec_t *r)
{
    const int64_t t0 = esp_timer_get_time();
    tags_read(ctx, rel, r);
    s_tag_us += esp_timer_get_time() - t0;
}

static void tags_read(const ctx_t *c, const char *rel, mediacat_rec_t *r)
{
    const char *abs = absolute(c, rel);
    if (!abs) return;

    if (cue_vpath_split(abs, NULL)) {
        /* From the sheet the walk already has open; the long way only
         * if it cannot say (a tag read from outside the walk). */
        if (!mwalk_cue_tags(rel, r->title, r->artist, r->album,
                            MEDIACAT_TAG_LEN)) {
            cuedir_tags(abs, r->title, r->artist, r->album, MEDIACAT_TAG_LEN);
        }
        return;
    }

    static id3_tags_t t;            /* 192 bytes; one caller */
    storage_io_acquire(CLS);
    FILE *f = storage_io_open(abs, "rb");
    storage_io_release();
    if (!f) return;
    memset(&t, 0, sizeof(t));
    if (covertag_read_tags(f, CLS, &t) == ESP_OK) {
        snprintf(r->title, sizeof(r->title), "%s", t.title);
        snprintf(r->artist, sizeof(r->artist), "%s", t.artist);
        snprintf(r->album, sizeof(r->album), "%s", t.album);
    }
    storage_io_acquire(CLS);
    storage_io_close(f);
    storage_io_release();
}

/*
 * Every track the walk offers goes through here first. A volume that
 * has been marked absent -- the card pulled, the drive gone -- stops
 * the walk at the next track, so the run closes its files and releases
 * its hold, and the deferred unmount can happen. Stopping is not
 * failing: nothing is buried, and the old index stands.
 */
static bool guarded(void *ctx, const char *path, midx_stamp_t st)
{
    const ctx_t *c = ctx;
    if (!storage_present(c->vol)) {
        ESP_LOGW(TAG, "%s went away; stopping", storage_label(c->vol));
        return false;
    }
    return c->fn(c->wctx, path, st);
}

static mwalk_result_t walk(void *ctx, mwalk_fn fn, void *wctx)
{
    ctx_t *c = ctx;
    c->fn = fn;
    c->wctx = wctx;
    return mwalk_volume(c->mount, guarded, c);
}

msync_result_t medialib_reconcile(storage_id_t vol, const volatile bool *abort,
                                  msync_stats_t *stats)
{
    static msync_stats_t local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    if (vol >= STORAGE_COUNT || !storage_present(vol)) return MSYNC_FAILED;

    const char *mount = storage_mount_path(vol);
    char index[32], temp[32];
    if (!storage_join_path(index, sizeof(index), mount, MEDIALIB_INDEX_NAME) ||
        !storage_join_path(temp, sizeof(temp), mount, MEDIALIB_TEMP_NAME)) {
        return MSYNC_FAILED;
    }

    /* An index from an earlier format is not read, only removed. */
    static const char *const old_names[] = MEDIALIB_OLD_INDEX_NAMES;
    for (size_t i = 0; i < sizeof(old_names) / sizeof(old_names[0]); i++) {
        char old[32];
        if (!storage_join_path(old, sizeof(old), mount, old_names[i])) continue;
        storage_io_acquire(CLS);
        const bool gone = remove(old) == 0;
        storage_io_release();
        if (gone) ESP_LOGI(TAG, "removed %s: an earlier format", old);
    }

    ctx_t c = { vol, mount, NULL, NULL };
    const msync_ops_t ops = {
        .cat_append = cat_append,
        .cat_read = cat_read,
        .tags = tags,
        .walk = walk,
        .cat_flush = cat_flush,
        .ctx = &c,
        .index_path = index,
        .temp_path = temp,
        .now = settings_now(),
        .clock = wifi_ntp_synced() ? MIDX_CLOCK_SYNCED : MIDX_CLOCK_FLOOR,
        .abort = abort,
    };

    const int64_t t0 = esp_timer_get_time();
    s_tag_us = s_cat_us = 0;
    if (!mediacat_session_open(vol)) return MSYNC_FAILED;
    const msync_result_t r = msync_run(&ops, stats);
    /* Already closed by cat_flush() on a run that installed; this is
     * the close for every other ending, and its result does not matter
     * -- nothing points at those lines. */
    mediacat_session_close();
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);

    static const char *const what[] = { "done", "stopped", "FAILED" };
    ESP_LOGI(TAG, "%s: %s in %d ms -- keep %d, add %d, update %d, "
             "revive %d, bury %d, %d catalog reads%s",
             storage_label(vol), what[r], ms, stats->keep, stats->add,
             stats->update, stats->revive, stats->bury, stats->cat_reads,
             stats->index_damaged ? "; the index was damaged and is gone" : "");
    const int tag_ms = (int)(s_tag_us / 1000), cat_ms = (int)(s_cat_us / 1000);
    ESP_LOGI(TAG, "%s: %d ms reading tags, %d ms in the catalog, "
             "%d ms walking and indexing", storage_label(vol), tag_ms, cat_ms,
             ms - tag_ms - cat_ms);

    if (r == MSYNC_DONE) storage_mark_hidden(index);
    return r;
}

/* ---- the task --------------------------------------------------------- */

static medialib_status_t s_status[STORAGE_COUNT];
static volatile bool     s_busy;
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;

/* The automatic run's bookkeeping. medialib_poll() is the only writer
 * of all of it except s_pending, which medialib_request() also clears. */
static volatile bool s_pending[STORAGE_COUNT];
static bool          s_was_present[STORAGE_COUNT];
static TickType_t    s_mounted_at[STORAGE_COUNT];
static uint32_t      s_seen_gen = UINT32_MAX;

static void reindex_task(void *arg)
{
    const storage_id_t vol = (storage_id_t)(intptr_t)arg;
    medialib_status_t *s = &s_status[vol];

    storage_hold_background(vol);
    const int64_t t0 = esp_timer_get_time();
    const msync_result_t r = medialib_reconcile(vol, NULL, &s->stats);
    s->ms = (int)((esp_timer_get_time() - t0) / 1000);
    storage_hold_background(STORAGE_COUNT);

    s->state = (r == MSYNC_DONE)    ? MEDIALIB_DONE
             : (r == MSYNC_STOPPED) ? MEDIALIB_STOPPED
                                    : MEDIALIB_FAILED;

    /* ESP-IDF counts stacks in bytes. */
    ESP_LOGI(TAG, "reindex task: %u of %u stack bytes never touched",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)MEDIALIB_STACK);

    s_busy = false;
    vTaskDelete(NULL);
}

bool medialib_request(storage_id_t vol)
{
    if (vol >= STORAGE_COUNT || !storage_present(vol)) return false;

    taskENTER_CRITICAL(&s_mux);
    const bool was = s_busy;
    s_busy = true;
    taskEXIT_CRITICAL(&s_mux);
    if (was) return false;

    memset(&s_status[vol].stats, 0, sizeof(s_status[vol].stats));
    s_status[vol].ms = 0;
    s_status[vol].state = MEDIALIB_RUNNING;

    /* This run is the one an automatic run was waiting to do. */
    s_pending[vol] = false;

    if (xTaskCreate(reindex_task, "reindex", MEDIALIB_STACK,
                    (void *)(intptr_t)vol, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the reindex task");
        s_status[vol].state = MEDIALIB_FAILED;
        s_busy = false;
        return false;
    }
    ESP_LOGI(TAG, "reindex %s: started", storage_label(vol));
    return true;
}

bool medialib_busy(void) { return s_busy; }

void medialib_status(storage_id_t vol, medialib_status_t *out)
{
    if (!out) return;
    if (vol >= STORAGE_COUNT) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_status[vol];
    out->pending = s_pending[vol];
}

void medialib_poll(void)
{
    const TickType_t now = xTaskGetTickCount();

    const uint32_t gen = storage_generation();
    if (gen != s_seen_gen) {
        s_seen_gen = gen;
        for (int v = 0; v < STORAGE_COUNT; v++) {
            const bool present = storage_present((storage_id_t)v);
            if (present && !s_was_present[v]) {
                s_pending[v] = true;
                s_mounted_at[v] = now;
            }
            if (!present) s_pending[v] = false;
            s_was_present[v] = present;
        }
    }

    if (s_busy) return;
    for (int v = 0; v < STORAGE_COUNT; v++) {
        if (!s_pending[v]) continue;
        if ((now - s_mounted_at[v]) < pdMS_TO_TICKS(MEDIALIB_SETTLE_MS)) continue;
        if (medialib_request((storage_id_t)v)) {
            ESP_LOGI(TAG, "reindex %s: automatic, on mount",
                     storage_label((storage_id_t)v));
            return;             /* one at a time; the other waits */
        }
        /* Refused. Gone again: nothing to do. Anything else (no memory
         * for the task) is not retried every 20 ms -- the next mount or
         * the button will try again. */
        s_pending[v] = false;
    }
}
