/*
 * replaygain.h -- one sidecar file per track, holding whatever has been
 * measured about it so far.
 *
 * The file is JSON Lines: one JSON object per line, and the last line
 * that parses and matches the track wins. It is still named
 * "." + filename + ".rgcache" -- a dotfile, which storage_is_hidden()
 * already excludes from every listing and playlist scan, so nothing had
 * to learn to hide it.
 *
 * WHY JSONL RATHER THAN A PACKED STRUCT
 *
 * The first version of this file was a packed C struct, which is
 * smaller and faster to parse and was the wrong shape for what this
 * actually is. Two things pushed it over:
 *
 *   1. The sections arrive at different times. The waveform comes from
 *      a frame walk, which can happen during a prefetch of a track
 *      nobody has played yet. The loudness comes from a full,
 *      uninterrupted play. Neither waits for the other, so a write of
 *      one must not clobber the other -- which is a read-modify-write,
 *      and a struct that grows a field is a version bump that discards
 *      every sidecar on the card.
 *   2. More metadata is expected. A format where adding a field costs
 *      nothing and old readers ignore what they do not know is worth
 *      more here than the bytes it costs.
 *
 * An append is also its own atomicity story: a line is written whole or
 * it is not, and a torn final line -- power lost mid-write -- is
 * detected by the reader, which falls back to the previous line. That
 * replaced the temp-file-and-rename the packed version used. Every line
 * therefore repeats filesize and mtime rather than relying on the first
 * one, so any line is self-describing and the fallback is a real
 * fallback rather than a line with no key on it.
 *
 * The file is exactly one line. Every write serialises the whole merged
 * record to a temp file and renames it over the old one, so re-measuring
 * a track replaces its sidecar rather than extending it -- and at
 * roughly a kilobyte it sits inside a single FAT cluster and cannot
 * fragment.
 *
 * ANYTHING THAT IS NOT THIS FORMAT IS DUMPED
 *
 * A sidecar whose first byte is not '{' was not written by this code,
 * and the next write discards it entirely rather than appending to it.
 * In practice that means 0200's packed binary record: a device with
 * sidecars already on the card gets reflashed, and an append-only
 * writer would otherwise put valid JSON directly onto a 752-byte
 * binary blob. Nothing breaks when it does -- the reader scans
 * backwards from the end and never reads far enough to reach the blob
 * -- but the file carries the dead bytes for ever and any less
 * forgiving reader would choke.
 *
 * Note the check is on FORMAT, not freshness. A sidecar that is
 * well-formed but stale (its filesize or mtime no longer match the
 * track) is still appended to like any other: the new line supersedes
 * the old and the reader already ignores what it supersedes. Discarding
 * on staleness would be the writer doing the reader's job.
 *
 * TWO VERSIONS, DELIBERATELY
 *
 *   REPLAYGAIN_FORMAT_VERSION   the shape of the record
 *   LOUDNESS_VERSION            what the loudness numbers MEAN
 *
 * They change for different reasons and must not invalidate each other.
 * Adding a field is a format bump and should not throw away a perfectly
 * good loudness measurement; changing the gate, the weighting, or
 * sample peak to true peak is a loudness bump and should not throw away
 * a waveform that took a whole-file read to produce. A stale loudness
 * version is reported as has_loudness = false, so the next full play
 * recomputes just that section.
 *
 * WHAT IS IN A LINE
 *
 *   {"format_version":1,
 *    "filesize":8760320,"mtime":1735500000,
 *    "waveform":{"sec":273,"columns":426,"level":"<base64>"},
 *    "loudness":{"version":1,"integrated_lufs":-18.4,
 *                "sample_peak_dbfs":-1.2,"blocks":2711}}
 *
 * Either section may be absent. `columns` is what was actually filled,
 * NOT always REPLAYGAIN_COLUMNS: a track too short to produce a full
 * envelope stores the columns it has and the UI stretches them across
 * the bar, which is better than padding with silence that never
 * happened.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#include "loudness.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The most columns a waveform is stored with. Pinned rather than
 * aliased to LCD_H_RES: this is a file format, and the panel changing
 * should not silently change what is on the card. A walk that produced
 * more than this is resampled down; one that produced fewer is stored
 * at its own width, see `columns` in replaygain_waveform_t.
 */
#define REPLAYGAIN_COLUMNS          (720)

/* The shape of the record. See "TWO VERSIONS" above. */
#define REPLAYGAIN_FORMAT_VERSION   (3)

/*
 * Read buffer for a sidecar. Not a growth cap: the file is rewritten
 * whole on every write and is one line, so it does not grow. A record
 * carrying every section including a 256-entry index is around 4 KB, so
 * this is roughly double the largest thing that can be written -- room
 * for a future section without room for a runaway allocation on the
 * decode path.
 */
#define REPLAYGAIN_READ_MAX         (8192)

/*
 * Seek index. Entries are (byte offset, sample position) pairs at a
 * fixed sample spacing, so a seek becomes a lookup and a short read
 * rather than decoder_open()'s whole-file scan -- which is 1.2 to 1.8
 * seconds of the open in every log taken so far, and nearly all of the
 * delay before first sound.
 *
 * The spacing is coarse on purpose. minimp3's own index is per-frame,
 * which for a 273 s track is ten thousand entries and far more
 * precision than a finger on a 720 px bar can ask for: one pixel is
 * about 380 ms there. Ten seconds per entry puts a 30 s track at 3
 * entries and a 10 minute one at 60, and the decoder covers the
 * remainder by decoding forward from the entry before the target --
 * which it must be able to do anyway, since the target is rarely on a
 * frame boundary.
 *
 * REPLAYGAIN_INDEX_MAX caps a very long file; past that the spacing
 * doubles, the same way the envelope's does.
 */
#define REPLAYGAIN_INDEX_SPACING_SEC (10)
#define REPLAYGAIN_INDEX_MAX         (256)

typedef struct {
    bool     present;       /* is any of the rest of this meaningful */
    uint32_t sec;           /* duration, 0 if the pass never learned one */
    int      columns;       /* what was actually filled, <= REPLAYGAIN_COLUMNS */
    uint8_t  level[REPLAYGAIN_COLUMNS];   /* peak magnitude, 0-255 */
} replaygain_waveform_t;

typedef struct {
    bool     present;       /* false when absent, or written by a
                             * different LOUDNESS_VERSION */
    float    integrated_lufs;
    float    sample_peak_dbfs;
    uint32_t blocks;        /* gated blocks; 0 means the answer came
                             * from the ungated fallback and is thin --
                             * see "SHORT TRACKS" in loudness.h */
} replaygain_loudness_t;

/*
 * Cover art location.
 *
 * `present` is "this section was written", NOT "there is art". The
 * distinction is the entire point: covertag.c currently answers "is
 * there a cover" by scanning the tag, which the logs show landing eight
 * to thirteen seconds into a track. Recording a definite no turns that
 * into a stat, and recording where it is turns a scan into a seek --
 * and covertag.c already computes both numbers on its way to the
 * answer, so nothing new is being measured.
 *
 * So: section absent means nobody has looked. Section present with
 * has_art false means somebody looked and there is none, which is a
 * fact worth as much as an offset.
 */
typedef struct {
    bool     present;
    bool     has_art;
    uint32_t offset;        /* byte offset of the image data */
    uint32_t length;        /* bytes; 0 with has_art false */
} replaygain_art_t;

/*
 * What the decoder reports once the file is open, so a listing can show
 * it without opening anything. Duration lives here rather than in the
 * waveform section, where it was only ever a side effect -- ADTS and
 * AMR have no other source for it and duration.c pays for it every
 * time.
 */
typedef struct {
    bool     present;
    uint32_t sec;           /* duration; 0 if genuinely unknown */
    uint32_t sample_rate;
    uint8_t  channels;
    uint16_t kbps;
    char     codec[8];      /* "mp3", "aac", "flac", ... */
    /*
     * Encoder delay and padding, when the file states them. Stored
     * because a file that has them is re-parsed for them on every open,
     * and a file that does not should not be re-checked for them at
     * all. has_gapless separates "zero trim" from "no trim information",
     * which are different files.
     */
    bool     has_gapless;
    uint16_t enc_delay;
    uint16_t enc_padding;
} replaygain_format_t;

/* Tags, so a listing does not open nine files to draw nine rows. Sized
 * to match player.c's own buffers; anything longer is truncated there
 * too, so storing more would be storing what nothing can display. */
typedef struct {
    bool present;
    char title[64];
    char artist[64];
    char album[64];
} replaygain_tags_t;

/*
 * Seek index. `spacing_sec` is what the entries are actually spaced at,
 * which is not necessarily REPLAYGAIN_INDEX_SPACING_SEC -- a long file
 * doubles it to stay under the cap, and a reader must use the stored
 * value rather than the constant.
 */
typedef struct {
    bool     present;
    int      count;
    uint32_t spacing_sec;
    uint32_t offset[REPLAYGAIN_INDEX_MAX];   /* byte offset into the file */
    uint32_t sample[REPLAYGAIN_INDEX_MAX];   /* sample position there */
} replaygain_index_t;

/*
 * How many times a measurement was started and thrown away.
 *
 * A track that is always skipped through is otherwise indistinguishable
 * from one never played, so it is measured from scratch on every play
 * for ever. Counting the abandonments makes that visible, and lets a
 * future policy stop trying, or accept a partial answer, after enough
 * of them. Nothing acts on it yet; it is recorded so the history exists
 * when something wants to.
 */
typedef struct {
    bool     present;
    uint32_t abandoned;
} replaygain_attempts_t;

typedef struct {
    uint32_t filesize;
    int64_t  mtime;
    replaygain_waveform_t waveform;
    replaygain_loudness_t loudness;
    replaygain_art_t      art;
    replaygain_format_t   format;
    replaygain_tags_t     tags;
    replaygain_index_t    index;
    replaygain_attempts_t attempts;
} replaygain_t;

/*
 * Read the sidecar for `path` (the track, not the sidecar).
 *
 * Fills *out with whatever is present and current. Returns false only
 * when there is nothing usable at all -- no file, no parsable line, or
 * a size/mtime that no longer matches the track. A file with a waveform
 * and no loudness returns true with `out->loudness.present` false, and
 * that is the ordinary case for a track that has been walked but never
 * played to the end.
 *
 * Opens its own handle; safe from any task.
 */
bool replaygain_load(const char *path, replaygain_t *out);

/*
 * Store the waveform section, leaving any loudness section alone.
 *
 * `level` is peak magnitude per column from loudness_envelope(), taken
 * off the decoded PCM during a play. Resampled down to
 * REPLAYGAIN_COLUMNS if there are more, max per bucket so a transient
 * survives; stored at its own width if there are fewer.
 *
 * Written on the same completed play that produces the loudness, so a
 * track has both or neither. FORMAT_VERSION 2 exists because version 1
 * stored framewalk.c's global_gain proxy under the same key: same
 * shape, different meaning, and drawing one as the other would be
 * wrong in a way nothing could see.
 */
esp_err_t replaygain_save_waveform(const char *path, const uint8_t *level,
                                   int columns, uint32_t sec);

/*
 * Store the loudness section, leaving any waveform section alone.
 *
 * Called once, after a track has played start to finish without a seek.
 * `blocks` is loudness_finish()'s gated block count and is stored as
 * the confidence figure rather than being turned into a boolean here.
 */
esp_err_t replaygain_save_loudness(const char *path, float integrated_lufs,
                                   float sample_peak_dbfs, uint32_t blocks);

/*
 * The v3 sections. Each merges into the record and leaves the others
 * alone, exactly as the two above do -- the sections are produced at
 * different moments by different code and none may clobber another.
 *
 * NOTHING CALLS THESE YET. This patch defines and tests the format; the
 * producers and consumers are the next one. They are here now so the
 * shape can be verified against a real read/write cycle before anything
 * depends on it, rather than being designed and wired in one step and
 * discovered to be wrong with sidecars already on cards.
 */

/* has_art false records a definite absence, which is the useful half --
 * see replaygain_art_t. */
esp_err_t replaygain_save_art(const char *path, bool has_art,
                              uint32_t offset, uint32_t length);

esp_err_t replaygain_save_format(const char *path,
                                 const replaygain_format_t *fmt);

esp_err_t replaygain_save_tags(const char *path, const char *title,
                               const char *artist, const char *album);

/* `spacing_sec` is stored alongside the entries; a reader must use it
 * rather than assuming the constant. */
esp_err_t replaygain_save_index(const char *path, const uint32_t *offset,
                                const uint32_t *sample, int count,
                                uint32_t spacing_sec);

/* Records one abandoned measurement, incrementing whatever is there. */
esp_err_t replaygain_note_abandoned(const char *path);

/*
 * The gain to apply for a measured track, in dB.
 *
 * REFERENCE minus the measured loudness, then held back so the peak
 * cannot pass full scale: a track measured quiet gets a positive gain,
 * and a positive gain on a track that already peaks near 0 dBFS clips.
 * The peak is what says how much headroom there is, which is the reason
 * it is stored next to the loudness rather than as a curiosity.
 *
 * The clamp is on the POSITIVE side only. Turning a loud track down can
 * never clip, so a large negative gain is applied in full; a large
 * positive one is cut to whatever the headroom allows and the track
 * simply ends up quieter than the reference. That is the honest
 * failure: a limiter would let it reach the target by squashing the
 * peaks, and this player has no business rewriting the waveform to hit
 * a number.
 *
 * Also bounded by REPLAYGAIN_MAX_BOOST_DB so a measurement that is
 * wrong, or a track that really is near-silent, cannot produce a gain
 * that arrives as a shock.
 *
 * Returns 0.0 when `l` holds no measurement, so an unmeasured track
 * plays at unity rather than being guessed at.
 */
float replaygain_gain_db(const replaygain_loudness_t *l);

/* Never boost by more than this, whatever the measurement says. */
#define REPLAYGAIN_MAX_BOOST_DB     (12.0f)

/* Leave this much below full scale when the peak limits the boost, so
 * rounding in the peak and inter-sample peaks the sample peak cannot
 * see (see "SAMPLE PEAK" in loudness.h) have somewhere to go. */
#define REPLAYGAIN_HEADROOM_DB      (1.0f)

#ifdef __cplusplus
}
#endif
