/*
 * cuedir.c -- see cuedir.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "tab5_cue";

/* Before cuesheet.h, which otherwise defines it away. */
#define CUE_DROP_LOG(track, why) \
    ESP_LOGW(TAG, "cue track %d dropped: %s", (int)(track), (why))

#include "cuedir.h"
#include "decoder.h"
#include "duration.h"
#include "storage.h"

/* A sheet is text and a big one is a few KB. Anything past this is not
 * a sheet anyone wrote, and reading it whole on a touch is not free. */
#define SHEET_MAX_BYTES     (64 * 1024)

/* The folder's names, for resolving FILE lines. The playlist's own cap,
 * for the same reason. */
#define DIR_MAX_NAMES       (1024)

#define VNAME_MAX           (300)
#define LABEL_MAX           (CUE_TEXT_MAX + 8)

typedef struct {
    char vname[VNAME_MAX];
    char label[LABEL_MAX];
    int  audio;                 /* index into names: the file it plays */
} cue_row_t;

struct cuedir {
    char     **names;           /* every file in the folder */
    int        nnames;
    bool      *hidden;          /* per name: covered by a sheet */
    cue_row_t *rows;            /* PSRAM, grown by sheet */
    int        nrows, caprows;
};

static void *ps_alloc(size_t n)
{
    return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool playable(const char *name) { return decoder_supports(name); }

/* The folder's plain files, strdup'd. */
static int read_names(const char *dir, char ***out)
{
    *out = NULL;
    DIR *d = opendir(dir);
    if (!d) return -1;
    char **names = ps_alloc(DIR_MAX_NAMES * sizeof(char *));
    if (!names) { closedir(d); return -1; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < DIR_MAX_NAMES) {
        if (e->d_type == DT_DIR) continue;
        if (storage_is_hidden(e->d_name)) continue;
        names[n] = strdup(e->d_name);
        if (!names[n]) break;
        n++;
    }
    closedir(d);
    *out = names;
    return n;
}

static void free_names(char **names, int n)
{
    if (!names) return;
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* Seconds, rounded up to CD frames, with a second's slack: duration_probe
 * rounds to the nearest second, and a track that starts in the last
 * half-second of a file must not be dropped for a rounding. A start in
 * that slack that really is past the end fails at open instead, which is
 * the cheaper mistake. */
static uint32_t audio_frames(const char *path, storage_io_class_t cls)
{
    FILE *f = storage_io_open(path, "rb");
    if (!f) return 0;
    const uint32_t sec = duration_probe(f, cls);
    storage_io_close(f);
    return sec ? (sec + 1) * CUE_FPS : 0;
}

/*
 * THE one derivation of a sheet's kept tracks. Read, parse, resolve
 * every FILE against the folder, drop the tracks under a FILE that did
 * not resolve, and finish each file at its length. file_name[f] is the
 * index into names of FILE f, or -1.
 */
static bool sheet_load(const char *dir, const char *sheet, char *const *names,
                       int nnames, storage_io_class_t cls, cue_sheet_t *cs,
                       int file_name[CUE_MAX_FILES])
{
    char path[512];
    if (!storage_join_path(path, sizeof path, dir, sheet)) return false;

    FILE *f = storage_io_open(path, "rb");
    if (!f) return false;
    char *text = ps_alloc(SHEET_MAX_BYTES);
    if (!text) { storage_io_close(f); return false; }
    const size_t n = storage_io_fread(text, SHEET_MAX_BYTES, f, cls);
    storage_io_close(f);

    const int got = cue_parse(text, n, cs);
    free(text);
    if (!got) {
        ESP_LOGW(TAG, "%s: no playable tracks in the sheet", sheet);
        return false;
    }

    for (int fi = 0; fi < cs->nfiles; fi++) {
        file_name[fi] = cue_resolve(cs->files[fi], sheet, cs->nfiles,
                                    (const char *const *)names, nnames, playable);
        if (file_name[fi] < 0) {
            ESP_LOGW(TAG, "%s: FILE \"%s\" is not in the folder", sheet,
                     cs->files[fi]);
        } else if (strcmp(names[file_name[fi]], cs->files[fi]) != 0) {
            ESP_LOGI(TAG, "%s: FILE \"%s\" taken as %s", sheet, cs->files[fi],
                     names[file_name[fi]]);
        }
    }

    /* Tracks under an unresolved FILE have nothing to play. */
    int w = 0;
    for (int r = 0; r < cs->ntracks; r++) {
        if (file_name[cs->tracks[r].file] < 0) {
            cs->dropped++;
            CUE_DROP_LOG(cs->tracks[r].number, "its FILE is not in the folder");
            continue;
        }
        if (w != r) cs->tracks[w] = cs->tracks[r];
        w++;
    }
    cs->ntracks = w;

    for (int fi = 0; fi < cs->nfiles; fi++) {
        if (file_name[fi] < 0) continue;
        char apath[512];
        uint32_t len = 0;
        if (storage_join_path(apath, sizeof apath, dir, names[file_name[fi]])) {
            len = audio_frames(apath, cls);
        }
        cue_finish(cs, fi, len);
    }

    if (cs->ntracks && cs->dropped) {
        ESP_LOGI(TAG, "%s: %d track%s, %d dropped", sheet, cs->ntracks,
                 cs->ntracks == 1 ? "" : "s", cs->dropped);
    }
    return cs->ntracks > 0;
}

static bool rows_grow(cuedir_t *cd, int more)
{
    if (cd->nrows + more <= cd->caprows) return true;
    const int cap = cd->caprows + (more > 32 ? more : 32);
    cue_row_t *r = heap_caps_realloc(cd->rows, (size_t)cap * sizeof *r,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r) return false;
    cd->rows = r;
    cd->caprows = cap;
    return true;
}

cuedir_t *cuedir_load(const char *dir, storage_io_class_t cls)
{
    char **names;
    const int nnames = read_names(dir, &names);
    if (nnames <= 0) { free_names(names, 0); return NULL; }

    bool any = false;
    for (int i = 0; i < nnames && !any; i++) any = cue_is_sheet(names[i]);
    if (!any) { free_names(names, nnames); return NULL; }

    cuedir_t *cd = ps_alloc(sizeof *cd);
    cue_sheet_t *cs = ps_alloc(sizeof *cs);
    bool *hidden = ps_alloc((size_t)nnames * sizeof(bool));
    if (!cd || !cs || !hidden) {
        free(cd); free(cs); free(hidden);
        free_names(names, nnames);
        return NULL;
    }
    cd->names = names;
    cd->nnames = nnames;
    cd->hidden = hidden;

    for (int i = 0; i < nnames; i++) {
        if (!cue_is_sheet(names[i])) continue;
        int file_name[CUE_MAX_FILES];
        if (!sheet_load(dir, names[i], names, nnames, cls, cs, file_name)) continue;
        if (!rows_grow(cd, cs->ntracks)) break;

        for (int fi = 0; fi < cs->nfiles; fi++) {
            if (file_name[fi] >= 0) cd->hidden[file_name[fi]] = true;
        }
        for (int t = 0; t < cs->ntracks; t++) {
            cue_row_t *r = &cd->rows[cd->nrows++];
            snprintf(r->vname, sizeof r->vname, "%s%c%02d", names[i],
                     CUE_VPATH_SEP, t + 1);
            r->audio = file_name[cs->tracks[t].file];
            if (cs->tracks[t].title[0]) {
                snprintf(r->label, sizeof r->label, "%02d  %s",
                         cs->tracks[t].number, cs->tracks[t].title);
            } else {
                snprintf(r->label, sizeof r->label, "%02d  Track %d",
                         cs->tracks[t].number, cs->tracks[t].number);
            }
        }
        ESP_LOGI(TAG, "%s: %d track%s", names[i], cs->ntracks,
                 cs->ntracks == 1 ? "" : "s");
    }
    free(cs);
    return cd;
}

void cuedir_free(cuedir_t *cd)
{
    if (!cd) return;
    free_names(cd->names, cd->nnames);
    free(cd->hidden);
    free(cd->rows);
    free(cd);
}

bool cuedir_hides(const cuedir_t *cd, const char *name)
{
    if (!cd || !name) return false;
    for (int i = 0; i < cd->nnames; i++) {
        if (cd->hidden[i] && strcmp(cd->names[i], name) == 0) return true;
    }
    return false;
}

int cuedir_count(const cuedir_t *cd) { return cd ? cd->nrows : 0; }

const char *cuedir_name(const cuedir_t *cd, int i)
{
    return (cd && i >= 0 && i < cd->nrows) ? cd->rows[i].vname : NULL;
}

const char *cuedir_label(const cuedir_t *cd, int i)
{
    return (cd && i >= 0 && i < cd->nrows) ? cd->rows[i].label : NULL;
}

const char *cuedir_audio(const cuedir_t *cd, int i)
{
    return (cd && i >= 0 && i < cd->nrows) ? cd->names[cd->rows[i].audio]
                                           : NULL;
}

bool cuedir_track(const char *vpath, storage_io_class_t cls, cuetrack_t *out)
{
    size_t sl = 0;
    const int pos = cue_vpath_split(vpath, &sl);
    if (!pos || !out) return false;

    /* Split "<dir>/<sheet>#NN" into its folder and sheet name. */
    char dir[512], sheet[256];
    if (sl >= sizeof dir) return false;
    memcpy(dir, vpath, sl);
    dir[sl] = '\0';
    char *slash = strrchr(dir, '/');
    if (!slash) return false;
    const size_t shl = strlen(slash + 1);
    if (shl == 0 || shl >= sizeof sheet) return false;
    memcpy(sheet, slash + 1, shl + 1);
    *slash = '\0';

    char **names;
    const int nnames = read_names(dir, &names);
    cue_sheet_t *cs = ps_alloc(sizeof *cs);
    bool ok = false;
    int file_name[CUE_MAX_FILES];

    if (nnames > 0 && cs &&
        sheet_load(dir, sheet, names, nnames, cls, cs, file_name) &&
        pos <= cs->ntracks) {
        const cue_track_t *t = &cs->tracks[pos - 1];
        ok = storage_join_path(out->audio, sizeof out->audio, dir,
                               names[file_name[t->file]]);
        out->start = t->start;
        out->end = cue_track_end(cs, pos - 1);
        out->number = t->number;
        snprintf(out->title, sizeof out->title, "%s", t->title);
        snprintf(out->performer, sizeof out->performer, "%s", t->performer);
        snprintf(out->album, sizeof out->album, "%s", cs->title);
    }
    if (!ok) ESP_LOGW(TAG, "%s: no such track any more", vpath);

    free(cs);
    free_names(names, nnames > 0 ? nnames : 0);
    return ok;
}

/*
 * The two lookups other tasks make -- the decode loop for the cover and
 * the title, media_task for the next track's -- share one cuetrack_t,
 * because one is a kilobyte, and one lock around the copy out.
 */
static cuetrack_t        s_look;
static StaticSemaphore_t s_look_buf;
static SemaphoreHandle_t s_look_lock;

static bool look(const char *vpath)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    if (!s_look_lock) s_look_lock = xSemaphoreCreateMutexStatic(&s_look_buf);
    taskEXIT_CRITICAL(&mux);
    xSemaphoreTake(s_look_lock, portMAX_DELAY);
    const bool ok = cuedir_track(vpath, STORAGE_IO_BACKGROUND, &s_look);
    if (!ok) xSemaphoreGive(s_look_lock);
    return ok;      /* held on success; the caller copies, then done() */
}

static void done(void) { xSemaphoreGive(s_look_lock); }

const char *cuedir_file_of(const char *path, char *out, size_t out_size)
{
    if (!cue_vpath_split(path, NULL) || !out || !out_size) return path;
    if (!look(path)) return path;
    snprintf(out, out_size, "%s", s_look.audio);
    done();
    return out;
}

bool cuedir_tags(const char *vpath, char *title, char *artist, char *album,
                 size_t each)
{
    if (!cue_vpath_split(vpath, NULL) || !look(vpath)) return false;
    /* On a character boundary: these land in 64-byte tag fields, and a
     * title cut through the middle of a character draws as garbage. */
    cue_text(title,  each, s_look.title,     strlen(s_look.title),     false);
    cue_text(artist, each, s_look.performer, strlen(s_look.performer), false);
    cue_text(album,  each, s_look.album,     strlen(s_look.album),     false);
    if (!title[0]) snprintf(title, each, "Track %d", s_look.number);
    done();
    return true;
}
