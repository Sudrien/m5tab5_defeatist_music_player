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
#include "storage_io.h"

#include "cJSON.h"

static const char *TAG = "tab5_settings";

/*
 * Dotfiles, and hidden on FAT once written.
 *
 * The old names had no dot and sat in the root of whatever volume held
 * them, which is the first thing anyone sees when they plug the card
 * into a computer to add music. storage_mark_hidden() covers the half
 * of "hidden" that a dot does not.
 */
#define SETTINGS_NAME   ".defeatist.dat"
#define BACKUP_NAME     ".defeatist.bak"
#define TEMP_NAME       ".defeatist.tmp"

/* What the same files were called before they gained their dot. Read,
 * never written: a card written by an older build is loaded once and
 * the next save lands on the new names. */
#define LEGACY_NAME     "defeatist.dat"
#define LEGACY_BACKUP   "defeatist.bak"

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
#define SETTINGS_MAX_LINE       (256)

/*
 * The file is append-only, and this is where it stops growing.
 *
 * Every save used to be a write plus a remove plus two renames -- four
 * FAT operations to store one integer, and on a USB drive that was
 * measured at nearly a second with the decoder waiting behind it:
 *
 *   W (47654) tab5_mp3: decoder_read blocked 920 ms
 *   I (48320) tab5_settings: saved /usb/.defeatist.dat (volume=20)
 *
 * An append is one open, one write at a known offset, one close. It
 * touches the last sector of the file and the directory entry's size;
 * it does not walk or rewrite the FAT chain, and it does not create or
 * destroy a directory entry. That is both faster and kinder to the
 * flash, which is the same reason in two words.
 *
 * 64 KB because it is a whole number of clusters at every allocation
 * size this program mounts with, so a file that reaches the cap has
 * been rewritten in place the whole time rather than chasing new
 * clusters. A record is on the order of tens of bytes, so the cap is
 * thousands of saves away -- the compaction below is a rare event by
 * construction, which is what makes it acceptable for it to be the
 * expensive shape.
 */
#define SETTINGS_MAX_FILE_BYTES (64 * 1024)

static uint8_t    s_volume = 50;
static volatile bool s_dirty;
static TickType_t s_dirty_since;

/*
 * Where the file lives -- the volume the music is coming from, which is
 * a question that has no answer at init and changes when someone taps
 * the other tab.
 *
 * NULL means nowhere to write yet. That is not an error and not the end
 * of the matter: the values live in memory regardless, and the first
 * volume that plays a track gets them written to it.
 */
static const char *s_root;

/* Whether a file has ever been read. Guards a second load: adopting a
 * volume mid-session takes the settings that are already in effect to
 * the new volume rather than replacing them with whatever that volume
 * remembers, because a volume change is a change of where the music is,
 * not a request to restore someone else's preferences. */
static bool s_loaded;

/* The file's size as last known, so a save can tell an append from a
 * compaction without stat()ing first. */
static size_t s_bytes;

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
 * One record.
 *
 * JSON Lines: one object per line, appended, last one wins. Unknown
 * keys are skipped rather than treated as errors, which is what makes a
 * card survive moving between builds -- an older firmware reading a
 * newer file keeps the settings it understands instead of throwing the
 * file away because of a key it has never heard of.
 *
 * Lines that are not JSON are tried as the old `key=value` form, so a
 * card written by an earlier build still loads. Nothing writes that
 * form any more; the first append after a load leaves a JSON record
 * below it, and the compaction eventually removes the rest.
 */
static bool parse_line(char *line)
{
    if (line[0] == '{') {
        cJSON *root = cJSON_Parse(line);
        if (!root) return false;

        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "volume");
        bool any = false;
        if (cJSON_IsNumber(v)) {
            int n = v->valueint;
            if (n < 0)   n = 0;
            if (n > 100) n = 100;
            s_volume = (uint8_t)n;
            any = true;
        }
        cJSON_Delete(root);
        return any;
    }

    char *eq = strchr(line, '=');
    if (!eq) return false;
    *eq = '\0';
    const char *key = line;
    const char *val = eq + 1;

    if (strcmp(key, "volume") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        s_volume = (uint8_t)v;
        return true;
    }
    ESP_LOGD(TAG, "unknown key '%s'", key);
    return false;
}

/*
 * Every record in the file, in order, last one winning.
 *
 * Applying all of them rather than seeking to the end and reading one:
 * a line can be truncated (power lost mid-append), and a line can be a
 * legacy key=value that carries a key a later JSON record does not.
 * Replaying the file is how both of those come out right, and at tens
 * of bytes a record it is a single sequential read of at most 64 KB.
 *
 * s_bytes is left holding the file's size, which is what decides
 * whether the next save appends or compacts.
 */
static bool load_file(const char *name)
{
    char path[128];
    if (!path_for(path, sizeof(path), name)) return false;

    FILE *f = storage_io_open(path, "r");
    if (!f) return false;

    char line[SETTINGS_MAX_LINE];
    size_t total = 0;
    int records = 0;
    bool any = false;

    while (fgets(line, sizeof(line), f)) {
        total += strlen(line);
        if (total > SETTINGS_MAX_FILE_BYTES) {
            /* Past the cap the file cannot legitimately reach, so
             * whatever this is, it is not ours. The values taken so far
             * are as trustworthy as they are going to get. */
            ESP_LOGW(TAG, "%s is longer than the cap; ignoring the rest", name);
            break;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;

        records++;
        /*
         * A line that will not parse is not fatal and not even
         * unexpected: the last one is the one a power cut can truncate,
         * and every earlier line has already been superseded anyway.
         */
        if (parse_line(line)) any = true;
    }
    storage_io_close(f);

    s_bytes = total;
    if (any) ESP_LOGD(TAG, "%s: %d records, %u bytes", name, records,
                      (unsigned)total);
    return any;
}

/*
 * One record, as a line.
 *
 * Shared by the append and the compaction so there is one definition of
 * what a record contains.
 */
static int record_line(char *out, size_t out_len)
{
    return snprintf(out, out_len, "{\"volume\":%u}\n", s_volume);
}

/*
 * Start the file again with a single current record.
 *
 * The expensive shape, kept for the one case that needs it: the file
 * has reached the cap and cannot be appended to any more. This is the
 * only path that still writes a temp file and renames over the old one,
 * and it is the only path where that protection is worth its cost --
 * everywhere else a lost write costs the newest of several thousand
 * records, and here it would cost the file.
 *
 *   1. write .defeatist.tmp in full, flush, close
 *   2. remove .defeatist.bak
 *   3. rename .defeatist.dat -> .defeatist.bak
 *   4. rename .defeatist.tmp -> .defeatist.dat
 *
 * Power lost between 2 and 4 leaves either .dat or .bak intact, and the
 * load tries them in that order. Power lost during 1 leaves both
 * untouched and costs a .tmp that the next compaction overwrites.
 *
 * The lease is taken per operation rather than once around the set, so
 * a playback read can get in between them. Any prefix of the sequence
 * leaves a loadable file, which is what makes that safe.
 */
static bool compact_file(void)
{
    char tmp[128], dat[128], bak[128];
    if (!path_for(tmp, sizeof(tmp), TEMP_NAME) ||
        !path_for(dat, sizeof(dat), SETTINGS_NAME) ||
        !path_for(bak, sizeof(bak), BACKUP_NAME)) {
        return false;
    }

    char line[SETTINGS_MAX_LINE];
    const int len = record_line(line, sizeof(line));
    if (len <= 0 || len >= (int)sizeof(line)) return false;

    FILE *f = storage_io_open(tmp, "w");
    if (!f) {
        ESP_LOGW(TAG, "cannot write %s (%s)", tmp, strerror(errno));
        return false;
    }

    const bool wrote = (fwrite(line, 1, (size_t)len, f) == (size_t)len);
    const bool flushed = (fflush(f) == 0);
    const int  flush_errno = errno;
    const bool closed = (storage_io_close(f) == 0);

    if (!wrote || !flushed || !closed) {
        ESP_LOGW(TAG, "compaction failed (%s); leaving the file alone",
                 strerror(flushed ? errno : flush_errno));
        remove(tmp);
        return false;
    }

    storage_io_acquire(STORAGE_IO_BACKGROUND);
    remove(bak);
    storage_io_release();

    storage_io_acquire(STORAGE_IO_BACKGROUND);
    rename(dat, bak);       /* fails harmlessly if there is no .dat yet */
    storage_io_release();

    storage_io_acquire(STORAGE_IO_BACKGROUND);
    const int installed = rename(tmp, dat);
    storage_io_release();

    if (installed != 0) {
        ESP_LOGW(TAG, "could not install %s", dat);
        return false;
    }

    storage_mark_hidden(dat);
    s_bytes = (size_t)len;
    ESP_LOGI(TAG, "compacted %s (volume=%u)", dat, s_volume);
    return true;
}

/*
 * Append one record, or compact when the file is full.
 *
 * The append is the whole point of the format. It is one open, one
 * write and one close, it extends the last cluster rather than
 * allocating or freeing any, and it never touches a second file -- so
 * the decoder is not waiting behind a rename while somebody drags the
 * volume slider.
 *
 * A failed append leaves the previous records exactly as they were, and
 * the previous record is one volume step away from the current one.
 * That is the whole of the crash safety this needs, and it is why the
 * rotation is not here.
 */
static void write_file(void)
{
    char dat[128];
    if (!path_for(dat, sizeof(dat), SETTINGS_NAME)) return;

    char line[SETTINGS_MAX_LINE];
    const int len = record_line(line, sizeof(line));
    if (len <= 0 || len >= (int)sizeof(line)) return;

    if (s_bytes + (size_t)len > SETTINGS_MAX_FILE_BYTES) {
        compact_file();
        return;
    }

    FILE *f = storage_io_open(dat, "a");
    if (!f) {
        /* Write-protected, full, or gone. None of those are worth
         * retrying every three seconds forever, so the dirty flag is
         * cleared by the caller either way. */
        ESP_LOGW(TAG, "cannot append to %s (%s)", dat, strerror(errno));
        return;
    }

    const bool wrote = (fwrite(line, 1, (size_t)len, f) == (size_t)len);
    const bool flushed = (fflush(f) == 0);
    const int  flush_errno = errno;
    const bool closed = (storage_io_close(f) == 0);

    if (!wrote || !flushed || !closed) {
        /*
         * The file may now end in a partial line. That is expected and
         * handled: the load skips a line it cannot parse, and every
         * line before it is a record that was good when it was written.
         */
        ESP_LOGW(TAG, "append failed (%s); the previous record stands",
                 strerror(flushed ? errno : flush_errno));
        return;
    }

    /* First write of the session on a volume that had no file: the dot
     * hides it here, this hides it on FAT. Cheap enough to repeat. */
    if (s_bytes == 0) storage_mark_hidden(dat);

    s_bytes += (size_t)len;
    ESP_LOGI(TAG, "saved %s (volume=%u, %u bytes)", dat, s_volume,
             (unsigned)s_bytes);
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
        /*
         * Nowhere to write is not a reason to drop the value. The flag
         * is set again so that the first volume to turn up gets it --
         * this is the whole of "kept in memory when there is no
         * storage".
         */
        if (!s_root) {
            s_dirty = true;
            continue;
        }

        if (storage_present(storage_of_path(s_root))) {
            write_file();
        } else {
            /* The volume went away between the change and the write.
             * Hold the value for the next one rather than losing it. */
            s_root = NULL;
            s_dirty = true;
        }
    }
}

void settings_init(void)
{
    /*
     * No search, and no early return.
     *
     * This used to pick a volume here and give up for the whole boot if
     * it found none. That was wrong twice over: a USB drive mounts
     * asynchronously and is routinely a second or two behind this call,
     * so a drive-only session lost its settings entirely; and the
     * volume it did find was not necessarily the one the music came
     * from.
     *
     * The root is now settled by settings_note_path() when a track
     * starts. Until then the defaults are in memory and are as usable
     * as anything loaded from a file.
     */
    if (xTaskCreate(settings_task, "settings", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no writer task; settings will not be saved");
    }
}
