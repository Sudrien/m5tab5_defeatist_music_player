/*
 * dnsreply.h -- the whole of the portal's DNS server, minus the socket.
 *
 * A captive portal works by lying about every name. A phone that joins
 * the setup network asks for its connectivity-check host, gets this
 * device's address back, requests a page it did not expect, and opens
 * the sign-in sheet -- which is the form. So the server answers every A
 * query with one address and never looks anything up.
 *
 * Pure bytes in, bytes out, so the parsing -- which is the part that
 * reads untrusted input from anyone in radio range -- runs under ASan on
 * a host. portal.c owns the socket and the task.
 *
 * WHAT IT ANSWERS
 *
 *   A / IN      one record, the portal's address, TTL 1 s. Short so a
 *               phone that leaves the portal does not keep resolving the
 *               world to a device it is no longer joined to.
 *   anything    NOERROR with no records. AAAA in particular: a phone
 *   else        that gets an IPv6 answer tries it first and waits.
 *
 * WHAT IT IGNORES, BY RETURNING 0
 *
 *   responses, non-QUERY opcodes, anything but exactly one question,
 *   truncated packets, compression pointers inside the question (a
 *   client has no reason to send one and a loop is the classic bug),
 *   and names past the 255-byte limit. Ignoring is right for all of
 *   them: nothing a phone sends to find a sign-in page looks like any
 *   of these, and a malformed packet does not earn a reply.
 *
 * Additional records (EDNS OPT) are dropped from the reply rather than
 * echoed, which is legal and means the reply never grows past the
 * question plus sixteen bytes.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Classic DNS over UDP. A query larger than this is not one a phone
 * sends while looking for a portal. */
#define DNSREPLY_MAX    (512)

/*
 * Build the reply to `q` (`n` bytes) in `out`. `ip` is the address to
 * give, four bytes in network order. Returns the reply's length, or 0
 * when the packet should get no reply at all.
 */
size_t dnsreply_build(const uint8_t *q, size_t n, const uint8_t ip[4],
                      uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif
