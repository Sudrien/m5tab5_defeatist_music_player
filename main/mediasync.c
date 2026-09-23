/*
 * mediasync.c -- see mediasync.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mediasync.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "storage_io.h"

static const char *TAG = "mediasync";

#define CLS     STORAGE_IO_BACKGROUND

/*
 * The merge's state. Static, and large for a stack: two paths, two
 * records of ~720 bytes, the previous path on each side.
 */
static struct {
    const msync_ops_t *ops;
    msync_stats_t     *st;

    FILE     *old;          /* the old index, NULL if there is none */
    uint32_t  nold, iold;   /* records in it, and the next to read */
    bool      have_cur;     /* cur is the old index's current record */
    midx_rec_t cur;
    char      cur_path[MIDX_PATH_MAX + 1];

    FILE     *out;          /* the new index, being written */

    char      prev_card[MIDX_PATH_MAX + 1];
    char      prev_cat[MIDX_PATH_MAX + 1];
    bool      any_card, any_cat;

    bool      failed, stopped, damaged;
    mediacat_rec_t rec;
} S;

/* ---- the old index, one record at a time ----------------------------- */

static void damaged(const char *why)
{
    ESP_LOGW(TAG, "%s: %s at record %u; it will be rebuilt",
             S.ops->index_path, why, (unsigned)S.iold);
    S.damaged = true;
    S.failed = true;
}

/*
 * Make S.cur the next old record, or clear have_cur at the end. The
 * record's full path goes into cur_path: the key when that is the whole
 * path, the catalog's line when it is not -- checked against the key,
 * because a catalog that disagrees is an index that cannot be used.
 */
static void cur_next(void)
{
    S.have_cur = false;
    if (!S.old || S.iold >= S.nold || S.failed) return;

    uint8_t raw[MIDX_REC_SIZE];
    if (storage_io_fread(raw, sizeof(raw), S.old, CLS) != sizeof(raw)) {
        damaged("short read");
        return;
    }
    if (!midx_rec_unpack(raw, &S.cur)) {
        damaged("not a record");
        return;
    }
    if (S.cur.path_len <= MIDX_KEY_LEN) {
        memcpy(S.cur_path, S.cur.key, (size_t)S.cur.path_len + 1);
    } else {
        S.st->cat_reads++;
        if (!S.ops->cat_read(S.ops->ctx, S.cur.cat_off, &S.rec) ||
            strlen(S.rec.path) != S.cur.path_len ||
            memcmp(S.rec.path, S.cur.key, MIDX_KEY_LEN) != 0) {
            damaged("a long path the catalog does not confirm");
            return;
        }
        memcpy(S.cur_path, S.rec.path, (size_t)S.cur.path_len + 1);
    }
    if (!midx_in_order(S.any_cat ? S.prev_cat : NULL, S.cur_path)) {
        damaged("out of order");
        return;
    }
    memcpy(S.prev_cat, S.cur_path, (size_t)S.cur.path_len + 1);
    S.any_cat = true;
    S.iold++;
    S.have_cur = true;
}

/* ---- the new index --------------------------------------------------- */

static void out_rec(const char *path, uint32_t off, uint8_t flags,
                    midx_stamp_t stamp)
{
    uint8_t raw[MIDX_REC_SIZE];
    if (!midx_rec_pack(raw, path, off, flags, stamp)) {
        ESP_LOGW(TAG, "cannot index %s", path);
        S.failed = true;
        return;
    }
    storage_io_acquire(CLS);
    const bool ok = fwrite(raw, 1, sizeof(raw), S.out) == sizeof(raw);
    storage_io_release();
    if (!ok) {
        ESP_LOGW(TAG, "writing %s failed", S.ops->temp_path);
        S.failed = true;
    }
}

/* Append S.rec, stamped now, and index it. */
static void append_and_index(uint8_t flags)
{
    S.rec.written = S.ops->now;
    S.rec.clock = S.ops->clock;
    uint32_t off = 0;
    if (!S.ops->cat_append(S.ops->ctx, &S.rec, &off)) {
        ESP_LOGW(TAG, "the catalog would not take %s", S.rec.path);
        S.failed = true;
        return;
    }
    out_rec(S.rec.path, off, flags, S.rec.stamp);
}

/* A fresh record for a card entry, tags read from the file. */
static void fresh(const char *path, midx_stamp_t stamp)
{
    memset(&S.rec, 0, sizeof(S.rec));
    memcpy(S.rec.path, path, strlen(path) + 1);
    S.rec.stamp = stamp;
    S.ops->tags(S.ops->ctx, path, &S.rec);
    /* Whatever tags() wrote, the path and stamp are the walk's. */
    memcpy(S.rec.path, path, strlen(path) + 1);
    S.rec.stamp = stamp;
    S.rec.deleted_at = 0;
}

/* The old record's catalog line, for REVIVE and BURY. False if the line
 * is gone or unreadable -- the callers have a fallback each. */
static bool old_line(void)
{
    S.st->cat_reads++;
    if (!S.ops->cat_read(S.ops->ctx, S.cur.cat_off, &S.rec)) return false;
    return strcmp(S.rec.path, S.cur_path) == 0;
}

static void apply(const midx_step_t *step, const midx_card_t *card)
{
    switch (step->action) {
    case MIDX_KEEP:
        S.st->keep++;
        out_rec(S.cur_path, S.cur.cat_off, S.cur.flags, S.cur.stamp);
        break;

    case MIDX_ADD:
    case MIDX_UPDATE:
        if (step->action == MIDX_ADD) S.st->add++;
        else                          S.st->update++;
        fresh(card->path, card->stamp);
        append_and_index(0);
        break;

    case MIDX_REVIVE:
        S.st->revive++;
        if (old_line()) {
            S.rec.deleted_at = 0;
        } else {
            /* The tombstone's line is gone: read the file as new. */
            fresh(card->path, card->stamp);
        }
        S.rec.stamp = card->stamp;
        append_and_index(0);
        break;

    case MIDX_BURY:
        S.st->bury++;
        if (!old_line()) {
            /* The live line is gone. A tombstone needs only the path
             * and stamp, and the index has both. */
            memset(&S.rec, 0, sizeof(S.rec));
            memcpy(S.rec.path, S.cur_path, (size_t)S.cur.path_len + 1);
            S.rec.stamp = S.cur.stamp;
        }
        /* deleted_at must be non-zero to mean anything. */
        S.rec.deleted_at = S.ops->now > 0 ? S.ops->now : 1;
        append_and_index(MIDX_F_DEAD);
        break;

    case MIDX_DONE:
    default:
        break;
    }
}

/* Drive midx_step() until it takes the card entry (or, with card NULL,
 * until the old index is used up). */
static void merge(const midx_card_t *card)
{
    for (;;) {
        if (S.failed) return;
        midx_cat_t cat, *cp = NULL;
        if (S.have_cur) {
            cat.path = S.cur_path;
            cat.stamp = S.cur.stamp;
            cat.deleted_at = (S.cur.flags & MIDX_F_DEAD) ? 1 : 0;
            cp = &cat;
        }
        const midx_step_t step = midx_step(card, cp);
        if (step.action == MIDX_DONE) return;
        apply(&step, card);
        if (S.failed) return;
        if (step.take_cat) cur_next();
        if (step.take_card) return;
    }
}

static bool on_card(void *ctx, const char *path, midx_stamp_t stamp)
{
    (void)ctx;
    if (S.ops->abort && *S.ops->abort) {
        S.stopped = true;
        return false;
    }
    if (!midx_in_order(S.any_card ? S.prev_card : NULL, path)) {
        ESP_LOGW(TAG, "the walk is out of order at %s", path);
        S.failed = true;
        return false;
    }
    const size_t n = strlen(path);
    if (n > MIDX_PATH_MAX) {
        S.failed = true;
        return false;
    }
    memcpy(S.prev_card, path, n + 1);
    S.any_card = true;

    const midx_card_t card = { path, stamp };
    merge(&card);
    return !S.failed;
}

/* ---- the run --------------------------------------------------------- */

static void close_file(FILE **f)
{
    if (!*f) return;
    storage_io_acquire(CLS);
    storage_io_close(*f);
    storage_io_release();
    *f = NULL;
}

static bool open_old(void)
{
    storage_io_acquire(CLS);
    S.old = storage_io_open(S.ops->index_path, "rb");
    long size = -1;
    if (S.old && fseek(S.old, 0, SEEK_END) == 0) size = ftell(S.old);
    const bool rewound = S.old && fseek(S.old, 0, SEEK_SET) == 0;
    storage_io_release();

    if (!S.old) return true;            /* no index yet: all ADD */
    if (size < 0 || !rewound || size % MIDX_REC_SIZE != 0) {
        damaged("not whole records");
        return false;
    }
    S.nold = (uint32_t)(size / MIDX_REC_SIZE);
    return true;
}

msync_result_t msync_run(const msync_ops_t *ops, msync_stats_t *stats)
{
    memset(&S, 0, sizeof(S));
    static msync_stats_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    if (!stats) stats = &dummy;
    memset(stats, 0, sizeof(*stats));
    if (!ops || !ops->cat_append || !ops->cat_read || !ops->tags ||
        !ops->walk || !ops->index_path || !ops->temp_path) {
        return MSYNC_FAILED;
    }
    S.ops = ops;
    S.st = stats;

    if (open_old()) {
        storage_io_acquire(CLS);
        S.out = storage_io_open(ops->temp_path, "wb");
        storage_io_release();
        if (!S.out) {
            ESP_LOGW(TAG, "cannot write %s", ops->temp_path);
            S.failed = true;
        }
    }

    if (!S.failed) {
        cur_next();
        const mwalk_result_t w = ops->walk(ops->ctx, on_card, NULL);
        if (w == MWALK_FAILED && !S.stopped) S.failed = true;
        if (w == MWALK_STOPPED && !S.failed) S.stopped = true;
        /* Only a complete walk may bury what it did not see. */
        if (!S.failed && !S.stopped) merge(NULL);
    }

    bool flushed = false;
    if (S.out) {
        storage_io_acquire(CLS);
        flushed = fflush(S.out) == 0;
        storage_io_release();
    }
    close_file(&S.out);
    close_file(&S.old);

    msync_result_t r = S.failed ? MSYNC_FAILED
                     : S.stopped ? MSYNC_STOPPED : MSYNC_DONE;
    if (r == MSYNC_DONE && !flushed) r = MSYNC_FAILED;

    storage_io_acquire(CLS);
    if (r == MSYNC_DONE) {
        remove(ops->index_path);
        if (rename(ops->temp_path, ops->index_path) != 0) {
            ESP_LOGW(TAG, "could not install %s", ops->index_path);
            r = MSYNC_FAILED;
        }
    } else {
        remove(ops->temp_path);
        if (S.damaged) remove(ops->index_path);
    }
    storage_io_release();

    stats->index_damaged = S.damaged;
    return r;
}
