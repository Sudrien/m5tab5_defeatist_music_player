/*
 * stationlist.h -- `stations.m3u` from the card's root, parsed, with
 * nothing to run.
 *
 * Phase 4 of the stream path is choosing a station, and the plan settled
 * on a file rather than a UI: `#EXTINF:-1,Name` then a URL, one pair per
 * station, at the card's root. It needs no keyboard, it can be edited on
 * a laptop or a phone, and it is the format every internet-radio
 * directory already exports -- including radio-browser, which is where
 * the station data is meant to come from.
 *
 * WHY THIS IS WRITTEN BEFORE PHASES 2 AND 3
 *
 * Out of order, deliberately: the board is unavailable, phase 2 and 3
 * are both "flash and read the log", and this is a parser over bytes
 * somebody typed, which is the kind of thing a host can test completely.
 * It is also the part with the widest input: every other file in the
 * network series reads bytes from a server, and this one reads bytes
 * from a person with a text editor, which is worse.
 *
 * WHAT A REAL FILE LOOKS LIKE, AND WHAT IT ACTUALLY LOOKS LIKE
 *
 * The clean case:
 *
 *     #EXTM3U
 *     #EXTINF:-1,WNZK-AM
 *     https://stream.zeno.fm/erunhwj5lekvv
 *
 * What arrives instead, all of which is handled here rather than
 * refused: a UTF-8 BOM from Notepad; CRLF from everywhere; a bare URL
 * with no #EXTINF at all, which is what happens when someone pastes;
 * blank lines; `#EXTINF:-1, Name` with a space after the comma; extra
 * `#EXT-X-` directives from a player that rewrote the file; and a name
 * containing a comma, which is legal and means the name is everything
 * after the *first* comma.
 *
 * WHAT IS REFUSED, AND WHY IT IS REFUSED HERE
 *
 * Only `http://` and `https://`. A `file://` line would be a path into
 * the card walked by a stream player that has no business there, and
 * anything else -- `javascript:`, `data:`, a Windows path -- is a line
 * that cannot be a station. Refusing at parse time means the rest of the
 * stream path never has to ask; `netstream_play()` gets a URL that is
 * already one of two schemes.
 *
 * A station with no name takes the URL's host, because a row with an
 * empty label is a row that cannot be chosen from.
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

/*
 * The cap. Eight saved Wi-Fi networks felt tight and this is a file, not
 * NVS, so it is larger; but it is bounded, because the chooser has to
 * draw it and a file with ten thousand lines is a card that was handed
 * the wrong export.
 */
#define STATIONLIST_MAX         (64)
#define STATION_NAME_MAX        (64)
#define STATION_URL_MAX         (512)   /* matches NETSTREAM_URL_MAX */

typedef struct {
    char name[STATION_NAME_MAX];
    char url[STATION_URL_MAX];
} station_t;

/* Why a line was not turned into a station. Counted, not just dropped:
 * a file that produced three stations out of forty lines is a file with
 * something wrong with it, and the chooser should be able to say so
 * rather than showing three and looking correct. */
typedef struct {
    int stations;
    int bad_scheme;     /* not http:// or https:// */
    int too_long;       /* URL longer than STATION_URL_MAX */
    int overflowed;     /* more than STATIONLIST_MAX */
} stationlist_stats_t;

/*
 * A bounded copy, and the reason this file has no snprintf in it.
 *
 * `snprintf(dst, sizeof dst, "%s", src)` is correct here -- every call
 * site has already checked the length -- but the compiler cannot see
 * that check from the call, and at -O2 it raises -Wformat-truncation,
 * which the firmware builds with -Werror. That diagnostic only appears
 * after the constant propagation -O2 does, so it is invisible at the -O1
 * the sanitiser build uses; the Makefile's WARNCFLAGS pass exists
 * because this has bitten twice before. Truncation is also the wrong
 * behaviour for a URL -- a truncated URL is a station that connects to
 * the wrong thing -- so the call sites reject rather than truncate, and
 * this copy is only ever reached with something that fits.
 */
static inline void station_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || !dst_size) return;
    size_t i = 0;
    if (src) while (src[i] && i + 1 < dst_size) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static inline bool station_url_ok(const char *url)
{
    if (!url) return false;
    /* Case-insensitive on the scheme only: RFC 3986 says the scheme is
     * case-insensitive and people do type HTTP://. */
    size_t i = 0;
    char scheme[8];
    while (i < sizeof(scheme) - 1 && url[i] && url[i] != ':') {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        scheme[i] = c;
        i++;
    }
    scheme[i] = '\0';
    if (url[i] != ':') return false;
    if (strcmp(scheme, "http") != 0 && strcmp(scheme, "https") != 0) return false;
    /* "http://" and one character of host. A scheme with nothing after
     * it is a line somebody was in the middle of editing. */
    if (url[i + 1] != '/' || url[i + 2] != '/' || url[i + 3] == '\0') return false;
    return true;
}

/*
 * The host out of a URL, for a station with no name. `https://a.b/c?d`
 * gives `a.b`. Userinfo and port are kept if present -- this is a label,
 * not a parse, and a station shown as `example.com:8000` is more useful
 * than one shown as `example.com`.
 */
static inline void station_host(const char *url, char *out, size_t out_size)
{
    if (!out || !out_size) return;
    out[0] = '\0';
    if (!url) return;
    const char *p = strstr(url, "://");
    if (!p) return;
    p += 3;
    size_t o = 0;
    while (*p && *p != '/' && *p != '?' && *p != '#' && o + 1 < out_size) {
        out[o++] = *p++;
    }
    out[o] = '\0';
}

/* Leading and trailing whitespace removed in place. */
static inline char *station_trim(char *s)
{
    if (!s) return s;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

/*
 * The name out of an `#EXTINF:` line. The format is
 * `#EXTINF:<seconds>,<name>` and the name is everything after the first
 * comma -- a name containing commas is legal and common ("Jazz, all
 * night"). Returns 0 when the line has no comma, which is a directive
 * this does not understand rather than an error.
 */
static inline int station_extinf_name(const char *line, char *out, size_t out_size)
{
    if (!line || !out || !out_size) return 0;
    out[0] = '\0';
    const char *comma = strchr(line, ',');
    if (!comma) return 0;
    comma++;
    while (*comma == ' ') comma++;      /* "#EXTINF:-1, Name" */
    size_t o = 0;
    while (*comma && o + 1 < out_size) out[o++] = *comma++;
    out[o] = '\0';
    station_trim(out);
    return out[0] ? 1 : 0;
}

/*
 * Parse a whole file. `text` need not be terminated; `n` is its length.
 * Fills `out` with up to `max` stations and returns how many. `stats`
 * may be NULL.
 *
 * The text is not modified.
 */
static inline int stationlist_parse(const char *text, size_t n,
                                    station_t *out, int max,
                                    stationlist_stats_t *stats)
{
    stationlist_stats_t local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof(*stats));
    if (!text || !out || max <= 0) return 0;

    /* A UTF-8 BOM, which Notepad adds and which would otherwise make the
     * first line neither a directive nor a URL. */
    size_t i = 0;
    if (n >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB &&
        (uint8_t)text[2] == 0xBF) {
        i = 3;
    }

    int count = 0;
    char pending[STATION_NAME_MAX];
    pending[0] = '\0';

    while (i < n) {
        /* One line, CR or LF terminated, copied out so the caller's text
         * is untouched. A line longer than the buffer is truncated and
         * then rejected by the scheme check or the length check -- there
         * is no legitimate 600-character #EXTINF. */
        char line[STATION_URL_MAX + STATION_NAME_MAX];
        size_t o = 0;
        bool truncated = false;
        while (i < n && text[i] != '\n' && text[i] != '\r') {
            if (o + 1 < sizeof(line)) line[o++] = text[i];
            else truncated = true;
            i++;
        }
        line[o] = '\0';
        while (i < n && (text[i] == '\n' || text[i] == '\r')) i++;

        char *t = station_trim(line);
        if (!t[0]) continue;

        if (t[0] == '#') {
            /* #EXTINF carries the next station's name. #EXTM3U and every
             * other directive are skipped without comment: a file that
             * has been through another player collects them, and none of
             * them stops this one working. */
            if (strncmp(t, "#EXTINF", 7) == 0) {
                if (!station_extinf_name(t, pending, sizeof(pending))) {
                    pending[0] = '\0';
                }
            }
            continue;
        }

        if (truncated || strlen(t) >= STATION_URL_MAX) {
            stats->too_long++;
            pending[0] = '\0';
            continue;
        }
        if (!station_url_ok(t)) {
            stats->bad_scheme++;
            pending[0] = '\0';
            continue;
        }
        if (count >= max || count >= STATIONLIST_MAX) {
            stats->overflowed++;
            pending[0] = '\0';
            continue;
        }

        station_copy(out[count].url, sizeof(out[count].url), t);
        if (pending[0]) {
            station_copy(out[count].name, sizeof(out[count].name), pending);
        } else {
            /* A bare URL, which is what pasting gives. The host is a
             * worse label than a name and a much better one than blank. */
            station_host(t, out[count].name, sizeof(out[count].name));
            if (!out[count].name[0]) {
                station_copy(out[count].name, sizeof(out[count].name), t);
            }
        }
        pending[0] = '\0';
        count++;
    }

    stats->stations = count;
    return count;
}

#ifdef __cplusplus
}
#endif
