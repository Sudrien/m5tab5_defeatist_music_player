/*
 * portalweb.c -- see portalweb.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "portalweb.h"

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
