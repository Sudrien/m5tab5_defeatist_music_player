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

    const cJSON *ar = cJSON_GetObjectItemCaseSensitive(root, "art");
    if (cJSON_IsObject(ar)) {
        const cJSON *ha = cJSON_GetObjectItemCaseSensitive(ar, "has_art");
        const cJSON *of = cJSON_GetObjectItemCaseSensitive(ar, "offset");
        const cJSON *ln = cJSON_GetObjectItemCaseSensitive(ar, "length");
        out->art.present = true;
        out->art.has_art = cJSON_IsTrue(ha);
        out->art.offset = cJSON_IsNumber(of) ? (uint32_t)of->valuedouble : 0;
        out->art.length = cJSON_IsNumber(ln) ? (uint32_t)ln->valuedouble : 0;
        /* A claimed cover with no length is not a location, it is a
         * broken record. Demoted to a definite absence rather than
         * believed, so the next look re-reads the tag. */
        if (out->art.has_art && out->art.length == 0) out->art.present = false;
    }

    const cJSON *fm = cJSON_GetObjectItemCaseSensitive(root, "format");
    if (cJSON_IsObject(fm)) {
        const cJSON *sc = cJSON_GetObjectItemCaseSensitive(fm, "sec");
        const cJSON *sr = cJSON_GetObjectItemCaseSensitive(fm, "sample_rate");
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(fm, "channels");
        const cJSON *kb = cJSON_GetObjectItemCaseSensitive(fm, "kbps");
        const cJSON *cd = cJSON_GetObjectItemCaseSensitive(fm, "codec");
        const cJSON *gd = cJSON_GetObjectItemCaseSensitive(fm, "enc_delay");
        const cJSON *gp = cJSON_GetObjectItemCaseSensitive(fm, "enc_padding");
        out->format.present = true;
        out->format.sec = cJSON_IsNumber(sc) ? (uint32_t)sc->valuedouble : 0;
        out->format.sample_rate = cJSON_IsNumber(sr) ? (uint32_t)sr->valuedouble : 0;
        out->format.channels = cJSON_IsNumber(ch) ? (uint8_t)ch->valuedouble : 0;
        out->format.kbps = cJSON_IsNumber(kb) ? (uint16_t)kb->valuedouble : 0;
        if (cJSON_IsString(cd)) {
            snprintf(out->format.codec, sizeof(out->format.codec), "%s",
                     cd->valuestring);
        }
        /* Both or neither: a delay without a padding is half a trim and
         * would clip the end of every gapless track that used it. */
        out->format.has_gapless = cJSON_IsNumber(gd) && cJSON_IsNumber(gp);
        if (out->format.has_gapless) {
            out->format.enc_delay = (uint16_t)gd->valuedouble;
            out->format.enc_padding = (uint16_t)gp->valuedouble;
        }
    }

    const cJSON *tg = cJSON_GetObjectItemCaseSensitive(root, "tags");
    if (cJSON_IsObject(tg)) {
        const cJSON *ti = cJSON_GetObjectItemCaseSensitive(tg, "title");
        const cJSON *ac = cJSON_GetObjectItemCaseSensitive(tg, "artist");
        const cJSON *al = cJSON_GetObjectItemCaseSensitive(tg, "album");
        out->tags.present = true;
        if (cJSON_IsString(ti)) snprintf(out->tags.title, sizeof(out->tags.title), "%s", ti->valuestring);
        if (cJSON_IsString(ac)) snprintf(out->tags.artist, sizeof(out->tags.artist), "%s", ac->valuestring);
        if (cJSON_IsString(al)) snprintf(out->tags.album, sizeof(out->tags.album), "%s", al->valuestring);
    }

    const cJSON *ix = cJSON_GetObjectItemCaseSensitive(root, "index");
    if (cJSON_IsObject(ix)) {
        const cJSON *sp = cJSON_GetObjectItemCaseSensitive(ix, "spacing_sec");
        const cJSON *of = cJSON_GetObjectItemCaseSensitive(ix, "offset");
        const cJSON *sa = cJSON_GetObjectItemCaseSensitive(ix, "sample");
        if (cJSON_IsArray(of) && cJSON_IsArray(sa) && cJSON_IsNumber(sp)) {
            const int no = cJSON_GetArraySize(of);
            const int ns = cJSON_GetArraySize(sa);
            /* Ragged arrays mean a truncated or edited line. Taking the
             * shorter would pair an offset with the wrong sample, which
             * seeks to the wrong place silently -- worse than no index. */
            if (no == ns && no > 0 && no <= REPLAYGAIN_INDEX_MAX) {
                bool good = true;
                for (int k = 0; k < no; k++) {
                    const cJSON *a = cJSON_GetArrayItem(of, k);
                    const cJSON *b = cJSON_GetArrayItem(sa, k);
                    if (!cJSON_IsNumber(a) || !cJSON_IsNumber(b)) { good = false; break; }
                    out->index.offset[k] = (uint32_t)a->valuedouble;
                    out->index.sample[k] = (uint32_t)b->valuedouble;
                }
                /* Offsets past the end of the file are a record about a
                 * different file that happened to match size and mtime,
                 * or a corrupted one. Either way, not seekable. */
                for (int k = 0; good && k < no; k++) {
                    if (out->index.offset[k] >= filesize) good = false;
                }
                if (good) {
                    out->index.present = true;
                    out->index.count = no;
                    out->index.spacing_sec = (uint32_t)sp->valuedouble;
                }
            }
        }
    }

    const cJSON *at = cJSON_GetObjectItemCaseSensitive(root, "attempts");
    if (cJSON_IsObject(at)) {
        const cJSON *ab = cJSON_GetObjectItemCaseSensitive(at, "abandoned");
        if (cJSON_IsNumber(ab)) {
            out->attempts.present = true;
            out->attempts.abandoned = (uint32_t)ab->valuedouble;
        }
    }

    ok = out->waveform.present || out->loudness.present ||
         out->art.present || out->format.present ||
         out->tags.present || out->index.present ||
         out->attempts.present;

done:
    cJSON_Delete(root);
    return ok;
}

bool replaygain_load(const char *path, replaygain_t *out)
{
    if (!path || !out) return false;

    /*
     * Cleared before anything can fail, so a false return leaves a
     * readable all-absent record rather than whatever the caller's
     * stack had. parse_line() clears it again on the way to a hit,
     * which costs a memset nobody notices and means no caller has to
     * remember that the struct is only valid when the bool is true --
     * the kind of rule that holds right up until one call site forgets
     * and reads a present flag out of uninitialised memory.
     */
    memset(out, 0, sizeof(*out));

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
    char *buf = malloc(REPLAYGAIN_READ_MAX + 1);
    if (!buf) {
        storage_io_close(f);
        return false;
    }
    const size_t got = fread(buf, 1, REPLAYGAIN_READ_MAX, f);
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
 * Serialise `rg` and write it as the sidecar's only line.
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

    if (rg->art.present) {
        cJSON *ar = cJSON_AddObjectToObject(root, "art");
        if (ar) {
            cJSON_AddBoolToObject(ar, "has_art", rg->art.has_art);
            if (rg->art.has_art) {
                cJSON_AddNumberToObject(ar, "offset", (double)rg->art.offset);
                cJSON_AddNumberToObject(ar, "length", (double)rg->art.length);
            }
        }
    }

    if (rg->format.present) {
        cJSON *fm = cJSON_AddObjectToObject(root, "format");
        if (fm) {
            cJSON_AddNumberToObject(fm, "sec", (double)rg->format.sec);
            cJSON_AddNumberToObject(fm, "sample_rate", (double)rg->format.sample_rate);
            cJSON_AddNumberToObject(fm, "channels", (double)rg->format.channels);
            cJSON_AddNumberToObject(fm, "kbps", (double)rg->format.kbps);
            if (rg->format.codec[0]) {
                cJSON_AddStringToObject(fm, "codec", rg->format.codec);
            }
            if (rg->format.has_gapless) {
                cJSON_AddNumberToObject(fm, "enc_delay", (double)rg->format.enc_delay);
                cJSON_AddNumberToObject(fm, "enc_padding", (double)rg->format.enc_padding);
            }
        }
    }

    if (rg->tags.present) {
        cJSON *tg = cJSON_AddObjectToObject(root, "tags");
        if (tg) {
            /* Written even when empty: a track with no title tag is a
             * fact, and omitting the key would make it look unread. */
            cJSON_AddStringToObject(tg, "title", rg->tags.title);
            cJSON_AddStringToObject(tg, "artist", rg->tags.artist);
            cJSON_AddStringToObject(tg, "album", rg->tags.album);
        }
    }

    if (rg->index.present && rg->index.count > 0) {
        cJSON *ix = cJSON_AddObjectToObject(root, "index");
        if (ix) {
            cJSON_AddNumberToObject(ix, "spacing_sec",
                                    (double)rg->index.spacing_sec);
            cJSON *of = cJSON_AddArrayToObject(ix, "offset");
            cJSON *sa = cJSON_AddArrayToObject(ix, "sample");
            for (int k = 0; of && sa && k < rg->index.count; k++) {
                cJSON_AddItemToArray(of, cJSON_CreateNumber((double)rg->index.offset[k]));
                cJSON_AddItemToArray(sa, cJSON_CreateNumber((double)rg->index.sample[k]));
            }
        }
    }

    if (rg->attempts.present) {
        cJSON *at = cJSON_AddObjectToObject(root, "attempts");
        if (at) {
            cJSON_AddNumberToObject(at, "abandoned",
                                    (double)rg->attempts.abandoned);
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
     * Always one line, always the whole record, written to a temp file
     * and renamed over the old one.
     *
     * Appending was the wrong call and a card showed why: a sidecar
     * written across the v1, v2 and v3 formats held six lines, four of
     * them dead, because sidecar_is_jsonl() only asks whether the first
     * byte is '{' -- and a stale-version line is perfectly good JSONL.
     * So a format bump never triggered a rewrite; it appended after the
     * corpses, and every future bump would double them again.
     *
     * The deeper point is that appending never bought anything here.
     * Every line is already a complete merged record, because the
     * caller loads and overlays before calling: line N+1 says
     * everything line N said. So an append writes the same bytes a
     * rewrite would and keeps the old copy as well. The only argument
     * for it was a torn write leaving the previous line readable, and
     * temp-and-rename answers that better -- a crash leaves the old
     * file whole, because the rename either happened or it did not.
     *
     * What this buys, beyond dropping the dead lines:
     *
     *   - The file is ~1.2 KB and stays there. A 64 KB cluster holds it
     *     in one piece, so the sidecar cannot fragment. That is why
     *     there is no size threshold any more -- the file never grows,
     *     so there is nothing to compact and no boundary to straddle.
     *     A growth cap of 65536 was in fact the worst possible value:
     *     it permitted growth to exactly the cluster edge before
     *     acting. The cap is gone; REPLAYGAIN_READ_MAX is now only the
     *     size of the read buffer.
     *
     *   - A stale-version line cannot survive, because nothing survives.
     *     Version handling stops needing a migration path at all.
     */
    char tmp_path[620];
    if ((int)snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cache_path)
        >= (int)sizeof(tmp_path)) {
        cJSON_free(line);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = storage_io_open(tmp_path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "could not open %s for writing", tmp_path);
        cJSON_free(line);
        return ESP_FAIL;
    }

    const size_t len = strlen(line);
    const bool wrote = (fwrite(line, 1, len, f) == len) &&
                       (fputc('\n', f) != EOF);
    const int cerr = storage_io_close(f);
    cJSON_free(line);

    if (!wrote || cerr != 0) {
        ESP_LOGW(TAG, "write failed for %s; keeping the old sidecar",
                 tmp_path);
        remove(tmp_path);
        return ESP_FAIL;
    }

    /* Atomic on FatFs: a directory entry update, not a copy. Either the
     * new record is there or the old one still is. */
    remove(cache_path);          /* FatFs rename() will not overwrite */
    if (rename(tmp_path, cache_path) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed", tmp_path, cache_path);
        remove(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "sidecar written: %s (%u bytes)", cache_path,
             (unsigned)(len + 1));
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

/* ------------------------------------------------------------------ */
/* v3 sections. See replaygain.h -- nothing calls these yet.           */

/* Every saver is the same three steps: stat for the key, load-or-empty
 * so the other sections survive, overlay one section, append. Written
 * out per section rather than behind a generic setter because the
 * overlay is the only part that differs and a callback taking a void*
 * would hide exactly the field assignments worth reading. */
#define RG_SAVE_PROLOGUE(fail)                                            \
    struct stat st;                                                       \
    if (stat(path, &st) != 0) {                                           \
        ESP_LOGW(TAG, "stat failed for %s; " fail, path);                 \
        return ESP_FAIL;                                                  \
    }                                                                     \
    replaygain_t rg;                                                      \
    load_or_empty(path, (uint32_t)st.st_size, (int64_t)st.st_mtime, &rg)

#define RG_SAVE_APPEND()                                                  \
    append_record(path, &rg, (uint32_t)st.st_size, (int64_t)st.st_mtime)

esp_err_t replaygain_save_art(const char *path, bool has_art,
                              uint32_t offset, uint32_t length)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    /* A cover at a location with no bytes is not a cover. Refused here
     * rather than written and rejected on the way back in. */
    if (has_art && length == 0) return ESP_ERR_INVALID_ARG;

    RG_SAVE_PROLOGUE("not caching art");
    rg.art.present = true;
    rg.art.has_art = has_art;
    rg.art.offset  = has_art ? offset : 0;
    rg.art.length  = has_art ? length : 0;
    return RG_SAVE_APPEND();
}

esp_err_t replaygain_save_format(const char *path,
                                 const replaygain_format_t *fmt)
{
    if (!path || !fmt) return ESP_ERR_INVALID_ARG;

    RG_SAVE_PROLOGUE("not caching format");
    rg.format = *fmt;
    rg.format.present = true;
    return RG_SAVE_APPEND();
}

esp_err_t replaygain_save_tags(const char *path, const char *title,
                               const char *artist, const char *album)
{
    if (!path) return ESP_ERR_INVALID_ARG;

    RG_SAVE_PROLOGUE("not caching tags");
    rg.tags.present = true;
    snprintf(rg.tags.title,  sizeof(rg.tags.title),  "%s", title  ? title  : "");
    snprintf(rg.tags.artist, sizeof(rg.tags.artist), "%s", artist ? artist : "");
    snprintf(rg.tags.album,  sizeof(rg.tags.album),  "%s", album  ? album  : "");
    return RG_SAVE_APPEND();
}

esp_err_t replaygain_save_index(const char *path, const uint32_t *offset,
                                const uint32_t *sample, int count,
                                uint32_t spacing_sec)
{
    if (!path || !offset || !sample || count <= 0 || !spacing_sec) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count > REPLAYGAIN_INDEX_MAX) count = REPLAYGAIN_INDEX_MAX;

    RG_SAVE_PROLOGUE("not caching index");
    rg.index.present = true;
    rg.index.count = count;
    rg.index.spacing_sec = spacing_sec;
    memcpy(rg.index.offset, offset, (size_t)count * sizeof(uint32_t));
    memcpy(rg.index.sample, sample, (size_t)count * sizeof(uint32_t));
    return RG_SAVE_APPEND();
}

esp_err_t replaygain_note_abandoned(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;

    RG_SAVE_PROLOGUE("not counting abandonment");
    /* Increment whatever was there. load_or_empty() zeroed it if there
     * was nothing, so a first abandonment lands at 1. */
    const uint32_t was = rg.attempts.present ? rg.attempts.abandoned : 0;
    rg.attempts.present = true;
    rg.attempts.abandoned = was + 1;
    return RG_SAVE_APPEND();
}
