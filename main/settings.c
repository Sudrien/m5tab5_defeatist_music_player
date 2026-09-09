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

#include <strings.h>

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
/*
 * Raised from 256 when the network keys arrived. A record is now the
 * four audio keys, two network keys, and an escaped absolute path; 256
 * left the path around 180 bytes, which real libraries exceed. A line
 * over the cap is treated as corrupt, and the failure that produces --
 * a resume track silently forgotten on deep folder trees -- is
 * invisible enough to be worth the slack.
 */
#define SETTINGS_MAX_LINE       (384)

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
/* On unless the file says otherwise. A player that quietly stopped
 * levelling because a key was missing would be indistinguishable from
 * one whose measurements had gone. */
static bool       s_rg_enabled = true;
/* Off, so a player that has never been told otherwise plays albums the
 * way they were cut. See settings_crossfade_sec(). */
static uint8_t    s_crossfade_sec;
static bool       s_crossfade_album;

/* Off. The radio does not come up because a firmware update happened.
 * See settings_wifi_enabled(). */
static bool       s_wifi_enabled;
/* On, but gated by the above on the way out. */
static bool       s_ntp_enabled = true;

/* The track that was last playing, absolute path, empty when nothing
 * has played yet on this file's volume. */
/*
 * THE RESUME TRACK IS PER-VOLUME. EVERYTHING ELSE IS NOT.
 *
 * Volume, ReplayGain and the two crossfade settings describe the
 * PLAYER: the person set them once and means them wherever the music is
 * coming from. The remembered track describes the VOLUME: "carry on
 * where I left off" means nothing when applied to a card that has never
 * held that file.
 *
 * They used to be one record on one volume, which made both wrong. A
 * boot from the SD card followed by playback from USB replaced the USB
 * file with the SD card's state -- losing the crossfade settings the
 * person had set while listening to USB -- and there was only ever one
 * remembered track, so returning to the other volume resumed something
 * that was not on it.
 *
 * So: the settings are mirrored to every volume present, and each
 * volume's file carries its own track. A file is still one record and
 * still readable by an older build; the difference is which volumes get
 * written and which field varies between them.
 */
static char       s_track[STORAGE_COUNT][512];

/* Whether this volume's file has been read for its track. Settings are
 * read once, from whichever volume is adopted first; a track is read
 * per volume, the first time that volume is seen. */
static bool       s_track_seen[STORAGE_COUNT];

/* The volume settings_track() answers about -- the last one noted. */
static storage_id_t s_last_id = STORAGE_COUNT;
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


/* Whether a file has ever been read. Guards a second load: adopting a
 * volume mid-session takes the settings that are already in effect to
 * the new volume rather than replacing them with whatever that volume
 * remembers, because a volume change is a change of where the music is,
 * not a request to restore someone else's preferences. */
static bool s_loaded;

/* Each file's size as last known, so a save can tell an append from a
 * compaction without stat()ing first. Per volume, because the two files
 * fill up independently. */
static size_t s_bytes[STORAGE_COUNT];

uint8_t settings_volume(void) { return s_volume; }

bool settings_rg_enabled(void) { return s_rg_enabled; }

uint8_t settings_crossfade_sec(void)  { return s_crossfade_sec; }
bool    settings_crossfade_album(void) { return s_crossfade_album; }

void settings_set_crossfade_sec(uint8_t sec)
{
    if (sec > SETTINGS_CROSSFADE_MAX) sec = SETTINGS_CROSSFADE_MAX;
    if (sec == s_crossfade_sec) return;
    s_crossfade_sec = sec;
    s_dirty = true;
    s_dirty_since = xTaskGetTickCount();
}

void settings_set_crossfade_album(bool on)
{
    if (on == s_crossfade_album) return;
    s_crossfade_album = on;
    s_dirty = true;
    s_dirty_since = xTaskGetTickCount();
}

bool settings_wifi_enabled(void) { return s_wifi_enabled; }

/* The effective answer, and the gate is here rather than in each caller
 * on purpose -- see the header. */
bool settings_ntp_enabled(void) { return s_wifi_enabled && s_ntp_enabled; }
bool settings_ntp_pref(void)    { return s_ntp_enabled; }

void settings_set_wifi_enabled(bool on)
{
    if (on == s_wifi_enabled) return;
    s_wifi_enabled = on;
    s_dirty = true;
    s_dirty_since = xTaskGetTickCount();
}

void settings_set_ntp_enabled(bool on)
{
    if (on == s_ntp_enabled) return;
    s_ntp_enabled = on;
    s_dirty = true;
    s_dirty_since = xTaskGetTickCount();
}

void settings_set_rg_enabled(bool on)
{
    if (on == s_rg_enabled) return;
    s_rg_enabled = on;
    s_dirty = true;
    s_dirty_since = xTaskGetTickCount();
}

/*
 * The remembered track of the volume last noted.
 *
 * There is one per volume now, so this needs to know which; s_last_id is
 * set by settings_note_path(), and restore_last_track() calls that
 * immediately before this. A caller that has noted nothing gets NULL,
 * which is the same answer it used to get before anything was loaded.
 */
const char *settings_track(void)
{
    if (s_last_id >= STORAGE_COUNT) return NULL;
    return s_track[s_last_id][0] ? s_track[s_last_id] : NULL;
}

void settings_set_track(const char *path)
{
    if (!path || !*path) return;
    /* Into its own volume's slot. A track on the SD card is not what
     * the USB drive should resume, and writing one record to both files
     * was how it became that. */
    const storage_id_t id = storage_of_path(path);
    if (id >= STORAGE_COUNT) return;
    if (strcmp(path, s_track[id]) == 0) return;
    snprintf(s_track[id], sizeof(s_track[id]), "%s", path);
    s_dirty = true;
    s_dirty_since = xTaskGetTickCount();
}

void settings_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (percent == s_volume) return;
    s_volume = percent;
    s_dirty = true;
    s_dirty_since = xTaskGetTickCount();
}

/* ------------------------------------------------------------------ */

static bool path_for(storage_id_t id, char *out, size_t out_len,
                     const char *name)
{
    if (id >= STORAGE_COUNT) return false;
    const char *root = storage_mount_path(id);
    if (!root) return false;
    return snprintf(out, out_len, "%s/%s", root, name) < (int)out_len;
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
static bool parse_line(char *line, storage_id_t id, bool take_settings,
                       bool take_track)
{
    if (line[0] == '{') {
        cJSON *root = cJSON_Parse(line);
        if (!root) return false;

        bool any = false;

        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "volume");
        if (take_settings && cJSON_IsNumber(v)) {
            int n = v->valueint;
            if (n < 0)   n = 0;
            if (n > 100) n = 100;
            s_volume = (uint8_t)n;
            any = true;
        }

        /*
         * Taken as written and checked by the caller, not here. A path
         * on a volume that is no longer mounted, or a file since
         * deleted, is a perfectly ordinary thing to find in this file
         * and is not a reason to reject the record it appears in.
         */
        /* cJSON_IsBool covers true and false; anything else -- a number,
         * a string "off" -- is left alone rather than coerced, so a
         * hand-edited line that is not quite right loses that key
         * instead of turning the feature off by accident. */
        const cJSON *rg = cJSON_GetObjectItemCaseSensitive(root, "replaygain");
        if (take_settings && cJSON_IsBool(rg)) {
            s_rg_enabled = cJSON_IsTrue(rg);
            any = true;
        }

        /* Clamped on the way in as well as on the way out. The file is
         * hand-editable and a number from it is not more trustworthy for
         * having been written by this program last time. */
        const cJSON *xf = cJSON_GetObjectItemCaseSensitive(root, "crossfade");
        if (take_settings && cJSON_IsNumber(xf)) {
            int v = xf->valueint;
            if (v < 0) v = 0;
            if (v > SETTINGS_CROSSFADE_MAX) v = SETTINGS_CROSSFADE_MAX;
            s_crossfade_sec = (uint8_t)v;
            any = true;
        }

        const cJSON *xa = cJSON_GetObjectItemCaseSensitive(root, "crossfade_album");
        if (take_settings && cJSON_IsBool(xa)) {
            s_crossfade_album = cJSON_IsTrue(xa);
            any = true;
        }

        const cJSON *wf = cJSON_GetObjectItemCaseSensitive(root, "wifi");
        if (take_settings && cJSON_IsBool(wf)) {
            s_wifi_enabled = cJSON_IsTrue(wf);
            any = true;
        }

        const cJSON *np = cJSON_GetObjectItemCaseSensitive(root, "ntp");
        if (take_settings && cJSON_IsBool(np)) {
            s_ntp_enabled = cJSON_IsTrue(np);
            any = true;
        }

        const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "track");
        if (take_track && cJSON_IsString(t) && t->valuestring &&
            t->valuestring[0] && id < STORAGE_COUNT) {
            snprintf(s_track[id], sizeof(s_track[id]), "%s", t->valuestring);
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

    if (!take_settings) {
        /* The legacy form carries no track, so a track-only read of an
         * old file has nothing to take and must not take the settings
         * either. */
        return false;
    }

    if (strcmp(key, "volume") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        s_volume = (uint8_t)v;
        return true;
    }
    if (strcmp(key, "crossfade") == 0) {
        int v = atoi(val);
        if (v < 0) v = 0;
        if (v > SETTINGS_CROSSFADE_MAX) v = SETTINGS_CROSSFADE_MAX;
        s_crossfade_sec = (uint8_t)v;
        return true;
    }
    if (strcmp(key, "crossfade_album") == 0) {
        s_crossfade_album = !(strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0);
        return true;
    }
    if (strcmp(key, "replaygain") == 0) {
        /* "0" and "false" both off, anything else on -- this form is
         * only ever hand-written or written by a build older than the
         * key, so it is read generously. */
        s_rg_enabled = !(strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0);
        return true;
    }
    /* No legacy form of the network keys exists -- they postdate the
     * JSON records by a long way -- so they are read here only because a
     * hand-written line is the one way they can appear in this form, and
     * refusing it would be arbitrary. */
    if (strcmp(key, "wifi") == 0) {
        s_wifi_enabled = !(strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0);
        return true;
    }
    if (strcmp(key, "ntp") == 0) {
        s_ntp_enabled = !(strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0);
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
static bool load_file(storage_id_t id, const char *name, bool take_settings,
                      bool take_track)
{
    char path[128];
    if (!path_for(id, path, sizeof(path), name)) return false;

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
        if (parse_line(line, id, take_settings, take_track)) any = true;
    }
    storage_io_close(f);

    s_bytes[id] = total;
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
static int record_line(storage_id_t id, char *out, size_t out_len)
{
    /*
     * The track is written through cJSON rather than into the format
     * string: a filename can contain a quote or a backslash, and one of
     * those would produce a line that the next boot cannot parse --
     * which, in an append-only file, is a line that silently costs
     * every record after it nothing and every record before it its
     * place as the newest.
     */
    const char *const rg = s_rg_enabled ? "true" : "false";
    const char *const xa = s_crossfade_album ? "true" : "false";
    const char *const wf = s_wifi_enabled ? "true" : "false";
    /* The stored preference, not the effective one. Writing the gated
     * value would mean turning the radio off and on again silently
     * reset the clock preference to whatever it was gated to. */
    const char *const np = s_ntp_enabled ? "true" : "false";

    /*
     * One format string for the settings half, used by all three exits
     * below, so a key added here cannot reach the file on some paths and
     * not others -- which is what an append-only format punishes: the
     * newest record wins whole, so a record missing a key silently
     * reverts it.
     */
#define SETTINGS_FIELDS_FMT "\"volume\":%u,\"replaygain\":%s," \
                            "\"crossfade\":%u,\"crossfade_album\":%s," \
                            "\"wifi\":%s,\"ntp\":%s"
#define SETTINGS_FIELDS_ARGS s_volume, rg, (unsigned)s_crossfade_sec, xa, \
                             wf, np

    if (id >= STORAGE_COUNT || !s_track[id][0]) {
        return snprintf(out, out_len, "{" SETTINGS_FIELDS_FMT "}\n",
                        SETTINGS_FIELDS_ARGS);
    }

    cJSON *str = cJSON_CreateString(s_track[id]);
    char *esc = str ? cJSON_PrintUnformatted(str) : NULL;
    cJSON_Delete(str);
    if (!esc) return snprintf(out, out_len, "{" SETTINGS_FIELDS_FMT "}\n",
                              SETTINGS_FIELDS_ARGS);

    const int n = snprintf(out, out_len,
                           "{" SETTINGS_FIELDS_FMT ",\"track\":%s}\n",
                           SETTINGS_FIELDS_ARGS, esc);
#undef SETTINGS_FIELDS_FMT
#undef SETTINGS_FIELDS_ARGS
    cJSON_free(esc);
    return n;
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
static bool compact_file(storage_id_t id)
{
    char tmp[128], dat[128], bak[128];
    if (!path_for(id, tmp, sizeof(tmp), TEMP_NAME) ||
        !path_for(id, dat, sizeof(dat), SETTINGS_NAME) ||
        !path_for(id, bak, sizeof(bak), BACKUP_NAME)) {
        return false;
    }

    char line[SETTINGS_MAX_LINE];
    const int len = record_line(id, line, sizeof(line));
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
    s_bytes[id] = (size_t)len;
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
static void write_file(storage_id_t id)
{
    char dat[128];
    if (!path_for(id, dat, sizeof(dat), SETTINGS_NAME)) return;

    char line[SETTINGS_MAX_LINE];
    const int len = record_line(id, line, sizeof(line));
    if (len <= 0 || len >= (int)sizeof(line)) return;

    if (s_bytes[id] + (size_t)len > SETTINGS_MAX_FILE_BYTES) {
        compact_file(id);
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
    if (s_bytes[id] == 0) storage_mark_hidden(dat);

    s_bytes[id] += (size_t)len;
    ESP_LOGI(TAG, "saved %s (volume=%u, %u bytes)", dat, s_volume,
             (unsigned)s_bytes[id]);
}

/*
 * Read one volume's file.
 *
 * `take_settings` is the difference between the boot load and every
 * later one. The first volume adopted supplies the settings; after that
 * the settings in force are the ones the person has been listening at,
 * and replacing them because a second volume turned up would be the
 * player changing its own volume behind them.
 *
 * The track is taken every time, per volume, because it is the one
 * field that belongs to the file rather than to the player.
 */
static bool load_volume(storage_id_t id, bool take_settings, bool take_track)
{
    const char *root = storage_mount_path(id);
    if (!root) return false;

    s_bytes[id] = 0;

    if (load_file(id, SETTINGS_NAME, take_settings, take_track)) {
        ESP_LOGI(TAG, "loaded %s/%s (volume=%u)", root, SETTINGS_NAME, s_volume);
        return true;
    }
    if (load_file(id, BACKUP_NAME, take_settings, take_track)) {
        /* Only compaction writes that file, so reaching it means a
         * compaction was interrupted. It is a real record, not a
         * guess. The size that came back is the backup's; the next save
         * appends to SETTINGS_NAME, which is absent or unusable. */
        ESP_LOGW(TAG, "using %s after a bad or missing %s",
                 BACKUP_NAME, SETTINGS_NAME);
        s_bytes[id] = 0;
        return true;
    }
    if (load_file(id, LEGACY_NAME, take_settings, take_track) ||
        load_file(id, LEGACY_BACKUP, take_settings, take_track)) {
        ESP_LOGI(TAG, "loaded %s/%s (volume=%u); saving as %s from now on",
                 root, LEGACY_NAME, s_volume, SETTINGS_NAME);
        s_bytes[id] = 0;
        s_dirty = true;
        s_dirty_since = xTaskGetTickCount();
        return true;
    }

    ESP_LOGI(TAG, "no settings file on %s", root);
    return false;
}

/*
 * Make sure this volume's file has been read for what is still wanted
 * from it.
 *
 * Two flags, not one, and the reason is a bug the host test caught
 * before the board could. The writer task has to read a volume before
 * appending to it -- it needs the byte count, and it needs the track
 * that file is already holding -- but the writer task must NEVER adopt
 * settings. Someone who changes the volume with no card in, then
 * inserts one, would have had their change overwritten by whatever that
 * card remembered, at the moment of the save that was supposed to
 * record it.
 *
 * So `may_adopt` is true only from settings_note_path(), which is a
 * track starting: a deliberate act by the person, and the one moment
 * where taking a volume's stored settings is what they asked for.
 *
 * Each half is read at most once. The track is read the first time the
 * volume is seen -- reading it again would undo settings_set_track(),
 * which is newer than anything on the card. The settings are read from
 * whichever volume adopts first and never again.
 */
static void ensure_volume_loaded(storage_id_t id, bool may_adopt)
{
    if (id >= STORAGE_COUNT) return;

    const bool want_settings = may_adopt && !s_loaded;
    const bool want_track    = !s_track_seen[id];
    if (!want_settings && !want_track) return;

    if (want_settings) s_loaded = true;
    s_track_seen[id] = true;
    load_volume(id, want_settings, want_track);
}

/*
 * Adopt a volume, which is the one a track is playing from.
 *
 * Called on every track start, so the common case is that this volume
 * has already been seen and this only records which one to answer
 * settings_track() about.
 *
 * Returns true when this call is what put the settings in force, so the
 * caller knows to push them at whatever acts on them. See settings.h.
 */
bool settings_note_path(const char *path)
{
    const storage_id_t id = storage_of_path(path);
    if (id == STORAGE_COUNT || !storage_present(id)) return false;

    const bool first = !s_loaded;
    ensure_volume_loaded(id, true);
    s_last_id = id;

    /* The other volume, if it is here, wants a copy of the settings --
     * they describe the player and not the card. Its own track is left
     * alone, which is the whole reason the two are stored apart. */
    s_dirty = true;
    s_dirty_since = xTaskGetTickCount();

    return first;
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

        /*
         * EVERY VOLUME PRESENT, NOT THE ONE PLAYING.
         *
         * The settings describe the player, so both cards should come
         * back with them next boot regardless of which one is in the
         * machine. Each file gets its own remembered track, which is
         * why this is a loop over volumes and not one write copied
         * twice.
         *
         * A volume that has not been read yet is read first: appending
         * a record to a file whose size is unknown would restart the
         * byte count from zero and defer the compaction indefinitely,
         * and reading it is also the only way to learn the track that
         * file is already holding.
         */
        int written = 0;
        for (int v = 0; v < STORAGE_COUNT; v++) {
            const storage_id_t id = (storage_id_t)v;
            if (!storage_present(id)) continue;
            /* Track and byte count only. Never the settings -- see
             * ensure_volume_loaded(): this runs on a save, and a save
             * must not undo the change it was scheduled to record. */
            ensure_volume_loaded(id, false);
            write_file(id);
            written++;
        }

        if (written == 0) {
            /*
             * Nowhere to write is not a reason to drop the value. The
             * flag is set again so that the first volume to turn up
             * gets it -- this is the whole of "kept in memory when
             * there is no storage".
             */
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
