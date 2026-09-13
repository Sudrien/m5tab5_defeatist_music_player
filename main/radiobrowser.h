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
 * This WAS the first half of the first step: the URL, the headers, and
 * the facts about the response, with a host test. No socket, no task, no
 * JSON parser, nothing that needs a board. 0401 adds the other half --
 * radiobrowser.c, which consumes the endpoints below over TLS and
 * caches what comes back -- and everything in this file that was pure
 * string handling has stayed pure string handling, so the host test
 * still covers the part a host can cover. Same order `stationlist.h`
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

/* ------------------------------------------------------------------ */
/* Browsing, which is not searching                                     */
/* ------------------------------------------------------------------ */

/*
 * WHY THERE IS A BROWSE PATH AT ALL, AND WHY IT CAME FIRST.
 *
 * This header was written around `search`, because a directory is a
 * thing you search. This player has no keyboard. A search box on a
 * device with no keyboard is a feature that can only be reached through
 * the Wi-Fi portal -- which is an access point that only exists while
 * Wi-Fi is DOWN, which is the one state in which the directory cannot
 * be reached. The two halves of the obvious design are mutually
 * exclusive and it took until somebody asked "is the directory too much
 * for a browsing directory" to notice.
 *
 * Browsing needs no typing. The chooser is already a drill-down list --
 * folders then files -- and a tag then its stations is the same gesture
 * against the same rows.
 *
 * THE FIRST LEVEL IS HARDCODED AND THE SECOND IS LIVE, which is the
 * only line in this file that is a compromise rather than a
 * consequence:
 *
 *   - Every STATION list has an M3U form. `/m3u/stations/bytag/jazz`,
 *     `bycountry`, `topvote`, `topclick` all parse with
 *     stationlist_parse() and the corpus it already has. No new format,
 *     no parser, no second thing to test.
 *   - The INDEX lists -- `/json/tags`, `/json/countries` -- have no M3U
 *     form, because they are not stations. A live first level therefore
 *     costs a JSON parser on the P4, which stations.h chose M3U
 *     specifically to avoid.
 *
 * So RADIOBROWSER_TAGS is a pinned list and the exit is written down:
 * when something here needs JSON for another reason, the first level
 * becomes /json/tags and this constant goes. Until then a pinned tag
 * rots far more slowly than a pinned mirror -- genre names on
 * radio-browser are years old -- and a tag that returns nothing is a
 * line in the status row rather than a failure.
 */
typedef enum {
    RADIOBROWSER_TOPVOTE,       /* most-voted overall; `value` unused */
    RADIOBROWSER_TOPCLICK,      /* most-listened overall; `value` unused */
    RADIOBROWSER_BYTAG,         /* `value` is a tag */
    RADIOBROWSER_BYCOUNTRY,     /* `value` is a country name */
    RADIOBROWSER_SEARCH,        /* `value` is a name fragment */
} radiobrowser_kind_t;

/*
 * The pinned first level. Twelve, which is one screen of the chooser's
 * rows and no scrolling, plus the two charts the caller adds above
 * them. Ordered roughly by how many stations the directory has under
 * each, so the first tap is the least likely to land on a thin one.
 */
#define RADIOBROWSER_TAGS   { "pop", "rock", "classical", "jazz", \
                              "news", "talk", "dance", "electronic", \
                              "country", "oldies", "metal", "ambient" }
#define RADIOBROWSER_TAG_COUNT  (12)

/*
 * Build a list URL.
 *
 * `value` is percent-encoded into a QUERY parameter for every kind,
 * including the ones radio-browser also exposes as a path segment. A
 * path segment containing an encoded slash is rewritten or rejected by
 * enough intermediaries that it is not worth finding out which, and a
 * tag like `rock & roll` is exactly the case that would find one.
 *
 * TOPVOTE and TOPCLICK take no value and their endpoints are plain
 * paths with a limit on the end, which is why they are a separate
 * branch rather than a search with an empty term -- an empty term is
 * refused by radiobrowser_term_ok(), deliberately and for its own
 * reasons.
 *
 * Returns false if anything would not fit, leaving `url` empty.
 */
static inline bool radiobrowser_list_url(char *url, size_t url_size,
                                         const char *host,
                                         radiobrowser_kind_t kind,
                                         const char *value)
{
    if (!url || url_size == 0) return false;
    url[0] = '\0';
    if (!host || !host[0]) return false;

    char enc[RADIOBROWSER_TERM_MAX * 3 + 1];
    enc[0] = '\0';
    const bool needs_value = (kind == RADIOBROWSER_BYTAG ||
                              kind == RADIOBROWSER_BYCOUNTRY ||
                              kind == RADIOBROWSER_SEARCH);
    if (needs_value) {
        char clean[RADIOBROWSER_TERM_MAX];
        if (!radiobrowser_term_ok(value, clean, sizeof(clean))) return false;
        if (!radiobrowser_encode(enc, sizeof(enc), clean)) return false;
    }

    const char *path =
        (kind == RADIOBROWSER_TOPVOTE)  ? "/m3u/stations/topvote/50" :
        (kind == RADIOBROWSER_TOPCLICK) ? "/m3u/stations/topclick/50" :
                                          "/m3u/stations/search?";
    const char *param =
        (kind == RADIOBROWSER_BYTAG)     ? "tag=" :
        (kind == RADIOBROWSER_BYCOUNTRY) ? "country=" :
        (kind == RADIOBROWSER_SEARCH)    ? "name=" : "";
    /* The charts carry their limit in the path, so their query opens
     * with `?` and the searches continue one already open. */
    const char *tail = needs_value
        ? "&hidebroken=true&order=votes&reverse=true&limit=50"
        : "?hidebroken=true";

    const char *parts[] = { "https://", host, path, param, enc, tail };
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

/* ------------------------------------------------------------------ */
/* The two rules the service asks for, as arithmetic                    */
/* ------------------------------------------------------------------ */

/*
 * Both of these are policy the API states in prose -- two to three
 * requests a second, results cached five to fifteen minutes -- and both
 * are one comparison. They are here rather than in radiobrowser.c so
 * that the rules can be tested on a host without a socket, which is the
 * same split netplan.h and bufferplan.h use for the same reason.
 *
 * Tick counts are unsigned and wrap. Every comparison below is written
 * as a difference against the stamp rather than as `now > stamp + N`,
 * because the second form is wrong for 49 days out of every 49 days and
 * right the rest of the time.
 */
static inline bool radiobrowser_gap_ok(uint32_t now_ms, uint32_t last_ms,
                                       bool ever)
{
    if (!ever) return true;
    return (uint32_t)(now_ms - last_ms) >= RADIOBROWSER_MIN_GAP_MS;
}

static inline bool radiobrowser_cache_fresh(uint32_t now_ms, uint32_t stamp_ms)
{
    return (uint32_t)(now_ms - stamp_ms) < RADIOBROWSER_CACHE_MS;
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

/* ------------------------------------------------------------------ */
/* The menu, which is one table and two callers                         */
/* ------------------------------------------------------------------ */

/*
 * The first level of the radio tab, as rows.
 *
 * HERE RATHER THAN IN browser.c, because two things need it and they
 * are on different tasks: the chooser draws the rows and the player
 * turns a row number into a request. A second copy of the table is the
 * thing that drifts -- and the way it would drift is the worst
 * available: row N labelled one thing and fetching another, which looks
 * like the directory being wrong rather than like this file being
 * wrong.
 *
 * Row 0 is the card's own list, so the way back from the directory is
 * the first row rather than a button somebody has to find. Then the two
 * charts, then the pinned tags.
 */
#define RADIOBROWSER_MENU_ROWS  (RADIOBROWSER_TAG_COUNT + 3)

static inline const char *radiobrowser_menu_label(int row)
{
    static const char *const tags[RADIOBROWSER_TAG_COUNT] = RADIOBROWSER_TAGS;
    if (row == 0) return "stations.m3u on the card";
    if (row == 1) return "Most voted";
    if (row == 2) return "Most listened";
    if (row >= 3 && row < RADIOBROWSER_MENU_ROWS) return tags[row - 3];
    return "";
}

/*
 * What row `row` fetches. False for row 0, which is not a fetch at all
 * -- it is the card, and the caller reloads rather than requests.
 *
 * `value` is left pointing at the table rather than copied, which is
 * safe because the table is static const and outlives everything; a
 * caller that wants to keep it past the call should copy it.
 */
static inline bool radiobrowser_menu_kind(int row, radiobrowser_kind_t *kind,
                                          const char **value)
{
    static const char *const tags[RADIOBROWSER_TAG_COUNT] = RADIOBROWSER_TAGS;
    if (row <= 0 || row >= RADIOBROWSER_MENU_ROWS) return false;
    if (value) *value = NULL;
    if (row == 1) { if (kind) *kind = RADIOBROWSER_TOPVOTE;  return true; }
    if (row == 2) { if (kind) *kind = RADIOBROWSER_TOPCLICK; return true; }
    if (kind)  *kind  = RADIOBROWSER_BYTAG;
    if (value) *value = tags[row - 3];
    return true;
}

/* ------------------------------------------------------------------ */
/* The client, which is radiobrowser.c                                  */
/* ------------------------------------------------------------------ */

/*
 * Fetch a list, from the cache if it is there and from a mirror if it
 * is not, as an M3U body ready for stationlist_parse().
 *
 * BLOCKS, for up to two mirror timeouts. Call it from the player task,
 * which is where every other slow thing in this program happens, and
 * never from ui_task -- the same rule stations_load() carries and for a
 * stronger reason: this one waits on a network rather than on a card.
 *
 * `value` is the tag, country or name fragment for the kinds that take
 * one and is ignored by the charts. `out` is NUL-terminated on success.
 *
 * False means no list, and the log says which of the three reasons it
 * was: the URL would not build, no mirror answered, or the body did not
 * fit. None of them is worth distinguishing at the call site -- what a
 * caller does about it is the same in all three -- which is why this
 * returns a bool rather than an esp_err_t.
 */
bool radiobrowser_list(radiobrowser_kind_t kind, const char *value,
                       char *out, size_t out_size, size_t *out_len);

/*
 * The station's own artwork URL, from the directory, by uuid.
 *
 * WHY THIS ONE FIELD IS WORTH A JSON READER. stations.h chose M3U to
 * keep a parser off the P4, and that stands for station lists. But M3U
 * has nowhere to put artwork, so the choice quietly cost every station
 * its picture, and `favicon` is the only field in the directory anyone
 * has wanted since. jsonpick.h is a string search with quote-awareness,
 * not a parser, and its header says at length why it must not grow into
 * one.
 *
 * The uuid comes from `#RADIOBROWSERUUID:` in the M3U, which
 * stationlist.h now carries on the station it belongs to. A
 * hand-written stations.m3u has none, so this is a directory feature
 * and callers must handle an empty uuid as the ordinary case rather
 * than as an error.
 *
 * BLOCKS, cached and mirror-failed-over like radiobrowser_list(), and
 * the same rule applies: the player task, never ui_task.
 *
 * False means no artwork, which is the COMMON answer -- most stations
 * in the directory have no favicon, many have a dead link, and some
 * have something that is not an image at all. Nothing here fetches the
 * picture; that is the caller's, and it is where the size and
 * content-type limits belong.
 */
bool radiobrowser_favicon(const char *uuid, char *out, size_t out_size);

/*
 * Tell the directory a station was played.
 *
 * Call it when audio actually reaches the speaker, not when a row is
 * tapped: a station that 404s, or redirects into nothing, is not a
 * play, and a listener scrolling through fifty rows must not inflate
 * fifty counts. See radiobrowser.c for why this one call bypasses the
 * cache.
 *
 * Blocks briefly. Player task. Failure is ignored on purpose -- no
 * station may fail to play because a counter did not increment.
 */
void radiobrowser_click(const char *uuid);

/*
 * Fetch a station's artwork from a URL that came from icy-logo or from
 * the directory.
 *
 * Bounded, content-type checked and magic-byte checked -- see
 * radiobrowser.c, which explains what each limit is for. On success the
 * caller owns *out and must free() it, and the bytes are a JPEG or PNG
 * that albumart_show() will accept.
 *
 * False is the ordinary outcome. Most stations have no artwork, many
 * have a dead link, and a blank square is not a fault.
 */
bool radiobrowser_art_fetch(const char *url, uint8_t **out, size_t *out_len);

/*
 * Drop everything held.
 *
 * For the case the cache cannot see: the listener has been somewhere
 * else, or has fixed their Wi-Fi, and wants the directory asked again
 * rather than told what it said five minutes ago. The chooser's reload
 * button is the natural caller.
 */
void radiobrowser_cache_clear(void);

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
