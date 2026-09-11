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
    PORTALWEB_NON_ASCII,            /* a byte >= 0x80: see below         */
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

/*
 * SPECIAL CHARACTERS, WHICH IS WHERE PORTALS ACTUALLY FAIL.
 *
 * A WPA passphrase is printable ASCII, and a phone keyboard is not. iOS
 * "smart punctuation" turns ' into a curly apostrophe, a dash into an en
 * dash, and a paste can bring a no-break space; the router's password
 * has none of those, so the join is refused and the person is sure they
 * typed it right. portalweb_check() reports these as PORTALWEB_NON_ASCII
 * rather than as a bad length, because "8 to 63 characters" is a false
 * explanation of a curly quote.
 *
 * The two functions below are for saying which, without saying the
 * password. The map project's portal logs the length, the first and last
 * characters and every symbol for the same debugging purpose; this keeps
 * only what diagnoses an ENCODING problem -- lengths, whitespace at the
 * ends, a capitalised first letter, and the code points of anything that
 * is not ASCII -- and nothing that narrows down which ASCII characters
 * were typed. No first or last character, and nothing derived from the
 * key.
 */

/*
 * One line for the log, e.g.
 *   "12 bytes, 11 chars, trailing space, U+2019 curly apostrophe"
 * Always terminated; cut short with "..." if it does not fit.
 */
void portalweb_describe(const char *secret, char *out, size_t out_size);

/*
 * For the phone's page: the first non-ASCII character, named, with the
 * ASCII a router most likely expects, e.g.
 *   "a curly apostrophe (U+2019) -- the router probably wants '"
 * or the code point alone for anything not in the short list. "" when
 * there is none. The character itself is never echoed: the name comes
 * from a table, not from the input.
 */
void portalweb_non_ascii_hint(const char *secret, char *out, size_t out_size);

/*
 * WHICH SECRET TO TRY FIRST.
 *
 * PSK-first exists so that a WPA2 network stores a key and never the
 * passphrase. On hardware, a WPA2/WPA3 transition network refused the
 * derived PSK with reason 202 twice, and the passphrase was stored
 * anyway -- the station offers SAE, and SAE cannot use a precomputed
 * key. So on a network the scan shows as WPA3-capable, the PSK attempt
 * only costs time: go straight to the passphrase.
 *
 * On a WPA2-only network, or one the scan did not see (hidden, typed
 * by hand, out of range when the scan ran), PSK first stays. Trying the
 * passphrase first there and storing a derived key afterwards would be
 * as good -- if the passphrase joins, the PBKDF2 key is correct by
 * construction -- but it would store a 64-hex key that no join on this
 * C6 had ever used when this was written.
 *
 * Half of that is now measured. Eighth flash, a phone hotspot on channel
 * 10, WPA2:
 *
 *   tab5_portal: trying Suscore (PSK, then passphrase: WPA2 network)
 *   tab5_wifi: joined Suscore
 *   tab5_portal: saved Suscore as PSK
 *
 * The derived 64-hex PSK joined on the first attempt. What is left is
 * the stored PSK joining at the next boot, from wifistore, through the
 * worker -- "saved network 1 of 2 in range: Suscore" and an address.
 */
typedef enum {
    PORTALWEB_NET_UNKNOWN = 0,      /* not in the scan, or an auth mode
                                       this does not reason about */
    PORTALWEB_NET_WPA2_ONLY,        /* WPA, WPA2, WPA/WPA2 */
    PORTALWEB_NET_WPA3_CAPABLE,     /* WPA3, WPA2/WPA3 */
} portalweb_net_t;

typedef enum {
    PORTALWEB_TRY_NONE = 0,             /* the pair did not check out */
    PORTALWEB_TRY_AS_TYPED,             /* a 64-hex PSK: exactly that */
    PORTALWEB_TRY_PSK_THEN_PASSPHRASE,  /* derive, try, fall back */
    PORTALWEB_TRY_PASSPHRASE_ONLY,      /* WPA3-capable: skip the PSK */
} portalweb_plan_t;

portalweb_plan_t portalweb_join_plan(portalweb_check_t kind, portalweb_net_t net);

/*
 * The form has a <select> of scanned networks and a text field for one
 * that is hidden or was not heard. `chosen` and `typed` are those two
 * decoded values, either possibly "". A typed name wins when there is
 * one -- typing is the deliberate act, and a <select> always submits
 * something. Returns whichever applies, or "" for neither; never NULL.
 */
const char *portalweb_pick_ssid(const char *chosen, const char *typed);

/* Lower-case hex of `n` bytes into `out`, which needs 2n + 1. */
void portalweb_hex(const uint8_t *in, size_t n, char *out);

#ifdef __cplusplus
}
#endif
