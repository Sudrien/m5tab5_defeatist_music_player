/*
 * favmatch.h -- when two URLs are the same station.
 *
 * Split out of favorites.c for stationlist.h's reason: this is a
 * function over bytes somebody typed, it decides whether a star appears
 * on a row, and it is the kind of thing a host can test completely.
 * favorites.c around it opens files and takes mutexes and can only be
 * tested by flashing.
 *
 * THE RULE, AND WHY IT IS THIS SMALL
 *
 * Scheme and host case-folded; everything from the path onwards
 * byte-exact. RFC 3986 says the scheme and host are case-insensitive
 * and the rest is not, and that is the whole of it.
 *
 * What it deliberately does NOT do: strip trailing slashes, drop
 * default ports, sort query parameters, follow redirects, or treat
 * http and https as the same station. Every one of those makes two
 * URLs that ARE different look the same, and the failure is silent --
 * a star on a row nobody starred, or an unstar that removes the wrong
 * entry. A missed match costs a second star in the file, which is
 * visible and harmless. An extra match costs the wrong station.
 *
 * So the asymmetry is deliberate: this errs toward saying no.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline char favmatch_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/*
 * Where the authority ends: the first '/', '?' or '#' after "://", or
 * the end of the string. 0 when there is no "://" at all.
 *
 * Returning 0 for "no scheme" rather than guessing lets the caller
 * fall back to a byte compare instead of folding the whole string,
 * which would make `A` and `a` the same station.
 */
static inline size_t favmatch_authority_end(const char *u)
{
    if (!u) return 0;
    const char *p = strstr(u, "://");
    if (!p) return 0;
    size_t i = (size_t)(p - u) + 3;
    while (u[i] && u[i] != '/' && u[i] != '?' && u[i] != '#') i++;
    return i;
}

static inline bool favmatch_eq(const char *a, const char *b)
{
    if (!a || !b) return false;

    const size_t ea = favmatch_authority_end(a);
    const size_t eb = favmatch_authority_end(b);

    /* No authority in one or both: byte-exact, because there is nothing
     * here that is known to be case-insensitive. station_url_ok() has
     * already refused these everywhere favorites.c reaches this from,
     * so it is the branch that should never run -- and it runs safely
     * rather than reading past the end looking for a host. */
    if (ea == 0 || eb == 0) return strcmp(a, b) == 0;

    if (ea != eb) return false;
    for (size_t i = 0; i < ea; i++) {
        if (favmatch_lower(a[i]) != favmatch_lower(b[i])) return false;
    }
    return strcmp(a + ea, b + eb) == 0;
}

#ifdef __cplusplus
}
#endif
