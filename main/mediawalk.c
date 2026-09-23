/*
 * mediawalk.c -- see mediawalk.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mediawalk.h"

#include <stdlib.h>
#include <string.h>

#include "cuedir.h"
#include "cuesheet.h"
#include "decoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mediadir.h"
#include "storage.h"
#include "storage_io.h"

static const char *TAG = "mediawalk";

#define CLS     STORAGE_IO_BACKGROUND

/*
 * K_SHEET and K_COVERED are kept but never offered: a sheet is not a
 * track, and audio a sheet covers is its tracks' -- but a cue track's
 * stamp is made of theirs, and they come from the same listing as
 * everything else rather than from a stat() each.
 */
enum { K_FILE, K_DIR, K_CUE, K_SHEET, K_COVERED };

typedef struct {
    uint32_t     name;      /* offset into the level's arena */
    uint8_t      kind;
    int          cue;       /* cuedir row, for K_CUE */
    midx_stamp_t stamp;     /* from the listing; K_CUE's is made later */
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

static bool level_add(level_t *lv, const char *name, uint8_t kind, int cue,
                      midx_stamp_t stamp)
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
    lv->ents[lv->n].stamp = stamp;
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


/*
 * Read the folder at s_path into lv, sorted. False if it could not be
 * read to the end -- which fails the walk, never shortens the folder.
 */
static bool level_load(level_t *lv)
{
    memset(lv, 0, sizeof(*lv));
    lv->plen = strlen(s_path);

    if (!mdir_open(s_path)) {
        ESP_LOGW(TAG, "cannot open %s", s_path);
        return false;
    }

    bool ok = true, any_sheet = false;
    for (;;) {
        mdir_ent_t e;
        const int got = mdir_next(&e);
        if (got < 0) {
            ESP_LOGW(TAG, "reading %s failed", s_path);
            ok = false;
            break;
        }
        if (got == 0) break;
        if (storage_is_hidden(e.name)) continue;

        if (e.is_dir) {
            ok = level_add(lv, e.name, K_DIR, 0, e.stamp);
        } else if (cue_is_sheet(e.name)) {
            any_sheet = true;   /* not a track: its tracks are */
            ok = level_add(lv, e.name, K_SHEET, 0, e.stamp);
        } else if (decoder_supports(e.name)) {
            ok = level_add(lv, e.name, K_FILE, 0, e.stamp);
        }
        if (!ok) {
            ESP_LOGW(TAG, "%s: more than %d entries, or no memory; "
                     "not indexing a partial folder", s_path, MWALK_DIR_MAX);
            break;
        }
    }
    mdir_close();
    if (!ok) return false;

    /*
     * Sheets become their tracks, and the audio they cover stops being a
     * track of its own, by the one derivation the chooser and the
     * decoder use (cuedir.h). Covered audio is kept, marked, for its
     * stamp.
     */
    if (any_sheet) {
        lv->cues = cuedir_load(s_path, CLS);
        if (lv->cues) {
            for (int i = 0; i < lv->n; i++) {
                if (lv->ents[i].kind == K_FILE &&
                    cuedir_hides(lv->cues, lv->arena + lv->ents[i].name)) {
                    lv->ents[i].kind = K_COVERED;
                }
            }
            const midx_stamp_t none = { 0, 0 };
            for (int i = 0; i < cuedir_count(lv->cues) && ok; i++) {
                ok = level_add(lv, cuedir_name(lv->cues, i), K_CUE, i, none);
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

/* An entry of this folder by name, or NULL. The entries are sorted. */
static const ent_t *level_find(const level_t *lv, const char *name,
                               size_t len)
{
    static char key[MIDX_PATH_MAX + 1];     /* not the stack */
    int lo = 0, hi = lv->n;
    if (len > MIDX_PATH_MAX) return NULL;
    memcpy(key, name, len);
    key[len] = '\0';
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        const int c = midx_name_cmp(lv->arena + lv->ents[mid].name, key);
        if (c == 0) return &lv->ents[mid];
        if (c < 0) lo = mid + 1;
        else       hi = mid;
    }
    return NULL;
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

/* A cue track's stamp: the sheet's and its audio's together, both from
 * this folder's listing. */
static bool cue_stamp(const level_t *lv, const ent_t *e, midx_stamp_t *out)
{
    const char *vname = lv->arena + e->name;
    const char *hash = strrchr(vname, CUE_VPATH_SEP);
    const char *audio = cuedir_audio(lv->cues, e->cue);
    if (!hash || !audio) return false;

    const ent_t *a = level_find(lv, vname, (size_t)(hash - vname));
    const ent_t *b = level_find(lv, audio, strlen(audio));
    if (!a || !b) return false;

    out->mtime = a->stamp.mtime > b->stamp.mtime ? a->stamp.mtime
                                                 : b->stamp.mtime;
    out->size = a->stamp.size + b->stamp.size;
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
        if (e->kind == K_SHEET || e->kind == K_COVERED) continue;

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

        midx_stamp_t st = e->stamp;
        if (e->kind == K_CUE && !cue_stamp(lv, e, &st)) {
            /* The sheet or its audio is not in the listing the sheet
             * was resolved against: the folder changed between the two
             * reads, or cuedir saw something this did not. Either way
             * this track's stamp cannot be made, and a walk that skips
             * a track reads it as deleted. */
            ESP_LOGW(TAG, "no stamp for %s", s_path);
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
