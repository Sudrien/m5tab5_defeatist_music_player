/*
 * cardtime.c -- see cardtime.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "cardtime.h"
#include "settings.h"
#include "storage.h"

static const char *TAG = "tab5_cardtime";

/*
 * Not the stack, for browser.c's reason and CLAUDE.md's rule: 512 bytes
 * is past what belongs on a task stack, and this runs on media_task,
 * which already carries minimp3's scratch.
 *
 * A single buffer is safe because there is one caller --
 * cardtime_note_volumes(), from the storage-generation branch of
 * media_task -- and it is not reentrant. If a second task ever wants
 * this, give it its own buffer rather than making this one bigger.
 */
static char s_path[512];

/* 5101: which entry gave the last candidate, so the log can name it. */
static char s_best_name[64];

/* 5112: the candidates of one scan are held (2 KB, from the heap, for
 * the length of a scan -- once per mount) so they can be weighed against
 * each other rather than the latest simply winning. */

/* The name of the entry at a filtered value, by a second pass. Only for
 * the log, and only when there is something to say. */
static void name_of(const char *mount, int64_t ref, int64_t want, char *out, size_t len)
{
    out[0] = '\0';
    DIR *d = opendir(mount);
    if (!d) return;
    const struct dirent *e;
    int scanned = 0;
    while ((e = readdir(d)) != NULL && scanned < CARDTIME_SCAN_MAX) {
        if (e->d_name[0] == '.' || cardtime_own(e->d_name)) continue;
        if (!storage_join_path(s_path, sizeof(s_path), mount, e->d_name)) continue;
        struct stat st;
        if (stat(s_path, &st) != 0) continue;
        scanned++;
        if (cardtime_filter((int64_t)st.st_mtime, ref) == want) {
            snprintf(out, len, "%.63s", e->d_name);
            break;
        }
    }
    closedir(d);
}

int64_t cardtime_root_candidate(const char *mount, int64_t ref)
{
    if (!mount || !*mount || ref <= 0) return 0;

    int64_t *cand = calloc(CARDTIME_SCAN_MAX, sizeof(int64_t));
    if (!cand) return 0;

    DIR *d = opendir(mount);
    if (!d) {
        /* A volume that reported present and will not open is the
         * storage layer's problem to log, not this one's. */
        ESP_LOGD(TAG, "%s: cannot open", mount);
        free(cand);
        return 0;
    }

    int scanned = 0, rejected = 0;
    s_best_name[0] = '\0';
    const struct dirent *e;

    while ((e = readdir(d)) != NULL && scanned < CARDTIME_SCAN_MAX) {
        if (e->d_name[0] == '.') continue;      /* "." and ".." and hidden */
        if (cardtime_own(e->d_name)) continue;  /* 5112: stamped from the floor */

        if (!storage_join_path(s_path, sizeof(s_path), mount, e->d_name)) continue;

        /*
         * stat() and not readdir()'s own fields: ESP-IDF's FAT VFS hands
         * back a POSIX struct dirent, which carries d_name and d_type and
         * no timestamps. At a volume root, bounded by CARDTIME_SCAN_MAX,
         * a stat() per entry is not worth going under the VFS for.
         */
        struct stat st;
        if (stat(s_path, &st) != 0) continue;

        cand[scanned] = cardtime_filter((int64_t)st.st_mtime, ref);
        scanned++;
        if (cand[scanned - 1] == 0 && (int64_t)st.st_mtime > ref + CARDTIME_MAX_AHEAD_S) {
            /*
             * Worth one line each. A file dated past the ceiling is the
             * thing this guard exists for, and a card carrying one will
             * carry it on every boot -- so a reader wondering why the
             * clock never improves has something to find.
             */
            if (rejected < 4) {
                ESP_LOGW(TAG, "%s/%s: mtime %lld is absurd, ignored",
                         mount, e->d_name, (long long)st.st_mtime);
            }
            rejected++;
        }
    }
    closedir(d);

    if (rejected > 4) {
        ESP_LOGW(TAG, "%s: %d absurd timestamps in all", mount, rejected);
    }

    int at;
    const int64_t best = cardtime_pick(cand, scanned, &at);
    int64_t lone = 0;                   /* the latest candidate, corroborated or not */
    for (int i = 0; i < scanned; i++) if (cand[i] > lone) lone = cand[i];
    free(cand);

    if (lone > best) {
        /* 5112: the one that used to win. Named, so it can be found. */
        char nm[64];
        name_of(mount, ref, lone, nm, sizeof(nm));
        ESP_LOGW(TAG, "%s/%s is dated %lld days after anything else on this "
                      "card; not taken as the time on its own",
                 mount, nm[0] ? nm : "?", (long long)((lone - (best ? best : ref)) / 86400));
    }
    if (best) name_of(mount, ref, best, s_best_name, sizeof(s_best_name));
    ESP_LOGD(TAG, "%s: %d entries, best %lld", mount, scanned, (long long)best);
    return best;
}

void cardtime_note_volumes(void)
{
    /*
     * The floor's current value is the reference, read back rather than
     * re-parsing the build stamp -- settings_init() has already seeded
     * it, and keeping one parse in the project is worth more than
     * insisting the reference be the build specifically.
     *
     * It may be higher than the build by now: a card's own settings
     * record raises it during load. That is fine and slightly better --
     * candidates get measured against the best time known. The ceiling
     * holds either way, and settings_note_ntp_time() independently
     * refuses anything below the live floor. Two checks, neither relying
     * on the other being right.
     */
    const int64_t ref = settings_last_ntp_epoch();
    if (ref <= 0) return;

    static const storage_id_t ids[] = { STORAGE_SD, STORAGE_USB };

    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if (!storage_present(ids[i])) continue;

        const char *mount = storage_mount_path(ids[i]);
        const int64_t cand = cardtime_root_candidate(mount, ref);
        if (cand <= 0) continue;

        if (settings_note_ntp_time(cand, esp_timer_get_time())) {
            ESP_LOGI(TAG, "%s raised the clock floor to %lld (%s)",
                     mount, (long long)cand, s_best_name);
        } else if (cand > settings_now()) {
            /* 5101: only NTP's time refuses a later candidate. */
            ESP_LOGW(TAG, "%s/%s is dated %lld days after NTP's time; its "
                          "date is wrong, and it is ignored",
                     mount, s_best_name,
                     (long long)((cand - settings_now()) / 86400));
        }
    }
}
