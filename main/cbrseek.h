/*
 * cbrseek.h -- seeking in files that carry no seek data, by proving the
 * byte rate is constant rather than by assuming it.
 *
 * WHAT THIS IS FOR
 *
 * decoder_seek_sec() is minimp3-only. minimp3 can seek because
 * MP3D_SEEK_TO_SAMPLE reads the whole file at open and builds a
 * sample-accurate index; every esp_audio_codec format returns
 * ESP_ERR_NOT_SUPPORTED and the drag is refused, because that backend's
 * simple decoder has no seek entry point and its parsers are
 * forward-only over a stream.
 *
 * But a forward-only parser is not the same as an unseekable file. If
 * the mapping from time to byte offset is linear, seeking is an fseek
 * and a reset of the input window -- the parser never has to know it
 * moved. That covers the whole of the "fixed bitrate, no seek data"
 * class: PCM WAV, constant-bitrate ADTS AAC, AMR at a fixed mode.
 *
 * WHY THIS IS NOT `file size / bitrate`
 *
 * duration.c refuses that calculation, in those words, and it is right
 * to: it is correct for CBR, drifts badly on VBR, and a seek bar that
 * lies is worse than one that does not move. The difference here is that
 * the linearity is CHECKED rather than hoped for.
 *
 *   - WAV states its byte rate in `fmt ` and its extent in `data`. PCM
 *     is linear by construction; there is nothing to verify.
 *   - ADTS states a frame length in every frame header. This walks
 *     short runs of frames at five points spread across the file and
 *     requires the mean frame length to agree between them, so a VBR
 *     stream -- whose quiet and loud sections have visibly different
 *     means -- is rejected rather than mapped wrongly. A stream that
 *     declares itself VBR through buffer_fullness = 0x7FF is rejected
 *     before any of that.
 *   - AMR frame size is a function of the mode bits in each frame
 *     header, so a constant mode is a constant frame size. Sampled the
 *     same way.
 *
 * A rejected file is exactly where it was: no duration from here, no
 * seek, the bar stays a groove. The failure mode is losing a feature,
 * not gaining a wrong answer.
 *
 * WHAT ACCURACY THIS BUYS
 *
 * WAV and AMR land exactly -- both have addressable frames of a size
 * known to the byte. ADTS lands on the first frame boundary at or after
 * the requested offset, so the error is under one frame (23 ms at
 * 44.1 kHz) plus whatever the tolerated deviation in mean frame length
 * has accumulated, which over a ten-minute track at the 1.5% bound is
 * under a second at the far end and zero at the near one. A finger on a
 * 720 px bar is asking for about 800 ms of a ten-minute track, so the
 * mapping is finer than the request.
 *
 * IT ALSO ANSWERS THE DURATION
 *
 * Which is the one thing 0206 traded away: raw ADTS and AMR lost their
 * only source of a length when the frame walk was deleted, and read
 * `--:--` until a full play recorded one in the sidecar. A proven byte
 * rate over a known extent is a length, from three small reads, on the
 * open path. duration.c does not grow this because duration.c's contract
 * is "what the container states about itself" and this is an inference
 * with a proof attached -- a different kind of answer, kept in a
 * different file.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    long     data_start;    /* first audio byte */
    long     data_end;      /* one past the last audio byte */
    uint32_t byte_rate;     /* bytes of audio per second, proven constant */
    uint32_t align;         /* offsets are rounded down to a multiple of this */
    bool     resync;        /* the offset must be walked onto a frame header */
    const char *what;       /* "wav pcm", "adts cbr", "amr" -- for the log */

    /* What a freshly-opened parser has to be shown before the bytes at
     * the seek target mean anything. See cbr_resume_preamble(). Zeroed
     * for the formats that need none. */
    uint16_t channels;
    uint16_t bits;
    uint32_t sample_rate;
    uint8_t  magic[9];      /* AMR: the file's own magic */
    uint8_t  magic_len;
} cbr_map_t;

/*
 * True when the file's time-to-offset mapping was proven linear.
 *
 * The handle's position is saved and restored, so this is safe to call
 * on a decoder's own handle before the first read. It costs five reads
 * of a few hundred bytes on the worst path and two on WAV.
 *
 * Format is sniffed from the bytes, not from the extension, for the
 * reason duration.c gives: the caller already chose a decoder by
 * extension, and a probe that trusted the same wrong extension would
 * return a confident number for a mislabelled file.
 */
bool cbr_probe(FILE *f, cbr_map_t *m);

/* Seconds, or 0 if the map is empty. Extent over rate, which is only a
 * duration because cbr_probe() established the rate is constant. */
uint32_t cbr_duration_sec(const cbr_map_t *m);

/*
 * Byte offset to read from for a given second, or -1 if it cannot be
 * placed. Clamped into the audio extent, so a seek to the very end lands
 * on the last frame rather than past it.
 *
 * Reads the file when the map wants a resync (ADTS), which is why this
 * takes the handle and is not arithmetic in the caller.
 */
long cbr_offset_for_sec(FILE *f, const cbr_map_t *m, uint32_t sec);

/*
 * The bytes a fresh parser must be fed before the audio at `off`.
 *
 * Seeking here moves the file under a parser that has already read the
 * container header, and 0700 is the bug that comes from assuming the
 * parser tolerates that: esp_audio_codec's WAV decoder does not, and a
 * seek left it erroring on the first block. So the decoder handle is
 * reopened on every seek and shown a header again -- a synthesised
 * 44-byte RIFF/WAVE for WAV, describing the same format the file
 * declares with a data length of what is actually left, and the file's
 * own magic for AMR. ADTS needs none: its frames are self-describing,
 * which is what the resync finds.
 *
 * Returns the number of bytes written, which may be 0. `cap` should be
 * at least 44.
 */
size_t cbr_resume_preamble(const cbr_map_t *m, long off, uint8_t *buf, size_t cap);

/*
 * First ADTS frame header at or after `off`, or -1.
 *
 * Exported for the recorded-table seek in 0716, which knows roughly
 * where a second lives but not where a frame starts. The validation is
 * the same one probe_adts() uses -- a candidate is only accepted when
 * the length it declares lands on another valid header -- because a raw
 * AAC payload contains FF F1 constantly and one sync pattern proves
 * nothing.
 */
long cbr_adts_resync(FILE *f, long off, long end);

#ifdef __cplusplus
}
#endif
