/*
 * dnsreply.c -- see dnsreply.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "dnsreply.h"

#include <string.h>

#define HDR         (12)
#define TYPE_A      (1)
#define CLASS_IN    (1)
#define NAME_MAX_B  (255)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

size_t dnsreply_build(const uint8_t *q, size_t n, const uint8_t ip[4],
                      uint8_t *out, size_t out_len)
{
    if (!q || !ip || !out || n < HDR || n > DNSREPLY_MAX) return 0;

    const uint8_t flags_hi = q[2];
    if (flags_hi & 0x80) return 0;                  /* a response    */
    if ((flags_hi >> 3) & 0x0F) return 0;           /* not QUERY     */
    if (rd16(q + 4) != 1) return 0;                 /* one question  */

    /* The question name: labels, no pointers, within the packet and
     * within the protocol's own length limit. */
    size_t p = HDR, name_len = 0;
    for (;;) {
        if (p >= n) return 0;
        const uint8_t len = q[p];
        if (len == 0) { p++; name_len++; break; }
        if (len & 0xC0) return 0;                   /* pointer/reserved */
        name_len += (size_t)len + 1;
        if (name_len > NAME_MAX_B) return 0;
        p += (size_t)len + 1;
    }
    if (p + 4 > n) return 0;                        /* QTYPE + QCLASS */
    const uint16_t qtype  = rd16(q + p);
    const uint16_t qclass = rd16(q + p + 2);
    p += 4;

    const int answer = (qtype == TYPE_A && qclass == CLASS_IN);
    const size_t need = p + (answer ? 16u : 0u);
    if (need > out_len) return 0;

    memcpy(out, q, p);                              /* header + question */
    out[2] = (uint8_t)(0x84 | (flags_hi & 0x01));   /* QR, AA, copy RD */
    out[3] = 0x00;                                  /* RA 0, NOERROR   */
    wr16(out + 6, answer ? 1 : 0);                  /* ANCOUNT */
    wr16(out + 8, 0);                               /* NSCOUNT */
    wr16(out + 10, 0);                              /* ARCOUNT */

    if (answer) {
        uint8_t *a = out + p;
        wr16(a, 0xC00C);                            /* name: the question */
        wr16(a + 2, TYPE_A);
        wr16(a + 4, CLASS_IN);
        a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 1;     /* TTL 1 s */
        wr16(a + 10, 4);
        memcpy(a + 12, ip, 4);
    }
    return need;
}
