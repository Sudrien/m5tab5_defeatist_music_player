/*
 * mediacat.c -- the catalog's lines, and getting them on and off the
 * card. mediacat.h says what a line is and why it is appended.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mediacat.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "storage_io.h"

static const char *TAG = "mediacat";

/* The one line buffer, shared by append and read. Not the stack: see
 * the header's THREADING note. */
static char s_line[MEDIACAT_LINE_MAX + 1];

/*
 * How big an integer survives a trip through cJSON. Not 2^53, which is
 * what a double holds exactly: cJSON prints with "%1.15g" and falls
 * back to "%1.17g" only if the result reads back DIFFERENT -- and its
 * test for different is a relative epsilon, so 2^53 prints as
 * 9.00719925474099e+15, is judged close enough, and comes back 2 short.
 * Fifteen digits is the limit that is actually exact. A value past it
 * is refused on the way in rather than written as a different number.
 * Nothing here comes near it: a size of 10^15 is a petabyte.
 */
#define EXACT_MAX   (999999999999999.0)     /* 10^15 - 1 */

static bool exact_int(double v, int64_t *out)
{
    if (!(v >= -EXACT_MAX && v <= EXACT_MAX)) return false;   /* and NaN */
    if (v != floor(v)) return false;
    *out = (int64_t)v;
    return true;
}

int mediacat_encode(const mediacat_rec_t *r, char *out, size_t out_size)
{
    if (!r || !out || out_size < 8) return -1;

    /* The index's own test for a path it can hold, so the two cannot
     * disagree about what a catalog line may name. */
    uint8_t probe[MIDX_REC_SIZE];
    if (!midx_rec_pack(probe, r->path, 0, 0, r->stamp)) return -1;
    if (r->clock != MIDX_CLOCK_FLOOR && r->clock != MIDX_CLOCK_SYNCED) {
        return -1;
    }
    if ((double)r->stamp.size > EXACT_MAX ||
        fabs((double)r->stamp.mtime) > EXACT_MAX ||
        r->written < 0 || (double)r->written > EXACT_MAX ||
        r->deleted_at < 0 || (double)r->deleted_at > EXACT_MAX) {
        return -1;
    }

    cJSON *o = cJSON_CreateObject();
    if (!o) return -1;

    const char clock[2] = { r->clock, '\0' };
    bool ok =
        cJSON_AddNumberToObject(o, "format_version",
                                MEDIACAT_FORMAT_VERSION) &&
        cJSON_AddStringToObject(o, "path", r->path) &&
        cJSON_AddNumberToObject(o, "mtime", (double)r->stamp.mtime) &&
        cJSON_AddNumberToObject(o, "size", (double)r->stamp.size);
    /* An absent tag is left out rather than written as "": most of a
     * library untagged is most of a catalog's bytes spent on nothing. */
    if (ok && r->title[0])  ok = cJSON_AddStringToObject(o, "title", r->title);
    if (ok && r->artist[0]) ok = cJSON_AddStringToObject(o, "artist", r->artist);
    if (ok && r->album[0])  ok = cJSON_AddStringToObject(o, "album", r->album);
    ok = ok &&
        cJSON_AddNumberToObject(o, "written", (double)r->written) &&
        cJSON_AddStringToObject(o, "clock", clock);
    if (ok && r->deleted_at) {
        ok = cJSON_AddNumberToObject(o, "deleted_at", (double)r->deleted_at);
    }

    /*
     * Into the caller's buffer rather than cJSON_PrintUnformatted()'s
     * malloc, since this runs once per track on a walk. cJSON's size
     * estimate can overrun by a few bytes -- its header says to allow
     * five -- and the '\n' needs one more.
     */
    int n = -1;
    const size_t cap = out_size < MEDIACAT_LINE_MAX ? out_size
                                                    : MEDIACAT_LINE_MAX;
    if (ok && cJSON_PrintPreallocated(o, out, (int)cap - 6, false)) {
        n = (int)strlen(out);
        out[n++] = '\n';
        out[n] = '\0';
    }
    cJSON_Delete(o);
    return n;
}

/* A string field into a fixed buffer, whole or not at all. Absent is
 * "" when `required` is false. */
static bool take_str(const cJSON *o, const char *key, char *dst, size_t n,
                     bool required)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) {
        dst[0] = '\0';
        return !required;
    }
    if (!cJSON_IsString(v) || !v->valuestring) return false;
    const size_t len = strlen(v->valuestring);
    if (len >= n) return false;
    memcpy(dst, v->valuestring, len + 1);
    return true;
}

static bool take_int(const cJSON *o, const char *key, int64_t *dst,
                     bool required)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!v) {
        *dst = 0;
        return !required;
    }
    return cJSON_IsNumber(v) && exact_int(v->valuedouble, dst);
}

bool mediacat_decode(const char *line, mediacat_rec_t *out)
{
    if (!line || !out) return false;
    memset(out, 0, sizeof(*out));

    /* Whole line or nothing: trailing whitespace (the '\n') is allowed,
     * trailing anything else is a line that is not one record. */
    cJSON *o = cJSON_ParseWithOpts(line, NULL, true);
    if (!o) return false;

    bool ok = cJSON_IsObject(o);

    int64_t ver = 0, size = 0, del = 0;
    char clock[2];
    ok = ok &&
        take_int(o, "format_version", &ver, true) &&
        ver == MEDIACAT_FORMAT_VERSION &&
        take_str(o, "path", out->path, sizeof(out->path), true) &&
        take_int(o, "mtime", &out->stamp.mtime, true) &&
        take_int(o, "size", &size, true) && size >= 0 &&
        take_str(o, "title", out->title, sizeof(out->title), false) &&
        take_str(o, "artist", out->artist, sizeof(out->artist), false) &&
        take_str(o, "album", out->album, sizeof(out->album), false) &&
        take_int(o, "written", &out->written, true) && out->written >= 0 &&
        take_str(o, "clock", clock, sizeof(clock), true) &&
        (clock[0] == MIDX_CLOCK_FLOOR || clock[0] == MIDX_CLOCK_SYNCED) &&
        take_int(o, "deleted_at", &del, false) && del >= 0;
    cJSON_Delete(o);
    if (!ok) return false;

    out->stamp.size = (uint64_t)size;
    out->clock = clock[0];
    out->deleted_at = del;

    /* The same test the encoder applied: a path the index cannot hold
     * is not a record, whoever wrote the line. */
    uint8_t probe[MIDX_REC_SIZE];
    return midx_rec_pack(probe, out->path, 0, 0, out->stamp);
}

bool mediacat_path(storage_id_t vol, char *out, size_t out_size)
{
    if (vol >= STORAGE_COUNT) return false;
    return storage_join_path(out, out_size, storage_mount_path(vol),
                             MEDIACAT_FILENAME);
}

/*
 * Open a catalog for appending, finishing a torn last line, and say
 * where the end is. Called holding the lease; NULL on failure.
 *
 * "a+": appends always go to the end, and the last byte can still be
 * read to see whether the previous append finished.
 */
static FILE *open_for_append(const char *path, long *end_out)
{
    FILE *f = storage_io_open(path, "a+");
    if (!f) return NULL;
    long end = -1;
    bool ok = fseek(f, 0, SEEK_END) == 0 && (end = ftell(f)) >= 0;
    if (ok && end > 0) {
        /* A torn last line: finish it, so the next one starts on its own. */
        int last = EOF;
        if (fseek(f, end - 1, SEEK_SET) == 0) last = fgetc(f);
        /* Back to the end before writing: C wants a seek between a
         * read and a write on one stream, even in append mode. */
        ok = fseek(f, 0, SEEK_END) == 0;
        if (ok && last != '\n') {
            ok = fputc('\n', f) != EOF;
            end += 1;
            ESP_LOGW(TAG, "%s ended mid-line; closed it off", path);
        }
    }
    if (!ok) {
        storage_io_close(f);
        return NULL;
    }
    *end_out = end;
    return f;
}

/* ---- a session: one handle for a whole reconcile ---------------------- */

static FILE        *s_sess;
static storage_id_t s_sess_vol = STORAGE_COUNT;
static long         s_sess_end;         /* where the next line starts */
static bool         s_sess_err;         /* a write failed: refuse the rest */
static bool         s_sess_new;         /* the file did not exist */

bool mediacat_session_open(storage_id_t vol)
{
    if (s_sess) return false;
    char path[32];
    if (!mediacat_path(vol, path, sizeof(path))) return false;
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    s_sess = open_for_append(path, &s_sess_end);
    storage_io_release();
    if (!s_sess) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return false;
    }
    s_sess_vol = vol;
    s_sess_err = false;
    s_sess_new = (s_sess_end == 0);
    return true;
}

bool mediacat_session_read(uint32_t offset, mediacat_rec_t *out)
{
    if (!s_sess) return false;
    /* mediacat_read_at() seeks first, which is also what C wants
     * between this stream's last write and a read. */
    return mediacat_read_at(s_sess, offset, out);
}

bool mediacat_session_close(void)
{
    if (!s_sess) return true;
    char path[32];
    const bool named = mediacat_path(s_sess_vol, path, sizeof(path));
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    bool ok = fflush(s_sess) == 0;
    ok = (storage_io_close(s_sess) == 0) && ok;
    storage_io_release();
    ok = ok && !s_sess_err;
    if (ok && named && s_sess_new) storage_mark_hidden(path);
    s_sess = NULL;
    s_sess_vol = STORAGE_COUNT;
    return ok;
}

/* ---- appending ------------------------------------------------------- */

bool mediacat_append(storage_id_t vol, const mediacat_rec_t *r,
                     uint32_t *offset)
{
    char path[32];
    if (!mediacat_path(vol, path, sizeof(path))) return false;

    const int n = mediacat_encode(r, s_line, sizeof(s_line));
    if (n <= 0) {
        ESP_LOGW(TAG, "not a record the catalog can hold: %s",
                 r ? r->path : "(null)");
        return false;
    }

    /*
     * Through the session when there is one: no open, no flush, no
     * close per line. The offset is tracked rather than asked for --
     * ftell() on an append stream with a buffer in it is a flush -- and
     * a write that fails refuses every line after it, because after a
     * short write the tracked end and the file's end disagree.
     */
    if (s_sess && s_sess_vol == vol) {
        if (s_sess_err) return false;
        if ((uint64_t)s_sess_end + (uint64_t)n > UINT32_MAX) {
            ESP_LOGW(TAG, "%s is at its size limit", path);
            return false;
        }
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        const bool ok = fseek(s_sess, 0, SEEK_END) == 0 &&
                        fwrite(s_line, 1, (size_t)n, s_sess) == (size_t)n;
        storage_io_release();
        if (!ok) {
            s_sess_err = true;
            ESP_LOGW(TAG, "append to %s failed", path);
            return false;
        }
        if (offset) *offset = (uint32_t)s_sess_end;
        s_sess_end += n;
        return true;
    }

    storage_io_acquire(STORAGE_IO_BACKGROUND);
    long end = -1;
    FILE *f = open_for_append(path, &end);
    bool ok = f != NULL;
    /* cat_off is 32 bits: refuse a line that would start past it. */
    if (ok && (uint64_t)end + (uint64_t)n > UINT32_MAX) {
        ESP_LOGW(TAG, "%s is at its size limit", path);
        ok = false;
    }
    if (ok) {
        ok = fwrite(s_line, 1, (size_t)n, f) == (size_t)n;
        ok = (fflush(f) == 0) && ok;
    }
    const int err = errno;
    if (f) ok = (storage_io_close(f) == 0) && ok;
    storage_io_release();

    if (!ok) {
        /* A partial line may now end the file. The next append closes
         * it off, and the reader skips it. */
        ESP_LOGW(TAG, "append to %s failed (%s)", path, strerror(err));
        (void)err;      /* the host's ESP_LOGW discards its arguments */
        return false;
    }
    if (end == 0) storage_mark_hidden(path);      /* a new file */
    if (offset) *offset = (uint32_t)end;
    return true;
}

bool mediacat_read_at(FILE *f, uint32_t offset, mediacat_rec_t *out)
{
    if (!f || !out) return false;

    /* Not storage_io_read_at(), which wants the whole length: a line
     * near the end of the file is shorter than the buffer. */
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    const bool sought = fseek(f, (long)offset, SEEK_SET) == 0;
    storage_io_release();
    if (!sought) return false;

    const size_t got = storage_io_fread(s_line, MEDIACAT_LINE_MAX, f,
                                        STORAGE_IO_BACKGROUND);
    if (got == 0) return false;

    /* The line must end within the buffer. No '\n' is a torn last line
     * or an offset that is not the start of one; either way, not a
     * record. A NUL before the '\n' is not a line this code wrote. */
    char *nl = memchr(s_line, '\n', got);
    if (!nl || memchr(s_line, '\0', (size_t)(nl - s_line))) return false;
    *nl = '\0';
    return mediacat_decode(s_line, out);
}
