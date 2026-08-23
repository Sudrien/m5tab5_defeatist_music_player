/*
 * decoder.c -- minimp3 for MP3, esp_audio_codec for everything else.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "decoder.h"

/* minimp3_ex's default file IO uses open()/mmap(), which FatFs does not
 * provide. MINIMP3_NO_STDIO compiles that path out and leaves the
 * callback interface, which is what we want anyway -- see mp3_io_read
 * below. */
#define MINIMP3_NO_STDIO
#define MINIMP3_IMPLEMENTATION

/* Must come before minimp3_ex.h. Renames our minimp3 out of the way of
 * the identically-named copy inside libesp_audio_codec.a -- see the
 * header for why that archive cannot simply be told not to have one. */
#include "minimp3_prefix.h"
#include "minimp3_ex.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

static const char *TAG = "tab5_dec";

/* ------------------------------------------------------------------ */

typedef enum { BACKEND_MINIMP3, BACKEND_ESP_CODEC } backend_t;

struct decoder {
    backend_t backend;
    FILE *f;
    decoder_info_t info;

    /* minimp3 */
    mp3dec_ex_t ex;
    mp3dec_io_t io;

    /* esp_audio_codec */
    esp_audio_simple_dec_handle_t esp_dec;
    uint8_t *inbuf;
    int in_len;         /* valid bytes in inbuf */
    int in_pos;         /* consumed bytes */
    bool eof;
};

#define ESP_IN_BUF  (8 * 1024)

/* ------------------------------------------------------------------ */
/* Extension mapping                                                   */
/* ------------------------------------------------------------------ */

static const struct {
    const char *ext;
    backend_t backend;
    esp_audio_simple_dec_type_t type;   /* ignored for minimp3 */
    const char *name;
} k_formats[] = {
    { ".mp3",  BACKEND_MINIMP3,   0,                              "mp3"  },
    { ".flac", BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC, "flac" },
    { ".wav",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_WAV,  "wav"  },
    { ".m4a",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_M4A,  "m4a"  },
    { ".mp4",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_M4A,  "m4a"  },
    { ".aac",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_AAC,  "aac"  },
    { ".ogg",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_OGG,  "ogg"  },
    { ".opus", BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_OGG,  "ogg"  },
    { ".ts",   BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_TS,   "ts"   },
    { ".amr",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_AMRNB,"amr"  },
};

/* There is no ESP_AUDIO_SIMPLE_DEC_TYPE_OPUS. There is _RAW_OPUS, and
 * the header is explicit that it "only supports input data with a size
 * of one encoded frame" -- it wants to be handed exactly one frame per
 * call, which the sliding window below deliberately does not do. Every
 * .opus file on disk is Ogg-encapsulated anyway, so both .opus and .ogg
 * route to the OGG parser, which does accept arbitrary input lengths.
 *
 * The same one-frame-per-call restriction rules out _ALAC, _VORBIS,
 * _ADPCM and _LC3 for this code path. They are not missing by oversight;
 * they need a framing layer this decoder does not have. */

/* .m4a is a container, not a codec. esp_audio_codec's M4A parser handles
 * AAC and ALAC inside it; anything else in there (notably Apple's
 * protected AAC) opens and then fails on the first frame. */

static int format_index(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return -1;
    for (size_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); i++) {
        if (strcasecmp(dot, k_formats[i].ext) == 0) return (int)i;
    }
    return -1;
}

bool decoder_supports(const char *path)
{
    return format_index(path) >= 0;
}

/* ------------------------------------------------------------------ */
/* minimp3 backend                                                     */
/* ------------------------------------------------------------------ */

static size_t mp3_io_read(void *buf, size_t size, void *user)
{
    return fread(buf, 1, size, (FILE *)user);
}

static int mp3_io_seek(uint64_t position, void *user)
{
    return fseek((FILE *)user, (long)position, SEEK_SET);
}

static esp_err_t minimp3_open(decoder_t *d, const char *path)
{
    d->f = fopen(path, "rb");
    if (!d->f) return ESP_ERR_NOT_FOUND;

    d->io.read = mp3_io_read;
    d->io.read_data = d->f;
    d->io.seek = mp3_io_seek;
    d->io.seek_data = d->f;

    /* MP3D_SEEK_TO_SAMPLE builds the index up front, which for a 60 MB
     * file on this SD bus is a noticeable pause before the first sample.
     * MP3D_DO_NOT_SCAN skips it: no seeking, no duration, but playback
     * starts immediately. Swap when scrubbing is wanted. */
    if (mp3dec_ex_open_cb(&d->ex, &d->io, MP3D_SEEK_TO_SAMPLE) != 0) {
        ESP_LOGE(TAG, "minimp3 could not open %s", path);
        fclose(d->f);
        d->f = NULL;
        return ESP_FAIL;
    }

    d->info.sample_rate = d->ex.info.hz;
    d->info.channels = d->ex.info.channels;
    d->info.bitrate_kbps = d->ex.info.bitrate_kbps;
    d->info.codec = "mp3";

    /* ex.detected_samples is nonzero only when a Xing/Info header was
     * found, which is also the only case where delay and padding are
     * known and trimmed. Say so, because "gapless" silently not
     * happening is exactly the kind of thing that gets rediscovered
     * three months later. */
    ESP_LOGI(TAG, "mp3: layer %d, %s",
             d->ex.info.layer,
             d->ex.detected_samples ? "Xing/LAME found, gapless trim active"
                                    : "no Xing header, no gapless trim");
    return ESP_OK;
}

static int minimp3_read(decoder_t *d, int16_t *out, int max_int16)
{
    const size_t got = mp3dec_ex_read(&d->ex, out, (size_t)max_int16);

    /* A short read is EOF *or* an error; ex.last_error distinguishes
     * them. minimp3 already skips damaged frames internally, so a
     * nonzero last_error here means the stream is unusable, not that one
     * frame was bad. */
    if (got != (size_t)max_int16 && d->ex.last_error) {
        ESP_LOGE(TAG, "mp3 decode error %d", d->ex.last_error);
        return -1;
    }

    /* Rate and channel count can change at a stream boundary. */
    d->info.sample_rate = d->ex.info.hz;
    d->info.channels = d->ex.info.channels;
    d->info.bitrate_kbps = d->ex.info.bitrate_kbps;
    return (int)got;
}

/* ------------------------------------------------------------------ */
/* esp_audio_codec backend                                             */
/* ------------------------------------------------------------------ */

static bool s_esp_codec_registered;

static esp_err_t esp_codec_open(decoder_t *d, const char *path, int fmt)
{
    if (!s_esp_codec_registered) {
        /* Registers every decoder the component was built with, which
         * is what CONFIG_AUDIO_DECODER_*_SUPPORT controls. MP3 is
         * switched off in sdkconfig.defaults so this does not register
         * a decoder we will never route to; the symbol collision it
         * also causes is handled separately, in minimp3_prefix.h. */
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
        s_esp_codec_registered = true;
    }

    d->f = fopen(path, "rb");
    if (!d->f) return ESP_ERR_NOT_FOUND;

    d->inbuf = heap_caps_malloc(ESP_IN_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d->inbuf) { fclose(d->f); d->f = NULL; return ESP_ERR_NO_MEM; }

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = k_formats[fmt].type,
        .dec_cfg = NULL,
        .cfg_size = 0,
        /* false = let the decoder's own parser find frame boundaries in
         * whatever we hand it. true would mean "this buffer is exactly
         * one frame", which is the mode the frame-at-a-time codecs above
         * require and which we cannot satisfy from a file stream. */
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&cfg, &d->esp_dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_codec has no %s decoder built in",
                 k_formats[fmt].name);
        free(d->inbuf);
        d->inbuf = NULL;
        fclose(d->f);
        d->f = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }

    d->info.codec = k_formats[fmt].name;
    /* Rate and channels are not known until the first frame comes out;
     * the caller must not configure I2S from info until decoder_read()
     * has returned at least once. */
    return ESP_OK;
}

static int esp_codec_read(decoder_t *d, int16_t *out, int max_int16)
{
    while (1) {
        /* Top up. The simple decoder takes arbitrary lengths and tells
         * us how much it consumed, so this is a plain sliding window --
         * no frame alignment to get right. */
        if (!d->eof && d->in_len - d->in_pos < ESP_IN_BUF / 2) {
            const int keep = d->in_len - d->in_pos;
            memmove(d->inbuf, d->inbuf + d->in_pos, (size_t)keep);
            d->in_pos = 0;
            d->in_len = keep;
            const size_t got = fread(d->inbuf + keep, 1,
                                     (size_t)(ESP_IN_BUF - keep), d->f);
            d->in_len += (int)got;
            if (got == 0) d->eof = true;
        }

        if (d->in_len - d->in_pos <= 0) return 0;       /* done */

        esp_audio_simple_dec_raw_t raw = {
            .buffer = d->inbuf + d->in_pos,
            .len = (uint32_t)(d->in_len - d->in_pos),
            .eos = d->eof,
        };
        esp_audio_simple_dec_out_t frame = {
            .buffer = (uint8_t *)out,
            .len = (uint32_t)(max_int16 * (int)sizeof(int16_t)),
        };

        const esp_audio_err_t err = esp_audio_simple_dec_process(d->esp_dec,
                                                                 &raw, &frame);
        d->in_pos += (int)raw.consumed;

        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            /* frame.needed_size now holds the real requirement. Bigger
             * than DECODER_MAX_INT16 means the header promised a block
             * size we did not budget for -- raise the constant rather
             * than silently truncating audio. */
            ESP_LOGE(TAG, "%s frame needs %u bytes, buffer is %u",
                     d->info.codec, (unsigned)frame.needed_size,
                     (unsigned)(max_int16 * sizeof(int16_t)));
            return -1;
        }
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "%s decode error %d", d->info.codec, (int)err);
            return -1;
        }

        if (frame.decoded_size > 0) {
            /* esp_audio_simple_dec_out_t carries no format fields -- it
             * is buffer, len, needed_size, decoded_size and nothing
             * else. Rate and channels come from a separate call, which
             * the header says is only valid once decoded_size is
             * nonzero. Hence asking here rather than above. */
            esp_audio_simple_dec_info_t fi;
            if (esp_audio_simple_dec_get_info(d->esp_dec, &fi) == ESP_AUDIO_ERR_OK) {
                if (fi.bits_per_sample != 16) {
                    /* The I2S slot config is 16-bit. A 24-bit FLAC would
                     * otherwise be handed over as-is and played as
                     * noise. Fail loudly instead. */
                    ESP_LOGE(TAG, "%s is %u-bit, this player is 16-bit only",
                             d->info.codec, (unsigned)fi.bits_per_sample);
                    return -1;
                }
                d->info.sample_rate = (int)fi.sample_rate;
                d->info.channels = (int)fi.channel;
                d->info.bitrate_kbps = (int)(fi.bitrate / 1000);
            }
            return (int)(frame.decoded_size / sizeof(int16_t));
        }
        /* Header-only chunk (container boxes, metadata blocks). Consumed
         * input, produced nothing -- go round again rather than
         * reporting a false EOF to the caller. */
        if (d->eof && d->in_pos >= d->in_len) return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

decoder_t *decoder_open(const char *path)
{
    const int fmt = format_index(path);
    if (fmt < 0) {
        ESP_LOGE(TAG, "no decoder for %s", path);
        return NULL;
    }

    decoder_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->backend = k_formats[fmt].backend;

    const esp_err_t ret = (d->backend == BACKEND_MINIMP3)
                        ? minimp3_open(d, path)
                        : esp_codec_open(d, path, fmt);
    if (ret != ESP_OK) {
        free(d);
        return NULL;
    }
    return d;
}

int decoder_read(decoder_t *d, int16_t *out, int max_int16, decoder_info_t *info)
{
    const int got = (d->backend == BACKEND_MINIMP3)
                  ? minimp3_read(d, out, max_int16)
                  : esp_codec_read(d, out, max_int16);
    if (info) *info = d->info;
    return got;
}

void decoder_close(decoder_t *d)
{
    if (!d) return;
    if (d->backend == BACKEND_MINIMP3) {
        mp3dec_ex_close(&d->ex);
    } else {
        if (d->esp_dec) esp_audio_simple_dec_close(d->esp_dec);
        free(d->inbuf);
    }
    if (d->f) fclose(d->f);
    free(d);
}
