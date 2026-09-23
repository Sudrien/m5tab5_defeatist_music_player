/*
 * mediawalktest -- the real main/mediawalk.c, walking a real directory
 * tree on the host.
 *
 * The walk is what a reconcile believes about the card, and a reconcile
 * reads a path missing from it as a file deleted. So the checks are the
 * two things mediawalk.h promises:
 *
 *   ORDER. Everything offered is strictly increasing under
 *   midx_path_cmp(), including across the "a/" versus "a b/" trap, and
 *   is exactly the expected set in exactly that order.
 *
 *   COMPLETENESS, OR NOTHING. A folder over MWALK_DIR_MAX, a missing
 *   mount, fail the walk rather than shorten it; what is left out
 *   (dotfiles, "._" sidecars, non-audio, audio a sheet covers, folders
 *   too deep, paths too long) is left out identically on every walk.
 *
 * Plus the cue stamp -- the sheet's and the audio's together, so
 * touching either changes it -- the lease never nested and always
 * returned, and ASan's leak check on every exit path.
 *
 * The OS calls are real (opendir, readdir, stat). What is stubbed is
 * the ESP-IDF surface: the storage arbiter, storage_is_hidden(),
 * decoder_supports(), and cuedir, whose real version needs the decoder
 * and duration probes. The fake cuedir below follows cuedir.h's
 * contract: a sheet "X.cue" with "X.flac" beside it is three tracks
 * "X.cue#01".."#03" that play from X.flac and hide it; a sheet with no
 * audio hides nothing.
 *
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <errno.h>

#include "cuedir.h"
#include "decoder.h"
#include "mediadir.h"
#include "mediawalk.h"
#include "storage.h"
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

/* ---- stubs ---------------------------------------------------------- */

static int s_depth, s_nested, s_leases;

void storage_io_acquire(storage_io_class_t cls)
{
    (void)cls;
    if (s_depth++) s_nested++;
    s_leases++;
}
void storage_io_release(void) { s_depth--; }

/*
 * mediadir over POSIX. The device reads FatFs's FILINFO; here the same
 * three things -- name, folder or not, size and mtime -- come from
 * readdir() and stat(). "." and ".." are skipped, as f_readdir() never
 * returns them. `s_fail_in`, when set, makes a folder whose path
 * contains it fail after its first entry: the read error the walk must
 * never mistake for the end of a folder.
 */
static DIR  *s_hd;
static char  s_hpath[2048], s_hname[512];
static const char *s_fail_in;
static int   s_hcount;

bool mdir_open(const char *path)
{
    storage_io_acquire(STORAGE_IO_BACKGROUND);
    s_hd = opendir(path);
    storage_io_release();
    snprintf(s_hpath, sizeof(s_hpath), "%s", path);
    s_hcount = 0;
    return s_hd != NULL;
}

int mdir_next(mdir_ent_t *out)
{
    for (;;) {
        if (s_fail_in && strstr(s_hpath, s_fail_in) && s_hcount >= 1) return -1;
        storage_io_acquire(STORAGE_IO_BACKGROUND);
        errno = 0;
        struct dirent *e = readdir(s_hd);
        const int err = errno;
        storage_io_release();
        if (!e) return err ? -1 : 0;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[2600];
        snprintf(p, sizeof(p), "%s/%s", s_hpath, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) return -1;
        snprintf(s_hname, sizeof(s_hname), "%s", e->d_name);
        out->name = s_hname;
        out->is_dir = S_ISDIR(st.st_mode);
        out->stamp.mtime = (int64_t)st.st_mtime;
        out->stamp.size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
        s_hcount++;
        return 1;
    }
}

void mdir_close(void)
{
    if (s_hd) closedir(s_hd);
    s_hd = NULL;
}

/* storage.c's rule: ".", "..", and every dotfile, which covers "._". */
bool storage_is_hidden(const char *name) { return name[0] == '.'; }

bool decoder_supports(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    static const char *const ext[] = { ".flac", ".mp3", ".ogg", ".wav" };
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (strcasecmp(dot, ext[i]) == 0) return true;
    return false;
}

struct cuedir {
    char rows[16][64];
    char audio[16][64];
    char hide[8][64];
    int  nrows, nhide;
};

cuedir_t *cuedir_load(const char *dir, storage_io_class_t cls)
{
    (void)cls;
    DIR *d = opendir(dir);
    if (!d) return NULL;
    cuedir_t *cd = calloc(1, sizeof(*cd));
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".cue") != 0) continue;
        char audio[64], path[1024];
        if (n > 40) continue;
        snprintf(audio, sizeof(audio), "%.*s.flac", (int)(n - 4), e->d_name);
        snprintf(path, sizeof(path), "%s/%s", dir, audio);
        struct stat st;
        if (stat(path, &st) != 0) continue;     /* nothing playable */
        strcpy(cd->hide[cd->nhide++], audio);
        for (int t = 1; t <= 3; t++) {
            snprintf(cd->rows[cd->nrows], 64, "%.40s#%02d", e->d_name, t);
            strcpy(cd->audio[cd->nrows++], audio);
        }
    }
    closedir(d);
    if (!cd->nrows) { free(cd); return NULL; }
    return cd;
}
void cuedir_free(cuedir_t *cd) { free(cd); }
bool cuedir_hides(const cuedir_t *cd, const char *name)
{
    for (int i = 0; cd && i < cd->nhide; i++)
        if (!strcmp(cd->hide[i], name)) return true;
    return false;
}
int cuedir_count(const cuedir_t *cd) { return cd ? cd->nrows : 0; }
const char *cuedir_name(const cuedir_t *cd, int i) { return cd->rows[i]; }
const char *cuedir_audio(const cuedir_t *cd, int i) { return cd->audio[i]; }
bool cuedir_row_tags(const cuedir_t *cd, int i, char *title, char *artist,
                     char *album, size_t each)
{
    if (!cd || i < 0 || i >= cd->nrows) return false;
    snprintf(title, each, "row %d of %s", i, cd->rows[i]);
    snprintf(artist, each, "performer");
    snprintf(album, each, "album");
    return true;
}

/* ---- a tree --------------------------------------------------------- */

static char s_root[256];

static void mk(const char *rel, const char *content)
{
    char p[2048];
    snprintf(p, sizeof(p), "%s/%s", s_root, rel);
    /* make every parent */
    for (char *q = p + strlen(s_root) + 1; (q = strchr(q, '/')); q++) {
        *q = 0;
        mkdir(p, 0755);
        *q = '/';
    }
    if (content) {
        FILE *f = fopen(p, "w");
        if (f) { fputs(content, f); fclose(f); }
    } else {
        mkdir(p, 0755);
    }
}

static void set_mtime(const char *rel, long t)
{
    char p[2048];
    snprintf(p, sizeof(p), "%s/%s", s_root, rel);
    struct timeval tv[2] = { { t, 0 }, { t, 0 } };
    utimes(p, tv);
}

/* ---- collecting a walk ---------------------------------------------- */

#define MAXGOT  (10000)
static char        *s_got[MAXGOT];
static midx_stamp_t s_gotst[MAXGOT];
static int          s_ngot, s_stop_after = -1, s_depth_bad;

/* What mwalk_cue_tags() said for each entry, from inside the callback. */
static int  s_cue_tagged, s_plain_tagged, s_wrong_path_tagged, s_cue_seen;
static int  s_cue_title_wrong;

static bool collect(void *ctx, const char *path, midx_stamp_t st)
{
    (void)ctx;
    if (s_depth) s_depth_bad++;         /* called holding the lease */
    {
        char t[64], a[64], al[64];
        const bool is_cue = strstr(path, ".cue#") != NULL;
        const bool got = mwalk_cue_tags(path, t, a, al, sizeof(t));
        if (is_cue) {
            s_cue_seen++;
            s_cue_tagged += got;
            /* The row's own tags: "row N of <that very track's name>". */
            const char *base = strrchr(path, '/');
            base = base ? base + 1 : path;
            if (got && (!strstr(t, base) || strcmp(a, "performer") ||
                        strcmp(al, "album"))) s_cue_title_wrong++;
        } else {
            s_plain_tagged += got;
        }
        /* Only for the path being offered. */
        s_wrong_path_tagged += mwalk_cue_tags("Album/Album.cue#01x", t, a, al,
                                              sizeof(t));
    }
    if (s_ngot < MAXGOT) {
        s_got[s_ngot] = strdup(path);
        s_gotst[s_ngot] = st;
        s_ngot++;
    }
    return s_stop_after < 0 || s_ngot < s_stop_after;
}

static void reset(void)
{
    for (int i = 0; i < s_ngot; i++) free(s_got[i]);
    s_ngot = 0;
    s_stop_after = -1;
}

static int idx_of(const char *p)
{
    for (int i = 0; i < s_ngot; i++) if (!strcmp(s_got[i], p)) return i;
    return -1;
}

static int path_qsort(const void *a, const void *b)
{
    return midx_path_cmp(*(const char *const *)a, *(const char *const *)b);
}

int main(void)
{
    printf("mediawalktest\n");
    snprintf(s_root, sizeof(s_root), "/tmp/mwalkXXXXXX");
    if (!mkdtemp(s_root)) { printf("no temp dir\n"); return 1; }
    /* the mount string is at most 15 bytes; a symlink keeps it short */
    char mount[16];
    snprintf(mount, sizeof(mount), "/tmp/mw%d", (int)getpid() % 100000);
    unlink(mount);
    if (symlink(s_root, mount) != 0) { printf("no symlink\n"); return 1; }

    /* The trap, the hidden, the non-audio, cue sheets, nesting. */
    mk("a/x.flac", "1");
    mk("a b/y.flac", "22");
    mk("a-b/z.mp3", "333");
    mk("a.flac", "4444");
    mk("A/upper.flac", "5");
    mk("_/under.ogg", "6");
    mk("b/c/d/e.flac", "7");
    mk("\xC3\xA9t\xC3\xA9/song.flac", "8");
    mk(".hidden.flac", "9");
    mk("._a.flac", "resource fork");
    mk(".Trashes/gone.flac", "x");
    mk("notes.txt", "x");
    mk("cover.jpg", "x");
    mk("Album/Album.cue", "FILE \"Album.flac\" WAVE\n");
    mk("Album/Album.flac", "image image image");
    mk("Album/bonus.flac", "b");
    mk("Broken/x.cue", "FILE \"gone.flac\" WAVE\n");
    mk("Broken/y.flac", "y");
    mk("Empty", NULL);
    set_mtime("Album/Album.cue", 1000);
    set_mtime("Album/Album.flac", 2000);

    /* Too deep: MWALK_DEPTH_MAX folders below the root is the last that
     * counts, and a file one below that is left out. */
    char deep[512] = "", ok_deep[600], bad_deep[600];
    for (int i = 0; i < MWALK_DEPTH_MAX; i++) strcat(deep, "d/");
    snprintf(ok_deep, sizeof(ok_deep), "%sok.flac", deep);
    snprintf(bad_deep, sizeof(bad_deep), "%sd/deeper.flac", deep);
    mk(ok_deep, "ok");
    mk(bad_deep, "no");

    /* Too long: two 250-byte folder names and a file is past
     * MIDX_PATH_MAX; one folder and a file is not. */
    char n250[251], fits[600], toolong[800];
    memset(n250, 'n', 250);
    n250[250] = 0;
    snprintf(fits, sizeof(fits), "L/%s/fits.flac", n250);
    snprintf(toolong, sizeof(toolong), "L/%s/%s/long.flac", n250, n250);
    mk(fits, "f");
    mk(toolong, "l");

    /* And at the limit to the byte: "M/" + 250 + "/" + a name making
     * exactly MIDX_PATH_MAX, which counts, and one byte more, which
     * does not. */
    char at_max[600], past_max[600], nm[300];
    const int room = MIDX_PATH_MAX - 2 - 250 - 1;      /* 253 */
    memset(nm, 'm', (size_t)room);
    memcpy(nm + room - 5, ".flac", 6);
    snprintf(at_max, sizeof(at_max), "M/%s/%s", n250, nm);
    memset(nm, 'p', (size_t)room + 1);
    memcpy(nm + room + 1 - 5, ".flac", 6);
    snprintf(past_max, sizeof(past_max), "M/%s/%s", n250, nm);
    mk(at_max, "a");
    mk(past_max, "p");

    const char *want[] = {
        "A/upper.flac", "Album/Album.cue#01", "Album/Album.cue#02",
        "Album/Album.cue#03", "Album/bonus.flac", "Broken/y.flac",
        "L/placeholder", "M/placeholder", "_/under.ogg", "a/x.flac", "a b/y.flac",
        "a-b/z.mp3", "a.flac", "b/c/d/e.flac", "deep/placeholder",
        "\xC3\xA9t\xC3\xA9/song.flac",
    };
    const int nwant = (int)(sizeof(want) / sizeof(want[0]));
    char *wantv[32];
    for (int i = 0; i < nwant; i++) {
        if (!strcmp(want[i], "L/placeholder"))         wantv[i] = strdup(fits);
        else if (!strcmp(want[i], "M/placeholder"))    wantv[i] = strdup(at_max);
        else if (!strcmp(want[i], "deep/placeholder")) wantv[i] = strdup(ok_deep);
        else                                           wantv[i] = strdup(want[i]);
    }
    qsort(wantv, (size_t)nwant, sizeof(char *), path_qsort);

    printf("  order and contents\n");
    mwalk_result_t r = mwalk_volume(mount, collect, NULL);
    CHECK(r == MWALK_DONE, "walk result %d", r);
    int inorder = 1;
    for (int i = 1; i < s_ngot; i++)
        if (!midx_in_order(s_got[i - 1], s_got[i])) inorder = 0;
    CHECK(inorder, "walk not strictly increasing");
    CHECK(s_ngot == nwant, "got %d tracks, want %d", s_ngot, nwant);
    for (int i = 0; i < nwant && i < s_ngot; i++)
        CHECK(!strcmp(s_got[i], wantv[i]), "#%d: got \"%.60s\", want \"%.60s\"",
              i, s_got[i], wantv[i]);
    CHECK(idx_of("a/x.flac") < idx_of("a b/y.flac"),
          "a/ must come before a b/ -- the strcmp trap");
    CHECK(idx_of("Album/Album.flac") < 0, "a covered image was listed");
    CHECK(strlen(at_max) == MIDX_PATH_MAX && idx_of(at_max) >= 0,
          "a path of exactly MIDX_PATH_MAX was left out");
    CHECK(idx_of(past_max) < 0, "a path one byte too long was offered");
    CHECK(idx_of("Broken/y.flac") >= 0, "a broken sheet hid real audio");
    CHECK(s_nested == 0 && s_depth == 0 && s_depth_bad == 0,
          "lease: %d nested, depth %d at end, %d callbacks under it",
          s_nested, s_depth, s_depth_bad);
    CHECK(s_leases > 0, "the walk never took the lease");
    CHECK(s_cue_seen == 3 && s_cue_tagged == 3 && s_cue_title_wrong == 0,
          "cue tags from the loaded sheet: %d of %d, %d wrong", s_cue_tagged,
          s_cue_seen, s_cue_title_wrong);
    CHECK(s_plain_tagged == 0, "a plain file got cue tags");
    CHECK(s_wrong_path_tagged == 0, "tags given for a path not being offered");
    {
        char t[64], a[64], al[64];
        CHECK(!mwalk_cue_tags("Album/Album.cue#01", t, a, al, sizeof(t)),
              "cue tags given outside the callback");
    }

    printf("  stamps\n");
    {
        char p[1024];
        snprintf(p, sizeof(p), "%s/a.flac", s_root);
        struct stat st;
        stat(p, &st);
        const int i = idx_of("a.flac");
        CHECK(i >= 0 && s_gotst[i].size == 4 &&
              s_gotst[i].mtime == (int64_t)st.st_mtime, "a plain file's stamp");

        const int c = idx_of("Album/Album.cue#02");
        const uint64_t cue_sz = strlen("FILE \"Album.flac\" WAVE\n"),
                       img_sz = strlen("image image image");
        CHECK(c >= 0 && s_gotst[c].mtime == 2000 &&
              s_gotst[c].size == cue_sz + img_sz,
              "cue stamp: mtime %lld size %llu (track %s)",
              c >= 0 ? (long long)s_gotst[c].mtime : 0LL,
              c >= 0 ? (unsigned long long)s_gotst[c].size : 0ULL,
              c >= 0 ? "found" : "missing");

        /* Touch the audio only: every track of the sheet must change. */
        const midx_stamp_t before = c >= 0 ? s_gotst[c] : (midx_stamp_t){ 0, 0 };
        set_mtime("Album/Album.flac", 3000);
        reset();
        mwalk_volume(mount, collect, NULL);
        const int c2 = idx_of("Album/Album.cue#02");
        CHECK(c2 >= 0 && !midx_stamp_eq(before, s_gotst[c2]),
              "a re-ripped image under an untouched sheet was not a change");
        /* Edit the sheet only, and set its mtime back to what it was:
         * the size still moves, so it is still a change. (An edit that
         * keeps the size AND leaves neither mtime later than before is
         * the one case this stamp cannot see -- as for any plain file.) */
        set_mtime("Album/Album.flac", 2000);
        mk("Album/Album.cue", "FILE \"Album.flac\" WAVE\nREM x\n");
        set_mtime("Album/Album.cue", 1000);
        reset();
        mwalk_volume(mount, collect, NULL);
        const int c3 = idx_of("Album/Album.cue#02");
        CHECK(c3 >= 0 && !midx_stamp_eq(before, s_gotst[c3]),
              "an edited sheet was not a change");
    }

    printf("  the same walk twice\n");
    {
        reset();
        mwalk_volume(mount, collect, NULL);
        char *first[64];
        midx_stamp_t fst[64];
        const int n1 = s_ngot;
        for (int i = 0; i < n1 && i < 64; i++) { first[i] = strdup(s_got[i]); fst[i] = s_gotst[i]; }
        reset();
        mwalk_volume(mount, collect, NULL);
        int same = n1 == s_ngot;
        for (int i = 0; same && i < n1 && i < 64; i++)
            same = !strcmp(first[i], s_got[i]) && midx_stamp_eq(fst[i], s_gotst[i]);
        CHECK(same, "two walks of an unchanged tree differ");
        for (int i = 0; i < n1 && i < 64; i++) free(first[i]);
    }

    printf("  stopping and failing\n");
    reset();
    s_stop_after = 3;
    r = mwalk_volume(mount, collect, NULL);
    CHECK(r == MWALK_STOPPED && s_ngot == 3, "stop: result %d after %d", r, s_ngot);
    CHECK(s_depth == 0, "lease held after a stop");

    /* A folder that errors part-way is not a shorter folder. */
    reset();
    s_fail_in = "/b/c";
    r = mwalk_volume(mount, collect, NULL);
    s_fail_in = NULL;
    CHECK(r == MWALK_FAILED, "a read error inside a folder did not fail the "
          "walk (result %d)", r);
    CHECK(idx_of("b/c/d/e.flac") < 0 && idx_of("\xC3\xA9t\xC3\xA9/song.flac") < 0,
          "the walk offered tracks from or after a folder it could not finish");
    CHECK(s_depth == 0, "lease held after a read error");

    reset();
    r = mwalk_volume("/nonexistent", collect, NULL);
    CHECK(r == MWALK_FAILED && s_ngot == 0, "a missing mount did not fail");

    reset();
    r = mwalk_volume("/tmp/this-mount-name-is-too-long", collect, NULL);
    CHECK(r == MWALK_FAILED, "an overlong mount string was accepted");

    /* One folder past the cap fails the walk -- it is not cut short,
     * because a cut folder reads as deleted tracks. */
    {
        char p[1024];
        for (int i = 0; i <= MWALK_DIR_MAX; i++) {
            snprintf(p, sizeof(p), "%s/Huge/%05d.flac", s_root, i);
            if (i == 0) { snprintf(p, sizeof(p), "%s/Huge", s_root); mkdir(p, 0755);
                          snprintf(p, sizeof(p), "%s/Huge/%05d.flac", s_root, i); }
            FILE *f = fopen(p, "w");
            if (f) fclose(f);
        }
        reset();
        r = mwalk_volume(mount, collect, NULL);
        CHECK(r == MWALK_FAILED, "a folder over MWALK_DIR_MAX did not fail "
              "the walk (result %d)", r);
        const int before_huge = idx_of("Album/bonus.flac") >= 0;
        CHECK(before_huge, "entries before the huge folder were not offered");
        CHECK(idx_of("_/under.ogg") < 0,
              "the walk carried on past a folder it could not finish");
        CHECK(s_depth == 0, "lease held after a failure");

        /* At the cap exactly is fine. */
        snprintf(p, sizeof(p), "%s/Huge/%05d.flac", s_root, MWALK_DIR_MAX);
        unlink(p);
        reset();
        r = mwalk_volume(mount, collect, NULL);
        CHECK(r == MWALK_DONE, "a folder of exactly MWALK_DIR_MAX failed");
    }

    reset();
    for (int i = 0; i < nwant; i++) free(wantv[i]);
    unlink(mount);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", s_root);
    if (system(cmd) != 0) printf("  (could not remove %s)\n", s_root);

    if (failures) {
        printf("FAILURES: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("all passed: %d checks, 0 failures\n", checks);
    return 0;
}
