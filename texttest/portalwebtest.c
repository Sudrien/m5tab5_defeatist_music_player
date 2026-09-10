/*
 * portalwebtest.c -- the portal's input handling and its DNS replies,
 * against the real main/portalweb.c and main/dnsreply.c. Both read bytes
 * from anyone in radio range, which is the whole reason they are pure.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dnsreply.h"
#include "portalweb.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

static bool field(const char *body, const char *key, char *out, size_t n)
{
    return portalweb_field(body, strlen(body), key, out, n);
}

/* A query for `name`, type/class as given. Returns its length. */
static size_t query(uint8_t *q, const char *name, uint16_t type, uint16_t cls)
{
    const uint8_t hdr[12] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0 };
    memcpy(q, hdr, 12);
    size_t p = 12;
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        const size_t l = dot ? (size_t)(dot - s) : strlen(s);
        q[p++] = (uint8_t)l;
        memcpy(q + p, s, l);
        p += l;
        s += l + (dot ? 1 : 0);
    }
    q[p++] = 0;
    q[p++] = (uint8_t)(type >> 8); q[p++] = (uint8_t)type;
    q[p++] = (uint8_t)(cls >> 8);  q[p++] = (uint8_t)cls;
    return p;
}

int main(void)
{
    char out[128];

    printf("form fields decode, and only whole ones\n");
    CHECK(field("ssid=Dave%27s+WiFi&pass=hunter22", "ssid", out, sizeof out) &&
          strcmp(out, "Dave's WiFi") == 0, "got '%s'", out);
    CHECK(field("ssid=a&pass=p%40ss+w%2Bord", "pass", out, sizeof out) &&
          strcmp(out, "p@ss w+ord") == 0, "got '%s'", out);
    CHECK(!field("xssid=a", "ssid", out, sizeof out), "prefix of another key matched");
    CHECK(!field("ssidx=a", "ssid", out, sizeof out), "longer key matched");
    CHECK(field("pass=x&ssid=", "ssid", out, sizeof out) && out[0] == '\0', "empty value");
    CHECK(!field("ssid", "ssid", out, sizeof out), "bare key taken as a field");
    CHECK(!field("", "ssid", out, sizeof out), "empty body");

    printf("a broken escape is refused, not guessed at\n");
    CHECK(!field("ssid=ab%4", "ssid", out, sizeof out), "short escape at end");
    CHECK(!field("ssid=ab%4&x=1", "ssid", out, sizeof out), "short escape before &");
    CHECK(!field("ssid=ab%zz", "ssid", out, sizeof out), "non-hex escape");
    CHECK(!field("ssid=ab%", "ssid", out, sizeof out), "lone %%");
    CHECK(!field("ssid=a%00b", "ssid", out, sizeof out), "NUL decoded");

    printf("a value that does not fit is refused, not truncated\n");
    CHECK(field("s=1234567", "s", out, 8) && strcmp(out, "1234567") == 0, "exact fit");
    CHECK(!field("s=12345678", "s", out, 8) && out[0] == '\0', "one over: '%s'", out);

    printf("the body is not assumed to be terminated\n");
    {
        const char body[] = { 's','s','i','d','=','a','b','c','X','X' };
        CHECK(portalweb_field(body, 8, "ssid", out, sizeof out) &&
              strcmp(out, "abc") == 0, "got '%s'", out);
    }

    printf("an SSID cannot become markup\n");
    CHECK(portalweb_escape("<b onload='x'>&\"", out, sizeof out) &&
          strcmp(out, "&lt;b onload=&#39;x&#39;&gt;&amp;&quot;") == 0, "got '%s'", out);
    CHECK(portalweb_escape("a\x01" "b\x7f", out, sizeof out) &&
          strcmp(out, "a\xEF\xBF\xBD" "b\xEF\xBF\xBD") == 0, "control bytes");
    CHECK(portalweb_escape("caf\xC3\xA9", out, sizeof out) &&
          strcmp(out, "caf\xC3\xA9") == 0, "UTF-8 passes through");
    CHECK(!portalweb_escape("&&", out, 10) && out[0] == '\0', "overflow not refused");
    CHECK(portalweb_escape("&&", out, 11) && strcmp(out, "&amp;&amp;") == 0, "exact fit");

    printf("the AP name is stable, hashed, and differs between devices\n");
    {
        const uint8_t a[6] = { 0x30, 0xED, 0xA0, 0x12, 0x34, 0x56 };
        const uint8_t b[6] = { 0x30, 0xED, 0xA0, 0x12, 0x34, 0x57 };
        char na[20], nb[20], na2[20];
        portalweb_ap_name(a, na, sizeof na);
        portalweb_ap_name(b, nb, sizeof nb);
        portalweb_ap_name(a, na2, sizeof na2);
        CHECK(strlen(na) == 14 && strncmp(na, "Defeatist-", 10) == 0, "got '%s'", na);
        CHECK(strcmp(na, na2) == 0, "not stable");
        CHECK(strcmp(na, nb) != 0, "neighbours collide: %s", na);
        CHECK(strstr(na, "3456") == NULL && strstr(na, "5634") == NULL, "publishes the octets: %s", na);
        char tiny[8] = "x";
        portalweb_ap_name(a, tiny, sizeof tiny);
        CHECK(tiny[0] == '\0', "truncated name written");
    }

    printf("credentials are checked by WPA's own lengths\n");
    CHECK(portalweb_check("home", "12345678") == PORTALWEB_OK_PASSPHRASE, "8");
    CHECK(portalweb_check("home", "123456789012345678901234567890123456789012345678901234567890123") == PORTALWEB_OK_PASSPHRASE, "63");
    CHECK(portalweb_check("home", "1234567") == PORTALWEB_BAD_SECRET, "7");
    CHECK(portalweb_check("home", "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef") == PORTALWEB_OK_PSK, "64 hex");
    CHECK(portalweb_check("home", "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdeg") == PORTALWEB_BAD_SECRET, "64 not hex");
    CHECK(portalweb_check("home", "pass\tword") == PORTALWEB_BAD_SECRET, "control char");
    CHECK(portalweb_check("home", "") == PORTALWEB_NO_SECRET, "open");
    CHECK(portalweb_check("", "12345678") == PORTALWEB_BAD_SSID, "empty ssid");
    CHECK(portalweb_check("123456789012345678901234567890123", "12345678") == PORTALWEB_BAD_SSID, "33-byte ssid");
    CHECK(portalweb_check("12345678901234567890123456789012", "12345678") == PORTALWEB_OK_PASSPHRASE, "32-byte ssid");

    printf("hex is lower case and terminated\n");
    {
        const uint8_t b[3] = { 0x00, 0xAB, 0xff };
        char h[7];
        portalweb_hex(b, 3, h);
        CHECK(strcmp(h, "00abff") == 0, "got '%s'", h);
    }

    printf("DNS: every A query gets this device\n");
    {
        uint8_t q[DNSREPLY_MAX], r[DNSREPLY_MAX];
        const uint8_t ip[4] = { 192, 168, 4, 1 };
        size_t n = query(q, "connectivitycheck.gstatic.com", 1, 1);
        size_t m = dnsreply_build(q, n, ip, r, sizeof r);
        CHECK(m == n + 16, "length %zu for query %zu", m, n);
        CHECK(r[0] == 0x12 && r[1] == 0x34, "id not echoed");
        CHECK((r[2] & 0x80) && (r[2] & 0x04) && (r[2] & 0x01), "flags %02x", r[2]);
        CHECK((r[3] & 0x0F) == 0, "rcode %d", r[3] & 0x0F);
        CHECK(r[6] == 0 && r[7] == 1, "ancount");
        CHECK(memcmp(r + 12, q + 12, n - 12) == 0, "question not echoed");
        CHECK(r[n] == 0xC0 && r[n + 1] == 0x0C, "name pointer");
        CHECK(memcmp(r + n + 12, ip, 4) == 0, "address");

        printf("DNS: AAAA gets no records, so the phone does not wait on v6\n");
        n = query(q, "captive.apple.com", 28, 1);
        m = dnsreply_build(q, n, ip, r, sizeof r);
        CHECK(m == n && r[7] == 0 && (r[3] & 0x0F) == 0, "len %zu an %d", m, r[7]);

        printf("DNS: EDNS additional records are not echoed\n");
        n = query(q, "example.com", 1, 1);
        q[11] = 1;                       /* ARCOUNT = 1 */
        const uint8_t opt[11] = { 0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0 };
        memcpy(q + n, opt, sizeof opt);
        m = dnsreply_build(q, n + sizeof opt, ip, r, sizeof r);
        CHECK(m == n + 16 && r[11] == 0, "len %zu arcount %d", m, r[11]);

        printf("DNS: what does not get a reply\n");
        n = query(q, "example.com", 1, 1);
        uint8_t bad[DNSREPLY_MAX];
        memcpy(bad, q, n); bad[2] |= 0x80;
        CHECK(dnsreply_build(bad, n, ip, r, sizeof r) == 0, "a response");
        memcpy(bad, q, n); bad[2] |= 0x10;
        CHECK(dnsreply_build(bad, n, ip, r, sizeof r) == 0, "opcode");
        memcpy(bad, q, n); bad[5] = 2;
        CHECK(dnsreply_build(bad, n, ip, r, sizeof r) == 0, "two questions");
        CHECK(dnsreply_build(q, n - 1, ip, r, sizeof r) == 0, "truncated qclass");
        CHECK(dnsreply_build(q, 11, ip, r, sizeof r) == 0, "short header");
        memcpy(bad, q, n); bad[12] = 0xC0; bad[13] = 0x0C;
        CHECK(dnsreply_build(bad, n, ip, r, sizeof r) == 0, "pointer in question");
        memcpy(bad, q, n); bad[12] = 63;   /* label runs off the end */
        CHECK(dnsreply_build(bad, n, ip, r, sizeof r) == 0, "label past end");
        CHECK(dnsreply_build(q, n, ip, r, n + 15) == 0, "reply buffer one short");
        CHECK(dnsreply_build(q, n, ip, r, n + 16) == n + 16, "reply buffer exact");

        /* A name of 64 labels of 3 bytes: 256 bytes, one past the limit. */
        size_t p = 12;
        memcpy(bad, q, 12);
        for (int i = 0; i < 64; i++) { bad[p++] = 3; bad[p++] = 'a'; bad[p++] = 'b'; bad[p++] = 'c'; }
        bad[p++] = 0; bad[p++] = 0; bad[p++] = 1; bad[p++] = 0; bad[p++] = 1;
        CHECK(dnsreply_build(bad, p, ip, r, sizeof r) == 0, "overlong name");

        /* Every truncation of a good query, and random garbage. */
        int replied = 0;
        for (size_t k = 0; k < n; k++) replied += dnsreply_build(q, k, ip, r, sizeof r) != 0;
        CHECK(replied == 0, "%d truncations replied", replied);
        uint32_t s = 1;
        for (int t = 0; t < 200000; t++) {
            size_t len = 12 + (s % 80);
            for (size_t k = 0; k < len; k++) { s = s * 1103515245u + 12345u; bad[k] = (uint8_t)(s >> 16); }
            bad[2] &= 0x07; bad[4] = 0; bad[5] = 1;   /* make most of them reach the parser */
            (void)dnsreply_build(bad, len, ip, r, sizeof r);
        }
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
