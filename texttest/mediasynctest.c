/*
 * mediasynctest -- the real main/mediasync.c, reconciling a modelled
 * card against real index files in /tmp.
 *
 * The engine takes its catalog, tag reader and walk as functions, so
 * here the catalog is an array (cJSON is not on the host; mediacat.c
 * is checked separately, see ARCHITECTURE.md 5012) and the walk is a
 * list. Everything the engine does with FILEs -- read the old index,
 * write the new one, remove, rename -- is real.
 *
 * What is checked is what mediasync.h promises:
 *
 *   - after a complete run, the index on disk is every path the card
 *     and the old index knew, in order, each pointing at a catalog line
 *     for that path and stamp, tombstones flagged dead;
 *   - an unchanged card writes no line and reads no tag, and leaves the
 *     index byte-identical;
 *   - a removed track is buried once, a returning one revived with the
 *     tags it had and no tag read, a changed one read again;
 *   - a failed walk, a stop, disorder, or a catalog that refuses a line
 *     leaves the old index byte-identical and no temporary file;
 *   - a damaged old index -- a bad record, a ragged size, a long path
 *     the catalog does not confirm -- fails the run and removes the
 *     index, and the next run rebuilds from nothing;
 *   - a tombstone's catalog line going missing still buries correctly;
 *   - the lease is never nested and always returned.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mediasync.h"
#include "storage_io.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                            \
    checks++;                                                            \
    if (!(cond)) {                                                       \
        failures++;                                                      \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                    \
        printf(__VA_ARGS__);                                             \
        printf("\n");                                                    \
    }                                                                    \
} while (0)

/* ---- the arbiter, stubbed ------------------------------------------- */

static int s_depth, s_nested;
void storage_io_acquire(storage_io_class_t c) { (void)c; if (s_depth++) s_nested++; }
void storage_io_release(void) { s_depth--; }
FILE *storage_io_open(const char *p, const char *m) { return fopen(p, m); }
int storage_io_close(FILE *f) { return fclose(f); }
size_t storage_io_fread(void *d, size_t n, FILE *f, storage_io_class_t c)
{
    (void)c;
    if (s_depth) s_nested++;            /* must not be called holding it */
    return fread(d, 1, n, f);
}

/* ---- a catalog in memory -------------------------------------------- */

#define CATMAX  (20000)
static mediacat_rec_t s_cat[CATMAX];
static int  s_ncat;
static int  s_appends, s_tagreads, s_fail_append_at = -1;
static int  s_lose_line = -1;           /* this line reads as missing */

#define OFF(i)  ((uint32_t)(i) * 1000u + 17u)

static bool cat_append(void *ctx, const mediacat_rec_t *r, uint32_t *off)
{
    (void)ctx;
    if (s_fail_append_at >= 0 && s_appends >= s_fail_append_at) return false;
    if (s_ncat >= CATMAX) return false;
    s_cat[s_ncat] = *r;
    *off = OFF(s_ncat);
    s_ncat++;
    s_appends++;
    return true;
}

static bool cat_read(void *ctx, uint32_t off, mediacat_rec_t *out)
{
    (void)ctx;
    if (off < 17 || (off - 17) % 1000) return false;
    const int i = (int)((off - 17) / 1000);
    if (i >= s_ncat || i == s_lose_line) return false;
    *out = s_cat[i];
    return true;
}

static void tags(void *ctx, const char *path, mediacat_rec_t *r)
{
    (void)ctx;
    s_tagreads++;
    snprintf(r->title, sizeof(r->title), "T%d %.50s", s_tagreads, path);
    snprintf(r->artist, sizeof(r->artist), "artist");
}

/* ---- a card, as a list ---------------------------------------------- */

typedef struct { char path[MIDX_PATH_MAX + 1]; midx_stamp_t st; } ent_t;

#define CARDMAX (4000)
static ent_t s_card[CARDMAX];
static int   s_ncard;
static int   s_walk_fail_at = -1, s_abort_at = -1;
static bool  s_swap;                    /* offer two entries out of order */
static volatile bool s_abort;

static mwalk_result_t walk(void *ctx, mwalk_fn fn, void *wctx)
{
    (void)ctx;
    for (int i = 0; i < s_ncard; i++) {
        if (i == s_walk_fail_at) return MWALK_FAILED;
        if (i == s_abort_at) s_abort = true;
        int k = i;
        if (s_swap && i == 5) k = 6;
        else if (s_swap && i == 6) k = 5;
        if (!fn(wctx, s_card[k].path, s_card[k].st)) return MWALK_STOPPED;
    }
    return MWALK_DONE;
}

static int path_qsort(const void *a, const void *b)
{
    return midx_path_cmp(((const ent_t *)a)->path, ((const ent_t *)b)->path);
}

/* ---- running it, and reading what it left --------------------------- */

static char s_idx[256], s_tmp[256];
static msync_stats_t s_st;
static int64_t s_now = 1000;

static msync_result_t run(void)
{
    s_appends = s_tagreads = 0;
    s_abort = false;
    s_now += 10;
    const msync_ops_t ops = {
        cat_append, cat_read, tags, walk, NULL,
        s_idx, s_tmp, s_now, MIDX_CLOCK_SYNCED, &s_abort,
    };
    return msync_run(&ops, &s_st);
}

static uint8_t *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { *n = -1; return NULL; }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n + 1);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/*
 * The index on disk agrees with the card: every card path live with its
 * stamp, every other path dead, all in order, every record pointing at a
 * catalog line for itself. Returns the number of records.
 */
static int verify(const char *what, int expect_dead)
{
    long n;
    uint8_t *b = slurp(s_idx, &n);
    if (!b) { CHECK(0, "%s: no index", what); return -1; }
    CHECK(n % MIDX_REC_SIZE == 0, "%s: ragged index", what);
    const int nrec = (int)(n / MIDX_REC_SIZE);
    int bad = 0, live = 0, dead = 0, ci = 0;
    char prev[MIDX_PATH_MAX + 1] = "";
    for (int i = 0; i < nrec; i++) {
        midx_rec_t r;
        if (!midx_rec_unpack(b + (size_t)i * MIDX_REC_SIZE, &r)) { bad++; continue; }
        mediacat_rec_t line;
        if (!cat_read(NULL, r.cat_off, &line)) { bad++; continue; }
        const char *path = line.path;
        if (strlen(path) != r.path_len ||
            memcmp(path, r.key, r.path_len < MIDX_KEY_LEN ? r.path_len : MIDX_KEY_LEN)) bad++;
        if (!midx_stamp_eq(line.stamp, r.stamp)) bad++;
        if (i && !midx_in_order(prev, path)) bad++;
        strcpy(prev, path);
        const bool is_dead = r.flags & MIDX_F_DEAD;
        if (is_dead != (line.deleted_at != 0)) bad++;
        if (is_dead) {
            dead++;
            /* not on the card */
            for (int k = 0; k < s_ncard; k++) if (!strcmp(s_card[k].path, path)) bad++;
        } else {
            live++;
            /* the card, in the same order */
            while (ci < s_ncard && midx_path_cmp(s_card[ci].path, path) < 0) { bad++; ci++; }
            if (ci >= s_ncard || strcmp(s_card[ci].path, path) ||
                !midx_stamp_eq(s_card[ci].st, r.stamp)) bad++;
            ci++;
        }
    }
    free(b);
    CHECK(bad == 0, "%s: %d records wrong", what, bad);
    CHECK(live == s_ncard, "%s: %d live for %d on the card", what, live, s_ncard);
    if (expect_dead >= 0)
        CHECK(dead == expect_dead, "%s: %d dead, want %d", what, dead, expect_dead);
    CHECK(!exists(s_tmp), "%s: temporary index left behind", what);
    CHECK(s_depth == 0 && s_nested == 0, "%s: lease depth %d, %d nested",
          what, s_depth, s_nested);
    return nrec;
}

/* The old index must come through a failure byte for byte. */
static void unchanged_since(const uint8_t *before, long nbefore, const char *what)
{
    long n;
    uint8_t *b = slurp(s_idx, &n);
    CHECK(b && n == nbefore && !memcmp(b, before, (size_t)n),
          "%s: the old index changed", what);
    CHECK(!exists(s_tmp), "%s: temporary index left behind", what);
    CHECK(s_depth == 0 && s_nested == 0, "%s: lease", what);
    free(b);
}

static void build_card(int n, uint32_t seed)
{
    s_ncard = 0;
    char longdir[200];
    memset(longdir, 'L', 110);
    longdir[110] = 0;
    for (int i = 0; i < n && s_ncard < CARDMAX; i++) {
        ent_t *e = &s_card[s_ncard++];
        switch (i % 5) {
        case 0: snprintf(e->path, sizeof(e->path), "a/%04d.flac", i); break;
        case 1: snprintf(e->path, sizeof(e->path), "a b/%04d.flac", i); break;
        case 2: snprintf(e->path, sizeof(e->path), "%s/%04d.flac", longdir, i); break;
        case 3: snprintf(e->path, sizeof(e->path), "Album/Album.cue#%02d", i % 99 + 1); break;
        default: snprintf(e->path, sizeof(e->path), "z/%u/%04d.mp3", seed % 7, i); break;
        }
        e->st.mtime = 1700000000 + i;
        e->st.size = 1000 + (uint64_t)i;
    }
    qsort(s_card, (size_t)s_ncard, sizeof(ent_t), path_qsort);
    /* The cue names repeat; keep the first of each. */
    int w = 0;
    for (int i = 0; i < s_ncard; i++)
        if (!w || strcmp(s_card[w - 1].path, s_card[i].path)) s_card[w++] = s_card[i];
    s_ncard = w;
}

int main(void)
{
    printf("mediasynctest\n");
    char dir[] = "/tmp/msyncXXXXXX";
    if (!mkdtemp(dir)) return 1;
    snprintf(s_idx, sizeof(s_idx), "%s/.defeatist.ix1", dir);
    snprintf(s_tmp, sizeof(s_tmp), "%s/.defeatist.ixn", dir);

    printf("  first run: everything added\n");
    build_card(600, 1);
    const int n0 = s_ncard;
    CHECK(run() == MSYNC_DONE, "first run");
    CHECK(s_st.add == n0 && s_tagreads == n0 && s_appends == n0,
          "added %d, read %d tags, appended %d, for %d", s_st.add, s_tagreads,
          s_appends, n0);
    verify("first run", 0);

    printf("  unchanged: nothing written, nothing read\n");
    long nb;
    uint8_t *before = slurp(s_idx, &nb);
    CHECK(run() == MSYNC_DONE, "unchanged run");
    CHECK(s_st.keep == n0 && s_appends == 0 && s_tagreads == 0,
          "keep %d appends %d tags %d", s_st.keep, s_appends, s_tagreads);
    unchanged_since(before, nb, "unchanged run");
    int longs = 0;
    for (int i = 0; i < s_ncard; i++) longs += strlen(s_card[i].path) > MIDX_KEY_LEN;
    CHECK(longs > 0 && s_st.cat_reads == longs,
          "catalog read %d times for %d long paths", s_st.cat_reads, longs);
    free(before);

    printf("  removed, changed, added\n");
    static ent_t orig[CARDMAX];
    memcpy(orig, s_card, sizeof(ent_t) * (size_t)s_ncard);
    const int norig = s_ncard;
    int removed = 0, changed = 0;
    {
        int w = 0;
        for (int i = 0; i < s_ncard; i++) {
            if (i % 7 == 3) { removed++; continue; }
            s_card[w] = s_card[i];
            if (i % 11 == 5) { s_card[w].st.size += 1; changed++; }
            w++;
        }
        s_ncard = w;
        /* and three new ones */
        strcpy(s_card[s_ncard++].path, "new/1.flac");
        strcpy(s_card[s_ncard++].path, "a/new.flac");
        strcpy(s_card[s_ncard++].path, "a b/new.flac");
        for (int k = 1; k <= 3; k++) s_card[s_ncard - k].st = (midx_stamp_t){ 5, 5 };
        qsort(s_card, (size_t)s_ncard, sizeof(ent_t), path_qsort);
    }
    CHECK(run() == MSYNC_DONE, "change run");
    CHECK(s_st.bury == removed && s_st.update == changed && s_st.add == 3,
          "bury %d/%d update %d/%d add %d/3", s_st.bury, removed,
          s_st.update, changed, s_st.add);
    CHECK(s_tagreads == changed + 3, "%d tag reads for %d", s_tagreads, changed + 3);
    verify("change run", removed);
    /* the tombstones carry this run's time */
    {
        long n;
        uint8_t *b = slurp(s_idx, &n);
        int wrong = 0;
        for (long i = 0; i < n / MIDX_REC_SIZE; i++) {
            midx_rec_t r;
            mediacat_rec_t line;
            midx_rec_unpack(b + i * MIDX_REC_SIZE, &r);
            if ((r.flags & MIDX_F_DEAD) && cat_read(NULL, r.cat_off, &line) &&
                line.deleted_at != s_now) wrong++;
        }
        CHECK(wrong == 0, "%d tombstones with the wrong time", wrong);
        free(b);
    }

    printf("  the dead stay buried once\n");
    const int cat_before = s_ncat;
    CHECK(run() == MSYNC_DONE, "rerun");
    CHECK(s_appends == 0 && s_ncat == cat_before, "a rerun appended %d", s_appends);
    verify("rerun", removed);

    printf("  the original card comes back\n");
    memcpy(s_card, orig, sizeof(ent_t) * (size_t)norig);
    s_ncard = norig;
    CHECK(run() == MSYNC_DONE, "restore");
    CHECK(s_st.revive == removed, "revived %d of %d", s_st.revive, removed);
    CHECK(s_st.update == changed, "re-read %d of %d changed", s_st.update, changed);
    CHECK(s_tagreads == changed, "%d tag reads: revives must not read", s_tagreads);
    verify("restore", 3);
    /* a revived track has the tags it was first given */
    {
        int kept = 0, tot = 0;
        for (int i = 0; i < norig; i++) {
            if (i % 7 != 3) continue;
            tot++;
            for (int k = s_ncat - 1; k >= 0; k--)
                if (!strcmp(s_cat[k].path, orig[i].path)) {
                    kept += !strncmp(s_cat[k].title, "T", 1) &&
                            s_cat[k].deleted_at == 0;
                    break;
                }
        }
        CHECK(kept == tot, "%d of %d revived tracks have their tags", kept, tot);
    }

    printf("  anything short of complete changes nothing\n");
    before = slurp(s_idx, &nb);
    {
        /* Drop a few from the card so a complete run WOULD write. */
        s_ncard -= 50;

        /* An incomplete walk has not looked at what it did not reach,
         * so it must not bury it -- not even as lines nothing points
         * at. */
        s_walk_fail_at = 20;
        CHECK(run() == MSYNC_FAILED, "failed walk");
        CHECK(s_st.bury == 0, "a failed walk buried %d", s_st.bury);
        unchanged_since(before, nb, "failed walk");
        s_walk_fail_at = -1;

        s_abort_at = 30;
        CHECK(run() == MSYNC_STOPPED, "abort");
        CHECK(s_st.bury == 0, "a stopped walk buried %d", s_st.bury);
        unchanged_since(before, nb, "abort");
        s_abort_at = -1;

        s_swap = true;
        CHECK(run() == MSYNC_FAILED, "disorder");
        unchanged_since(before, nb, "disorder");
        s_swap = false;

        s_fail_append_at = 2;
        CHECK(run() == MSYNC_FAILED, "catalog refused");
        unchanged_since(before, nb, "catalog refused");
        s_fail_append_at = -1;

        s_ncard += 50;
    }
    free(before);

    printf("  a tombstone whose line is gone\n");
    {
        /* The live line of the first "a/" track goes missing, then the
         * track leaves the card. */
        long n;
        uint8_t *b = slurp(s_idx, &n);
        midx_rec_t r;
        CHECK(b && n >= MIDX_REC_SIZE && midx_rec_unpack(b, &r),
              "no index to lose a line from");
        if (!b || n < MIDX_REC_SIZE || !midx_rec_unpack(b, &r)) { free(b); goto out; }
        s_lose_line = (int)((r.cat_off - 17) / 1000);
        free(b);
        char gone[MIDX_PATH_MAX + 1];
        strcpy(gone, s_card[0].path);
        memmove(s_card, s_card + 1, sizeof(ent_t) * (size_t)(s_ncard - 1));
        s_ncard--;
        CHECK(run() == MSYNC_DONE, "bury without a line");
        CHECK(s_st.bury == 1, "buried %d", s_st.bury);
        s_lose_line = -1;
        verify("bury without a line", -1);
    }

    printf("  a damaged index is removed and rebuilt\n");
    {
        long n;
        uint8_t *b = slurp(s_idx, &n);
        CHECK(b && n > 4 * MIDX_REC_SIZE, "no index to damage");
        if (!b || n <= 4 * MIDX_REC_SIZE) { free(b); goto out; }
        b[3 * MIDX_REC_SIZE + 107] = 1;         /* a reserved byte */
        FILE *f = fopen(s_idx, "wb");
        fwrite(b, 1, (size_t)n, f);
        fclose(f);
        free(b);
        CHECK(run() == MSYNC_FAILED && s_st.index_damaged, "bad record");
        CHECK(!exists(s_idx), "a damaged index was left in place");
        CHECK(run() == MSYNC_DONE && s_st.add == s_ncard,
              "rebuild added %d of %d", s_st.add, s_ncard);
        verify("rebuild", 0);

        /* ragged */
        f = fopen(s_idx, "ab");
        fputc('x', f);
        fclose(f);
        CHECK(run() == MSYNC_FAILED && s_st.index_damaged && !exists(s_idx),
              "ragged index");
        CHECK(run() == MSYNC_DONE, "rebuild after ragged");

        /*
         * A long path the catalog contradicts, inside the key -- and
         * contradicted so that it still SORTS in place, or the order
         * check would catch it first and this would test nothing else.
         * The last of the long "LLL..." paths, with an L at byte 50
         * made an M: still after every other L path, still before "a/".
         */
        b = slurp(s_idx, &n);
        int victim = -1;
        for (long i = 0; b && i < n / MIDX_REC_SIZE; i++) {
            midx_rec_t r;
            if (midx_rec_unpack(b + i * MIDX_REC_SIZE, &r) &&
                r.path_len > MIDX_KEY_LEN) victim = (int)((r.cat_off - 17) / 1000);
        }
        free(b);
        CHECK(victim >= 0, "no long path to damage");
        if (victim >= 0) s_cat[victim].path[50] = 'M';
        CHECK(run() == MSYNC_FAILED && s_st.index_damaged && !exists(s_idx),
              "a contradicted long path was believed");
        if (victim >= 0) s_cat[victim].path[50] = 'L';
        CHECK(run() == MSYNC_DONE, "rebuild after contradiction");
        verify("final", 0);
    }

out:;
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("  (could not remove %s)\n", dir);

    if (failures) {
        printf("FAILURES: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("all passed: %d checks, 0 failures\n", checks);
    return 0;
}
