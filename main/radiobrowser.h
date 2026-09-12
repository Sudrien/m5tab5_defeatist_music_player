/*
 * radiobrowser.h -- the radio-browser.info request, built and nothing
 * more.
 *
 * The README has named https://www.radio-browser.info as where station
 * data comes from since v0.4.0 was scoped, and until now not one line of
 * code has mentioned it. Everything in the stream path was chosen for it
 * -- `stations.m3u` is M3U because radio-browser serves M3U, and
 * `stationlist.h` says so in its first paragraph -- but the bet was never
 * checked against the server's actual output, and nothing has ever
 * fetched anything.
 *
 * This is the first half of the first step: the URL, the headers, and
 * the facts about the response, with a host test. No socket, no task, no
 * JSON parser, nothing that needs a board. Same order `stationlist.h`
 * went in before phases 2 and 3, and for the same reason -- this is
 * string handling over bytes a person typed, which a host can test
 * completely, and it is the part that decides whether the M3U bet holds.
 *
 * THE BET HOLDS, AND radiobrowsertest.c IS THE PROOF
 *
 * The server's M3U is not quite the file `stations.m3u.example` shows.
 * It carries a directive nothing here had seen:
 *
 *     #EXTM3U
 *     #RADIOBROWSERUUID:01234567-89ab-cdef-0123-456789abcdef
 *     #EXTINF:-1,Best Radio
 *     http://stream.example.com/mp3_128
 *
 *     #RADIOBROWSERUUID:...
 *     #EXTINF:-1,Other Radio
 *     http://stream.example2.com/mp3_256
 *
 * `stationlist_parse()` handles it already, and by rule rather than by
 * luck: every `#` line that is not `#EXTINF` is skipped without comment,
 * which is the clause written for files that have been through another
 * player. The blank line between entries is skipped for the same reason.
 * So the response goes into the existing parser with the existing test
 * corpus and no new format. That is the whole return on choosing M3U,
 * and it is now demonstrated rather than assumed.
 *
 * WHAT MUST NOT BE THROWN AWAY: THE UUID
 *
 * `#RADIOBROWSERUUID` is skipped, not kept, and that is correct for now
 * and a cost later. The API asks that `/json/url/{stationuuid}` be
 * called whenever a listener starts a stream, so the directory can count
 * which stations are actually played -- it is how the service stays
 * useful, and it is the only thing radio-browser asks of a client in
 * return for having no key and no account. It cannot be done without the
 * UUID, and the UUID is only in the response.
 *
 * So a `stations.m3u` written from a search MUST keep the comment lines
 * verbatim rather than being rebuilt from parsed stations. Whoever adds
 * the writing: copy the body through, do not regenerate it.
 *
 * WHICH SERVER, AND WHY NOT THE OBVIOUS ONE
 *
 * There is no single host. The documented way is a DNS lookup of
 * `all.api.radio-browser.info`, which is a round-robin pool of mirror
 * addresses, then a reverse lookup per address to get a mirror's name;
 * or the SRV record `_api._tcp.radio-browser.info`, which gives the
 * names directly. A `/json/servers` endpoint exists and the docs say to
 * use it ONLY if the client cannot do DNS at all.
 *
 * **The pool name cannot be used over TLS.** `all.api.radio-browser.info`
 * has never had a certificate valid for that name -- it is a DNS
 * construct rather than a server -- so an HTTPS request to it fails
 * verification, which is a thing every client that has tried it has had
 * to work around. This player does TLS through esp_http_client and is
 * not going to stop verifying certificates to reach a station
 * directory.
 *
 * Hence RADIOBROWSER_HOSTS: named mirrors, tried in order, each of which
 * does have a certificate. That is a hardcoded list, which is the thing
 * the DNS lookup exists to avoid, and the docs are explicit that any of
 * them may go away. It is accepted deliberately and with the exit
 * written down: the fix is an SRV query, esp_netif has no SRV resolver,
 * and adding one to reach a station list is a larger piece of work than
 * the feature. A mirror going away costs a failed search and the next
 * name in the list.
 *
 * WHAT THE SERVICE ASKS, WHICH IS NOT NOTHING
 *
 * No key, no account -- which is why it was picked -- but three things
 * are asked and all three are cheap:
 *
 *   - A descriptive User-Agent of the form `appname/appversion`. It is
 *     how the maintainer knows who is using the service and can reach
 *     them. RADIOBROWSER_UA_FMT builds it from the project's own
 *     version, so it cannot drift.
 *   - Two to three requests a second, no more, and results cached for
 *     five to fifteen minutes. A search from a form on a phone is
 *     nowhere near this; a retry loop over the mirror list could be, so
 *     RADIOBROWSER_MIN_GAP_MS is stated here rather than left to the
 *     caller's judgement.
 *   - The click count above.
 *
 * `hidebroken=true` is not politeness but it belongs in the same list:
 * the directory checks every station daily, and a player that offers
 * stations the directory already knows are dead spends the listener's
 * time proving it.
 *
 * HOST-TESTED, NOT COMPILED FOR THE TARGET
 *
 * `texttest/radiobrowsertest.c`, and it links `stationlist.h` as well so
 * that the encoder and the parser are checked against each other rather
 * than separately. Pure inline functions over strings: no FreeRTOS, no
 * esp_http_client. Nothing here has been through the cross compiler, and
 * per CLAUDE.md that is stated rather than assumed.
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
 * Mirrors, in the order they are tried. Names rather than the pool, for
 * the certificate reason above.
 *
 * Two, not one, and not six. One is a single point of failure for a
 * service whose own documentation says any server may go down; six is a
 * list that rots faster than it helps, because every name in it is a
 * promise this file cannot keep.
 */
#define RADIOBROWSER_HOSTS  { "de1.api.radio-browser.info", \
                              "nl1.api.radio-browser.info" }
#define RADIOBROWSER_HOST_COUNT (2)

/* Minimum spacing between requests. The API asks for 2-3 per second at
 * most; 400 ms is inside that with room for the clock being coarse. */
#define RADIOBROWSER_MIN_GAP_MS (400)

/*
 * How long a result may be treated as current. The API asks for 5-15
 * minutes; the low end, because a search is a thing a person does once
 * and a stale answer is worse than a second request.
 */
#define RADIOBROWSER_CACHE_MS   (5 * 60 * 1000)

/* User-Agent. Built from the project version so it cannot drift from the
 * firmware that sent it -- pass esp_app_get_description()->version. */
#define RADIOBROWSER_UA_FMT     "DefeatistMusicPlayer/%s"

/*
 * Longest URL this builds, and longest search term accepted.
 *
 * The term is bounded well below the URL because percent-encoding can
 * triple it: a 64-byte term of nothing but spaces and accented letters
 * is 192 bytes encoded, and the fixed part of the path and query is
 * about 110. 512 leaves the arithmetic uncomfortable-proof rather than
 * exactly sufficient, which is the difference between a bound and a
 * coincidence.
 */
#define RADIOBROWSER_URL_MAX    (512)
#define RADIOBROWSER_TERM_MAX   (64)

/*
 * How many stations a search returns.
 *
 * The server's default limit is 100000, which would be a 40 MB M3U into
 * a 64 KB buffer. STATIONLIST_MAX is 64 and STATIONS_FILE_MAX is 64 KB,
 * so 50 is under both with room for the comment lines the response
 * carries and the ones a hand-edited file already had.
 */
#define RADIOBROWSER_LIMIT      (50)

/* ------------------------------------------------------------------ */
/* Percent-encoding                                                    */
/* ------------------------------------------------------------------ */

/*
 * Unreserved per RFC 3986, and nothing else.
 *
 * Deliberately the smallest safe set rather than the largest legal one.
 * The input is a search term typed into a form by a person, so it can
 * contain `&`, `=`, `#`, `+`, `%` and UTF-8, every one of which changes
 * the meaning of the query string or the request line if it goes through
 * unencoded -- and `+` is the nasty one, because it is legal in a path
 * and means a space in a query. Encoding a character that did not need
 * it costs two bytes; not encoding one that did is a different request
 * from the one the listener asked for.
 */
static inline bool radiobrowser_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

/*
 * Percent-encode `src` into `dst`. Returns false if it would not fit, in
 * which case `dst` is an empty string rather than a truncated one.
 *
 * Truncation is refused rather than performed because a cut UTF-8
 * sequence is not a shorter search, it is an invalid one, and a cut
 * `%E2` is a literal percent the server will reject. There is no
 * half-encoded string worth sending.
 */
static inline bool radiobrowser_encode(char *dst, size_t dst_size,
                                       const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!dst || dst_size == 0) return false;
    dst[0] = '\0';
    if (!src) return true;

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (radiobrowser_unreserved(*p)) {
            if (o + 1 >= dst_size) { dst[0] = '\0'; return false; }
            dst[o++] = (char)*p;
        } else {
            if (o + 3 >= dst_size) { dst[0] = '\0'; return false; }
            dst[o++] = '%';
            dst[o++] = hex[*p >> 4];
            dst[o++] = hex[*p & 0x0F];
        }
    }
    dst[o] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* The request                                                         */
/* ------------------------------------------------------------------ */

/*
 * Trim and bound a term the way the form's field cannot.
 *
 * Returns false for a term that is empty once trimmed, which is the
 * commonest form submission there is -- somebody pressed the button --
 * and which must not become a search for everything. `limit=50` would
 * make that a 50-station list of whatever sorts first, which looks like
 * the search having worked.
 */
static inline bool radiobrowser_term_ok(const char *term, char *out,
                                        size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!term) return false;

    while (*term == ' ' || *term == '\t') term++;
    size_t n = strlen(term);
    while (n > 0 && (term[n - 1] == ' ' || term[n - 1] == '\t')) n--;
    if (n == 0) return false;
    if (n >= out_size) return false;

    memcpy(out, term, n);
    out[n] = '\0';
    return true;
}

/*
 * Build the search URL for `host`, searching station names for `term`.
 *
 * `/m3u/stations/search?name=...`, which is the endpoint whose output
 * `stationlist_parse()` already reads. Not `/m3u/stations/byname/{term}`,
 * though it exists and is shorter: the term would be a path segment, and
 * a path segment containing an encoded slash is rewritten or rejected by
 * enough intermediaries that it is not worth finding out which. In a
 * query string the encoding is unambiguous.
 *
 * The fixed parameters and why each is there:
 *
 *   hidebroken=true   the directory checks stations daily and knows
 *                     which are dead; offering them makes the listener
 *                     discover it instead
 *   order=votes       with reverse, the most-voted first. A name search
 *                     for "jazz" matches hundreds and the default order
 *                     is alphabetical, which puts the numerals first
 *   reverse=true      see above
 *   limit=50          the server's default is 100000
 *
 * Returns false if anything would not fit, leaving `url` empty.
 */
static inline bool radiobrowser_search_url(char *url, size_t url_size,
                                           const char *host,
                                           const char *term)
{
    if (!url || url_size == 0) return false;
    url[0] = '\0';
    if (!host || !host[0]) return false;

    char clean[RADIOBROWSER_TERM_MAX];
    if (!radiobrowser_term_ok(term, clean, sizeof(clean))) return false;

    char enc[RADIOBROWSER_TERM_MAX * 3 + 1];
    if (!radiobrowser_encode(enc, sizeof(enc), clean)) return false;

    /* Assembled by hand rather than with snprintf, so that this header
     * needs no stdio and the test can run it under ASan with no
     * libc formatting in the way. Every append is bounds-checked against
     * the same `o`. */
    const char *parts[] = {
        "https://", host, "/m3u/stations/search?name=", enc,
        "&hidebroken=true&order=votes&reverse=true&limit=50",
    };
    size_t o = 0;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        const size_t len = strlen(parts[i]);
        if (o + len >= url_size) { url[0] = '\0'; return false; }
        memcpy(url + o, parts[i], len);
        o += len;
    }
    url[o] = '\0';
    return true;
}

/*
 * The click-count URL for a station UUID.
 *
 * Not called by anything yet -- nothing keeps the UUID, see the note at
 * the top -- and here anyway, because the endpoint is the obligation and
 * a header that describes the obligation without naming the call leaves
 * the next person to look it up again.
 *
 * The UUID is checked for shape rather than parsed: 36 characters of hex
 * and dashes. It goes into a path segment, so a term that is not a UUID
 * must not reach it, and "is it a UUID" is a cheaper question than
 * percent-encoding a path.
 */
static inline bool radiobrowser_uuid_ok(const char *uuid)
{
    if (!uuid) return false;
    size_t n = 0;
    for (const char *p = uuid; *p; p++, n++) {
        const char c = *p;
        const bool hex = (c >= '0' && c <= '9') ||
                         (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex && c != '-') return false;
    }
    return n == 36;
}

static inline bool radiobrowser_click_url(char *url, size_t url_size,
                                          const char *host,
                                          const char *uuid)
{
    if (!url || url_size == 0) return false;
    url[0] = '\0';
    if (!host || !host[0]) return false;
    if (!radiobrowser_uuid_ok(uuid)) return false;

    const char *parts[] = { "https://", host, "/json/url/", uuid };
    size_t o = 0;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        const size_t len = strlen(parts[i]);
        if (o + len >= url_size) { url[0] = '\0'; return false; }
        memcpy(url + o, parts[i], len);
        o += len;
    }
    url[o] = '\0';
    return true;
}

#ifdef __cplusplus
}
#endif
