/*
 * mp4seek.h -- seeking .m4a by reading its sample tables and feeding
 * the AAC decoder directly.
 *
 * WHY THIS ONE IS NOT LIKE THE OTHER FOUR
 *
 * 0700 through 0706 all seek by moving the file underneath
 * esp_audio_codec's parser: prove the byte rate is a line, or bisect
 * for a frame or page or packet, then hand the parser bytes from
 * somewhere else. That works because those parsers are stateless about
 * position -- they find their own frame boundaries in whatever arrives.
 *
 * The M4A parser is not. `m4a_parse.c.obj` in
 * `libesp_audio_simple_dec.a` is 6652 bytes and its strings say what it
 * does: `Chunk number %d`, `Sample number %d`, `STSC map count %d`,
 * `Fail to allocate memory for stco` / `stsz` / `stsc`, `All sample
 * sent`. It reads the sample tables into memory at open and WALKS them,
 * driving position itself and telling the caller which bytes to skip.
 *
 * So there is no position to move it to. Reopening restarts it at
 * sample zero, and feeding it bytes from elsewhere desynchronises it
 * against a table it believes it is tracking. This is the one format
 * where reading the archive says no rather than yes.
 *
 * WHAT THIS DOES INSTEAD
 *
 * Reads the same tables here, and stops using that parser. `moov` gives
 * `stts` for timing, `stsc`/`stco`/`stsz` for where each sample is, and
 * `esds` for the AudioSpecificConfig; from the config an ADTS header
 * can be synthesised per sample, and the AAC decoder -- which takes
 * arbitrary-length input and resynchronises on its own sync word --
 * plays the result.
 *
 * That makes .m4a exactly seekable: a table lookup, not a bisection.
 * It also makes the position exact rather than within a frame, which
 * none of the other four are.
 *
 * WHAT IT DELIBERATELY DOES NOT COVER
 *
 * ALAC, and anything else in an MP4 that is not AAC. ALAC cannot be
 * remuxed to ADTS -- there is no such framing -- so those files stay on
 * esp_audio_codec's M4A path exactly as they are today, unseekable and
 * working. `mp4_probe()` returning false is the switch, and every
 * reason it can return false lands there: a sample entry that is not
 * `mp4a`, a missing `esds`, a table too large to hold, an offset past
 * 4 GB.
 *
 * Nothing about the fallback path changes. That is the point of putting
 * the decision in a probe rather than in the format table.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The largest sample table this will hold.
 *
 * Eight bytes per sample, so 200k samples is 1.6 MB of PSRAM -- and at
 * 1024 samples per AAC frame and 44.1 kHz, 200k frames is 77 minutes.
 * A file longer than that gets the unseekable fallback rather than an
 * allocation nobody budgeted for.
 */
#define MP4_MAX_SAMPLES     (200000)

typedef struct {
    bool     ok;
    uint32_t sample_rate;
    uint8_t  channels;

    /* ADTS header fields, from the AudioSpecificConfig. AAC only. */
    uint8_t  profile;           /* audioObjectType - 1 */
    uint8_t  sf_index;
    uint8_t  chan_cfg;

    /*
     * ALAC instead of AAC.
     *
     * There is no header to synthesise for ALAC -- no ADTS equivalent
     * exists -- so the samples go to the decoder as they are, one per
     * call, with the table saying where each one ends. That is the
     * framing layer ESP_AUDIO_SIMPLE_DEC_TYPE_ALAC asks for and the
     * reason this can be done at all.
     *
     * `cookie` is the whole `alac` atom out of the sample entry: four
     * bytes of size, the type, four of version and flags, then the
     * ALACSpecificConfig. Which part of that the decoder wants is not
     * documented, so decoder.c tries the atom and then the config, and
     * `cookie_cfg_off` is where the second attempt starts.
     */
    bool     alac;
    uint8_t *cookie;
    uint32_t cookie_len;
    uint32_t cookie_cfg_off;

    /* The largest sample in the file. The frame path has to be able to
     * hold one whole sample, and this is how big to make that. */
    uint32_t max_size;

    uint32_t timescale;
    uint64_t duration;          /* in timescale units */

    uint32_t count;             /* samples */
    uint32_t *offset;           /* byte offset of each */
    uint32_t *size;             /* bytes */

    /* time-to-sample, kept as the table states it rather than expanded:
     * it is nearly always one entry saying "every sample is 1024". */
    uint32_t *stts_count;
    uint32_t *stts_delta;
    int       stts_n;

    uint32_t cur;               /* next sample to hand out */
    long     pos;               /* where the file handle is, so a
                                 * contiguous run costs no seeks */
} mp4_t;

/* Read the tables. False means "use the ordinary M4A path", which is
 * every failure including the ones that are not errors -- see above. */
bool mp4_probe(FILE *f, mp4_t *m);
void mp4_free(mp4_t *m);

/* Total length, or 0. */
uint32_t mp4_duration_sec(const mp4_t *m);

/*
 * Point the reader at a time. Returns the position actually landed on,
 * which is the start of the sample containing `sec` -- exact, because
 * the table says where every sample begins.
 */
uint32_t mp4_seek_sec(mp4_t *m, uint32_t sec);

/*
 * Copy exactly one sample into `dst`, for the codecs that want a frame
 * at a time and no header in front of it. Returns its size, or 0 at the
 * end of the table or if it will not fit.
 *
 * Advances `cur` the same way mp4_read() does, so a seek followed by
 * this reads from where the seek landed.
 */
uint32_t mp4_read_frame(FILE *f, mp4_t *m, uint8_t *dst, size_t cap);

/*
 * Fill `dst` with whole ADTS frames, header synthesised per sample.
 * Returns bytes written, 0 at the end of the table. AAC only -- ALAC
 * uses mp4_read_frame().
 *
 * Whole frames only: a partial one at the end of the buffer would be
 * completed on the next call, which is fine for a decoder that
 * reassembles and pointless work for one that does not need it.
 */
size_t mp4_read(FILE *f, mp4_t *m, uint8_t *dst, size_t cap);

#ifdef __cplusplus
}
#endif
