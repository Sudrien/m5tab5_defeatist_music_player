/*
 * settings.c -- defeatist.dat, and the care needed to write it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "settings.h"
#include "storage.h"

static const char *TAG = "tab5_settings";

#define SETTINGS_NAME   "defeatist.dat"
#define BACKUP_NAME     "defeatist.bak"
#define TEMP_NAME       "defeatist.tmp"

/*
 * How long after the last change the file is written.
 *
 * A volume drag emits one change per UI poll -- fifty a second on a
 * long one -- and each write is an erase-modify-write of a FAT cluster
 * on a card that is also feeding the decoder. Following every change
 * would put a card write between the decoder and its next read forty
 * times a second, which is exactly the starvation the PCM ring exists to
 * absorb, caused by the thing meant to be storing a preference.
 *
 * Three seconds is longer than any drag and shorter than the gap between
 * putting the player down and picking it up again.
 */
#define SETTINGS_SETTLE_MS      (3000)

/*
 * Long enough for the longest plausible file; anything longer is not a
 * settings file this program wrote and is treated as corrupt.
 *
 * Prefixed because LINE_MAX is not ours: POSIX defines it in
 * <limits.h>, sys/param.h pulls it in, and redefining it was a warning
 * today and would be somebody else's confusing bug later. The others are
 * prefixed for consistency rather than necessity -- a file where one
 * constant is spelled differently from its neighbours invites the
 * question of what is special about it, and the answer would be
 * "nothing, it just collided once".
 */
#define SETTINGS_MAX_FILE_BYTES (1024)
#define SETTINGS_MAX_LINE       (128)

static uint8_t    s_volume = 50;
static volatile bool s_dirty;
static TickType_t s_dirty_since;

/* Where the file lives, decided once at init. NULL means nowhere was
 * mounted and nothing will be written this boot. */
static const char *s_root;

uint8_t settings_volume(void) { return s_volume; }

void settings_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (percent == s_volume) return;
    s_volume = percent;
    s_dirty = true;
    s_dirty_since = xTaskGetTickCount();
}

/* ------------------------------------------------------------------ */

static bool path_for(char *out, size_t out_len, const char *name)
{
    if (!s_root) return false;
    return snprintf(out, out_len, "%s/%s", s_root, name) < (int)out_len;
}

/*
 * One key=value line.
 *
 * Unknown keys are skipped rather than treated as errors, which is what
 * makes a card survive moving between builds: an older firmware reading
 * a newer file keeps the settings it understands instead of throwing the
 * file away because of a line it has never heard of.
 */
static void parse_line(char *line)
{
    char *eq = strchr(line, '=');
    if (!eq) return;
    *eq = '\0';
    const char *key = line;
    const char *val = eq + 1;

    if (strcmp(key, "volume") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        s_volume = (uint8_t)v;
    } else {
        ESP_LOGD(TAG, "unknown key '%s'", key);
    }
}

static bool load_file(const char *name)
{
    char path[128];
    if (!path_for(path, sizeof(path), name)) return false;

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[SETTINGS_MAX_LINE];
    size_t total = 0;
    bool any = false;
    while (fgets(line, sizeof(line), f)) {
        total += strlen(line);
        if (total > SETTINGS_MAX_FILE_BYTES) {
            /* Not a file this program wrote. Stopping here rather than
             * reading on: whatever it is, the values taken from it so
             * far are as trustworthy as they are going to get, and the
             * defaults cover the rest. */
            ESP_LOGW(TAG, "%s is too long; ignoring the remainder", name);
            break;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        parse_line(line);
        any = true;
    }
    fclose(f);
    return any;
}

/*
 * Write, then rotate. The order is the whole point.
 *
 * A truncated write must not be able to destroy the last good copy, and
 * on FAT there is no atomic replace to lean on. So the new file is
 * written under a third name and only once it is closed does anything
 * touch the other two:
 *
 *   1. write defeatist.tmp in full, flush, close
 *   2. remove defeatist.bak
 *   3. rename defeatist.dat -> defeatist.bak
 *   4. rename defeatist.tmp -> defeatist.dat
 *
 * Power lost between 2 and 4 leaves either .dat or .bak intact, and
 * load() tries them in that order. Power lost during 1 leaves both
 * untouched and costs a .tmp that the next write overwrites.
 */
static void write_file(void)
{
    char tmp[128], dat[128], bak[128];
    if (!path_for(tmp, sizeof(tmp), TEMP_NAME) ||
        !path_for(dat, sizeof(dat), SETTINGS_NAME) ||
        !path_for(bak, sizeof(bak), BACKUP_NAME)) {
        return;
    }

    FILE *f = fopen(tmp, "w");
    if (!f) {
        /* A card can be write-protected, full, or gone. None of those
         * are worth retrying every three seconds forever, so the dirty
         * flag is cleared by the caller either way. */
        ESP_LOGW(TAG, "cannot write %s (%s)", tmp, strerror(errno));
        return;
    }

    fprintf(f, "# m5tab5 defeatist music player\n");
    fprintf(f, "volume=%u\n", s_volume);

    /*
     * Separated, and both report errno.
     *
     * "write failed" said nothing about which call failed or why, which
     * is not enough to act on: a full card, a card that went away
     * mid-write, and an exhausted file handle table all produce that one
     * line and want three different fixes.
     *
     * fclose() releases the stream whether or not it succeeds, so it is
     * called even when the flush failed -- returning early on the flush
     * would leak a FILE and, with MAX_OPEN_FILES what it is, a handful
     * of those would stop the player opening tracks.
     */
    const bool flushed = (fflush(f) == 0);
    const int flush_errno = errno;
    const bool closed = (fclose(f) == 0);

    if (!flushed || !closed) {
        ESP_LOGW(TAG, "write failed (%s); leaving the previous file alone",
                 strerror(flushed ? errno : flush_errno));
        /* The half-written temp file is removed rather than left for the
         * next write to overwrite. It is not a valid settings file and
         * the rotation never looks at it, so leaving it is just litter
         * on somebody's card. */
        remove(tmp);
        return;
    }

    remove(bak);
    rename(dat, bak);       /* fails harmlessly on the first ever write */
    if (rename(tmp, dat) != 0) {
        ESP_LOGW(TAG, "could not install %s", dat);
        return;
    }

    ESP_LOGI(TAG, "saved (volume=%u)", s_volume);
}

static void settings_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        if (!s_dirty) continue;
        /* Signed difference: the tick counter wraps every 49 days at
         * 1 kHz and "now > then" is wrong once per wrap on a device
         * people leave running. */
        if ((int32_t)(xTaskGetTickCount() - s_dirty_since) <
            (int32_t)pdMS_TO_TICKS(SETTINGS_SETTLE_MS)) {
            continue;
        }

        /* Cleared before the write, not after. A change arriving while
         * the card is busy sets it again and is picked up on the next
         * pass; clearing afterwards would swallow that change instead. */
        s_dirty = false;

        /* The volume the file is meant to hold could have moved since
         * the flag was set, and that is fine -- write_file() reads the
         * current value, which is the one worth keeping. */
        if (storage_present(storage_of_path(s_root))) write_file();
    }
}

void settings_init(void)
{
    /* The card first, the USB volume second. A drive is the more likely
     * of the two to be carried between machines, and settings following
     * a stick around is surprising in a way settings living on the
     * player's own card is not. */
    for (int v = 0; v < STORAGE_COUNT; v++) {
        if (storage_present((storage_id_t)v)) {
            s_root = storage_mount_path((storage_id_t)v);
            break;
        }
    }

    if (!s_root) {
        ESP_LOGI(TAG, "no volume mounted; settings not persisted this boot");
        return;
    }

    if (load_file(SETTINGS_NAME)) {
        ESP_LOGI(TAG, "loaded %s/%s (volume=%u)", s_root, SETTINGS_NAME, s_volume);
    } else if (load_file(BACKUP_NAME)) {
        /* The rotation above means this is the previous good copy, not a
         * guess. Reaching it means the last write was interrupted. */
        ESP_LOGW(TAG, "using %s after a bad or missing %s", BACKUP_NAME, SETTINGS_NAME);
    } else {
        ESP_LOGI(TAG, "no settings file; using defaults");
    }

    /* Priority 2: below the UI and well below the audio writer. Nothing
     * waits on this and it holds the card's VFS lock while it writes. */
    if (xTaskCreate(settings_task, "settings", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no writer task; settings will not be saved");
    }
}
