/*
 * addrpin.h -- connecting to one chosen address of a host, and moving to
 * the next when it does not answer. 5068.
 *
 * WHY. live02.rfi.fr is an alias for a pool of twelve IPv4 addresses
 * (live-reflector-rr.ice.infomaniak.ch, 60 s TTL). A browser that finds
 * one of them silent tries the next; esp-tls connects to the first
 * address getaddrinfo() hands back and nothing else, and lwIP caches that
 * answer for the TTL -- so five attempts in 39 s all went to the same
 * address and all timed out, on a station Firefox plays at once.
 *
 * HOW. netstream resolves the host itself (up to
 * CONFIG_LWIP_DNS_MAX_HOST_IP addresses), picks one by a turn counter
 * that advances on every failed connect, and opens the URL with that
 * address in place of the name. The name goes back in as the Host
 * header, which is what the server routes on.
 *
 * PLAIN HTTP ONLY. For https the name also has to be the TLS server name
 * and the certificate's name, and esp_http_client takes that
 * (`common_name`) only at init -- where it would then be wrong for a
 * redirect to a different host on the next hop. So https connects by name
 * as before; it is resolved only so the log can say which address it
 * most likely used.
 *
 * This file is the string half, which a host test can reach: taking a URL
 * apart and putting it back with an address in it. The lookup is in
 * netstream.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ADDRPIN_HOST_MAX   (128)

typedef struct {
    bool   https;
    char   host[ADDRPIN_HOST_MAX];
    int    port;        /* as written; 0 when the URL gives none */
    size_t rest;        /* offset of the path, query or end in the URL */
} addrpin_url_t;

/* A dotted IPv4 literal, or anything bracketed (IPv6). Nothing to pin. */
static inline bool addrpin_is_literal(const char *host)
{
    if (host[0] == '[') return true;
    int dots = 0;
    for (const char *p = host; *p; p++) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') return false;
    }
    return dots == 3;
}

/*
 * Take http(s)://host[:port][rest] apart. False for another scheme, a
 * URL with credentials in it (user@host -- rewriting that is not worth
 * getting wrong), an empty or over-long host, a bad port, or a host that
 * is already an address.
 */
static inline bool addrpin_parse(const char *url, addrpin_url_t *u)
{
    memset(u, 0, sizeof(*u));
    size_t i;
    if (strncmp(url, "http://", 7) == 0) {
        i = 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        i = 8;
        u->https = true;
    } else {
        return false;
    }

    const size_t h0 = i;
    /* Credentials anywhere in the authority: not rewritten. */
    const size_t auth_end = h0 + strcspn(url + h0, "/?#");
    const char *at = memchr(url + h0, '@', auth_end - h0);
    if (at) return false;

    while (i < auth_end && url[i] != ':') i++;
    const size_t hlen = i - h0;
    if (hlen == 0 || hlen >= ADDRPIN_HOST_MAX) return false;
    memcpy(u->host, url + h0, hlen);
    u->host[hlen] = '\0';

    if (i < auth_end) {             /* url[i] == ':' */
        i++;
        int port = 0, digits = 0;
        while (i < auth_end && url[i] >= '0' && url[i] <= '9') {
            port = port * 10 + (url[i] - '0');
            if (++digits > 5) return false;
            i++;
        }
        if (digits == 0 || i != auth_end || port < 1 || port > 65535) return false;
        u->port = port;
    }
    u->rest = i;
    return !addrpin_is_literal(u->host);
}

/* The URL again, with `ip` where the host was. False if it does not fit. */
static inline bool addrpin_build(const char *url, const addrpin_url_t *u,
                                 const char *ip, char *out, size_t out_size)
{
    int n;
    if (u->port) {
        n = snprintf(out, out_size, "%s://%s:%d%s", u->https ? "https" : "http",
                     ip, u->port, url + u->rest);
    } else {
        n = snprintf(out, out_size, "%s://%s%s", u->https ? "https" : "http",
                     ip, url + u->rest);
    }
    return n > 0 && (size_t)n < out_size;
}

/* The Host header the name-based request would have sent. esp_http_client
 * writes the port only when the URL has one, and so does this. */
static inline void addrpin_host_header(const addrpin_url_t *u, char *out,
                                       size_t out_size)
{
    if (u->port) snprintf(out, out_size, "%s:%d", u->host, u->port);
    else         snprintf(out, out_size, "%s", u->host);
}
