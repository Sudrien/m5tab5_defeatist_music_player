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
 *   5. The index record packs and unpacks losslessly and refuses what
 *      the packer could not have written; a search finds every path in
 *      at most log2(n)+1 reads, reads the catalog only for a path longer
 *      than the key, agrees with a brute-force search on every query;
 *      a folder listing costs a search per child rather than a read per
 *      descendant; and a catalog that disagrees with the index is
 *      reported rather than believed.
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
#define PATHLEN     (MIDX_PATH_MAX + 1)

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

/* ---- the index file ------------------------------------------------- */

static uint8_t s_idx[MAXPATHS][MIDX_REC_SIZE];
static char    s_full[MAXPATHS][PATHLEN];     /* the catalog, by line */
static uint32_t s_nidx;
static int     s_reads, s_fullreads;
static int     s_lie = -1;                    /* line fullpath() lies about */
static char    s_scratch[PATHLEN];

#define OFF(i)      ((uint32_t)(i) * 1000u + 17u)

static bool mem_read(void *ctx, uint32_t i, midx_rec_t *out)
{
    (void)ctx;
    s_reads++;
    return i < s_nidx && midx_rec_unpack(s_idx[i], out);
}

static bool mem_fullpath(void *ctx, uint32_t off, char *buf, size_t n)
{
    (void)ctx;
    s_fullreads++;
    if (off < 17 || (off - 17) % 1000 != 0) return false;
    const uint32_t i = (off - 17) / 1000;
    if (i >= s_nidx) return false;
    if ((int)i == s_lie) {
        snprintf(buf, n, "%s", s_full[i]);
        buf[10] ^= 1;                   /* one byte off, inside the key */
        return true;
    }
    snprintf(buf, n, "%s", s_full[i]);
    return true;
}

static midx_src_t mem_src(void)
{
    midx_src_t src = { mem_read, mem_fullpath, NULL, s_nidx,
                       s_scratch, sizeof(s_scratch), false };
    return src;
}

static int path_qsort(const void *a, const void *b)
{
    return midx_path_cmp((const char *)a, (const char *)b);
}

static int log2up(uint32_t n)
{
    int b = 0;
    while ((1u << b) < n) b++;
    return b;
}

/* Brute force over the sorted paths. */
static uint32_t brute_at(const char *q)
{
    uint32_t i = 0;
    while (i < s_nidx && midx_path_cmp(s_full[i], q) < 0) i++;
    return i;
}

static uint32_t brute_past(const char *q)
{
    uint32_t i = 0;
    const size_t p = strlen(q);
    while (i < s_nidx && (midx_path_cmp(s_full[i], q) < 0 ||
                          strncmp(s_full[i], q, p) == 0)) i++;
    return i;
}

/*
 * A card with long paths as well as short: two folders whose names
 * alone run past the key and agree for longer than it, so paths under
 * them tie on all 104 bytes and can only be told apart by the catalog.
 * Plus paths of exactly KEY-1, KEY and KEY+1 bytes, the boundary.
 */
static void build_index(void)
{
    char longa[128], longb[128];
    memset(longa, 'L', 110);
    longa[110] = 0;
    strcpy(longb, longa);
    longb[108] = 'M';           /* differs past the key */

    /* Many small trees, so the index is thousands of records deep and a
     * search actually has to search. */
    s_ncard = 0;
    for (int t = 0; t < 400 && s_ncard < MAXPATHS / 2; t++) {
        char top[16];
        snprintf(top, sizeof(top), "t%03d", t);
        gen_dir(top, 1);
    }
    /* Every path under these ties with its neighbours on the whole key. */
    for (int t = 0; t < 40 && s_ncard < MAXPATHS - 100; t++) {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s/s%02d", (t & 1) ? longb : longa, t);
        gen_dir(sub, 1);
    }

    /* A folder of a few big folders -- an artist with long discs, or a
     * box set -- which is the shape the skip exists for: three children
     * with six hundred tracks beneath them. */
    for (int a = 0; a < 3; a++)
        for (int t = 0; t < 200 && s_ncard < MAXPATHS - 10; t++) {
            snprintf(s_card[s_ncard].path, PATHLEN, "Box Set/Disc %d/%03d.flac",
                     a + 1, t);
            s_card[s_ncard].stamp.mtime = 1;
            s_card[s_ncard].stamp.size = 1;
            s_ncard++;
        }

    for (int len = MIDX_KEY_LEN - 1; len <= MIDX_KEY_LEN + 1; len++) {
        memset(s_card[s_ncard].path, 'k', (size_t)len);
        s_card[s_ncard].path[len] = 0;
        s_card[s_ncard].stamp.mtime = -5;       /* before 1970: signed */
        s_card[s_ncard].stamp.size = (uint64_t)1 << 40;
        s_ncard++;
    }

    s_nidx = (uint32_t)s_ncard;
    for (uint32_t i = 0; i < s_nidx; i++) strcpy(s_full[i], s_card[i].path);
    qsort(s_full, s_nidx, PATHLEN, path_qsort);
    for (uint32_t i = 0; i < s_nidx; i++) {
        const midx_stamp_t st = { (int64_t)i * 3 - 7, (uint64_t)i * 11 };
        const bool ok = midx_rec_pack(s_idx[i], s_full[i], OFF(i),
                                      (i % 5 == 0) ? MIDX_F_DEAD : 0, st);
        CHECK(ok, "pack refused \"%s\"", s_full[i]);
    }
}

static void check_record(void)
{
    printf("  the index record\n");
    int bad = 0;
    for (uint32_t i = 0; i < s_nidx; i++) {
        midx_rec_t r;
        if (!midx_rec_unpack(s_idx[i], &r)) { bad++; continue; }
        const size_t len = strlen(s_full[i]);
        const size_t k = len < MIDX_KEY_LEN ? len : MIDX_KEY_LEN;
        if (r.path_len != len || strlen(r.key) != k ||
            memcmp(r.key, s_full[i], k) != 0 || r.cat_off != OFF(i) ||
            r.stamp.mtime != (int64_t)i * 3 - 7 ||
            r.stamp.size != (uint64_t)i * 11 ||
            r.flags != ((i % 5 == 0) ? MIDX_F_DEAD : 0)) bad++;
    }
    CHECK(bad == 0, "%d records did not survive a round trip", bad);

    uint8_t rec[MIDX_REC_SIZE];
    const midx_stamp_t z = { 0, 0 };
    char big[MIDX_PATH_MAX + 2];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    CHECK(!midx_rec_pack(rec, "", 0, 0, z), "packed an empty path");
    CHECK(!midx_rec_pack(rec, "/sd/a", 0, 0, z), "packed an absolute path");
    CHECK(!midx_rec_pack(rec, big, 0, 0, z), "packed a path past the max");
    big[MIDX_PATH_MAX] = 0;
    CHECK(midx_rec_pack(rec, big, 0, 0, z), "refused a path at the max");
    {
        midx_rec_t rb;
        CHECK(midx_rec_unpack(rec, &rb) && rb.path_len == MIDX_PATH_MAX,
              "a path at the max did not come back at its length");
    }
    CHECK(!midx_rec_pack(rec, "a", 0, 0x80, z), "packed an unknown flag");

    /* Everything the packer could not have written is refused. */
    midx_rec_t r;
    struct { const char *what; int at; uint8_t v; int longpath; } m[] = {
        { "reserved byte set",        107, 1,    0 },
        { "unknown flag",             106, 2,    0 },
        { "path_len zero",            104, 0,    0 },
        { "path_len past the max",    105, 2,    0 },
        { "byte in the padding",      50,  'z',  0 },
        { "NUL inside a long key",    50,  0,    1 },
        { "leading slash",            0,   '/',  0 },
    };
    for (size_t t = 0; t < sizeof(m) / sizeof(m[0]); t++) {
        midx_rec_pack(rec, m[t].longpath ? big : "short/path.flac", 1, 0, z);
        rec[m[t].at] = m[t].v;
        if (m[t].at == 104) rec[105] = 0;
        CHECK(!midx_rec_unpack(rec, &r), "unpacked a record with %s",
              m[t].what);
    }
}

static void check_lookup(void)
{
    printf("  looking things up\n");
    const int budget = log2up(s_nidx) + 1;
    int worst = 0, notfound = 0, catreads = 0;
    for (uint32_t i = 0; i < s_nidx; i++) {
        midx_src_t src = mem_src();
        midx_rec_t r;
        s_reads = s_fullreads = 0;
        const int64_t got = midx_find(&src, s_full[i], &r);
        if (got != (int64_t)i || src.err) notfound++;
        if (s_reads > worst) worst = s_reads;
        if (strlen(s_full[i]) <= MIDX_KEY_LEN) catreads += s_fullreads;
    }
    CHECK(notfound == 0, "%d of %u paths not found", notfound, s_nidx);
    CHECK(worst <= budget + 1, "a find took %d reads for %u records "
          "(budget %d)", worst, s_nidx, budget + 1);
    CHECK(catreads == 0, "short paths read the catalog %d times", catreads);

    /* Every query, present or not, agrees with brute force in both
     * modes: the paths themselves, and each one lengthened, shortened,
     * nudged a byte either way, and turned into a folder prefix. */
    int disagree = 0, falsehit = 0;
    char q[PATHLEN + 2];
    for (uint32_t i = 0; i < s_nidx; i++) {
        const size_t len = strlen(s_full[i]);
        for (int v = 0; v < 6; v++) {
            strcpy(q, s_full[i]);
            switch (v) {
            case 0: break;
            case 1: strcat(q, "x"); break;
            case 2: q[len - 1] = 0; break;
            case 3: q[len - 1]++; break;
            case 4: q[len - 1]--; break;
            case 5: {
                char *sl = strrchr(q, '/');
                if (sl) sl[1] = 0; else strcpy(q, "");
                break;
            }
            }
            midx_src_t src = mem_src();
            if (midx_seek(&src, q, MIDX_AT) != brute_at(q)) disagree++;
            if (midx_seek(&src, q, MIDX_PAST_PREFIX) != brute_past(q))
                disagree++;
            if (src.err) disagree++;
            midx_rec_t r;
            const int64_t f = midx_find(&src, q, &r);
            const uint32_t b = brute_at(q);
            const bool present = b < s_nidx && strcmp(s_full[b], q) == 0;
            if ((f >= 0) != present) falsehit++;
        }
    }
    CHECK(disagree == 0, "%d seeks disagreed with brute force", disagree);
    CHECK(falsehit == 0, "%d finds wrong about presence", falsehit);
}

/* Every folder that has a path under it, "" for the root. */
static int all_dirs(char (*out)[PATHLEN], int max)
{
    int n = 0;
    strcpy(out[n++], "");
    for (uint32_t i = 0; i < s_nidx && n < max; i++) {
        for (const char *p = s_full[i]; (p = strchr(p, '/')); p++) {
            char d[PATHLEN];
            const size_t l = (size_t)(p - s_full[i]) + 1;
            memcpy(d, s_full[i], l);
            d[l] = 0;
            int seen = 0;
            for (int k = 0; k < n && !seen; k++) seen = !strcmp(out[k], d);
            if (!seen && n < max) strcpy(out[n++], d);
        }
    }
    return n;
}

static void check_listing(void)
{
    printf("  listing a folder\n");
    static char dirs[MAXPATHS][PATHLEN];
    const int nd = all_dirs(dirs, MAXPATHS);
    const int per = log2up(s_nidx) + 3;
    int wrong = 0, costly = 0, deep = 0, deep_cheap = 0;
    static char name[PATHLEN], sub[PATHLEN * 2];

    for (int d = 0; d < nd; d++) {
        const char *dir = dirs[d];

        /* What the listing should say, by brute force: each child once,
         * in order, folders marked. */
        char want[512][PATHLEN];
        int  want_dir[512], nw = 0;
        for (uint32_t i = 0; i < s_nidx; i++) {
            if (strncmp(s_full[i], dir, strlen(dir)) != 0) continue;
            const bool isdir = midx_child(dir, s_full[i], name);
            if (nw && !strcmp(want[nw - 1], name)) continue;
            if (nw < 512) {
                strcpy(want[nw], name);
                want_dir[nw++] = isdir;
            }
        }

        /* The listing as an lsinfo would do it. */
        midx_src_t src = mem_src();
        s_reads = 0;
        int ng = 0, ok = 1;
        uint32_t i = midx_seek(&src, dir, MIDX_AT);
        while (i < s_nidx && !src.err) {
            /* A broken skip lands on the same folder for ever; fail it
             * rather than hang the suite. */
            if (ng > nw) { ok = 0; break; }
            midx_rec_t r;
            if (!mem_read(NULL, i, &r)) { ok = 0; break; }
            if (!midx_rec_has_prefix(&src, dir, &r)) break;
            const char *full = midx_rec_fullpath(&src, &r);
            if (!full) { ok = 0; break; }
            const bool isdir = midx_child(dir, full, name);
            if (ng >= nw || strcmp(want[ng], name) || want_dir[ng] != isdir)
                ok = 0;
            ng++;
            if (isdir) {
                snprintf(sub, sizeof(sub), "%s%s/", dir, name);
                i = midx_seek(&src, sub, MIDX_PAST_PREFIX);
            } else {
                i++;
            }
        }
        if (!ok || ng != nw || src.err) wrong++;
        if (s_reads > (nw + 1) * per) costly++;

        /* Where the folder is deep -- more beneath it than a search per
         * child would cost -- the listing must beat reading it through. */
        int desc = 0;
        for (uint32_t k = 0; k < s_nidx; k++)
            desc += strncmp(s_full[k], dir, strlen(dir)) == 0;
        if (desc > (nw + 1) * per) {
            deep++;
            deep_cheap += s_reads < desc;
        }
    }
    CHECK(wrong == 0, "%d of %d folder listings wrong", wrong, nd);
    CHECK(costly == 0, "%d listings read more than a search per child",
          costly);
    /* The point of PAST_PREFIX. The corpus has to contain deep folders
     * for this to mean anything, so their absence is a failure too. */
    CHECK(deep > 0, "no folder deep enough to test the skip");
    CHECK(deep_cheap == deep, "%d of %d deep folders listed no cheaper "
          "than reading every track beneath them", deep - deep_cheap, deep);
    printf("    (%d folders; %d deep enough for the skip to pay)\n", nd, deep);
}

static void check_disagreement(void)
{
    printf("  a catalog that disagrees is reported\n");
    uint32_t longest = 0;
    for (uint32_t i = 0; i < s_nidx; i++)
        if (strlen(s_full[i]) > strlen(s_full[longest])) longest = i;
    CHECK(strlen(s_full[longest]) > MIDX_KEY_LEN, "no long path to test");

    s_lie = (int)longest;
    midx_src_t src = mem_src();
    midx_rec_t r;
    const int64_t f = midx_find(&src, s_full[longest], &r);
    CHECK(f < 0 && src.err, "a wrong catalog line was believed");
    s_lie = -1;

    /* A catalog read that fails outright. */
    src = mem_src();
    src.fullpath = NULL;
    CHECK(midx_find(&src, s_full[longest], &r) < 0 && src.err,
          "a missing catalog was not an error");

    /* A scratch buffer too small for the path is refused, not overrun. */
    src = mem_src();
    src.scratch_n = MIDX_KEY_LEN;
    CHECK(midx_find(&src, s_full[longest], &r) < 0 && src.err,
          "a short scratch buffer was used");
}

/* ---- FAT time ------------------------------------------------------- */

#include <time.h>

static void check_fat_time(void)
{
    printf("  FAT dates and times\n");
    /* Every valid date and a spread of times, against the C library's
     * own UTC conversion. */
    int wrong = 0, n = 0;
    for (int y = 0; y < 128; y++)
        for (int m = 1; m <= 12; m++)
            for (int d = 1; d <= 31; d++) {
                struct tm tm = { 0 };
                tm.tm_year = 80 + y; tm.tm_mon = m - 1; tm.tm_mday = d;
                tm.tm_hour = (y + d) % 24; tm.tm_min = (m * 7) % 60;
                tm.tm_sec = (d * 2) % 60;
                const time_t t = timegm(&tm);
                /* timegm normalises 31 February; FAT would store it as
                 * written. Only compare dates that exist. */
                if (tm.tm_mday != d) continue;
                const uint16_t fd = (uint16_t)((y << 9) | (m << 5) | d);
                const uint16_t ft = (uint16_t)((((y + d) % 24) << 11) |
                                               (((m * 7) % 60) << 5) |
                                               ((d * 2) % 60 / 2));
                n++;
                if (midx_fat_time(fd, ft) != (int64_t)t) wrong++;
            }
    CHECK(n > 40000 && wrong == 0, "%d of %d dates converted wrongly", wrong, n);

    /* The extremes FAT can hold. */
    CHECK(midx_fat_time((0 << 9) | (1 << 5) | 1, 0) == 315532800,
          "1980-01-01 is 315532800");
    CHECK(midx_fat_time((127u << 9) | (12 << 5) | 31,
                        (23u << 11) | (59 << 5) | 29) == 4354819198LL,
          "2107-12-31 23:59:58");

    /* Two-second resolution: every tick is a different stamp. */
    const uint16_t day = (46 << 9) | (9 << 5) | 22;
    CHECK(midx_fat_time(day, 0) != midx_fat_time(day, 1),
          "two seconds apart gave one stamp");

    /* Corrupt fields still give a stamp, never one a real date gives,
     * and a change to them is still a change. */
    const int64_t bad1 = midx_fat_time((46 << 9) | (0 << 5) | 1, 0);
    const int64_t bad2 = midx_fat_time((46 << 9) | (13 << 5) | 1, 0);
    CHECK(bad1 < 0 && bad2 < 0 && bad1 != bad2,
          "corrupt dates: %lld and %lld", (long long)bad1, (long long)bad2);
    CHECK(midx_fat_time(day, (24u << 11)) < 0, "hour 24 taken as real");
}

int main(void)
{
    printf("mediaindextest\n");
    check_fat_time();
    check_order();
    check_walk();
    check_reconcile();
    check_disorder();
    build_index();
    check_record();
    check_lookup();
    check_listing();
    check_disagreement();

    if (failures) {
        printf("FAILURES: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("all passed: %d checks, 0 failures\n", checks);
    return 0;
}
