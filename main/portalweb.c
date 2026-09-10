/*
 * portalweb.c -- see portalweb.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "portalweb.h"

#include <stdio.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool portalweb_field(const char *body, size_t len, const char *key,
                     char *out, size_t out_size)
{
    if (!out || !out_size) return false;
    out[0] = '\0';
    if (!body || !key) return false;

    const size_t klen = strlen(key);
    size_t i = 0;
    while (i < len) {
        /* One pair: up to the next '&' or the end. */
        size_t end = i;
        while (end < len && body[end] != '&') end++;

        if (end - i > klen && memcmp(body + i, key, klen) == 0 &&
            body[i + klen] == '=') {
            size_t o = 0;
            for (size_t k = i + klen + 1; k < end; k++) {
                char c = body[k];
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    /* Two hex digits, both inside this pair. */
                    if (k + 2 >= end) { out[0] = '\0'; return false; }
                    const int hi = hexval(body[k + 1]);
                    const int lo = hexval(body[k + 2]);
                    if (hi < 0 || lo < 0) { out[0] = '\0'; return false; }
                    c = (char)((hi << 4) | lo);
                    k += 2;
                    if (c == '\0') { out[0] = '\0'; return false; }
                }
                if (o + 1 >= out_size) { out[0] = '\0'; return false; }
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        /* A key with an empty value ("ssid=") is end - i == klen + 1,
         * which the test above takes; a bare key with no '=' is not a
         * field. */
        i = end + 1;
    }
    return false;
}

bool portalweb_escape(const char *in, char *out, size_t out_size)
{
    if (!out || !out_size) return false;
    out[0] = '\0';
    if (!in) return true;

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        const char *rep = NULL;
        char one[2] = { (char)*p, '\0' };
        switch (*p) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;";  break;
        default:
            rep = (*p < 0x20 || *p == 0x7F) ? "\xEF\xBF\xBD" : one;
        }
        const size_t rl = strlen(rep);
        if (o + rl + 1 > out_size) { out[0] = '\0'; return false; }
        memcpy(out + o, rep, rl);
        o += rl;
    }
    out[o] = '\0';
    return true;
}

void portalweb_ap_name(const uint8_t mac[6], char *out, size_t out_size)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) {
        h ^= mac ? mac[i] : 0;
        h *= 16777619u;
    }
    const uint16_t tag = (uint16_t)((h >> 16) ^ (h & 0xFFFF));
    static const char X[] = "0123456789ABCDEF";
    const char name[] = {
        'D','e','f','e','a','t','i','s','t','-',
        X[(tag >> 12) & 0xF], X[(tag >> 8) & 0xF],
        X[(tag >> 4) & 0xF],  X[tag & 0xF], '\0'
    };
    if (!out || !out_size) return;
    if (out_size < sizeof(name)) { out[0] = '\0'; return; }
    memcpy(out, name, sizeof(name));
}

portalweb_check_t portalweb_check(const char *ssid, const char *secret)
{
    const size_t sl = ssid ? strlen(ssid) : 0;
    if (sl == 0 || sl > 32) return PORTALWEB_BAD_SSID;

    const size_t pl = secret ? strlen(secret) : 0;
    if (pl == 0) return PORTALWEB_NO_SECRET;

    /* Before the length: UTF-8 changes the byte count, and a curly
     * quote in a 10-character password is not a length problem. */
    for (size_t i = 0; i < pl; i++) {
        if ((unsigned char)secret[i] >= 0x80) return PORTALWEB_NON_ASCII;
    }

    if (pl == 64) {
        for (size_t i = 0; i < pl; i++) {
            if (hexval(secret[i]) < 0) return PORTALWEB_BAD_SECRET;
        }
        return PORTALWEB_OK_PSK;
    }
    if (pl < 8 || pl > 63) return PORTALWEB_BAD_SECRET;
    for (size_t i = 0; i < pl; i++) {
        const unsigned char c = (unsigned char)secret[i];
        if (c < 0x20 || c > 0x7E) return PORTALWEB_BAD_SECRET;
    }
    return PORTALWEB_OK_PASSPHRASE;
}

void portalweb_hex(const uint8_t *in, size_t n, char *out)
{
    static const char x[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = x[in[i] >> 4];
        out[2 * i + 1] = x[in[i] & 0xF];
    }
    out[2 * n] = '\0';
}

/*
 * Decode one UTF-8 sequence at `p`. Returns its length in bytes (1..4)
 * and the code point in *cp, or 1 with *cp = 0xFFFFFFFF for a byte that
 * does not start a valid sequence -- which is reported as such, since a
 * phone sending invalid UTF-8 is itself the finding.
 */
static size_t utf8_one(const unsigned char *p, uint32_t *cp)
{
    const unsigned char c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    size_t len;
    uint32_t v, min;
    if ((c & 0xE0) == 0xC0)      { len = 2; v = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07; min = 0x10000; }
    else { *cp = 0xFFFFFFFFu; return 1; }
    for (size_t i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFFFFFFu; return 1; }
        v = (v << 6) | (p[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        *cp = 0xFFFFFFFFu;
        return 1;
    }
    *cp = v;
    return len;
}

static const struct { uint32_t cp; const char *name; const char *ascii; } LOOKALIKE[] = {
    { 0x2018, "curly opening quote",     "'"   },
    { 0x2019, "curly apostrophe",        "'"   },
    { 0x201C, "curly opening double quote", "\"" },
    { 0x201D, "curly closing double quote", "\"" },
    { 0x2013, "en dash",                 "-"   },
    { 0x2014, "em dash",                 "-"   },
    { 0x2026, "ellipsis",                "..." },
    { 0x00A0, "no-break space",          "a plain space" },
    { 0x00B4, "acute accent",            "'"   },
    { 0x02BC, "modifier apostrophe",     "'"   },
};

static int lookalike(uint32_t cp)
{
    for (size_t i = 0; i < sizeof(LOOKALIKE) / sizeof(LOOKALIKE[0]); i++) {
        if (LOOKALIKE[i].cp == cp) return (int)i;
    }
    return -1;
}

/* Append to a bounded buffer; on overflow, end in "..." and stop. */
static bool put(char *out, size_t size, size_t *o, const char *s)
{
    if (!size) return false;
    const size_t n = strlen(s);
    if (*o + n + 1 <= size) {
        memcpy(out + *o, s, n + 1);
        *o += n;
        return true;
    }
    if (size >= 4) {
        const size_t at = (*o + 4 <= size) ? *o : size - 4;
        memcpy(out + at, "...", 4);
        *o = at + 3;
    } else {
        out[size - 1] = '\0';
    }
    return false;
}

void portalweb_describe(const char *secret, char *out, size_t out_size)
{
    if (!out || !out_size) return;
    out[0] = '\0';
    if (!secret) return;

    const size_t bytes = strlen(secret);
    size_t chars = 0;
    for (size_t i = 0; i < bytes; chars++) {
        uint32_t cp;
        i += utf8_one((const unsigned char *)secret + i, &cp);
    }

    size_t o = 0;
    char buf[48];
    snprintf(buf, sizeof(buf), "%zu byte%s, %zu char%s", bytes,
             bytes == 1 ? "" : "s", chars, chars == 1 ? "" : "s");
    if (!put(out, out_size, &o, buf)) return;
    if (bytes && secret[0] == ' ' && !put(out, out_size, &o, ", leading space")) return;
    if (bytes && secret[bytes - 1] == ' ' && !put(out, out_size, &o, ", trailing space")) return;
    if (bytes && secret[0] >= 'A' && secret[0] <= 'Z' &&
        !put(out, out_size, &o, ", first letter capital")) return;

    for (size_t i = 0; i < bytes;) {
        uint32_t cp;
        const size_t n = utf8_one((const unsigned char *)secret + i, &cp);
        if (cp == 0xFFFFFFFFu) {
            snprintf(buf, sizeof(buf), ", invalid UTF-8 byte 0x%02X",
                     (unsigned char)secret[i]);
            if (!put(out, out_size, &o, buf)) return;
        } else if (cp < 0x20 || cp == 0x7F) {
            snprintf(buf, sizeof(buf), ", control U+%04X", (unsigned)cp);
            if (!put(out, out_size, &o, buf)) return;
        } else if (cp >= 0x80) {
            const int k = lookalike(cp);
            snprintf(buf, sizeof(buf), ", U+%04X%s%s", (unsigned)cp,
                     k >= 0 ? " " : "", k >= 0 ? LOOKALIKE[k].name : "");
            if (!put(out, out_size, &o, buf)) return;
        }
        i += n;
    }
}

void portalweb_non_ascii_hint(const char *secret, char *out, size_t out_size)
{
    if (!out || !out_size) return;
    out[0] = '\0';
    if (!secret) return;

    for (size_t i = 0; secret[i];) {
        uint32_t cp;
        const size_t n = utf8_one((const unsigned char *)secret + i, &cp);
        if (cp == 0xFFFFFFFFu) {
            snprintf(out, out_size, "a byte that is not valid text (0x%02X)",
                     (unsigned char)secret[i]);
            return;
        }
        if (cp >= 0x80) {
            const int k = lookalike(cp);
            if (k >= 0) {
                snprintf(out, out_size, "a %s (U+%04X) -- the router probably wants %s",
                         LOOKALIKE[k].name, (unsigned)cp, LOOKALIKE[k].ascii);
            } else {
                snprintf(out, out_size, "the character U+%04X, which a Wi-Fi "
                         "password cannot contain", (unsigned)cp);
            }
            return;
        }
        i += n;
    }
}

portalweb_plan_t portalweb_join_plan(portalweb_check_t kind, portalweb_net_t net)
{
    switch (kind) {
    case PORTALWEB_OK_PSK:
        /* Typed as a key. Even on a WPA3 network that is what was given,
         * and the refusal will say so. */
        return PORTALWEB_TRY_AS_TYPED;
    case PORTALWEB_OK_PASSPHRASE:
        return net == PORTALWEB_NET_WPA3_CAPABLE
             ? PORTALWEB_TRY_PASSPHRASE_ONLY
             : PORTALWEB_TRY_PSK_THEN_PASSPHRASE;
    default:
        return PORTALWEB_TRY_NONE;
    }
}
