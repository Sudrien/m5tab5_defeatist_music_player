/*
 * replaygain.c -- see replaygain.h for the design. This file is the
 * on-disk record and the path it lives at.
 *
 * SPDX-License-Identifier: MIT
 */
#include "replaygain.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "storage.h"
#include "storage_io.h"

static const char *TAG = "tab5_rg";

/*
 * The packed on-disk record. Not replaygain_t directly -- that struct is
 * free to grow padding or change field order under a compiler's rules,
 * and a sidecar written by one build has to be readable by the next one
 * built from the same source. magic + version are checked before
 * anything else is trusted; reserved is here so a future field (the
 * ReplayGain dB/peak pair, or a seek index -- see "WHAT THIS DOES NOT DO
 * YET" in the header) can be added without another version bump eating
 * every sidecar on the card the day it ships.
 */
#define REPLAYGAIN_MAGIC  (0x52474331u)  /* "RGC1" as bytes, arch-neutral */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t filesize;
    int64_t  mtime;
    uint16_t columns;      /* REPLAYGAIN_COLUMNS at write time */
    uint8_t  has_levels;   /* 0/1, see replaygain_t */
    uint8_t  reserved;
    uint32_t frames;       /* mirrors framewalk_t.frames */
    uint32_t sec;          /* mirrors framewalk_t.sec */
    uint8_t  waveform[REPLAYGAIN_COLUMNS];
} replaygain_record_t;

/*
 * "." + filename + ".rgcache", next to the track. storage_is_hidden()
 * already excludes anything starting with '.' from directory listings
 * and playlist scans, so this needs no separate hiding logic -- it is
 * invisible to the browser for the same reason "._Resource" forks are.
 *
 * Built from the track's own path rather than a hash of it: a hash
 * would still need somewhere to live, and a fixed sibling name is one
 * fewer thing that can collide or need its own directory.
 */
static bool sidecar_path(const char *track_path, char *out, size_t out_len,
                         const char *suffix)
{
    const char *slash = strrchr(track_path, '/');
    const char *name = slash ? slash + 1 : track_path;
    const size_t dir_len = (size_t)(name - track_path);  /* includes '/' */

    /* dir + "." + name + suffix, proven to fit rather than snprintf'd
     * and hoped -- storage.h takes the same position on path joins and
     * the reasoning is identical: a truncated path is a path to a
     * different file, or to none, and opening either silently is worse
     * than refusing. */
    const size_t need = dir_len + 1 /* '.' */ + strlen(name) + strlen(suffix) + 1;
    if (need > out_len) return false;

    memcpy(out, track_path, dir_len);
    out[dir_len] = '.';
    strcpy(out + dir_len + 1, name);
    strcat(out, suffix);
    return true;
}

bool replaygain_load(const char *path, replaygain_t *out)
{
    if (!path || !out) return false;

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "stat failed for %s; no cache lookup", path);
        return false;
    }

    char cache_path[600];
    if (!sidecar_path(path, cache_path, sizeof(cache_path), ".rgcache")) {
        ESP_LOGW(TAG, "sidecar path too long for %s", path);
        return false;
    }

    FILE *f = storage_io_open(cache_path, "rb");
    if (!f) return false;  /* ordinary case: no cache yet */

    replaygain_record_t rec;
    const size_t got = fread(&rec, 1, sizeof(rec), f);
    storage_io_close(f);

    if (got != sizeof(rec)) {
        ESP_LOGI(TAG, "sidecar truncated for %s; will re-walk", path);
        return false;
    }
    if (rec.magic != REPLAYGAIN_MAGIC) {
        ESP_LOGI(TAG, "sidecar not ours for %s; will re-walk", path);
        return false;
    }
    if (rec.version != REPLAYGAIN_VERSION) {
        ESP_LOGI(TAG, "sidecar version %u != %u for %s; will re-walk",
                 (unsigned)rec.version, (unsigned)REPLAYGAIN_VERSION, path);
        return false;
    }
    if (rec.columns != REPLAYGAIN_COLUMNS) {
        ESP_LOGI(TAG, "sidecar column count %u != %u for %s; will re-walk",
                 (unsigned)rec.columns, (unsigned)REPLAYGAIN_COLUMNS, path);
        return false;
    }
    if (rec.filesize != (uint32_t)st.st_size || rec.mtime != (int64_t)st.st_mtime) {
        ESP_LOGI(TAG, "sidecar stale for %s (size/mtime changed); "
                      "will re-walk", path);
        return false;
    }

    out->filesize = rec.filesize;
    out->mtime = rec.mtime;
    out->has_levels = (rec.has_levels != 0);
    out->frames = rec.frames;
    out->sec = rec.sec;
    memcpy(out->waveform, rec.waveform, sizeof(out->waveform));

    ESP_LOGI(TAG, "waveform from sidecar: %s", cache_path);
    return true;
}

/* Same edge-based max-per-bucket resample framewalk.c's resample() uses
 * for the live envelope, applied here to go from however many columns
 * the walk actually filled down to the fixed on-disk width. Max, not
 * mean, for the reason framewalk.c gives: a mean over several source
 * columns turns a transient into a bump, and the transient is the part
 * that makes one track's shape recognisable from another's. */
static void resample_to_sidecar(const uint8_t *src, int n, uint8_t *dst)
{
    if (n <= 0) {
        memset(dst, 0, REPLAYGAIN_COLUMNS);
        return;
    }
    for (int c = 0; c < REPLAYGAIN_COLUMNS; c++) {
        const uint32_t a = (uint32_t)(((uint64_t)c * (uint32_t)n) / REPLAYGAIN_COLUMNS);
        uint32_t b = (uint32_t)(((uint64_t)(c + 1) * (uint32_t)n) / REPLAYGAIN_COLUMNS);
        if (b <= a) b = a + 1;
        if (b > (uint32_t)n) b = (uint32_t)n;

        uint8_t m = 0;
        for (uint32_t i = a; i < b; i++) if (src[i] > m) m = src[i];
        dst[c] = m;
    }
}

esp_err_t replaygain_save(const char *path, const framewalk_t *w)
{
    if (!path || !w) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "stat failed for %s; not caching", path);
        return ESP_FAIL;
    }

    replaygain_record_t rec = {
        .magic = REPLAYGAIN_MAGIC,
        .version = REPLAYGAIN_VERSION,
        .filesize = (uint32_t)st.st_size,
        .mtime = (int64_t)st.st_mtime,
        .columns = REPLAYGAIN_COLUMNS,
        .has_levels = (uint8_t)(w->has_levels && w->columns > 0),
        .reserved = 0,
        .frames = w->frames,
        .sec = w->sec,
    };

    if (rec.has_levels) {
        resample_to_sidecar(w->level, w->columns, rec.waveform);
    } else {
        /* No per-frame loudness for this format (AAC, AMR). Write a
         * flat zero waveform rather than skipping the sidecar entirely
         * -- filesize/mtime alone are still worth caching so a repeat
         * play does not pay for a whole-file walk just to learn "no
         * envelope" a second time. has_levels=false is what stops that
         * flat zero being drawn as a real (silent) track. */
        memset(rec.waveform, 0, sizeof(rec.waveform));
    }

    char final_path[600];
    char tmp_path[608];
    if (!sidecar_path(path, final_path, sizeof(final_path), ".rgcache")) {
        ESP_LOGW(TAG, "sidecar path too long for %s; not caching", path);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!sidecar_path(path, tmp_path, sizeof(tmp_path), ".rgcache.tmp")) {
        ESP_LOGW(TAG, "sidecar tmp path too long for %s; not caching", path);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = storage_io_open(tmp_path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "could not open %s for writing", tmp_path);
        return ESP_FAIL;
    }

    const size_t wrote = fwrite(&rec, 1, sizeof(rec), f);
    const int cerr = storage_io_close(f);

    if (wrote != sizeof(rec) || cerr != 0) {
        ESP_LOGW(TAG, "write failed for %s; leaving old sidecar in place",
                 tmp_path);
        remove(tmp_path);
        return ESP_FAIL;
    }

    /* Atomic on FatFs: rename() is a directory-entry update, not a
     * copy. A crash between here and the write above leaves a stray
     * .tmp that the next write overwrites and every reader ignores --
     * never a half-written file under the name replaygain_load() will
     * open. */
    if (rename(tmp_path, final_path) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed", tmp_path, final_path);
        remove(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "sidecar written: %s (%u bytes, %d source columns)",
             final_path, (unsigned)sizeof(rec), w->columns);
    return ESP_OK;
}
