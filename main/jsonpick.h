/*
 * jsonpick.h -- one string field out of one JSON object.
 *
 * THIS IS NOT A JSON PARSER AND MUST NOT BECOME ONE.
 *
 * stations.h chose M3U over JSON specifically to avoid putting a parser
 * on the P4, and that decision stands for station LISTS: they are the
 * bulk data, they arrive fifty at a time, and the M3U endpoints give
 * them for free through code that is already tested.
 *
 * What that decision cost is everything the directory knows about a
 * station that is not its name and URL -- and the one worth having is
 * `favicon`, the station's own artwork, which has no M3U form because
 * M3U has nowhere to put it. Every station in the directory has one.
 *
 * So this reads ONE STRING FIELD from ONE FLAT OBJECT, which is the
 * whole of what /json/stations/byuuid/{uuid} needs: a single-element
 * array holding one object of scalar fields. It does no allocation, no
 * recursion, no number conversion, no unicode, and it does not build a
 * document. It is a string search with quote-awareness bolted on, and
 * the header says so in capitals because the temptation, the first time
 * somebody wants a second field of a different shape, will be to "just
 * extend it".
 *
 * WHAT IT DELIBERATELY GETS WRONG
 *
 * A key nested inside a sub-object is found as readily as a top-level
 * one -- there is no depth tracking, because tracking depth is the
 * first half of writing a parser. For the byuuid response, whose
 * objects are flat, that difference cannot arise. If a caller ever
 * points this at a nested document it will return a plausible wrong
 * answer, which is the failure mode to remember: it is silent.
 *
 * If more than a field or two is ever wanted, or anything nested, the
 * answer is a real parser as a component, chosen and tested as one --
 * not this file with more code in it.
 *
 * WHAT IT GETS RIGHT, because these are what break a naive strstr()
 *
 *  - A key must be a KEY: quoted, and followed by a colon. `"favicon"`
 *    appearing inside another field's VALUE does not match, which
 *    matters because the directory's `homepage` and `tags` routinely
 *    contain arbitrary text.
 *  - Escapes in the value: \" \\ \/ \n \r \t are unescaped, and \uXXXX
 *    is passed through as-is rather than half-decoded.
 *  - null, a number, or an array as the value: reported as absent
 *    rather than as a string, because a caller that got `null` back as
 *    the four characters "null" would go and fetch http://null.
 *
 * NO FREERTOS AND NO ALLOCATION, so jsonpicktest covers all of it.
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

/*
 * Step over a JSON string that starts at `p` (which must be the opening
 * quote), returning the character after the closing quote, or NULL if
 * the string never closes.
 *
 * Separate because it is needed twice -- once to skip keys and values
 * that are not wanted, once to read the one that is -- and a second
 * copy of escape handling is exactly where a scanner like this goes
 * wrong.
 */
static inline const char *jsonpick_skip_string(const char *p, const char *end)
{
    if (p >= end || *p != '"') return NULL;
    p++;
    while (p < end) {
        if (*p == '\\') {
            p += 2;             /* the escaped character, whatever it is */
            continue;
        }
        if (*p == '"') return p + 1;
        p++;
    }
    return NULL;
}

/*
 * Copy a JSON string value into `out`, unescaping as it goes.
 *
 * Truncates rather than overflowing, and reports what it did: a
 * truncated URL is a different URL, not a shorter one, and fetching it
 * would be a request to somewhere nobody asked for.
 */
static inline bool jsonpick_copy_string(const char *p, const char *end,
                                        char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (p >= end || *p != '"') return false;
    p++;

    size_t o = 0;
    while (p < end) {
        if (*p == '"') {
            out[o] = '\0';
            return true;
        }
        if (o + 1 >= out_size) { out[0] = '\0'; return false; }   /* truncate */

        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
                case 'n': out[o++] = '\n'; break;
                case 'r': out[o++] = '\r'; break;
                case 't': out[o++] = '\t'; break;
                case 'b': out[o++] = '\b'; break;
                case 'f': out[o++] = '\f'; break;
                case '"': out[o++] = '"';  break;
                case '\\': out[o++] = '\\'; break;
                case '/': out[o++] = '/';  break;
                case 'u':
                    /* Passed through rather than decoded. A station name
                     * with a \u in it is a display string this does not
                     * read, and half-decoding UTF-16 surrogate pairs is
                     * the kind of thing that looks fine until a station
                     * in Seoul turns up. */
                    if (o + 6 >= out_size) { out[0] = '\0'; return false; }
                    out[o++] = '\\';
                    out[o++] = 'u';
                    for (int k = 1; k <= 4 && p + k < end; k++) {
                        out[o++] = p[k];
                    }
                    p += 4;
                    break;
                default:
                    out[o++] = *p;
                    break;
            }
            p++;
            continue;
        }
        out[o++] = *p++;
    }
    /* Ran out before the closing quote. Cleared, because everything
     * copied so far is the front of a string whose end the network
     * never sent -- and a half-copied URL is not a shorter URL, it is a
     * different one, with no terminator behind it. Both failure paths
     * above clear for the same reason; the host test asserts all
     * three, and caught two of them. */
    out[0] = '\0';
    return false;
}

/*
 * Find `key` used as a key, and copy its string value into `out`.
 *
 * False when the key is absent, when its value is not a string (null, a
 * number, an object, an array), or when the value would not fit. `out`
 * is an empty string in every one of those cases, so a caller that
 * ignores the return value gets nothing rather than something stale.
 */
static inline bool jsonpick_string(const char *json, size_t n,
                                   const char *key,
                                   char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!json || !key || !key[0] || n == 0) return false;

    const char *p = json;
    const char *end = json + n;
    const size_t keylen = strlen(key);

    while (p < end) {
        if (*p != '"') { p++; continue; }

        /*
         * A quoted token. Whether it is a key is decided by what
         * FOLLOWS it, which is the whole reason this is not a strstr:
         * the directory's `homepage` and `tags` fields regularly
         * contain the text of other field names, and a search that
         * matched inside a value would read one station's tags as
         * another station's favicon.
         */
        const char *after = jsonpick_skip_string(p, end);
        if (!after) return false;       /* unterminated: give up, do not guess */

        const bool match = ((size_t)(after - p) == keylen + 2) &&
                           (strncmp(p + 1, key, keylen) == 0);

        const char *q = after;
        while (q < end && (*q == ' ' || *q == '\t' ||
                           *q == '\n' || *q == '\r')) q++;

        if (q < end && *q == ':') {
            q++;
            while (q < end && (*q == ' ' || *q == '\t' ||
                               *q == '\n' || *q == '\r')) q++;
            if (match) {
                /* Found, and its value is whatever it is. A non-string
                 * is reported as absent rather than coerced. */
                if (q < end && *q == '"') {
                    return jsonpick_copy_string(q, end, out, out_size);
                }
                return false;
            }
            /* Not the key wanted. Skip a string value whole, so that
             * its contents cannot be scanned as if they were
             * structure. */
            if (q < end && *q == '"') {
                const char *skipped = jsonpick_skip_string(q, end);
                p = skipped ? skipped : end;
                continue;
            }
        }
        p = after;
    }
    return false;
}

#ifdef __cplusplus
}
#endif
