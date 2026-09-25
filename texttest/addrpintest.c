/*
 * addrpintest.c -- 5068's URL half: taking a station URL apart and
 * putting it back with an address where the name was.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "addrpin.h"

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++;      \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);   \
    printf("\n"); } } while (0)

static void pin(const char *url, const char *ip, const char *want_url,
                const char *want_host)
{
    addrpin_url_t u;
    char out[512], host[160];
    const bool ok = addrpin_parse(url, &u);
    CHECK(ok, "did not parse %s", url);
    if (!ok) return;
    CHECK(addrpin_build(url, &u, ip, out, sizeof(out)), "build %s", url);
    CHECK(strcmp(out, want_url) == 0, "%s -> %s, want %s", url, out, want_url);
    addrpin_host_header(&u, host, sizeof(host));
    CHECK(strcmp(host, want_host) == 0, "Host %s, want %s", host, want_host);
}

static void refuse(const char *url)
{
    addrpin_url_t u;
    CHECK(!addrpin_parse(url, &u), "should not pin %s", url);
}

int main(void)
{
    printf("addrpintest\n");

    /* The station that started it. */
    pin("http://live02.rfi.fr/rfimonde-64.mp3", "185.74.70.34",
        "http://185.74.70.34/rfimonde-64.mp3", "live02.rfi.fr");
    /* A port stays in both. */
    pin("http://stream1.dancewave.online:8080/dance.mp3", "1.2.3.4",
        "http://1.2.3.4:8080/dance.mp3", "stream1.dancewave.online:8080");
    pin("https://centova5.transmissaodigital.com:20104/live", "5.6.7.8",
        "https://5.6.7.8:20104/live", "centova5.transmissaodigital.com:20104");
    /* No path, a query, a fragment. */
    pin("http://example.com", "9.9.9.9", "http://9.9.9.9", "example.com");
    pin("http://example.com?x=1", "9.9.9.9", "http://9.9.9.9?x=1", "example.com");
    pin("http://example.com:81#f", "9.9.9.9", "http://9.9.9.9:81#f", "example.com:81");
    {
        addrpin_url_t u;
        CHECK(addrpin_parse("https://a.b/c", &u) && u.https, "https flag");
        CHECK(addrpin_parse("http://a.b/c", &u) && !u.https, "http flag");
    }

    /* Nothing to pin, or not safe to rewrite. */
    refuse("http://185.74.70.34/x");
    refuse("http://[2001:db8::1]:8000/x");
    refuse("http://user:pw@example.com/x");
    refuse("http://user@example.com:80/x");
    refuse("ftp://example.com/x");
    refuse("http:///x");
    refuse("http://example.com:/x");
    refuse("http://example.com:99999/x");
    refuse("http://example.com:80a/x");
    {
        char big[300];
        memset(big, 'a', sizeof(big));
        memcpy(big, "http://", 7);
        big[sizeof(big) - 1] = '\0';
        refuse(big);                        /* host over ADDRPIN_HOST_MAX */
    }
    /* An '@' in the path is not credentials. */
    pin("http://example.com/a@b", "1.1.1.1", "http://1.1.1.1/a@b", "example.com");
    /* Too small an output buffer says so. */
    {
        addrpin_url_t u;
        char small[12];
        CHECK(addrpin_parse("http://example.com/long/path", &u), "parse");
        CHECK(!addrpin_build("http://example.com/long/path", &u, "1.2.3.4",
                             small, sizeof(small)), "overflow not reported");
    }

    printf("%d checks, %d failures\n", checks, failures);
    printf(failures ? "FAILURES\n" : "all passed\n");
    return failures ? 1 : 0;
}
