/*
 * cardtime.c -- see cardtime.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
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

int64_t cardtime_root_candidate(const char *mount, int64_t ref)
{
    if (!mount || !*mount || ref <= 0) return 0;

    DIR *d = opendir(mount);
    if (!d) {
        /* A volume that reported present and will not open is the
         * storage layer's problem to log, not this one's. */
        ESP_LOGD(TAG, "%s: cannot open", mount);
        return 0;
    }

    int64_t best = 0;
    int scanned = 0, rejected = 0;
    const struct dirent *e;

    while ((e = readdir(d)) != NULL && scanned < CARDTIME_SCAN_MAX) {
        if (e->d_name[0] == '.') continue;      /* "." and ".." and hidden */

        if (!storage_join_path(s_path, sizeof(s_path), mount, e->d_name)) continue;

        /*
         * stat() and not readdir()'s own fields: ESP-IDF's FAT VFS hands
         * back a POSIX struct dirent, which carries d_name and d_type and
         * no timestamps. FatFs's native f_readdir fills a FILINFO with
         * the date and time already in it, so a future reader wanting
         * this for a whole-tree walk should go under the VFS rather than
         * pay a stat() per file. At a volume root, bounded by
         * CARDTIME_SCAN_MAX, the difference is not worth the layering.
         */
        struct stat st;
        if (stat(s_path, &st) != 0) continue;

        scanned++;

        const int64_t cand = cardtime_filter((int64_t)st.st_mtime, ref);
        if (cand > best) {
            best = cand;
        } else if (cand == 0 && (int64_t)st.st_mtime > ref + CARDTIME_MAX_AHEAD_S) {
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
            ESP_LOGI(TAG, "%s raised the clock floor to %lld",
                     mount, (long long)cand);
        }
    }
}
