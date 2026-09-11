/*
 * codecplan.h -- which decoder a stream's bytes go to, with nothing to
 * run.
 *
 * Phase 2 decodes from the ring, and before it can it has to know what
 * is in it. There are two sources of that answer and they disagree
 * often enough that which one wins is the whole decision:
 *
 *   - **Content-Type**, which is a header a human configured years ago
 *     and nobody has looked at since. `audio/mpeg` on an AAC stream is
 *     routine; `application/octet-stream` is a station that never set
 *     it; `audio/aacp` may be HE-AAC in ADTS or may be plain AAC-LC.
 *   - **The first bytes**, which are what the decoder will actually be
 *     handed. `streamsniff.h` already reads them, and the probe proved
 *     it on hardware: WNZK announced `audio/aac` and the bytes were
 *     ADTS, and both agreed that time.
 *
 * **The bytes win.** Content-Type is consulted only when the bytes say
 * nothing, which happens when a station starts mid-frame in a way the
 * sniffer will not confirm, or when the first block is an ID3 tag.
 * Believing a header over the buffer means handing AAC to minimp3,
 * which does not fail cleanly -- it produces noise, and noise from a
 * live stream with no seek bar is hard to tell from a bad connection.
 *
 * WHAT IS DECODED, AND WHAT IS REFUSED WITH A REASON
 *
 * Phase 2's scope is ADTS AAC through esp_audio_codec's simple decoder
 * -- the path `decoder.c` already uses for `.aac` files -- and MP3
 * through minimp3's frame decoder rather than `mp3dec_ex`, which wants
 * to seek. Everything else is refused, but refused *by name*: a station
 * URL that turns out to be an Ogg stream, an HLS playlist or an error
 * page are three different things to say on a screen, and "failed" for
 * all three is the version that generates the bug report nobody can
 * act on.
 *
 * The playlist cases matter more than they look. A great many station
 * URLs in the wild are `.pls` or `.m3u` files that *contain* the stream
 * URL, and radio-browser hands them out. Resolving them is explicitly
 * not in phase 2's scope, so the least this can do is say which kind it
 * found rather than playing a text file as audio.
 *
 * THE ID3 CASE IS NOT AN ERROR
 *
 * A shoutcast MP3 stream can open with an ID3v2 tag. The sniffer
 * correctly reports `SNIFF_ID3`, which is not a codec -- it is a
 * wrapper with a length in it. `id3_skip_bytes()` reads that length so
 * the caller can drop the tag and sniff again, rather than treating a
 * legal MP3 stream as unknown.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "streamsniff.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STREAM_CODEC_NONE = 0,  /* nothing decodable decided yet */
    STREAM_CODEC_MP3,       /* minimp3, frame at a time */
    STREAM_CODEC_AAC_ADTS,  /* esp_audio_codec simple decoder */
} stream_codec_t;

/* Why, in enough detail to put on a screen. */
typedef enum {
    CODECPLAN_FROM_BYTES = 0,   /* the first bytes said so */
    CODECPLAN_FROM_TYPE,        /* the bytes said nothing; Content-Type did */
    CODECPLAN_NEED_SKIP,        /* an ID3 tag is in front; drop it and re-sniff */
    CODECPLAN_NEED_MORE,        /* not enough bytes yet to say anything */
    CODECPLAN_IS_PLAYLIST,      /* a .m3u/.pls/HLS file, not a stream */
    CODECPLAN_IS_PAGE,          /* HTML: an error page where audio was expected */
    CODECPLAN_UNSUPPORTED,      /* real audio, not a format this decodes */
    CODECPLAN_UNKNOWN,          /* neither source said anything usable */
} codecplan_why_t;

typedef struct {
    stream_codec_t   codec;
    codecplan_why_t  why;
    /* A short line for the screen. Never NULL. */
    const char      *message;
} codecplan_t;

static inline const char *stream_codec_name(stream_codec_t c)
{
    switch (c) {
    case STREAM_CODEC_MP3:      return "MP3";
    case STREAM_CODEC_AAC_ADTS: return "AAC (ADTS)";
    default:                    return "none";
    }
}

/*
 * The length of an ID3v2 tag at the front of a buffer, header included,
 * or 0 if there is not one. The size is four syncsafe bytes -- seven
 * bits each, high bit always clear -- which is the field most often got
 * wrong by reading it as a plain big-endian integer, and the error is
 * silent for tags under 2 MB in a way that corrupts the first frame.
 */
static inline size_t id3_skip_bytes(const uint8_t *b, size_t n)
{
    if (!b || n < 10) return 0;
    if (memcmp(b, "ID3", 3) != 0) return 0;
    /* Any of the size bytes having its high bit set means this is not a
     * syncsafe integer and the tag is malformed; refuse rather than
     * skip a wrong distance into the audio. */
    for (int i = 6; i < 10; i++) if (b[i] & 0x80) return 0;
    const size_t size = ((size_t)b[6] << 21) | ((size_t)b[7] << 14) |
                        ((size_t)b[8] << 7)  | (size_t)b[9];
    /* A footer adds ten more bytes, flagged in bit 4 of the flags byte. */
    const size_t footer = (b[5] & 0x10) ? 10 : 0;
    return 10 + size + footer;
}

/*
 * Content-Type, mapped. Parameters are ignored: `audio/aacp; charset=x`
 * is `audio/aacp`. Returns STREAM_CODEC_NONE for anything not decoded,
 * which the caller distinguishes from "not audio" by its own means.
 */
static inline stream_codec_t codecplan_from_type(const char *ct)
{
    if (!ct) return STREAM_CODEC_NONE;
    while (*ct == ' ') ct++;

    char t[48];
    size_t i = 0;
    while (ct[i] && ct[i] != ';' && ct[i] != ' ' && i + 1 < sizeof(t)) {
        char c = ct[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        t[i] = c;
        i++;
    }
    t[i] = '\0';

    /* audio/mpeg is MP3 far more often than not, and when it is not the
     * bytes will already have said so -- this function is only reached
     * when they said nothing. */
    if (strcmp(t, "audio/mpeg") == 0 || strcmp(t, "audio/mp3") == 0 ||
        strcmp(t, "audio/x-mpeg") == 0 || strcmp(t, "audio/mpeg3") == 0 ||
        strcmp(t, "audio/x-mpeg-3") == 0) {
        return STREAM_CODEC_MP3;
    }
    /* aacp is HE-AAC, which arrives in ADTS and whose core rate the ADTS
     * header already describes -- see the note in streamsniff.h. */
    if (strcmp(t, "audio/aac") == 0 || strcmp(t, "audio/aacp") == 0 ||
        strcmp(t, "audio/x-aac") == 0 || strcmp(t, "audio/mp4a-latm") == 0) {
        return STREAM_CODEC_AAC_ADTS;
    }
    return STREAM_CODEC_NONE;
}

/* Whether a Content-Type names a playlist rather than audio. */
static inline bool codecplan_type_is_playlist(const char *ct)
{
    if (!ct) return false;
    static const char *kinds[] = {
        "audio/x-mpegurl", "audio/mpegurl", "application/x-mpegurl",
        "application/vnd.apple.mpegurl", "audio/x-scpls",
        "application/pls+xml", "audio/scpls",
    };
    char t[48];
    size_t i = 0;
    while (ct[i] && ct[i] != ';' && ct[i] != ' ' && i + 1 < sizeof(t)) {
        char c = ct[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        t[i] = c;
        i++;
    }
    t[i] = '\0';
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        if (strcmp(t, kinds[k]) == 0) return true;
    }
    return false;
}

/*
 * The decision. `sniffed` is what streamsniff.h made of the first bytes
 * (SNIFF_UNKNOWN if there are not enough yet), `have_bytes` is how many
 * were looked at, and `content_type` is the header or NULL.
 *
 * `have_bytes` exists so that "the sniffer has not seen enough" is not
 * reported as "this is not audio". A station whose first block has not
 * arrived should say Buffering, not Unsupported.
 */
#define CODECPLAN_MIN_SNIFF_BYTES   (64)

static inline codecplan_t codecplan_choose(sniff_t sniffed, size_t have_bytes,
                                           const char *content_type)
{
    codecplan_t r = { STREAM_CODEC_NONE, CODECPLAN_UNKNOWN, "Unrecognised stream" };

    /* The bytes first, always. A header is a claim; these are the thing
     * the decoder will be handed. */
    switch (sniffed) {
    case SNIFF_MP3:
        r.codec = STREAM_CODEC_MP3;
        r.why = CODECPLAN_FROM_BYTES;
        r.message = "MP3";
        return r;
    case SNIFF_AAC_ADTS:
        r.codec = STREAM_CODEC_AAC_ADTS;
        r.why = CODECPLAN_FROM_BYTES;
        r.message = "AAC";
        return r;
    case SNIFF_ID3:
        /* Not a codec: a wrapper with a length in it. */
        r.why = CODECPLAN_NEED_SKIP;
        r.message = "Reading tag";
        return r;
    case SNIFF_M3U:
        r.why = CODECPLAN_IS_PLAYLIST;
        r.message = "That link is a playlist, not a stream";
        return r;
    case SNIFF_PLS:
        r.why = CODECPLAN_IS_PLAYLIST;
        r.message = "That link is a playlist, not a stream";
        return r;
    case SNIFF_HTML:
        r.why = CODECPLAN_IS_PAGE;
        r.message = "The station returned a web page";
        return r;
    case SNIFF_OGG:
        r.why = CODECPLAN_UNSUPPORTED;
        r.message = "Ogg streams are not supported yet";
        return r;
    case SNIFF_FLAC:
        r.why = CODECPLAN_UNSUPPORTED;
        r.message = "FLAC streams are not supported yet";
        return r;
    default:
        break;
    }

    /* The bytes said nothing. Before blaming the station, check whether
     * there were enough of them to blame anything. */
    if (have_bytes < CODECPLAN_MIN_SNIFF_BYTES) {
        r.why = CODECPLAN_NEED_MORE;
        r.message = "Buffering";
        return r;
    }

    /* A Content-Type that names a playlist is worth believing even
     * without matching bytes: a .pls served as text may begin with a
     * comment or a BOM that the sniffer will not recognise. */
    if (codecplan_type_is_playlist(content_type)) {
        r.why = CODECPLAN_IS_PLAYLIST;
        r.message = "That link is a playlist, not a stream";
        return r;
    }

    const stream_codec_t from_type = codecplan_from_type(content_type);
    if (from_type != STREAM_CODEC_NONE) {
        r.codec = from_type;
        r.why = CODECPLAN_FROM_TYPE;
        r.message = from_type == STREAM_CODEC_MP3 ? "MP3" : "AAC";
        return r;
    }
    return r;
}

/* Whether a decision is one phase 2 can act on now. */
static inline bool codecplan_ready(const codecplan_t *r)
{
    return r && r->codec != STREAM_CODEC_NONE;
}

/* Whether waiting for more bytes could still change the answer. */
static inline bool codecplan_waiting(const codecplan_t *r)
{
    return r && (r->why == CODECPLAN_NEED_MORE || r->why == CODECPLAN_NEED_SKIP);
}

#ifdef __cplusplus
}
#endif
