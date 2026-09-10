/*
 * portalweb.h -- the parts of the setup portal that handle text from a
 * stranger's phone, with no network code in them.
 *
 * Everything here reads bytes that anyone in radio range can send while
 * the AP is open, which is why it is separate from portal.c: it runs
 * under ASan and UBSan on a host, the same bargain wifistore made.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Find `key` in an application/x-www-form-urlencoded body and decode its
 * value into `out` as a NUL-terminated string.
 *
 * False when the key is absent, when a %-escape is malformed, when the
 * value decodes to a NUL byte (an SSID may be arbitrary bytes, but not
 * one that ends a C string early and so names a different network), or
 * when it does not fit. A value that does not fit is refused rather than
 * truncated: a truncated passphrase is a different passphrase, and a
 * truncated SSID is a different network.
 *
 * The body is NOT assumed to be terminated; `len` is its length.
 */
bool portalweb_field(const char *body, size_t len, const char *key,
                     char *out, size_t out_size);

/*
 * HTML-escape `in` into `out`: & < > " ' become entities, and bytes
 * below 0x20 or equal to 0x7F become U+FFFD's UTF-8 so a crafted SSID
 * cannot put control characters into the page. False, with `out` set to
 * "", if it does not fit.
 */
bool portalweb_escape(const char *in, char *out, size_t out_size);

/*
 * "Defeatist-XXXX" from a MAC: four hex digits of an FNV-1a hash of all
 * six bytes, not the last two octets. See portal.h for why hashed.
 */
void portalweb_ap_name(const uint8_t mac[6], char *out, size_t out_size);

typedef enum {
    PORTALWEB_OK_PASSPHRASE = 0,    /* 8..63 printable ASCII             */
    PORTALWEB_OK_PSK,               /* exactly 64 hex digits, as typed   */
    PORTALWEB_BAD_SSID,             /* empty or longer than 32 bytes     */
    PORTALWEB_NO_SECRET,            /* empty: open networks unsupported  */
    PORTALWEB_BAD_SECRET,           /* wrong length or characters        */
} portalweb_check_t;

/*
 * Whether a submitted pair can be tried at all, before the radio is
 * touched. The lengths are WPA's own: a passphrase is 8 to 63 printable
 * ASCII characters and 64 characters can only be a hex PSK.
 *
 * Open networks are refused, because wifistore stores a secret for every
 * record and has nowhere to put "none". That is a gap, not a decision.
 */
portalweb_check_t portalweb_check(const char *ssid, const char *secret);

/* Lower-case hex of `n` bytes into `out`, which needs 2n + 1. */
void portalweb_hex(const uint8_t *in, size_t n, char *out);

#ifdef __cplusplus
}
#endif
