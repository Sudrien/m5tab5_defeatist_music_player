/*
 * mediawalk.c -- see mediawalk.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mediawalk.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cuedir.h"
#include "cuesheet.h"
#include "decoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage.h"
#include "storage_io.h"

static const char *TAG = "mediawalk";

#define CLS     STORAGE_IO_BACKGROUND

enum { K_FILE, K_DIR, K_CUE };

typedef struct {
    uint32_t name;      /* offset into the level's arena */
    uint8_t  kind;
    int      cue;       /* cuedir row, for K_CUE */
} ent_t;

/*
 * One open folder. Names live in one arena per folder rather than a
 * strdup each: a few thousand small allocations would land in internal
 * RAM, which is the heap this device is short of. The arena and the
 * entry array are PSRAM, and grow by doubling.
 */
typedef struct {
    char     *arena;
    size_t    used, cap;
    ent_t    *ents;
    int       n, capn, next;
    size_t    plen;     /* s_path's length at this folder */
    cuedir_t *cues;
} level_t;

static level_t s_lv[MWALK_DEPTH_MAX + 1];

/* mount + "/" + a relative path, with room to notice one too long. */
static char s_path[16 + MIDX_PATH_MAX + 2];
static size_t s_mount_len;

/* Scratch for a cue track's sheet and audio paths. */
static char s_aux[sizeof(s_path)];

static void *ps_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* There is no heap_caps_realloc in the host shim, and one copy on
 * growth is nothing next to the readdir that caused it. */
static bool grow(void **p, size_t have, size_t want)
{
    void *q = ps_alloc(want);
    if (!q) return false;
    if (*p) {
        memcpy(q, *p, have);
        heap_caps_free(*p);
    }
    *p = q;
    return true;
}

static void level_free(level_t *lv)
{
    heap_caps_free(lv->arena);
    heap_caps_free(lv->ents);
    cuedir_free(lv->cues);
    memset(lv, 0, sizeof(*lv));
}

static bool level_add(level_t *lv, const char *name, uint8_t kind, int cue)
{
    if (lv->n >= MWALK_DIR_MAX) return false;
    const size_t len = strlen(name) + 1;
    if (lv->used + len > lv->cap) {
        size_t cap = lv->cap ? lv->cap * 2 : 4096;
        while (cap < lv->used + len) cap *= 2;
        if (!grow((void **)&lv->arena, lv->used, cap)) return false;
        lv->cap = cap;
    }
    if (lv->n >= lv->capn) {
        const int capn = lv->capn ? lv->capn * 2 : 64;
        if (!grow((void **)&lv->ents, (size_t)lv->n * sizeof(ent_t),
                  (size_t)capn * sizeof(ent_t))) return false;
        lv->capn = capn;
    }
    memcpy(lv->arena + lv->used, name, len);
    lv->ents[lv->n].name = (uint32_t)lv->used;
    lv->ents[lv->n].kind = kind;
    lv->ents[lv->n].cue = cue;
    lv->n++;
    lv->used += len;
    return true;
}

/* qsort has no context argument in C11, and one walk runs at a time. */
static const char *s_sort_arena;

static int ent_cmp(const void *a, const void *b)
{
    const ent_t *x = a, *y = b;
    return midx_name_cmp(s_sort_arena + x->name, s_sort_arena + y->name);
}

static int leased_stat(const char *path, struct stat *st)
{
    storage_io_acquire(CLS);
    const int rc = stat(path, st);
    storage_io_release();
    return rc;
}

/*
 * Read the folder at s_path into lv, sorted. False if it could not be
 * read to the end -- which fails the walk, never shortens the folder.
 */
static bool level_load(level_t *lv)
{
    memset(lv, 0, sizeof(*lv));
    lv->plen = strlen(s_path);

    storage_io_acquire(CLS);
    DIR *d = opendir(s_path);
    storage_io_release();
    if (!d) {
        ESP_LOGW(TAG, "cannot open %s", s_path);
        return false;
    }

    bool ok = true, any_sheet = false;
    for (;;) {
        /* readdir() says "end" and "error" the same way; only errno
         * tells them apart, so it is cleared first. */
        errno = 0;
        storage_io_acquire(CLS);
        struct dirent *e = readdir(d);
        const int err = errno;
        storage_io_release();
        if (!e) {
            if (err) {
                ESP_LOGW(TAG, "reading %s failed (%d)", s_path, err);
                ok = false;
            }
            break;
        }
        if (storage_is_hidden(e->d_name)) continue;

        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            /* Not what ESP-IDF's FAT gives, but cheap to be right about. */
            struct stat st;
            const size_t k = lv->plen;
            if (k + 1 + strlen(e->d_name) >= sizeof(s_path)) continue;
            s_path[k] = '/';
            strcpy(s_path + k + 1, e->d_name);
            const int rc = leased_stat(s_path, &st);
            s_path[k] = '\0';
            if (rc != 0) { ok = false; break; }
            is_dir = S_ISDIR(st.st_mode);
        }

        if (is_dir) {
            ok = level_add(lv, e->d_name, K_DIR, 0);
        } else if (cue_is_sheet(e->d_name)) {
            any_sheet = true;   /* not an entry: its tracks are */
        } else if (decoder_supports(e->d_name)) {
            ok = level_add(lv, e->d_name, K_FILE, 0);
        }
        if (!ok) {
            ESP_LOGW(TAG, "%s: more than %d entries, or no memory; "
                     "not indexing a partial folder", s_path, MWALK_DIR_MAX);
            break;
        }
    }
    storage_io_acquire(CLS);
    closedir(d);
    storage_io_release();
    if (!ok) return false;

    /*
     * Sheets become their tracks, and hide the audio they cover, by the
     * one derivation the chooser and the decoder use (cuedir.h). Hidden
     * files are dropped by compacting in place; order does not matter
     * yet, the sort is next.
     */
    if (any_sheet) {
        lv->cues = cuedir_load(s_path, CLS);
        if (lv->cues) {
            int w = 0;
            for (int i = 0; i < lv->n; i++) {
                if (lv->ents[i].kind == K_FILE &&
                    cuedir_hides(lv->cues, lv->arena + lv->ents[i].name)) {
                    continue;
                }
                lv->ents[w++] = lv->ents[i];
            }
            lv->n = w;
            for (int i = 0; i < cuedir_count(lv->cues) && ok; i++) {
                ok = level_add(lv, cuedir_name(lv->cues, i), K_CUE, i);
            }
            if (!ok) return false;
        }
    }

    /* An empty folder has no array at all, and qsort(NULL, 0) is
     * undefined whatever the count says. */
    if (lv->n > 1) {
        s_sort_arena = lv->arena;
        qsort(lv->ents, (size_t)lv->n, sizeof(ent_t), ent_cmp);
    }
    return true;
}

/* s_path = the level's folder + "/" + name. False if too long. */
static bool path_enter(const level_t *lv, const char *name)
{
    const size_t n = strlen(name);
    if (lv->plen + 1 + n >= sizeof(s_path)) return false;
    s_path[lv->plen] = '/';
    memcpy(s_path + lv->plen + 1, name, n + 1);
    return strlen(s_path) - s_mount_len - 1 <= MIDX_PATH_MAX;
}

/* A cue track's stamp: the sheet's and its audio's together. */
static bool cue_stamp(const level_t *lv, const ent_t *e, midx_stamp_t *out)
{
    const char *vname = lv->arena + e->name;
    const char *hash = strrchr(vname, CUE_VPATH_SEP);
    const char *audio = cuedir_audio(lv->cues, e->cue);
    if (!hash || !audio) return false;

    struct stat a, b;
    const size_t k = lv->plen;
    const size_t sl = (size_t)(hash - vname);
    if (k + 1 + sl >= sizeof(s_aux) ||
        k + 1 + strlen(audio) >= sizeof(s_aux)) return false;

    memcpy(s_aux, s_path, k);
    s_aux[k] = '/';
    memcpy(s_aux + k + 1, vname, sl);
    s_aux[k + 1 + sl] = '\0';
    if (leased_stat(s_aux, &a) != 0) return false;

    strcpy(s_aux + k + 1, audio);
    if (leased_stat(s_aux, &b) != 0) return false;

    out->mtime = (int64_t)(a.st_mtime > b.st_mtime ? a.st_mtime : b.st_mtime);
    out->size = (uint64_t)a.st_size + (uint64_t)b.st_size;
    return true;
}

/* The entry being offered, while the callback runs; NULL otherwise. */
static const level_t *s_cur_lv;
static const ent_t   *s_cur_e;

bool mwalk_cue_tags(const char *path, char *title, char *artist,
                    char *album, size_t each)
{
    if (!s_cur_e || s_cur_e->kind != K_CUE || !path) return false;
    if (strcmp(path, s_path + s_mount_len + 1) != 0) return false;
    return cuedir_row_tags(s_cur_lv->cues, s_cur_e->cue, title, artist,
                           album, each);
}

static void free_all(int depth)
{
    for (int i = 0; i <= depth; i++) level_free(&s_lv[i]);
}

mwalk_result_t mwalk_volume(const char *mount, mwalk_fn fn, void *ctx)
{
    s_mount_len = strlen(mount);
    if (!fn || s_mount_len == 0 || s_mount_len >= 16) return MWALK_FAILED;
    memcpy(s_path, mount, s_mount_len + 1);

    int depth = 0;
    if (!level_load(&s_lv[0])) {
        level_free(&s_lv[0]);
        return MWALK_FAILED;
    }

    while (depth >= 0) {
        level_t *lv = &s_lv[depth];
        if (lv->next >= lv->n) {
            level_free(lv);
            depth--;
            if (depth >= 0) s_path[s_lv[depth].plen] = '\0';
            continue;
        }
        const ent_t *e = &lv->ents[lv->next++];
        const char *name = lv->arena + e->name;

        if (!path_enter(lv, name)) {
            s_path[lv->plen] = '\0';
            ESP_LOGW(TAG, "path too long, not indexed: %s/%s",
                     s_path + s_mount_len, name);
            continue;
        }

        if (e->kind == K_DIR) {
            if (depth + 1 > MWALK_DEPTH_MAX) {
                ESP_LOGW(TAG, "deeper than %d, not indexed: %s",
                         MWALK_DEPTH_MAX, s_path);
                s_path[lv->plen] = '\0';
                continue;
            }
            depth++;
            if (!level_load(&s_lv[depth])) {
                free_all(depth);
                return MWALK_FAILED;
            }
            continue;
        }

        midx_stamp_t st = { 0, 0 };
        bool have;
        if (e->kind == K_CUE) {
            have = cue_stamp(lv, e, &st);
        } else {
            struct stat sb;
            have = leased_stat(s_path, &sb) == 0;
            if (have) {
                st.mtime = (int64_t)sb.st_mtime;
                st.size = (uint64_t)sb.st_size;
            }
        }
        if (!have) {
            /* Listed a moment ago and now unreadable: the card is going
             * or something is wrong with it. Either way the rest of
             * this walk cannot be believed. */
            ESP_LOGW(TAG, "cannot stat %s", s_path);
            free_all(depth);
            return MWALK_FAILED;
        }

        s_cur_lv = lv;
        s_cur_e = e;
        const bool go = fn(ctx, s_path + s_mount_len + 1, st);
        s_cur_lv = NULL;
        s_cur_e = NULL;
        s_path[lv->plen] = '\0';
        if (!go) {
            free_all(depth);
            return MWALK_STOPPED;
        }
    }
    return MWALK_DONE;
}
