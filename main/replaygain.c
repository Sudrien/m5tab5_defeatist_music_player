/*
 * replaygain.c -- see replaygain.h for the format and why it is JSONL.
 *
 * SPDX-License-Identifier: MIT
 */
#include "replaygain.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"

#include "storage.h"
#include "storage_io.h"

static const char *TAG = "tab5_rg";

/* A line with a full 720-column waveform is about a kilobyte of base64
 * plus the rest of the object. Two of them plus slack. */
#define RG_LINE_MAX     (2048)

/* ------------------------------------------------------------------ */
/* base64, because raw envelope bytes are not JSON-safe.               */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t out_len)
{
    const size_t need = ((n + 2) / 3) * 4 + 1;
    if (out_len < need) return 0;

    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t a = in[i];
        const uint32_t b = (i + 1 < n) ? in[i + 1] : 0;
        const uint32_t c = (i + 2 < n) ? in[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;

        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, uint8_t *out, size_t out_len)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (const char *p = in; *p; p++) {
        if (*p == '=' || *p == '\r' || *p == '\n') break;
        const int v = b64_val(*p);
        if (v < 0) continue;               /* skip anything unexpected */
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < out_len) out[o++] = (uint8_t)((acc >> bits) & 0xFF);
            else return o;
        }
    }
    return o;
}

/* ------------------------------------------------------------------ */

/*
 * "." + filename + ".rgcache", next to the track. A dotfile, so
 * storage_is_hidden() keeps it out of the chooser and the playlist scan
 * for the same reason it keeps out macOS's "._Name.mp3" forks.
 *
 * The arithmetic is written out rather than snprintf'd for the reason
 * storage_join_path() gives: a truncated path is a path to a different
 * file, and opening that silently is worse than refusing.
 */
static bool sidecar_path(const char *track_path, char *out, size_t out_len)
{
    const char *slash = strrchr(track_path, '/');
    const char *name = slash ? slash + 1 : track_path;
    const size_t dir_len = (size_t)(name - track_path);
    static const char suffix[] = ".rgcache";

    const size_t need = dir_len + 1 + strlen(name) + strlen(suffix) + 1;
    if (need > out_len) return false;

    memcpy(out, track_path, dir_len);
    out[dir_len] = '.';
    strcpy(out + dir_len + 1, name);
    strcat(out, suffix);
    return true;
}

/* Parse one JSON object into *out. False when the line is not usable,
 * which includes a torn line and a line about a different revision of
 * the track. */
static bool parse_line(const char *line, uint32_t filesize, int64_t mtime,
                       replaygain_t *out)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) return false;

    bool ok = false;

    const cJSON *fv = cJSON_GetObjectItemCaseSensitive(root, "format_version");
    const cJSON *fs = cJSON_GetObjectItemCaseSensitive(root, "filesize");
    const cJSON *mt = cJSON_GetObjectItemCaseSensitive(root, "mtime");

    if (!cJSON_IsNumber(fv) || fv->valueint != REPLAYGAIN_FORMAT_VERSION) goto done;
    if (!cJSON_IsNumber(fs) || !cJSON_IsNumber(mt)) goto done;

    /* Staleness. Checked per line rather than once for the file,
     * because every line carries the key and the last good line is the
     * one being trusted. */
    if ((uint32_t)fs->valuedouble != filesize) goto done;
    if ((int64_t)mt->valuedouble != mtime) goto done;

    memset(out, 0, sizeof(*out));
    out->filesize = filesize;
    out->mtime = mtime;

    const cJSON *wf = cJSON_GetObjectItemCaseSensitive(root, "waveform");
    if (cJSON_IsObject(wf)) {
        const cJSON *lv = cJSON_GetObjectItemCaseSensitive(wf, "level");
        const cJSON *cols = cJSON_GetObjectItemCaseSensitive(wf, "columns");
        const cJSON *sc = cJSON_GetObjectItemCaseSensitive(wf, "sec");

        if (cJSON_IsString(lv) && cJSON_IsNumber(cols)) {
            int n = cols->valueint;
            if (n > REPLAYGAIN_COLUMNS) n = REPLAYGAIN_COLUMNS;
            if (n > 0) {
                const size_t got = b64_decode(lv->valuestring,
                                              out->waveform.level,
                                              sizeof(out->waveform.level));
                /* A short decode means a truncated or damaged line.
                 * Take what decoded rather than the declared width, or
                 * the tail of the envelope is whatever was in the
                 * struct. */
                if (got < (size_t)n) n = (int)got;
            }
            if (n > 0) {
                out->waveform.present = true;
                out->waveform.columns = n;
                out->waveform.sec = cJSON_IsNumber(sc)
                                    ? (uint32_t)sc->valuedouble : 0;
            }
        }
    }

    const cJSON *ld = cJSON_GetObjectItemCaseSensitive(root, "loudness");
    if (cJSON_IsObject(ld)) {
        const cJSON *ver = cJSON_GetObjectItemCaseSensitive(ld, "version");
        const cJSON *lu = cJSON_GetObjectItemCaseSensitive(ld, "integrated_lufs");
        const cJSON *pk = cJSON_GetObjectItemCaseSensitive(ld, "sample_peak_dbfs");
        const cJSON *bl = cJSON_GetObjectItemCaseSensitive(ld, "blocks");

        /* A different LOUDNESS_VERSION is not corruption -- it is a
         * number measured by different rules. Dropped so the next full
         * play recomputes it, while the waveform above survives. */
        if (cJSON_IsNumber(ver) && ver->valueint == LOUDNESS_VERSION &&
            cJSON_IsNumber(lu)) {
            out->loudness.present = true;
            out->loudness.integrated_lufs = (float)lu->valuedouble;
            out->loudness.sample_peak_dbfs = cJSON_IsNumber(pk)
                                             ? (float)pk->valuedouble : 0.0f;
            out->loudness.blocks = cJSON_IsNumber(bl)
                                   ? (uint32_t)bl->valuedouble : 0;
        }
    }

    ok = out->waveform.present || out->loudness.present;

done:
    cJSON_Delete(root);
    return ok;
}

bool replaygain_load(const char *path, replaygain_t *out)
{
    if (!path || !out) return false;

    struct stat st;
    if (stat(path, &st) != 0) return false;

    char cache_path[600];
    if (!sidecar_path(path, cache_path, sizeof(cache_path))) {
        ESP_LOGW(TAG, "sidecar path too long for %s", path);
        return false;
    }

    FILE *f = storage_io_open(cache_path, "rb");
    if (!f) return false;              /* ordinary: nothing cached yet */

    /* Small file by construction -- compaction keeps it so. Read it
     * whole rather than seeking around for line boundaries. */
    char *buf = malloc(REPLAYGAIN_COMPACT_BYTES + 1);
    if (!buf) {
        storage_io_close(f);
        return false;
    }
    const size_t got = fread(buf, 1, REPLAYGAIN_COMPACT_BYTES, f);
    storage_io_close(f);
    buf[got] = '\0';

    /*
     * Last usable line wins, so scan backwards. A torn final line --
     * power lost mid-append -- simply fails to parse and the line
     * before it is used, which is the whole reason every line repeats
     * the staleness key.
     */
    bool found = false;
    char *end = buf + got;
    while (end > buf && !found) {
        char *start = end;
        while (start > buf && start[-1] != '\n') start--;

        /* Trim the newline the terminator sits on. */
        char saved = *end;
        *end = '\0';
        if (end > start) found = parse_line(start, (uint32_t)st.st_size,
                                            (int64_t)st.st_mtime, out);
        *end = saved;

        end = (start > buf) ? start - 1 : buf;
        if (start == buf) break;
    }

    free(buf);

    if (found) {
        ESP_LOGI(TAG, "sidecar: %s%s", cache_path,
                 out->loudness.present ? " (waveform+loudness)"
                                       : " (waveform only)");
    }
    return found;
}

/* Same edge-based max-per-bucket rule framewalk.c's resample() uses.
 * Max rather than mean: a mean over several source columns turns a
 * transient into a bump, and the transient is what makes one track's
 * shape recognisable from another's. */
static void resample_levels(const uint8_t *src, int n, uint8_t *dst, int cols)
{
    for (int c = 0; c < cols; c++) {
        const uint32_t a = (uint32_t)(((uint64_t)c * (uint32_t)n) / (uint32_t)cols);
        uint32_t b = (uint32_t)(((uint64_t)(c + 1) * (uint32_t)n) / (uint32_t)cols);
        if (b <= a) b = a + 1;
        if (b > (uint32_t)n) b = (uint32_t)n;

        uint8_t m = 0;
        for (uint32_t i = a; i < b; i++) if (src[i] > m) m = src[i];
        dst[c] = m;
    }
}

/*
 * Is the file at `cache_path` JSONL written by this code at all?
 *
 * Deliberately NOT "does replaygain_load() succeed". That answers a
 * different question: load() also fails on a sidecar whose filesize or
 * mtime no longer match the track, and a stale-but-well-formed sidecar
 * must still be appended to like any other -- the new line supersedes
 * the old one and the reader already ignores what it supersedes.
 * Rewriting on staleness would be harmless but would silently discard
 * the previous record, which is the reader's job to do, not the
 * writer's.
 *
 * What this catches is content that is not this format at all. In
 * practice that is 0200's packed binary record: a firmware that had
 * already written sidecars gets reflashed, and the first 0201 write
 * appends valid JSON directly onto a 752-byte binary blob. Nothing
 * breaks -- replaygain_load() scans backwards from the end and never
 * reads far enough to reach the blob -- but the file carries the dead
 * bytes for ever, and any stricter reader, or anything that is not this
 * exact backwards scan, would choke on it.
 *
 * Cheap by construction: reads only the first byte. Every line this
 * code writes is a JSON object, so a sidecar in this format always
 * starts with '{'. Anything else -- 0200's "1CGR" magic, a truncated
 * write, a file some other tool left -- is not ours and gets dumped.
 *
 * An empty or missing file is "ours": there is nothing to dump, and the
 * open below creates it.
 */
static bool sidecar_is_jsonl(const char *cache_path)
{
    FILE *f = storage_io_open(cache_path, "rb");
    if (!f) return true;                 /* nothing there yet */

    const int c = fgetc(f);
    storage_io_close(f);

    if (c == EOF) return true;           /* empty; nothing to dump */
    return c == '{';
}

/*
 * Serialise `rg` as one line and add it to the sidecar -- appended
 * normally, or written over the whole file when it has grown past the
 * cap or when what is there is not this format (see
 * sidecar_is_jsonl()).
 *
 * Read-modify-write is the caller's job: it loads, overlays its own
 * section and hands the whole thing here. That keeps the merge in one
 * obvious place per section rather than inside a writer that would have
 * to know which fields it was allowed to touch.
 */
static esp_err_t append_record(const char *path, const replaygain_t *rg,
                               uint32_t filesize, int64_t mtime)
{
    char cache_path[600];
    if (!sidecar_path(path, cache_path, sizeof(cache_path))) {
        ESP_LOGW(TAG, "sidecar path too long for %s; not caching", path);
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON_AddNumberToObject(root, "format_version", REPLAYGAIN_FORMAT_VERSION);
    cJSON_AddNumberToObject(root, "filesize", (double)filesize);
    cJSON_AddNumberToObject(root, "mtime", (double)mtime);

    if (rg->waveform.present && rg->waveform.columns > 0) {
        cJSON *wf = cJSON_AddObjectToObject(root, "waveform");
        if (wf) {
            cJSON_AddNumberToObject(wf, "sec", (double)rg->waveform.sec);
            cJSON_AddNumberToObject(wf, "columns", rg->waveform.columns);

            char *b64 = malloc(((size_t)rg->waveform.columns + 2) / 3 * 4 + 1);
            if (b64) {
                if (b64_encode(rg->waveform.level,
                               (size_t)rg->waveform.columns, b64,
                               ((size_t)rg->waveform.columns + 2) / 3 * 4 + 1)) {
                    cJSON_AddStringToObject(wf, "level", b64);
                }
                free(b64);
            }
        }
    }

    if (rg->loudness.present) {
        cJSON *ld = cJSON_AddObjectToObject(root, "loudness");
        if (ld) {
            cJSON_AddNumberToObject(ld, "version", LOUDNESS_VERSION);
            cJSON_AddNumberToObject(ld, "integrated_lufs",
                                    (double)rg->loudness.integrated_lufs);
            cJSON_AddNumberToObject(ld, "sample_peak_dbfs",
                                    (double)rg->loudness.sample_peak_dbfs);
            cJSON_AddNumberToObject(ld, "blocks", (double)rg->loudness.blocks);
        }
    }

    char *line = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!line) return ESP_ERR_NO_MEM;

    /*
     * Rewrite rather than append in two cases: the file has grown past
     * the cap, or what is already there is not this format at all (see
     * sidecar_is_jsonl()). Both open "wb", which truncates -- the
     * record being written already carries everything the earlier lines
     * said, because the caller merged it before handing it here.
     */
    struct stat cst;
    const bool too_big = (stat(cache_path, &cst) == 0 &&
                          cst.st_size > REPLAYGAIN_COMPACT_BYTES);
    const bool foreign = !sidecar_is_jsonl(cache_path);
    const bool rewrite = too_big || foreign;


    FILE *f = storage_io_open(cache_path, rewrite ? "wb" : "ab");
    if (!f) {
        ESP_LOGW(TAG, "could not open %s for writing", cache_path);
        cJSON_free(line);
        return ESP_FAIL;
    }

    const size_t len = strlen(line);
    const bool wrote = (fwrite(line, 1, len, f) == len) &&
                       (fputc('\n', f) != EOF);
    const int cerr = storage_io_close(f);
    cJSON_free(line);

    if (!wrote || cerr != 0) {
        ESP_LOGW(TAG, "write failed for %s", cache_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "sidecar %s: %s",
             foreign ? "rewritten (was not JSONL)"
             : too_big ? "compacted" : "updated",
             cache_path);
    return ESP_OK;
}

/* Load current state, or start an empty one, so a save of one section
 * carries the other section forward instead of dropping it. */
static void load_or_empty(const char *path, uint32_t filesize, int64_t mtime,
                          replaygain_t *rg)
{
    if (!replaygain_load(path, rg)) {
        memset(rg, 0, sizeof(*rg));
    }
    rg->filesize = filesize;
    rg->mtime = mtime;
}

esp_err_t replaygain_save_waveform(const char *path, const uint8_t *level,
                                   int columns, uint32_t sec)
{
    if (!path || !level || columns <= 0) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "stat failed for %s; not caching", path);
        return ESP_FAIL;
    }

    replaygain_t rg;
    load_or_empty(path, (uint32_t)st.st_size, (int64_t)st.st_mtime, &rg);

    rg.waveform.present = true;
    rg.waveform.sec = sec;

    if (columns > REPLAYGAIN_COLUMNS) {
        rg.waveform.columns = REPLAYGAIN_COLUMNS;
        resample_levels(level, columns, rg.waveform.level, REPLAYGAIN_COLUMNS);
    } else {
        /*
         * Stored at its own width rather than stretched to the panel.
         * The UI knows how wide the bar is and this does not; padding
         * here would bake in silence the track does not contain.
         */
        rg.waveform.columns = columns;
        memcpy(rg.waveform.level, level, (size_t)columns);
    }

    return append_record(path, &rg, (uint32_t)st.st_size,
                         (int64_t)st.st_mtime);
}

esp_err_t replaygain_save_loudness(const char *path, float integrated_lufs,
                                   float sample_peak_dbfs, uint32_t blocks)
{
    if (!path) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "stat failed for %s; not caching loudness", path);
        return ESP_FAIL;
    }

    replaygain_t rg;
    load_or_empty(path, (uint32_t)st.st_size, (int64_t)st.st_mtime, &rg);

    rg.loudness.present = true;
    rg.loudness.integrated_lufs = integrated_lufs;
    rg.loudness.sample_peak_dbfs = sample_peak_dbfs;
    rg.loudness.blocks = blocks;

    const esp_err_t err = append_record(path, &rg, (uint32_t)st.st_size,
                                        (int64_t)st.st_mtime);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "loudness: %.2f LUFS, peak %.2f dBFS, %" PRIu32
                      " gated blocks",
                 (double)integrated_lufs, (double)sample_peak_dbfs, blocks);
    }
    return err;
}

float replaygain_gain_db(const replaygain_loudness_t *l)
{
    if (!l || !l->present) return 0.0f;

    float g = LOUDNESS_REFERENCE_LUFS - l->integrated_lufs;

    if (g > 0.0f) {
        /* Headroom the peak leaves. A peak of -3 dBFS can take +2 dB
         * before it reaches -1. Only ever reduces the boost; a track
         * with no headroom gets none. */
        float room = -l->sample_peak_dbfs - REPLAYGAIN_HEADROOM_DB;
        if (room < 0.0f) room = 0.0f;
        if (g > room) g = room;
        if (g > REPLAYGAIN_MAX_BOOST_DB) g = REPLAYGAIN_MAX_BOOST_DB;
    }
    return g;
}
