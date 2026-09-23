/*
 * mediaindextest -- the order the media index keeps paths in, and the
 * merge-join that reconciles it against a card.
 *
 * Both are the kind of wrong that is invisible on the board. An order
 * that disagrees with the walk does not crash: it tombstones a folder
 * and re-adds it, re-reading every tag, on every walk, and the only
 * symptom is a reconcile that is slow for no reason anybody can see.
 * A step that re-buries a dead file does not crash either: it appends
 * a line per dead file per walk, for ever.
 *
 * So the checks are properties over a generated card rather than a few
 * hand-picked paths:
 *
 *   1. midx_path_cmp() is a total order, and '/' sorts below every
 *      byte that can be in a name.
 *   2. A depth-first walk with each directory sorted by midx_name_cmp()
 *      comes out strictly increasing under midx_path_cmp() -- the
 *      property the merge-join rests on. And the same walk is NOT in
 *      strcmp() order, so the corpus demonstrably reaches the trap
 *      the header describes rather than passing by never meeting it.
 *   3. A reconcile driven by midx_step() leaves a catalog whose live
 *      records are exactly the card; reads tags only for ADD and
 *      UPDATE; does nothing at all on an unchanged second walk; keeps a
 *      tombstone's original time; and revives a returning file without
 *      reading it.
 *   4. midx_in_order() refuses disorder and a repeat.
 *
 * Header-only, like cardtimetest: this compiles mediaindex.h itself,
 * so there is nothing to drift from.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mediaindex.h"

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

static int sgn(int v) { return (v > 0) - (v < 0); }

/* ---- a deterministic generator -------------------------------------- */

static uint32_t s_rng = 12345u;
static uint32_t rnd(uint32_t n)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 8) % n;
}

/*
 * Name fragments chosen to straddle '/': space, '!', '#', '-', '.' are
 * all below 0x2F, '0' 'A' '_' 'a' above it, and "\xC3\xA9" (e-acute)
 * has its high bit set, which a signed compare would sort first. "a"
 * appears alone so that "a" and "a b" style prefixes are common.
 */
static const char *const FRAG[] = {
    "a", " ", "!", "#", "-", ".", "0", "A", "_", "b", "\xC3\xA9", "a",
};
#define NFRAG   (sizeof(FRAG) / sizeof(FRAG[0]))

#define MAXPATHS    (4096)
#define PATHLEN     (256)

typedef struct {
    char        path[PATHLEN];
    midx_stamp_t stamp;
} file_t;

static file_t s_card[MAXPATHS];
static int    s_ncard;

static int name_qsort(const void *a, const void *b)
{
    return midx_name_cmp(*(const char *const *)a, *(const char *const *)b);
}

/* Build one directory's names, unique, then walk them in sorted order:
 * files emitted, folders descended -- what the real walk will do. */
static void gen_dir(const char *prefix, int depth)
{
    char  names[8][16];
    int   isdir[8];
    const int n = 1 + (int)rnd(6);
    int   have = 0;
    for (int tries = 0; have < n && tries < 40; tries++) {
        char nm[16] = "";
        const int parts = 1 + (int)rnd(3);
        for (int p = 0; p < parts; p++) strcat(nm, FRAG[rnd(NFRAG)]);
        int dup = 0;
        for (int i = 0; i < have; i++) dup |= strcmp(names[i], nm) == 0;
        if (dup) continue;
        strcpy(names[have], nm);
        isdir[have] = depth < 3 && rnd(3) == 0;
        have++;
    }

    const char *order[8];
    for (int i = 0; i < have; i++) order[i] = names[i];
    qsort(order, (size_t)have, sizeof(order[0]), name_qsort);

    for (int k = 0; k < have; k++) {
        const int i = (int)(order[k] - names[0]) / (int)sizeof(names[0]);
        char full[PATHLEN];
        snprintf(full, sizeof(full), "%s%s%s", prefix, *prefix ? "/" : "",
                 names[i]);
        if (isdir[i]) {
            gen_dir(full, depth + 1);
        } else if (s_ncard < MAXPATHS) {
            strcpy(s_card[s_ncard].path, full);
            s_card[s_ncard].stamp.mtime = 1700000000 + (int64_t)rnd(100000);
            s_card[s_ncard].stamp.size  = 1000 + rnd(1u << 20);
            s_ncard++;
        }
    }
}

/* ---- a catalog, modelled -------------------------------------------- */

/*
 * The latest record per path, kept sorted -- the shape the index hands
 * the merge. `appends` counts the lines the real catalog would gain,
 * and `tag_reads` the files it would have to open.
 */
typedef struct {
    char        path[PATHLEN];
    midx_stamp_t stamp;
    int64_t     deleted_at;
    int         tags;           /* which read produced the tags */
} rec_t;

static rec_t s_cat[MAXPATHS * 2];
static int   s_ncat;
static int   s_appends, s_tag_reads, s_tag_serial;
static int   s_count[MIDX_BURY + 1];

static rec_t s_next[MAXPATHS * 2];

/*
 * One reconcile, as the real one will run: walk both sides in step,
 * check order on everything taken, append what midx_step() says.
 * Returns false if it stopped on disorder.
 */
static bool reconcile(const file_t *card, int ncard, int64_t now)
{
    int ci = 0, xi = 0, nn = 0;
    const char *prev_card = NULL, *prev_cat = NULL;
    memset(s_count, 0, sizeof(s_count));
    s_appends = s_tag_reads = 0;

    for (;;) {
        midx_card_t cv, *cp = NULL;
        midx_cat_t  xv, *xp = NULL;
        if (ci < ncard) {
            if (!midx_in_order(prev_card, card[ci].path)) return false;
            cv.path = card[ci].path;
            cv.stamp = card[ci].stamp;
            cp = &cv;
        }
        if (xi < s_ncat) {
            if (!midx_in_order(prev_cat, s_cat[xi].path)) return false;
            xv.path = s_cat[xi].path;
            xv.stamp = s_cat[xi].stamp;
            xv.deleted_at = s_cat[xi].deleted_at;
            xp = &xv;
        }

        const midx_step_t st = midx_step(cp, xp);
        if (st.action == MIDX_DONE) break;
        s_count[st.action]++;

        rec_t r;
        switch (st.action) {
        case MIDX_ADD:
        case MIDX_UPDATE:
            strcpy(r.path, cp->path);
            r.stamp = cp->stamp;
            r.deleted_at = 0;
            r.tags = ++s_tag_serial;
            s_tag_reads++;
            s_appends++;
            break;
        case MIDX_REVIVE:
            r = s_cat[xi];
            r.deleted_at = 0;               /* tags carried, not read */
            s_appends++;
            break;
        case MIDX_BURY:
            r = s_cat[xi];
            r.deleted_at = now;
            s_appends++;
            break;
        case MIDX_KEEP:
        default:
            r = s_cat[xi];                  /* KEEP always takes the index */
            break;
        }
        s_next[nn++] = r;

        if (st.take_card) prev_card = card[ci++].path;
        if (st.take_cat)  prev_cat  = s_cat[xi++].path;
        if (!st.take_card && !st.take_cat) {
            CHECK(0, "a step that advanced neither side");
            return false;
        }
    }
    memcpy(s_cat, s_next, sizeof(rec_t) * (size_t)nn);
    s_ncat = nn;
    return true;
}

static int live_count(void)
{
    int n = 0;
    for (int i = 0; i < s_ncat; i++) n += s_cat[i].deleted_at == 0;
    return n;
}

/* The live records are exactly the card, path and stamp. */
static bool live_matches(const file_t *card, int ncard)
{
    int j = 0;
    for (int i = 0; i < s_ncat; i++) {
        if (s_cat[i].deleted_at) continue;
        if (j >= ncard) return false;
        if (strcmp(s_cat[i].path, card[j].path) != 0) return false;
        if (!midx_stamp_eq(s_cat[i].stamp, card[j].stamp)) return false;
        j++;
    }
    return j == ncard;
}

static const rec_t *cat_find(const char *path)
{
    for (int i = 0; i < s_ncat; i++)
        if (strcmp(s_cat[i].path, path) == 0) return &s_cat[i];
    return NULL;
}

/* ---- the checks ----------------------------------------------------- */

static void check_order(void)
{
    printf("  the order\n");

    /* The case in the header, exactly. */
    CHECK(midx_path_cmp("a/x.flac", "a b/y.flac") < 0,
          "a/ must sort before a b/");
    CHECK(strcmp("a/x.flac", "a b/y.flac") > 0,
          "the premise: strcmp puts a b/ first");
    CHECK(midx_path_cmp("a", "a b") < 0, "a name before its extension");
    CHECK(midx_path_cmp("a", "a/x") < 0, "a folder before its contents");
    CHECK(midx_path_cmp("", "a") < 0, "empty first");
    CHECK(midx_path_cmp("x", "x") == 0, "equal is equal");
    CHECK(midx_path_cmp("Album.cue#03", "Album.cue#10") < 0,
          "two-digit cue tracks sort as tracks");

    /* '/' below every byte a name can hold, both ways round. */
    for (int b = 1; b < 256; b++) {
        if (b == '/') continue;
        char n[3] = { 'a', (char)b, 0 };
        CHECK(midx_path_cmp("a/", n) < 0 && midx_path_cmp(n, "a/") > 0,
              "'/' did not sort below 0x%02X", b);
    }

    /* Unsigned: UTF-8 after ASCII, not before it. */
    CHECK(midx_path_cmp("z", "\xC3\xA9") < 0, "a high byte sorted as signed");

    /* A total order on the fragments: antisymmetric, transitive. */
    const char *s[64];
    char buf[64][16];
    for (int i = 0; i < 64; i++) {
        buf[i][0] = 0;
        const int parts = 1 + (int)rnd(3);
        for (int p = 0; p < parts; p++) {
            strcat(buf[i], FRAG[rnd(NFRAG)]);
            if (rnd(4) == 0) strcat(buf[i], "/");
        }
        s[i] = buf[i];
    }
    int bad = 0;
    for (int i = 0; i < 64; i++)
        for (int j = 0; j < 64; j++) {
            const int ij = sgn(midx_path_cmp(s[i], s[j]));
            const int ji = sgn(midx_path_cmp(s[j], s[i]));
            if (ij != -ji) bad++;
            if ((ij == 0) != (strcmp(s[i], s[j]) == 0)) bad++;
            for (int k = 0; k < 64; k++)
                if (ij < 0 && midx_path_cmp(s[j], s[k]) < 0 &&
                    midx_path_cmp(s[i], s[k]) >= 0) bad++;
        }
    CHECK(bad == 0, "not a total order: %d violations", bad);
}

static void check_walk(void)
{
    printf("  the walk comes out in the order\n");
    int trap = 0;
    for (int round = 0; round < 50; round++) {
        s_ncard = 0;
        gen_dir("", 0);
        for (int i = 1; i < s_ncard; i++) {
            CHECK(midx_in_order(s_card[i - 1].path, s_card[i].path),
                  "round %d: walk out of order at \"%s\" then \"%s\"",
                  round, s_card[i - 1].path, s_card[i].path);
            if (strcmp(s_card[i - 1].path, s_card[i].path) > 0) trap++;
        }
    }
    /* If this is zero the corpus never met the case the order exists
     * for, and the check above proved nothing. */
    CHECK(trap > 0, "the corpus never produced a walk strcmp disagrees with");
    printf("    (%d adjacent pairs strcmp would have got backwards)\n", trap);
}

static void check_reconcile(void)
{
    printf("  the reconcile\n");
    static file_t card[MAXPATHS];

    for (int round = 0; round < 30; round++) {
        s_ncard = 0;
        gen_dir("", 0);
        const int n = s_ncard;
        memcpy(card, s_card, sizeof(file_t) * (size_t)n);
        s_ncat = 0;
        s_tag_serial = 0;

        /* First walk of a new card: everything ADD. */
        CHECK(reconcile(card, n, 1000), "round %d: stopped", round);
        CHECK(s_count[MIDX_ADD] == n && s_tag_reads == n,
              "round %d: first walk added %d of %d", round,
              s_count[MIDX_ADD], n);
        CHECK(live_matches(card, n), "round %d: first walk mismatch", round);

        /* Second walk, nothing changed: nothing written at all. */
        CHECK(reconcile(card, n, 2000), "round %d: stopped", round);
        CHECK(s_appends == 0 && s_count[MIDX_KEEP] == n,
              "round %d: an unchanged walk appended %d", round, s_appends);

        /* Change a third, remove a third. */
        static file_t less[MAXPATHS];
        int m = 0, changed = 0, removed = 0;
        for (int i = 0; i < n; i++) {
            const uint32_t r = rnd(3);
            if (r == 0) { removed++; continue; }
            less[m] = card[i];
            if (r == 1) {
                /* Alternate which half of the stamp moves: each alone
                 * must be enough. */
                if (changed & 1) less[m].stamp.size++;
                else             less[m].stamp.mtime += 2;
                changed++;
            }
            m++;
        }
        CHECK(reconcile(less, m, 3000), "round %d: stopped", round);
        CHECK(s_count[MIDX_BURY] == removed, "round %d: buried %d of %d",
              round, s_count[MIDX_BURY], removed);
        CHECK(s_count[MIDX_UPDATE] == changed, "round %d: updated %d of %d",
              round, s_count[MIDX_UPDATE], changed);
        CHECK(s_tag_reads == changed,
              "round %d: %d tag reads for %d changes", round, s_tag_reads,
              changed);
        CHECK(live_matches(less, m), "round %d: live != card", round);
        CHECK(live_count() == m, "round %d: live count", round);

        /* Walk again with the same files gone: the tombstones keep
         * their time and nothing is appended. */
        CHECK(reconcile(less, m, 4000), "round %d: stopped", round);
        CHECK(s_appends == 0, "round %d: a dead file was buried again "
              "(%d appends)", round, s_appends);
        int kept_time = 1;
        for (int i = 0; i < s_ncat; i++)
            if (s_cat[i].deleted_at && s_cat[i].deleted_at != 3000)
                kept_time = 0;
        CHECK(kept_time, "round %d: a tombstone's time moved", round);

        /* The card comes back as it was. Unchanged files revive with the
         * tags they had; changed ones are read again. */
        int want_revive = 0, want_update = 0;
        static int revive_tags[MAXPATHS];
        for (int i = 0; i < n; i++) {
            const rec_t *r = cat_find(card[i].path);
            revive_tags[i] = -1;
            if (!r) continue;
            if (r->deleted_at && midx_stamp_eq(r->stamp, card[i].stamp)) {
                want_revive++;
                revive_tags[i] = r->tags;
            } else if (!midx_stamp_eq(r->stamp, card[i].stamp)) {
                want_update++;
            }
        }
        CHECK(reconcile(card, n, 5000), "round %d: stopped", round);
        CHECK(s_count[MIDX_REVIVE] == want_revive,
              "round %d: revived %d, wanted %d", round, s_count[MIDX_REVIVE],
              want_revive);
        CHECK(s_tag_reads == want_update,
              "round %d: %d tag reads, wanted %d (revives must not read)",
              round, s_tag_reads, want_update);
        CHECK(live_matches(card, n), "round %d: restored card mismatch",
              round);
        int kept_tags = 1;
        for (int i = 0; i < n; i++) {
            if (revive_tags[i] < 0) continue;
            const rec_t *r = cat_find(card[i].path);
            if (!r || r->tags != revive_tags[i]) kept_tags = 0;
        }
        CHECK(kept_tags, "round %d: a revived file lost its tags", round);
    }

    /* A tombstoned file that comes back CHANGED is read, not revived. */
    {
        s_ncat = 0;
        file_t f = { "x.flac", { 100, 10 } };
        reconcile(&f, 1, 1);
        reconcile(NULL, 0, 2);
        CHECK(s_count[MIDX_BURY] == 1, "single file not buried");
        f.stamp.size = 11;
        reconcile(&f, 1, 3);
        CHECK(s_count[MIDX_UPDATE] == 1 && s_count[MIDX_REVIVE] == 0,
              "a changed returnee was revived with stale tags");
        CHECK(s_cat[0].deleted_at == 0, "the update left it tombstoned");
    }

    /* Both sides empty is DONE at once, and advances nothing. */
    {
        const midx_step_t st = midx_step(NULL, NULL);
        CHECK(st.action == MIDX_DONE && !st.take_card && !st.take_cat,
              "empty merge did something");
    }
}

static void check_disorder(void)
{
    printf("  disorder stops the merge\n");
    CHECK(midx_in_order(NULL, "a"), "the first element is in order");
    CHECK(!midx_in_order("a", "a"), "a repeat was accepted");
    CHECK(!midx_in_order("a b/y", "a/x"), "strcmp order was accepted");

    s_ncat = 0;
    file_t good[3] = { { "a/x", {1, 1} }, { "a b/y", {1, 1} }, { "b", {1, 1} } };
    CHECK(reconcile(good, 3, 1), "an ordered walk stopped");
    file_t bad[3] = { { "a b/y", {1, 1} }, { "a/x", {1, 1} }, { "b", {1, 1} } };
    CHECK(!reconcile(bad, 3, 2), "a strcmp-ordered walk ran to the end");
}

int main(void)
{
    printf("mediaindextest\n");
    check_order();
    check_walk();
    check_reconcile();
    check_disorder();

    if (failures) {
        printf("FAILURES: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("all passed: %d checks, 0 failures\n", checks);
    return 0;
}
