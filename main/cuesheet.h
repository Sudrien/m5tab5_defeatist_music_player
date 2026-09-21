/*
 * cuesheet.h -- a .cue file, parsed, with nothing to run.
 *
 * The first half of cue support, written against test_audio_files/22-26
 * (1002) before anything plays a track out of one. Same bargain as
 * stationlist.h: this is a parser over bytes that a ripper wrote and a
 * person may since have edited, which a host can test completely, and
 * which the board can only test one flash at a time.
 *
 * WHAT IS KEPT
 *
 * Per sheet: the album TITLE and PERFORMER, and up to CUE_MAX_FILES FILE
 * names in the order they appear. Per track: its number, which FILE it
 * starts in, its INDEX 01 as CD frames into that file, its TITLE and
 * PERFORMER (falling back to the album's), and whether it had a pregap.
 *
 * Everything else -- REM, FLAGS, ISRC, CATALOG, SONGWRITER, CDTEXTFILE,
 * POSTGAP, INDEX 02 and up -- is read past. None of it changes where a
 * track starts or ends.
 *
 * WHERE A TRACK STARTS AND ENDS
 *
 * A track starts at its INDEX 01, in the file that INDEX 01 is under.
 * INDEX 00 is a pregap and is not the start: 23's track 2 has its pregap
 * at 8 s and starts at 10. A pregap under the PREVIOUS file (25, EAC's
 * gaps-appended layout) changes nothing either: the track is in the file
 * its INDEX 01 is in.
 *
 * A track ends at the next kept track's INDEX 01 in the same file, or at
 * the end of the file. So a pregap plays as the tail of the track before
 * it -- which is what playing the disc straight through would do, and it
 * means that skipping to a track lands on its first note, not on two
 * seconds of the silence before it.
 *
 * WHAT IS DROPPED, AND THE SHEET SURVIVES IT
 *
 * A sheet is refused whole only when nothing in it is a playable track.
 * Anything smaller drops the one track, counts it in `dropped`, and says
 * why through CUE_DROP_LOG -- a no-op on the host, ESP_LOGW on the board:
 *
 *   - no INDEX 01                      (26 track 02)
 *   - a field out of range: seconds >= 60 or frames >= 75  (26 track 03)
 *   - not an AUDIO track               (26 track 07, MODE1/2352)
 *   - a track before any FILE, or past CUE_MAX_TRACKS
 *
 * And two that cue_finish() drops, once per FILE, when that file's
 * length is known:
 *
 *   - a start at or past the end of the audio (26 track 08). The length
 *     of the file is not in the sheet.
 *   - a start before the previous kept track's (26 track 06).
 *
 * THE ORDER OF THOSE TWO IS THE POINT. 26 runs 04 at 20 s, 08 at 70 s,
 * 09 at 50 s. Checked for order first, 08 is fine and 09 "goes
 * backwards" -- and then 08 is dropped for being past the end, and the
 * sheet has lost a good track to a bad one. Past-the-end first, and 09
 * follows 04 as it should. So the order check cannot live in the parser,
 * which does not know the length; cue_finish() must be called for every
 * file before any start or end is used.
 *
 * TEXT: UTF-8, OR WINDOWS-1252, DECIDED ONCE
 *
 * A BOM is stripped. Then the whole sheet is checked: if every byte
 * sequence in it is valid UTF-8 it is read as UTF-8, and otherwise the
 * whole of it is read as Windows-1252 and converted. Once per sheet and
 * not per line, because a sheet does not change encoding half way, and
 * a per-line guess would read an ASCII line and an accented one from the
 * same file differently for no reason. 23's `Caf\xE9` is the case.
 *
 * Titles are cut to CUE_TEXT_MAX bytes on a UTF-8 boundary, never in
 * the middle of a character. An unclosed quote takes the rest of the
 * line (26's album title) rather than losing it.
 *
 * NOTHING HERE ON A STACK. cue_sheet_t is about 30 KB. The caller
 * provides it, and on the board that is a static or a heap block.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUE_MAX_TRACKS      (99)    /* the Red Book's own limit */
#define CUE_MAX_FILES       (16)
#define CUE_TEXT_MAX        (128)   /* bytes of UTF-8, with the NUL */
#define CUE_FILE_MAX        (128)
#define CUE_FPS             (75)    /* CD frames per second */

#ifndef CUE_DROP_LOG
#define CUE_DROP_LOG(track, why) ((void)(track), (void)(why))
#endif

typedef struct {
    uint8_t  number;                    /* as the sheet numbered it */
    uint8_t  file;                      /* index into cue_sheet_t.files */
    bool     pregap;                    /* had an INDEX 00 */
    uint32_t start;                     /* INDEX 01, CD frames into file */
    char     title[CUE_TEXT_MAX];
    char     performer[CUE_TEXT_MAX];
} cue_track_t;

typedef struct {
    char        title[CUE_TEXT_MAX];
    char        performer[CUE_TEXT_MAX];
    char        files[CUE_MAX_FILES][CUE_FILE_MAX];
    int         nfiles;
    cue_track_t tracks[CUE_MAX_TRACKS];
    int         ntracks;
    int         dropped;
    bool        cp1252;                 /* the sheet was not UTF-8 */
} cue_sheet_t;

/* ---- text ---------------------------------------------------------- */

/* Length of the valid UTF-8 sequence at s (at most n bytes), or 0. */
static inline size_t cue_utf8_len(const unsigned char *s, size_t n)
{
    if (n == 0) return 0;
    const unsigned c = s[0];
    size_t len;
    unsigned min;
    if (c < 0x80) return 1;
    else if (c >= 0xC2 && c <= 0xDF) { len = 2; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; min = 0x10000; }
    else return 0;
    if (n < len) return 0;
    unsigned cp = c & (0xFF >> (len + 1));
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

static inline bool cue_is_utf8(const char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        const size_t l = cue_utf8_len(u + i, n - i);
        if (!l) return false;
        i += l;
    }
    return true;
}

/* Windows-1252's 0x80-0x9F, which are not Latin-1. 0 for the five
 * holes; those become U+FFFD. */
static inline unsigned cue_cp1252(unsigned char c)
{
    static const uint16_t hi[32] = {
        0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
        0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
    };
    if (c < 0x80 || c >= 0xA0) return c;
    const unsigned cp = hi[c - 0x80];
    return cp ? cp : 0xFFFD;
}

/*
 * Copy src[0..n) into dst as UTF-8, converting from Windows-1252 when
 * asked, and stopping before a character that would not fit whole.
 */
static inline void cue_text(char *dst, size_t dst_size, const char *src,
                            size_t n, bool cp1252)
{
    if (!dst || !dst_size) return;
    size_t o = 0;
    const unsigned char *u = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) {
        unsigned char enc[4];
        size_t el;
        if (!cp1252) {
            /* Already known valid: copy a whole character or none. */
            el = cue_utf8_len(u + i, n - i);
            if (!el) break;
            memcpy(enc, u + i, el);
            i += el - 1;
        } else {
            const unsigned cp = cue_cp1252(u[i]);
            if (cp < 0x80) {
                enc[0] = (unsigned char)cp; el = 1;
            } else if (cp < 0x800) {
                enc[0] = (unsigned char)(0xC0 | (cp >> 6));
                enc[1] = (unsigned char)(0x80 | (cp & 0x3F)); el = 2;
            } else {
                enc[0] = (unsigned char)(0xE0 | (cp >> 12));
                enc[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                enc[2] = (unsigned char)(0x80 | (cp & 0x3F)); el = 3;
            }
        }
        if (o + el >= dst_size) break;
        memcpy(dst + o, enc, el);
        o += el;
    }
    dst[o] = '\0';
}

/* ---- lines --------------------------------------------------------- */

static inline bool cue_space(char c) { return c == ' ' || c == '\t'; }

/* Case-insensitive keyword at p, followed by a space or the end. */
static inline bool cue_kw(const char *p, const char *end, const char *kw)
{
    size_t k = strlen(kw);
    if ((size_t)(end - p) < k) return false;
    for (size_t i = 0; i < k; i++) {
        char c = p[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != kw[i]) return false;
    }
    return p + k == end || cue_space(p[k]);
}

static inline const char *cue_skip(const char *p, const char *end)
{
    while (p < end && cue_space(*p)) p++;
    return p;
}

static inline const char *cue_word_end(const char *p, const char *end)
{
    while (p < end && !cue_space(*p)) p++;
    return p;
}

/*
 * The string argument at p: quoted, or one bare word. An unclosed quote
 * takes the rest of the line. *out and *len are the bytes between the
 * quotes; the return is where parsing continues.
 */
static inline const char *cue_string(const char *p, const char *end,
                                     const char **out, size_t *len)
{
    p = cue_skip(p, end);
    if (p < end && *p == '"') {
        const char *s = ++p;
        while (p < end && *p != '"') p++;
        *out = s;
        *len = (size_t)(p - s);
        return p < end ? p + 1 : p;
    }
    const char *s = p;
    p = cue_word_end(p, end);
    *out = s;
    *len = (size_t)(p - s);
    return p;
}

/* Up to three decimal digits; -1 if there are none or more. */
static inline int cue_num(const char **pp, const char *end)
{
    const char *p = *pp;
    int v = 0, d = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        if (++d > 3) return -1;
        v = v * 10 + (*p++ - '0');
    }
    if (!d) return -1;
    *pp = p;
    return v;
}

/*
 * MM:SS:FF into frames. Minutes may run past 99 -- a long image does --
 * but seconds must be under 60 and frames under 75. -1 on anything else.
 */
static inline long cue_msf(const char *p, const char *end)
{
    p = cue_skip(p, end);
    const int m = cue_num(&p, end);
    if (m < 0 || p >= end || *p++ != ':') return -1;
    const int s = cue_num(&p, end);
    if (s < 0 || s >= 60 || p >= end || *p++ != ':') return -1;
    const int f = cue_num(&p, end);
    if (f < 0 || f >= CUE_FPS) return -1;
    if (cue_skip(p, end) != end) return -1;
    return ((long)m * 60 + s) * CUE_FPS + f;
}

/* ---- the sheet ----------------------------------------------------- */

/*
 * Parse n bytes of sheet into *out. Returns the number of tracks kept;
 * 0 means the sheet is refused.
 */
static inline int cue_parse(const char *buf, size_t n, cue_sheet_t *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!buf) return 0;

    if (n >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        buf += 3;
        n -= 3;
    }
    out->cp1252 = !cue_is_utf8(buf, n);

    /* The track being read. Committed when the next TRACK, the end, or
     * nothing more arrives; see commit() below. */
    cue_track_t cur;
    bool   in_track = false, cur_audio = false, have01 = false, bad = false;
    int    cur_file = -1;          /* the FILE the next line is under */
    const char *why = NULL;

#define CUE_COMMIT() do {                                                  \
    if (in_track) {                                                        \
        if (!cur_audio)       why = "not an audio track";                  \
        else if (bad)         why = "a time out of range";                 \
        else if (!have01)     why = "no INDEX 01";                         \
        else if (out->ntracks >= CUE_MAX_TRACKS)                           \
                              why = "more than 99 tracks";                 \
        else                  why = NULL;                                  \
        if (why) {                                                         \
            out->dropped++;                                                \
            CUE_DROP_LOG(cur.number, why);                                 \
        } else {                                                           \
            out->tracks[out->ntracks++] = cur;                             \
        }                                                                  \
        in_track = false;                                                  \
    }                                                                      \
} while (0)

    const char *p = buf, *stop = buf + n;
    while (p < stop) {
        const char *eol = p;
        while (eol < stop && *eol != '\n' && *eol != '\r') eol++;
        const char *next = eol;
        while (next < stop && (*next == '\n' || *next == '\r')) next++;

        const char *q = cue_skip(p, eol);
        const char *e = eol;
        while (e > q && cue_space(e[-1])) e--;

        const char *s;
        size_t len;

        if (cue_kw(q, e, "FILE")) {
            /* NOT a commit. A FILE between a TRACK and its INDEX 01 is
             * 25's layout -- the pregap in one file, the start in the
             * next -- and ending the track here drops it for having no
             * INDEX 01. The track ends at the next TRACK or the end. */
            if (out->nfiles < CUE_MAX_FILES) {
                cue_string(q + 4, e, &s, &len);
                cue_text(out->files[out->nfiles], CUE_FILE_MAX, s, len,
                         out->cp1252);
                cur_file = out->nfiles++;
            } else {
                cur_file = -1;      /* tracks under it will be dropped */
            }
        } else if (cue_kw(q, e, "TRACK")) {
            CUE_COMMIT();
            const char *r = cue_skip(q + 5, e);
            const int num = cue_num(&r, e);
            memset(&cur, 0, sizeof cur);
            cur.number = (uint8_t)(num > 0 && num < 256 ? num : 0);
            memcpy(cur.performer, out->performer, sizeof cur.performer);
            r = cue_skip(r, e);
            cur_audio = cue_kw(r, e, "AUDIO") && cur_file >= 0;
            have01 = false;
            bad = false;
            in_track = true;
        } else if (cue_kw(q, e, "INDEX")) {
            const char *r = cue_skip(q + 5, e);
            const int idx = cue_num(&r, e);
            const long at = cue_msf(r, e);
            if (!in_track) {
                /* An INDEX with no TRACK: nothing to attach it to. */
            } else if (idx < 0 || at < 0) {
                bad = true;
            } else if (idx == 0) {
                cur.pregap = true;
            } else if (idx == 1) {
                if (cur_file < 0) {
                    cur_audio = false;
                } else {
                    cur.file = (uint8_t)cur_file;
                    cur.start = (uint32_t)at;
                    have01 = true;
                }
            }
        } else if (cue_kw(q, e, "TITLE")) {
            cue_string(q + 5, e, &s, &len);
            if (in_track) cue_text(cur.title, CUE_TEXT_MAX, s, len, out->cp1252);
            else          cue_text(out->title, CUE_TEXT_MAX, s, len, out->cp1252);
        } else if (cue_kw(q, e, "PERFORMER")) {
            cue_string(q + 9, e, &s, &len);
            if (in_track) cue_text(cur.performer, CUE_TEXT_MAX, s, len, out->cp1252);
            else          cue_text(out->performer, CUE_TEXT_MAX, s, len, out->cp1252);
        }
        /* Everything else -- REM, FLAGS, ISRC, POSTGAP, garbage -- is
         * read past. */

        p = next;
    }
    CUE_COMMIT();
#undef CUE_COMMIT

    return out->ntracks;
}

/*
 * Finish `file` now that its length is known: drop the tracks in it that
 * start at or past its end, THEN the ones that start before the track
 * kept ahead of them -- in that order, see above. `len` is in CD frames;
 * 0 means unknown and skips the first pass only. Returns the number of
 * tracks left in the whole sheet.
 */
static inline int cue_finish(cue_sheet_t *cs, int file, uint32_t len)
{
    if (!cs) return 0;
    int w = 0;
    bool have = false;
    uint32_t last = 0;
    for (int r = 0; r < cs->ntracks; r++) {
        const cue_track_t *t = &cs->tracks[r];
        if (t->file == file) {
            if (len && t->start >= len) {
                cs->dropped++;
                CUE_DROP_LOG(t->number, "starts past the end of the audio");
                continue;
            }
            if (have && t->start <= last) {
                cs->dropped++;
                CUE_DROP_LOG(t->number, "starts before the track before it");
                continue;
            }
            have = true;
            last = t->start;
        }
        if (w != r) cs->tracks[w] = *t;
        w++;
    }
    cs->ntracks = w;
    return w;
}

/*
 * Where track i ends, in CD frames into its file: the next kept track's
 * start in the same file, or 0 for "the end of the file".
 */
static inline uint32_t cue_track_end(const cue_sheet_t *cs, int i)
{
    if (!cs || i < 0 || i + 1 >= cs->ntracks) return 0;
    const cue_track_t *t = &cs->tracks[i], *nx = &cs->tracks[i + 1];
    return nx->file == t->file ? nx->start : 0;
}

/* CD frames to samples at `rate`. Exact for 44100 (588 per frame). */
static inline uint64_t cue_frames_to_samples(uint32_t frames, uint32_t rate)
{
    return (uint64_t)frames * rate / CUE_FPS;
}

/* CD frames to hundredths of a second, rounded -- the unit the seek
 * paths report in since 0809. */
static inline uint32_t cue_frames_to_cs(uint32_t frames)
{
    return (uint32_t)(((uint64_t)frames * 100 + CUE_FPS / 2) / CUE_FPS);
}

/* ---- the path a track is known by --------------------------------- */

/*
 * A cue track is named "<sheet>.cue#NN" everywhere a path goes: the
 * playlist, the chooser, history, favourites, the sidecar key. NN is the
 * track's 1-based position among the sheet's KEPT tracks, two digits so
 * a sort by path is a sort by track.
 *
 * Position rather than the sheet's own TRACK number, because numbers in
 * a hand-edited sheet can repeat or skip (26 skips 05) and a name must
 * pick out exactly one track. Kept rather than written, because the
 * name has to find the same track the chooser showed, and the chooser
 * only shows kept ones.
 */
#define CUE_VPATH_SEP   '#'

/*
 * If path is a cue track, return its position (1..CUE_MAX_TRACKS) and
 * the length of the sheet's path in *sheet_len; otherwise 0. Case-
 * insensitive on ".cue", as FAT is.
 */
static inline int cue_vpath_split(const char *path, size_t *sheet_len)
{
    if (!path) return 0;
    const char *h = strrchr(path, CUE_VPATH_SEP);
    if (!h || h - path < 4) return 0;
    const char *x = h - 4;
    if (x[0] != '.' || (x[1] | 0x20) != 'c' || (x[2] | 0x20) != 'u' ||
        (x[3] | 0x20) != 'e') return 0;
    const char *d = h + 1;
    int v = 0, nd = 0;
    while (*d >= '0' && *d <= '9') {
        v = v * 10 + (*d++ - '0');
        if (++nd > 3) return 0;
    }
    if (*d || nd == 0 || v < 1 || v > CUE_MAX_TRACKS) return 0;
    if (sheet_len) *sheet_len = (size_t)(h - path);
    return v;
}

static inline bool cue_is_sheet(const char *name)
{
    const size_t n = name ? strlen(name) : 0;
    if (n < 5) return false;
    const char *x = name + n - 4;
    return x[0] == '.' && (x[1] | 0x20) == 'c' && (x[2] | 0x20) == 'u' &&
           (x[3] | 0x20) == 'e';
}

static inline int cue_casecmp_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
    return 0;
}

/* Length of name without its extension. */
static inline size_t cue_stem_len(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && dot != name ? (size_t)(dot - name) : strlen(name);
}

/*
 * Which of the directory's names a FILE line means, or -1.
 *
 *   1. the name itself, case-insensitively, as FAT would find it;
 *   2. the same stem with another extension `playable` accepts -- the
 *      image was compressed after the sheet was written, CDImage.wav
 *      now CDImage.flac;
 *   3. only for a sheet with ONE file: the sheet's own stem with any
 *      playable extension. 24's case, and most sheets in the wild: EAC
 *      wrote "CDImage.wav" and the pair was renamed together.
 *
 * Rule 3 is refused for multi-file sheets because it would point every
 * FILE at the same audio and play one file N times.
 *
 * `want` is only a name: a sheet's FILE line with a directory in it
 * ("..\\audio\\x.flac", written on another machine) is matched on the
 * part after the last slash of either kind.
 */
static inline int cue_resolve(const char *want, const char *sheet_name,
                              int sheet_files, const char *const *names,
                              int n, bool (*playable)(const char *))
{
    if (!want || !names) return -1;
    const char *b = want;
    for (const char *p = want; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    const size_t bl = strlen(b), bs = cue_stem_len(b);

    for (int i = 0; i < n; i++) {
        if (strlen(names[i]) == bl && cue_casecmp_n(names[i], b, bl) == 0)
            return i;
    }
    for (int i = 0; i < n; i++) {
        if (cue_stem_len(names[i]) == bs && cue_casecmp_n(names[i], b, bs) == 0 &&
            (!playable || playable(names[i])))
            return i;
    }
    if (sheet_files == 1 && sheet_name) {
        const size_t ss = cue_stem_len(sheet_name);
        for (int i = 0; i < n; i++) {
            if (cue_stem_len(names[i]) == ss &&
                cue_casecmp_n(names[i], sheet_name, ss) == 0 &&
                !cue_is_sheet(names[i]) && (!playable || playable(names[i])))
                return i;
        }
    }
    return -1;
}

#ifdef __cplusplus
}
#endif
