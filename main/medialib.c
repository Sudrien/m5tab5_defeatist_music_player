/*
 * medialib.c -- see medialib.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "medialib.h"

#include <stdio.h>
#include <string.h>

#include "covertag.h"
#include "cuedir.h"
#include "cuesheet.h"
#include "esp_log.h"
#include "esp_timer.h"
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
} ctx_t;

/* The volume's absolute path for a relative one. Static: one reconcile
 * at a time, and 512 bytes is not a stack local. */
static char s_abs[16 + MIDX_PATH_MAX + 2];

static const char *absolute(const ctx_t *c, const char *rel)
{
    if (!storage_join_path(s_abs, sizeof(s_abs), c->mount, rel)) return NULL;
    return s_abs;
}

static bool cat_append(void *ctx, const mediacat_rec_t *r, uint32_t *off)
{
    const ctx_t *c = ctx;
    return mediacat_append(c->vol, r, off);
}

/*
 * Opened per read rather than held for the run. Reads are the minority
 * -- a path longer than the index key, a revive, a bury -- and a
 * handle held open beside the one mediacat_append() opens would be a
 * second view of a file being appended to, with its own cached size
 * and sector.
 */
static bool cat_read(void *ctx, uint32_t off, mediacat_rec_t *out)
{
    const ctx_t *c = ctx;
    char path[32];
    if (!mediacat_path(c->vol, path, sizeof(path))) return false;
    storage_io_acquire(CLS);
    FILE *f = storage_io_open(path, "rb");
    storage_io_release();
    if (!f) return false;
    const bool ok = mediacat_read_at(f, off, out);
    storage_io_acquire(CLS);
    storage_io_close(f);
    storage_io_release();
    return ok;
}

/*
 * Tags the way the player gets them for the screen: a cue track's from
 * its sheet (an image's own tags describe the whole disc), anything
 * else's from the file. A track with no tags is still indexed; MPD
 * shows it by name.
 */
static void tags(void *ctx, const char *rel, mediacat_rec_t *r)
{
    const ctx_t *c = ctx;
    const char *abs = absolute(c, rel);
    if (!abs) return;

    if (cue_vpath_split(abs, NULL)) {
        cuedir_tags(abs, r->title, r->artist, r->album, MEDIACAT_TAG_LEN);
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

static mwalk_result_t walk(void *ctx, mwalk_fn fn, void *wctx)
{
    const ctx_t *c = ctx;
    return mwalk_volume(c->mount, fn, wctx);
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

    ctx_t c = { vol, mount };
    const msync_ops_t ops = {
        .cat_append = cat_append,
        .cat_read = cat_read,
        .tags = tags,
        .walk = walk,
        .ctx = &c,
        .index_path = index,
        .temp_path = temp,
        .now = settings_now(),
        .clock = wifi_ntp_synced() ? MIDX_CLOCK_SYNCED : MIDX_CLOCK_FLOOR,
        .abort = abort,
    };

    const int64_t t0 = esp_timer_get_time();
    const msync_result_t r = msync_run(&ops, stats);
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);

    static const char *const what[] = { "done", "stopped", "FAILED" };
    ESP_LOGI(TAG, "%s: %s in %d ms -- keep %d, add %d, update %d, "
             "revive %d, bury %d, %d catalog reads%s",
             storage_label(vol), what[r], ms, stats->keep, stats->add,
             stats->update, stats->revive, stats->bury, stats->cat_reads,
             stats->index_damaged ? "; the index was damaged and is gone" : "");

    if (r == MSYNC_DONE) storage_mark_hidden(index);
    return r;
}
