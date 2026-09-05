/*
 * decoder.c -- minimp3 for MP3, esp_audio_codec for everything else.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

/* BOUNDARY_NO_INDEX lives with the other seek diagnostics. */
#include "player_diag.h"

#include "decoder.h"
#include "storage_io.h"

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
#include "esp_alac_dec.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

#include "cbrseek.h"
#include "flacseek.h"
#include "oggseek.h"
#include "tsseek.h"
#include "mp4seek.h"
#include "duration.h"

static const char *TAG = "tab5_dec";

/*
 * esp_audio_codec's own log tag, spelled here because it is not ours and
 * there is no header that declares it -- it is a string inside the
 * precompiled archive, recovered from the log line it prints. If a
 * future component renames it, the suppression below silently stops
 * working and the warning comes back, which is the right way for this to
 * fail: noisy, not silent.
 */
#define SDEC_TAG    "AUD_SDEC"

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
    esp_audio_simple_dec_type_t esp_type;  /* kept for the reopen on seek */
    uint8_t *inbuf;
    int in_len;         /* valid bytes in inbuf */
    int in_pos;         /* consumed bytes */
    bool eof;
    bool folding;       /* the stream is 24-bit and is being folded */
    bool frame_mode;    /* the decoder wants exactly one frame per call
                         * -- ALAC in MP4, framed by the sample table */
    int  in_cap;        /* allocated size of inbuf: ESP_IN_BUF, or one
                         * whole sample when framing needs more */

    /* Container-probed length, for the backends that cannot report one.
     * Cached rather than recomputed: it seeks the file handle around,
     * which is cheap but not free, and the answer is fixed. */
    bool probed;
    uint32_t probe_sec;

    /* Constant-bitrate seek map, for the esp_audio_codec backend. Empty
     * unless cbr_probe() proved the file's time-to-offset mapping is a
     * straight line -- see cbrseek.h. */
    cbr_map_t cbr;
    bool cbr_ok;

    /* FLAC seeks by bisecting frame headers instead, because there is no
     * line to prove -- see flacseek.h. At most one of these two is ever
     * true. */
    flac_seek_t flac;

    /* And Ogg, which bisects page granule positions -- see oggseek.h,
     * and in particular what the precompiled parser turned out to do
     * with a page from a new position. */
    ogg_seek_t ogg;

    /* And the transport stream, which bisects PES timestamps on a fixed
     * packet lattice -- the easiest of the four, because there is no
     * resynchronisation to get right. See tsseek.h. */
    ts_seek_t ts;

    /*
     * MP4 is the one the archive said no to, so it is the one that does
     * not use their parser at all: the tables are read here, the AAC is
     * remuxed to ADTS, and the AAC decoder is fed instead of the M4A
     * one. See mp4seek.h. Empty for ALAC and anything else that cannot
     * be remuxed, which then takes the ordinary M4A path unchanged.
     */
    mp4_t mp4;

    /*
     * A table somebody recorded during a previous play, for the stream
     * that has no other way -- see decoder_needs_table(). Owned here:
     * the caller's copy lives in a record that is written and reloaded
     * around us.
     */
    uint32_t *tbl_off;
    uint32_t *tbl_frm;
    int       tbl_count;
    bool      tbl_ok;
    bool      tbl_wanted;       /* nothing else can seek this stream */

    /*
     * Bytes that must reach the parser before the file's own do.
     *
     * WAV and AMR resume behind a preamble of tens of bytes, which fits
     * in the input window with room to spare. Vorbis does not: its three
     * header packets include the codebooks, several KB and sometimes
     * more than the 8 KB window. So the preamble is a source the window
     * is filled FROM, ahead of the file, rather than something written
     * into it once.
     *
     * Borrowed, not owned -- it points into the ogg_seek_t's own
     * storage, which outlives every read that consumes it.
     */
    const uint8_t *pre;
    size_t pre_len;
    size_t pre_pos;

    /* True when the index minimp3 is holding came from a sidecar rather
     * than from its own scan. Only used to keep the harvest below from
     * writing back what it was just given. */
    bool indexed;
    size_t index_count;         /* how many entries were installed */
};

#define ESP_IN_BUF  (8 * 1024)

/* ------------------------------------------------------------------ */
/* Extension mapping                                                   */
/* ------------------------------------------------------------------ */

/*
 * The trim column is the answer for the FORMAT, before the file is
 * opened. MP3 is the one entry that cannot be decided here -- whether
 * the delay is known depends on whether that particular file has a Xing
 * header -- so it is filled in at open from ex.detected_samples and the
 * table value is only its starting point.
 *
 * WAV and FLAC are EXACT because there is nothing to trim: both store a
 * sample count and neither has an encoder delay to remove. That is not
 * an assumption about the backend, it is a property of the containers.
 *
 * Everything else is UNKNOWN, and that is a statement about what has
 * been checked rather than about the formats. AAC, m4a and Opus all
 * have encoder delay and all carry metadata describing it -- iTunSMPB
 * or the edit list, the Opus ID header's pre-skip -- but
 * esp_audio_simple_dec has no surface for any of it. Its API takes bytes
 * and returns PCM; the info struct carries frame_size, not a delay. So
 * the honest answer is that nobody here has verified the ends are being
 * removed, and UNKNOWN is what that means.
 *
 * Opus is the one worth calling out: its pre-skip is not an optional
 * nicety, the spec requires decoders to discard it. If a .opus file
 * plays with 312 samples of nothing at the front, it is this line that
 * says nobody has confirmed otherwise.
 */
static const struct {
    const char *ext;
    backend_t backend;
    esp_audio_simple_dec_type_t type;   /* ignored for minimp3 */
    const char *name;
    decoder_trim_t trim;
} k_formats[] = {
    { ".mp3",  BACKEND_MINIMP3,   0,                              "mp3",  DECODER_TRIM_UNKNOWN },
    { ".flac", BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC, "flac", DECODER_TRIM_EXACT   },
    { ".wav",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_WAV,  "wav",  DECODER_TRIM_EXACT   },
    { ".m4a",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_M4A,  "m4a",  DECODER_TRIM_UNKNOWN },
    { ".mp4",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_M4A,  "m4a",  DECODER_TRIM_UNKNOWN },
    { ".aac",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_AAC,  "aac",  DECODER_TRIM_UNKNOWN },
    { ".ogg",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_OGG,  "ogg",  DECODER_TRIM_UNKNOWN },
    { ".opus", BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_OGG,  "ogg",  DECODER_TRIM_UNKNOWN },
    { ".ts",   BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_TS,   "ts",   DECODER_TRIM_UNKNOWN },
    { ".amr",  BACKEND_ESP_CODEC, ESP_AUDIO_SIMPLE_DEC_TYPE_AMRNB,"amr",  DECODER_TRIM_UNKNOWN },
};

const char *decoder_trim_name(decoder_trim_t t)
{
    switch (t) {
    case DECODER_TRIM_EXACT: return "exact";
    case DECODER_TRIM_NONE:  return "untrimmed";
    default:                 return "unknown";
    }
}

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
    /* Playback class: this is the read nothing may be queued in front
     * of. minimp3 calls it during mp3dec_ex_open_cb()'s index build as
     * well as during playback, which is the pause before the first
     * sample -- that one wants the lease just as much. */
    return storage_io_fread(buf, size, (FILE *)user, STORAGE_IO_PLAYBACK);
}

static int mp3_io_seek(uint64_t position, void *user)
{
    return fseek((FILE *)user, (long)position, SEEK_SET);
}

/*
 * Validate and install a caller-supplied table into minimp3's own index.
 *
 * minimp3 keeps (offset, sample) pairs in exactly this shape, so the
 * installation is a copy and two field assignments. What makes it safe
 * rather than a poke at internals is `indexes_built`: the flag minimp3
 * sets when its own scan has run is the flag that says "this index is
 * complete, do not build one", and that is precisely the claim being
 * made here.
 *
 * ONE COST, STATED RATHER THAN DISCOVERED
 *
 * The entries are ten seconds apart and minimp3's are per frame, 26 ms
 * apart. mp3dec_ex_seek() backs off MINIMP3_PREDECODE_FRAMES entries
 * before the target to fill the bit reservoir -- two frames' worth of
 * back-off on its own index, and twenty seconds' worth on this one --
 * and then decodes forward to the requested sample. So a seek costs up
 * to thirty seconds of MP3 decode where it used to cost a lookup.
 *
 * That is the trade this patch makes: a few hundred milliseconds on
 * each seek in exchange for 1.2 to 1.8 seconds on every play. It is
 * worth taking and it is worth measuring, which is why decoder_seek_sec()
 * now logs how long it spent. IF THAT NUMBER IS BAD, the fix is a
 * denser table, not a return to scanning at open: the spacing is stored
 * in the record precisely so it can change without invalidating what is
 * already written.
 *
 * Every rejection below is a silent-wrong-seek hazard rather than a
 * crash, which is why they are checked here rather than trusted from
 * the file: an index whose pairs are out of order seeks to the wrong
 * place and nothing downstream can tell.
 */
static bool minimp3_install_index(decoder_t *d, const decoder_index_t *ix)
{
    if (!ix || ix->count <= 0 || !ix->offset || !ix->frame) return false;
    if (!d->ex.info.channels || !d->ex.info.hz) return false;

    for (int k = 1; k < ix->count; k++) {
        if (ix->offset[k] <= ix->offset[k - 1] ||
            ix->frame[k]  <= ix->frame[k - 1]) {
            ESP_LOGW(TAG, "seek table is not increasing at %d; ignoring it", k);
            return false;
        }
    }

    mp3dec_frame_t *frames = calloc((size_t)ix->count, sizeof(*frames));
    if (!frames) return false;

    for (int k = 0; k < ix->count; k++) {
        frames[k].offset = ix->offset[k];
        /* minimp3 counts int16 values across all channels. The stored
         * record counts PCM frames. This multiply is the whole of the
         * difference and getting it wrong seeks to half or double the
         * requested point on stereo -- the same trap ex.samples sets
         * two functions down. */
        frames[k].sample = (uint64_t)ix->frame[k] * (uint64_t)d->ex.info.channels;
    }

    /* mp3dec_ex_close() frees index.frames unconditionally, so this
     * allocation is handed over rather than owned here. */
    d->ex.index.frames = frames;
    d->ex.index.num_frames = (size_t)ix->count;
    d->ex.index.capacity = (size_t)ix->count;
    d->ex.indexes_built = 1;
    d->indexed = true;
    d->index_count = (size_t)ix->count;
    return true;
}

static esp_err_t minimp3_open(decoder_t *d, const char *path,
                              const decoder_index_t *ix)
{
    d->f = storage_io_open(path, "rb");
    if (!d->f) return ESP_ERR_NOT_FOUND;

    d->io.read = mp3_io_read;
    d->io.read_data = d->f;
    d->io.seek = mp3_io_seek;
    d->io.seek_data = d->f;

    /* MP3D_SEEK_TO_SAMPLE builds the index up front, which for a 60 MB
     * file on this SD bus is a noticeable pause before the first sample.
     * MP3D_DO_NOT_SCAN skips it: no seeking, no duration, but playback
     * starts immediately. Swap when scrubbing is wanted. */
    /*
     * MP3D_DO_NOT_SCAN only when there is a table to stand in for the
     * scan. Without one the scan is still the only thing that can
     * produce a duration or a seek on a Xing-less file, and turning it
     * off unconditionally would trade a slow open for a dead seek bar
     * -- which is what BOUNDARY_NO_INDEX exists to do deliberately and
     * temporarily, and is not a default.
     */
    const bool have_index = (ix && ix->count > 0);
    const int open_flags = MP3D_SEEK_TO_SAMPLE |
                           (have_index ? MP3D_DO_NOT_SCAN : 0);

#if BOUNDARY_NO_INDEX
    /*
     * DIAGNOSTIC, 0509. See the flag in player.c.
     *
     * The index build is a whole-file read that happens at a track
     * boundary while the outgoing track still has twenty seconds queued
     * -- the T-20 the cyan flash has been timed against since the first
     * note, and the one candidate no patch has removed rather than
     * throttled. This removes it.
     *
     * The seek bar dies with it, which is the cost of the answer.
     */
    ESP_LOGW(TAG, "BOUNDARY_NO_INDEX: skipping the index build");
    if (mp3dec_ex_open_cb(&d->ex, &d->io, MP3D_DO_NOT_SCAN) != 0) {
#else
    if (mp3dec_ex_open_cb(&d->ex, &d->io, open_flags) != 0) {
#endif
        ESP_LOGE(TAG, "minimp3 could not open %s", path);
        storage_io_close(d->f);
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
    /*
     * And now it is recorded rather than only said. detected_samples is
     * nonzero exactly when minimp3_ex found a Xing/Info header, which is
     * also the only case in which it knows the delay and padding and has
     * removed them -- so this one word is the whole of the MP3 answer.
     *
     * NONE rather than UNKNOWN on the other branch, deliberately: a
     * Xing-less MP3 is not a file whose trim state has gone unchecked,
     * it is a file that has been checked and has encoder silence on both
     * ends. The caller can tell those apart and should.
     */
    d->info.trim = d->ex.detected_samples ? DECODER_TRIM_EXACT
                                          : DECODER_TRIM_NONE;

    /*
     * After the open, because the channel count the entries are scaled
     * by is not known until minimp3 has read a frame header -- and
     * before anything can seek, because a half-installed table is a
     * wrong seek rather than a slow one.
     *
     * A table that fails validation leaves indexes_built at whatever
     * the open set, which for a DO_NOT_SCAN open is 0: minimp3 then
     * builds its own index on the first seek, one whole-file read, and
     * the seek works. Slower than intended and never wrong.
     */
    if (have_index && !minimp3_install_index(d, ix)) {
        ESP_LOGW(TAG, "mp3: seek table rejected; minimp3 will build its own");
    } else if (have_index) {
        ESP_LOGI(TAG, "mp3: seek table installed, %d entries every %" PRIu32 " s;"
                      " no scan at open",
                 ix->count, ix->spacing_sec);
    }

    ESP_LOGI(TAG, "mp3: layer %d, %s",
             d->ex.info.layer,
             d->ex.detected_samples ? "Xing/LAME found, gapless trim active"
                                    : "no Xing header, no gapless trim");

    /*
     * What the index cost bought, so the trade is on one line when the
     * time comes to take MP3D_DO_NOT_SCAN.
     *
     * ex.samples is int16 values across all channels -- the same units
     * mp3dec_ex_seek() takes -- so the frame count is samples over
     * channels, and dividing by the rate without dividing by channels
     * first is the classic way to report half the duration of a stereo
     * file.
     */
    if (d->ex.samples && d->ex.info.hz && d->ex.info.channels) {
        const uint64_t frames = d->ex.samples / (uint64_t)d->ex.info.channels;
        ESP_LOGI(TAG, "mp3: index built, %" PRIu64 " samples, %" PRIu32 " s, seekable",
                 (uint64_t)d->ex.samples,
                 (uint32_t)(frames / (uint64_t)d->ex.info.hz));
    } else if (d->indexed) {
        /*
         * A table was installed, so this IS seekable -- ex.samples is
         * zero because MP3D_DO_NOT_SCAN means minimp3 never counted the
         * file, not because nothing can seek it. The length comes from
         * the sidecar's format section, which is where it came from
         * when the scan produced it.
         *
         * The old line said "no index; duration unknown, not seekable"
         * here, which was three claims and two of them wrong. A log
         * that contradicts what the player then does is worse than no
         * log: the first board run of 0703 read as a regression.
         */
        ESP_LOGI(TAG, "mp3: seekable from the table; length from the sidecar");
    } else {
        ESP_LOGI(TAG, "mp3: no index; duration unknown, not seekable");
    }
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

/*
 * Take a table for a stream that has no other way to seek.
 *
 * Factored out of the open because 0805 also installs one part way
 * through a track: the header walk that produces it runs behind the
 * music, so the table arrives after the first sound rather than before
 * it. Same validation either way -- both arrays strictly increasing, or
 * the whole thing is refused rather than half-installed.
 */
static bool install_table(decoder_t *d, const decoder_index_t *ix,
                          const char *what)
{
    if (!d->tbl_wanted || !ix || ix->count <= 1 || !ix->offset || !ix->frame) {
        return false;
    }
    for (int k = 1; k < ix->count; k++) {
        if (ix->offset[k] <= ix->offset[k - 1] ||
            ix->frame[k]  <= ix->frame[k - 1]) {
            ESP_LOGW(TAG, "adts: %s is not increasing; ignoring", what);
            return false;
        }
    }

    uint32_t *off = malloc((size_t)ix->count * sizeof(uint32_t));
    uint32_t *frm = malloc((size_t)ix->count * sizeof(uint32_t));
    if (!off || !frm) { free(off); free(frm); return false; }

    memcpy(off, ix->offset, (size_t)ix->count * sizeof(uint32_t));
    memcpy(frm, ix->frame, (size_t)ix->count * sizeof(uint32_t));

    /* Replacing, not adding: a second table for the same stream is the
     * walk's answer arriving after a stored one was already in. */
    free(d->tbl_off);
    free(d->tbl_frm);
    d->tbl_off = off;
    d->tbl_frm = frm;
    d->tbl_count = ix->count;
    d->tbl_ok = true;
    ESP_LOGI(TAG, "adts: %s, %d entries every %" PRIu32 " s",
             what, ix->count, ix->spacing_sec);
    return true;
}

static esp_err_t esp_codec_open(decoder_t *d, const char *path, int fmt,
                                const decoder_index_t *ix)
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

    d->f = storage_io_open(path, "rb");
    if (!d->f) return ESP_ERR_NOT_FOUND;

    d->in_cap = ESP_IN_BUF;
    d->inbuf = heap_caps_malloc((size_t)d->in_cap,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d->inbuf) { storage_io_close(d->f); d->f = NULL; return ESP_ERR_NO_MEM; }

    /*
     * Before the decoder is opened, because it decides WHICH decoder.
     * A remuxable .m4a is opened as AAC and never meets the M4A parser;
     * everything else falls through with the type the table gives, and
     * the fallback is the behaviour that shipped.
     */
    esp_audio_simple_dec_type_t type = k_formats[fmt].type;
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_M4A && mp4_probe(d->f, &d->mp4)) {
        /*
         * ALAC IS THE OTHER THING IN AN M4A, AND IT CAN SEEK NOW.
         *
         * The note above says _ALAC is ruled out because it wants
         * exactly one encoded frame per call and the sliding window
         * cannot promise that. Inside an MP4 it can: the sample table
         * says where every frame begins and ends, which is the framing
         * layer that was missing. So the same table that makes AAC
         * seekable makes ALAC decodable a frame at a time, and seekable
         * with it.
         *
         * No header is synthesised -- there is no ADTS for ALAC. The
         * samples go over as they are and the decoder is told the
         * shape of them once, in the magic cookie.
         */
        type = d->mp4.alac ? ESP_AUDIO_SIMPLE_DEC_TYPE_ALAC
                           : ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        d->frame_mode = d->mp4.alac;
    } else if (d->mp4.encrypted) {
        /*
         * REFUSED HERE, NOT ONE FRAME LATER.
         *
         * The probe found an encryption wrapper in the sample entry --
         * FairPlay, Common Encryption or Audible. Nothing downstream
         * can decrypt it, so the M4A parser would open the file
         * cleanly, hand over a sample of ciphertext and fail on it,
         * which reaches the listener as a track that starts and dies.
         * Every other probe failure falls through to that parser
         * because the file plays; this one cannot.
         *
         * ESP_ERR_NOT_SUPPORTED rather than a decode error, because it
         * is a fact about the file and not about a frame: retrying,
         * reseeking or reopening will all do the same thing.
         */
        ESP_LOGE(TAG, "%s is encrypted; there is no key here and nothing "
                      "downstream can play it", k_formats[fmt].name);
        free(d->inbuf);
        d->inbuf = NULL;
        storage_io_close(d->f);
        d->f = NULL;
        mp4_free(&d->mp4);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /*
     * A frame decoder is handed one whole sample, so the buffer has to
     * fit the largest one in the file rather than a convenient window.
     * ALAC at 4096 samples of 24-bit stereo is about 24 KB worst case
     * and the 8 KB window would truncate it into nonsense. Failing the
     * allocation is not fatal: the M4A parser takes the file back and
     * plays it the way it always has.
     */
    if (d->frame_mode && d->mp4.max_size > (uint32_t)d->in_cap) {
        uint8_t *bigger = heap_caps_malloc(d->mp4.max_size,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (bigger) {
            free(d->inbuf);
            d->inbuf = bigger;
            d->in_cap = (int)d->mp4.max_size;
        } else {
            ESP_LOGW(TAG, "alac: no room for a %" PRIu32 " B frame; "
                          "leaving it to the M4A parser", d->mp4.max_size);
            mp4_free(&d->mp4);
            d->frame_mode = false;
            type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
        }
    }

    esp_alac_dec_cfg_t alac_cfg = {
        .codec_spec_info = d->mp4.cookie,
        .spec_info_len = d->mp4.cookie_len,
    };

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = type,
        .dec_cfg = d->frame_mode ? &alac_cfg : NULL,
        .cfg_size = d->frame_mode ? (int)sizeof(alac_cfg) : 0,

        /*
         * false lets the decoder's own parser find frame boundaries in
         * whatever we hand it, which is what a file read as a stream
         * needs. true means "this buffer is exactly one frame" -- the
         * mode the frame-at-a-time codecs require, and which only the
         * MP4 sample table can satisfy. See the ALAC note above.
         */
        .use_frame_dec = d->frame_mode,
    };
    d->esp_type = type;

    /*
     * THE COOKIE IS TRIED BOTH WAYS ON PURPOSE.
     *
     * "Magic cookie" means the whole `alac` atom in some writings and
     * the 24-byte ALACSpecificConfig inside it in others, and
     * esp_alac_dec.h says only "Codec specified information (Magic
     * Cookie)". Rather than guess and ship a file that opens on one
     * version of the component and not the next, the atom is offered
     * first and the config after it, and the log says which one the
     * decoder accepted. If neither opens, the M4A parser gets the file
     * back and plays it exactly as it did before -- unseekable, which
     * is what ALAC has always been here.
     */
    /*
     * The vendored decoder grumbles on every ALAC open:
     *
     *   W AUD_SDEC: Not find default parser for 1128352833
     *
     * 1128352833 is 0x41434C41, which is 'ALAC' read the other way up.
     * It is looking for a default *parser* -- the thing that finds frame
     * boundaries in a stream -- and there is not one for ALAC. There
     * does not need to be: `use_frame_dec` is true here precisely
     * because the MP4 sample table does the framing and each buffer
     * handed over is already exactly one frame. The library is reporting
     * the absence of a component this path does not use.
     *
     * It is a warning about nothing, printed once per ALAC track, on a
     * path that then works -- and a log that cries wolf on every file of
     * a format is a log people stop reading. AUD_SDEC lives inside the
     * precompiled esp_audio_codec archive, so there is nothing to patch
     * at source; the tag's threshold is raised across the calls that
     * provoke it and put back afterwards.
     *
     * Narrow on purpose, in both senses. Only this tag, and only around
     * the ALAC opens -- a genuine AUD_SDEC error from anywhere else, or
     * from these calls at ESP_LOG_ERROR, still prints. Restoring the
     * previous level rather than assuming ESP_LOG_INFO means this stays
     * correct under a build with a different default.
     */
    const esp_log_level_t sdec_was = esp_log_level_get(SDEC_TAG);
    if (d->frame_mode) esp_log_level_set(SDEC_TAG, ESP_LOG_ERROR);

    if (d->frame_mode &&
        esp_audio_simple_dec_open(&cfg, &d->esp_dec) != ESP_AUDIO_ERR_OK) {
        alac_cfg.codec_spec_info = d->mp4.cookie + d->mp4.cookie_cfg_off;
        alac_cfg.spec_info_len = d->mp4.cookie_len - d->mp4.cookie_cfg_off;
        if (esp_audio_simple_dec_open(&cfg, &d->esp_dec) == ESP_AUDIO_ERR_OK) {
            ESP_LOGI(TAG, "alac: opened on the config, not the atom");
        } else {
            ESP_LOGW(TAG, "alac: the decoder took neither cookie; "
                          "handing the file back to the M4A parser");
            mp4_free(&d->mp4);
            d->frame_mode = false;
            cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
            cfg.dec_cfg = NULL;
            cfg.cfg_size = 0;
            cfg.use_frame_dec = false;
            d->esp_type = cfg.dec_type;
            type = cfg.dec_type;
        }
    }

    /* Back to normal before the fallback open below, which is an
     * ordinary M4A open and has every reason to be heard. */
    esp_log_level_set(SDEC_TAG, sdec_was);

    if (!d->esp_dec &&
        esp_audio_simple_dec_open(&cfg, &d->esp_dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_codec has no %s decoder built in",
                 k_formats[fmt].name);
        free(d->inbuf);
        d->inbuf = NULL;
        storage_io_close(d->f);
        d->f = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }

    d->info.codec = k_formats[fmt].name;
    /* Per format, and constant for the file. Nothing this backend
     * returns can revise it: esp_audio_simple_dec never reports a delay
     * or a padding count, which is itself the reason most of the column
     * is UNKNOWN. See the table. */
    d->info.trim = k_formats[fmt].trim;

    /*
     * Can this file be seeked by arithmetic? For PCM WAV, CBR ADTS and
     * fixed-mode AMR the answer is yes and the proof is three to five
     * small reads; for everything else cbr_probe() says no and this
     * backend stays unseekable, which is where it was.
     *
     * Done at open rather than at the first drag because it reads the
     * file, and the open is the one moment this handle is not competing
     * with the decode loop for the device.
     */
    /* Not asked at all when the MP4 path took the file: its tables are
     * the answer, and a second mechanism claiming the same file is one
     * that can disagree with the first. */
    d->cbr_ok = d->mp4.ok ? false : cbr_probe(d->f, &d->cbr);

    /*
     * And the variable-rate half. A FLAC states nothing about its byte
     * rate that would survive cbr_probe(), and does not need to: every
     * frame header says which sample it starts at, so the position of a
     * byte can be read rather than estimated. This reads STREAMINFO and
     * finds where the audio starts; the bisection happens at the drag.
     */
    if (!d->mp4.ok && !d->cbr_ok) {
        (void)flac_seek_probe(d->f, &d->flac);
    }
    if (!d->mp4.ok && !d->cbr_ok && !d->flac.ok) {
        (void)ogg_seek_probe(d->f, &d->ogg);
    }
    if (!d->mp4.ok && !d->cbr_ok && !d->flac.ok && !d->ogg.ok) {
        (void)ts_seek_probe(d->f, &d->ts);
    }

    /*
     * Last, and only for raw ADTS: a stream that got this far has been
     * offered a proven byte rate, three bisections and a sample table
     * and matched none of them. Its frames are self-syncing, so the
     * only missing half is where a second lives -- which a previous
     * play can have written down.
     */
    d->tbl_wanted = !d->mp4.ok && !d->cbr_ok && !d->flac.ok &&
                    !d->ogg.ok && !d->ts.ok &&
                    d->esp_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;

    install_table(d, ix, "recorded table");

    /*
     * FLAC STARTS AT ITS AUDIO, NOT AT ITS METADATA.
     *
     * esp_audio_codec's elementary-stream parser searches a bounded
     * window for a frame sync and gives up past it:
     *
     *     E ESP_ES_PARSER: Search overlimited 512000
     *     E tab5_dec: flac decode error -7
     *
     * A FLAC with more than that much metadata in front of its audio --
     * which is any file carrying a large embedded cover; the one that
     * found this has 1876654 bytes of it -- never reaches its first
     * frame, and the whole album plays nothing. The I/O line says
     * exactly that: 512 KB read, which is the search limit to the byte.
     *
     * The probe has already read STREAMINFO and knows where the audio
     * begins, and the seek path has replayed a synthesised header in
     * front of it since 0704. This is the same three lines applied at
     * open: position the file at the first frame, and put "fLaC" plus a
     * last-block STREAMINFO in the window ahead of it. The parser sees a
     * header and then a frame, which is what it would have seen at the
     * front of a file with no pictures in it.
     *
     * Unconditional rather than gated on a size, deliberately. A
     * threshold would leave two open paths that differ only on files
     * nobody has, which is the second path flacseek.c declines to have
     * for the SEEKTABLE and for the same reason. This is also exactly
     * what a seek to 0 s already does, and that is proven on hardware:
     * `flac: seek to 0s -> offset 8288, landed 0.00s (+42 B header)`.
     *
     * Falls back to reading from the top if anything here does not
     * work, which is the behaviour every FLAC had before this.
     */
    if (d->flac.ok && d->flac.first_frame > 0) {
        uint8_t pre[FLAC_PREAMBLE_BYTES];
        const size_t plen = flac_seek_preamble(&d->flac, pre, sizeof(pre));
        if (plen && plen <= (size_t)ESP_IN_BUF &&
            fseek(d->f, d->flac.first_frame, SEEK_SET) == 0) {
            memcpy(d->inbuf, pre, plen);
            d->in_len = (int)plen;
            d->in_pos = 0;
            d->eof = false;
            ESP_LOGI(TAG, "flac: opening at the audio, %ld B of metadata "
                          "skipped (+%u B header)",
                     d->flac.first_frame, (unsigned)plen);
        }
    }

    /* Rate and channels are not known until the first frame comes out;
     * the caller must not configure I2S from info until decoder_read()
     * has returned at least once. */
    return ESP_OK;
}

/*
 * 24-bit PCM folded to the 16 bits everything downstream is built from.
 *
 * WHY THE FOLD IS HERE AND NOT ANYWHERE ELSE
 *
 * This is the first moment the samples exist and the last moment before
 * they belong to the rest of the player. The ring is int16, the
 * ReplayGain scale is int16, the crossfade, the envelope, the loudness
 * gate and the volume are all int16, and the I2S slot config is 16-bit.
 * Converting here means none of them learn that the file was ever
 * anything else.
 *
 * In place, forward, three bytes read for every two written, so the
 * destination trails the source by a growing margin and never catches
 * it. The count returned shrinks accordingly -- decoded_size / 3 rather
 * than / 2 -- and is smaller than the buffer the caller sized for
 * int16, so there is nothing to overflow.
 *
 * ROUNDED, NOT TRUNCATED. Dropping the low byte biases every sample
 * toward zero, which is a DC-ish error that costs exactly as much
 * arithmetic as rounding does. Half an LSB and a clamp: the clamp only
 * fires on a sample within 128 of full scale, where rounding up would
 * wrap the sign, and that is the one input that makes truncation look
 * correct by accident.
 *
 * No dither. The noise it would replace sits at -96 dBFS, under a
 * headphone amp on a tablet, and it would cost two RNG calls a sample
 * at 2.6 million samples a minute to hide something nothing here can
 * resolve.
 */
static int fold_24_to_16(int16_t *buf, uint32_t bytes)
{
    const uint8_t *src = (const uint8_t *)buf;
    int16_t *dst = buf;
    const uint32_t n = bytes / 3;

    for (uint32_t i = 0; i < n; i++) {
        /* Little-endian, sign in the top byte: the cast to int8_t is
         * what extends it, rather than a shift pair that would depend
         * on the signedness of a right shift. */
        int32_t s = ((int32_t)(int8_t)src[2] << 16) |
                    ((int32_t)src[1] << 8) | (int32_t)src[0];
        src += 3;

        s = (s + 128) >> 8;
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        *dst++ = (int16_t)s;
    }
    return (int)n;
}

static int esp_codec_read(decoder_t *d, int16_t *out, int max_int16)
{
    /* A reopen that failed leaves no handle. Reporting the stream as
     * unusable is the only safe answer; calling into it is not. */
    if (!d->esp_dec) return -1;

    while (1) {
        /* Top up. The simple decoder takes arbitrary lengths and tells
         * us how much it consumed, so this is a plain sliding window --
         * no frame alignment to get right. */
        /*
         * Frame at a time. The window logic below cannot be reused:
         * use_frame_dec means the buffer handed over must be one whole
         * frame and nothing more, so the buffer is refilled only when
         * it is empty and is filled with exactly one sample.
         */
        if (d->frame_mode && d->in_len - d->in_pos <= 0) {
            d->in_pos = 0;
            d->in_len = (int)mp4_read_frame(d->f, &d->mp4, d->inbuf,
                                            (size_t)d->in_cap);
            if (d->in_len <= 0) {
                d->in_len = 0;
                d->eof = true;
                return 0;
            }
        }

        if (!d->frame_mode && !d->eof &&
            d->in_len - d->in_pos < ESP_IN_BUF / 2) {
            const int keep = d->in_len - d->in_pos;
            memmove(d->inbuf, d->inbuf + d->in_pos, (size_t)keep);
            d->in_pos = 0;
            d->in_len = keep;
            size_t room = (size_t)(ESP_IN_BUF - keep);
            size_t got = 0;

            /* The preamble first, and only then the file. A partially
             * consumed preamble is the ordinary case for Vorbis, whose
             * headers are larger than this window. */
            if (d->pre && d->pre_pos < d->pre_len) {
                const size_t take = (d->pre_len - d->pre_pos < room)
                                  ? (d->pre_len - d->pre_pos) : room;
                memcpy(d->inbuf + keep, d->pre + d->pre_pos, take);
                d->pre_pos += take;
                got = take;
                room -= take;
            }
            if (room) {
                /* The MP4 path does not read the file as a stream: it
                 * reads the samples the table points at and writes an
                 * ADTS header in front of each. Everything downstream
                 * sees an ordinary AAC stream. */
                got += d->mp4.ok
                     ? mp4_read(d->f, &d->mp4, d->inbuf + keep + got, room)
                     : storage_io_fread(d->inbuf + keep + got, room,
                                        d->f, STORAGE_IO_PLAYBACK);
            }
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
            bool fold = false;
            if (esp_audio_simple_dec_get_info(d->esp_dec, &fi) == ESP_AUDIO_ERR_OK) {
                if (fi.bits_per_sample == 24) {
                    /* Folded below, once, on the way out. Announced on
                     * the first frame only -- it is a property of the
                     * file, not an event. */
                    if (!d->folding) {
                        d->folding = true;
                        ESP_LOGI(TAG, "%s is 24-bit; rounding to 16",
                                 d->info.codec);
                    }
                    fold = true;
                } else if (fi.bits_per_sample != 16) {
                    /*
                     * 32 IS NOT A WIDER 24 AND MUST STAY REFUSED.
                     *
                     * esp_audio_simple_dec_info_t reports a bit count
                     * and nothing about how to read it, so a 32-bit
                     * stream is integer or IEEE float and the struct
                     * cannot say which. Folding float samples as
                     * integers is full-scale noise into headphones, and
                     * guessing has no safe side. See files 19 and 20,
                     * which are the same audio in both and are
                     * indistinguishable from here.
                     */
                    ESP_LOGE(TAG, "%s is %u-bit, this player is 16-bit only",
                             d->info.codec, (unsigned)fi.bits_per_sample);
                    return -1;
                }
                d->info.sample_rate = (int)fi.sample_rate;
                d->info.channels = (int)fi.channel;
                d->info.bitrate_kbps = (int)(fi.bitrate / 1000);
            }
            if (fold) return fold_24_to_16(out, frame.decoded_size);
            return (int)(frame.decoded_size / sizeof(int16_t));
        }
        /* Header-only chunk (container boxes, metadata blocks). Consumed
         * input, produced nothing -- go round again rather than
         * reporting a false EOF to the caller. */
        if (d->eof && d->in_pos >= d->in_len) return 0;
    }
}

/*
 * Seek by moving the file, and reopening the parser onto it.
 *
 * esp_audio_simple_dec has no seek call and never will from here -- it
 * takes bytes and returns PCM. The first version of this moved the file
 * and left the decoder handle alone, on the reasoning that a parser
 * which has already read the container header does not care where the
 * following bytes come from.
 *
 * That is 0700, and it was wrong in the way that matters. The WAV
 * decoder does not merely remember the format, it tracks its position
 * within the `data` chunk it was told about, so a jump landed it
 * somewhere it did not expect and the next process() call failed. The
 * decode loop reads a failure as the end of the track: play_file()
 * returned TRACK_ENDED, the next track started decoding into the other
 * ring while up to twenty seconds of the seeked-from position was still
 * queued in this one -- and because TRACK_ENDED is what arms the
 * crossfade, the two were mixed. On repeat-one, or anywhere the next
 * track was the same file, that is a track playing over itself. The
 * seek "worked"; what it did was end the track.
 *
 * So the parser is put into a state that is defined rather than assumed:
 * the handle is closed and reopened, and fed a preamble that describes
 * the audio at the new offset -- a synthesised RIFF/WAVE header for WAV,
 * the file's magic for AMR, nothing for ADTS, whose frames say what they
 * are. See cbr_resume_preamble().
 *
 * Reopening costs one alloc and one free per seek, on a path that is
 * already dropping seconds of queued audio. It buys the property that
 * every seek starts the parser from the same place a fresh open would.
 *
 * The first frame decoded after a jump is not quite right on AAC, which
 * has inter-frame window overlap the decoder no longer has the previous
 * frame for. That is one frame -- 23 ms at 44.1 kHz -- and is what
 * seeking into a lossy stream sounds like everywhere.
 */
static esp_err_t esp_codec_seek(decoder_t *d, uint32_t sec, uint32_t *landed_cs)
{
    long off;
    uint8_t preamble[FLAC_PREAMBLE_BYTES];
    size_t plen = 0;
    uint32_t at_cs = sec * 100;

    if (d->cbr_ok) {
        off = cbr_offset_for_sec(d->f, &d->cbr, sec);
        /* A byte offset in a constant-rate stream IS a time, so this
         * one can be read straight back off the answer. */
        if (off >= 0 && d->cbr.byte_rate && off >= d->cbr.data_start) {
            at_cs = (uint32_t)((((uint64_t)off - (uint64_t)d->cbr.data_start)
                                * 100) / d->cbr.byte_rate);
        }
    } else if (d->flac.ok) {
        uint64_t landed_sample = 0;
        off = flac_seek_find(d->f, &d->flac, sec, &landed_sample);
        if (off >= 0 && d->flac.sample_rate) {
            at_cs = (uint32_t)((landed_sample * 100) / d->flac.sample_rate);
        }
    } else if (d->ogg.ok) {
        off = ogg_seek_find(d->f, &d->ogg, sec, &at_cs);
    } else if (d->ts.ok) {
        off = ts_seek_find(d->f, &d->ts, sec, &at_cs);
    } else if (d->tbl_ok) {
        /*
         * The recorded table. Entries are where the decoder's next byte
         * was when a given frame count had come out, so the offset is
         * at or before the frame that contains the target -- and then
         * resynced forward, because a byte offset is not a frame start
         * and handing the parser the middle of one wastes a frame at
         * best.
         */
        const uint32_t rate = (uint32_t)(d->info.sample_rate
                                         ? d->info.sample_rate : 44100);
        const uint64_t want = (uint64_t)sec * rate;
        int i = 0;
        for (int k = 0; k < d->tbl_count; k++) {
            if ((uint64_t)d->tbl_frm[k] <= want) i = k; else break;
        }
        long end = 0;
        if (fseek(d->f, 0, SEEK_END) == 0) end = ftell(d->f);
        off = cbr_adts_resync(d->f, (long)d->tbl_off[i], end);
        if (off >= 0) at_cs = (uint32_t)(((uint64_t)d->tbl_frm[i] * 100) / rate);
    } else if (d->mp4.ok) {
        /*
         * The one seek here that is a lookup rather than a search, and
         * the one that needs no preamble and no reopen: every frame
         * handed to the decoder carries its own ADTS header, so a jump
         * is just the next frame coming from a different sample. The
         * offset below is nominal -- mp4_read() reads from the table,
         * not from the file position -- so it only has to be a value
         * that is not an error.
         */
        at_cs = mp4_seek_cs(&d->mp4, sec);
        off = 0;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (off < 0) return ESP_FAIL;

    if (!d->mp4.ok && fseek(d->f, off, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "%s: fseek to %ld failed", d->info.codec, off);
        return ESP_FAIL;
    }

    /* The window holds bytes from the old position. Anything left in it
     * would be decoded before the seek took audible effect, which is the
     * same "the seek was ignored and then happened late" the PCM ring
     * flush exists to prevent, one buffer further up. */
    d->in_len = 0;
    d->in_pos = 0;
    d->eof = false;

    /*
     * The MP4 path skips the reopen. It is the only mechanism here
     * whose parser was never handed a container in the first place --
     * it sees ADTS frames and nothing else -- so there is no header
     * state to restore and no half-assembled packet to discard.
     */
    if (d->mp4.ok) {
        if (landed_cs) *landed_cs = at_cs;
        ESP_LOGI(TAG, "mp4: seek to %" PRIu32 "s, landed %" PRIu32 ".%02" PRIu32
                      "s, sample %" PRIu32,
                 sec, at_cs / 100, at_cs % 100, d->mp4.cur);
        return ESP_OK;
    }

    if (d->esp_dec) {
        esp_audio_simple_dec_close(d->esp_dec);
        d->esp_dec = NULL;
    }
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = d->esp_type,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&cfg, &d->esp_dec) != ESP_AUDIO_ERR_OK) {
        /* Nothing can recover this: the file is fine and the decoder is
         * gone. esp_codec_read() reports the stream unusable from here,
         * which ends the track -- honestly this time, and with a line in
         * the log saying why. */
        d->esp_dec = NULL;
        ESP_LOGE(TAG, "%s: could not reopen the decoder after a seek",
                 d->info.codec);
        return ESP_FAIL;
    }

    /* The preamble goes into the window ahead of the file's own bytes,
     * so the first process() call after this sees a header and then
     * audio, exactly as it would at an ordinary open. */
    d->pre = NULL;
    d->pre_len = 0;
    d->pre_pos = 0;

    if (d->cbr_ok) {
        plen = cbr_resume_preamble(&d->cbr, off, d->inbuf, ESP_IN_BUF);
        d->in_len = (int)plen;
    } else if (d->flac.ok) {
        plen = flac_seek_preamble(&d->flac, preamble, sizeof(preamble));
        if (plen) memcpy(d->inbuf, preamble, plen);
        d->in_len = (int)plen;
    } else {
        /* Ogg's headers are too big for the window and the TS pair is
         * 376 bytes, but both are borrowed rather than copied and both
         * go through the same path -- one queueing mechanism beats two,
         * and the window fills from it on the next read. */
        /* ADTS is the one case with nothing to replay: every frame
         * carries its own header, which is the whole reason a bare
         * offset is enough for it. */
        d->pre = d->ogg.ok ? ogg_seek_preamble(&d->ogg, &d->pre_len)
               : d->ts.ok  ? ts_seek_preamble(&d->ts, &d->pre_len)
                           : NULL;
        plen = d->pre_len;
        d->in_len = 0;
    }

    if (landed_cs) *landed_cs = at_cs;
    ESP_LOGI(TAG, "%s: seek to %" PRIu32 "s -> offset %ld, landed %" PRIu32
                  ".%02" PRIu32 "s (+%u B header)",
             d->cbr_ok ? d->cbr.what
                       : d->flac.ok ? "flac"
                       : d->ogg.ok  ? "ogg"
                       : d->ts.ok   ? "ts" : "adts table",
             sec, off, at_cs / 100, at_cs % 100,
             (unsigned)plen);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

decoder_t *decoder_open(const char *path)
{
    return decoder_open_indexed(path, NULL);
}

decoder_t *decoder_open_indexed(const char *path, const decoder_index_t *ix)
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
                        ? minimp3_open(d, path, ix)
                        : esp_codec_open(d, path, fmt, ix);
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

uint32_t decoder_duration_sec(decoder_t *d)
{
    if (!d) return 0;

    if (d->backend == BACKEND_MINIMP3) {
        if (d->ex.samples && d->ex.info.hz && d->ex.info.channels) {
            /* ex.samples counts int16 values across all channels, not
             * frames. */
            return (uint32_t)(d->ex.samples / d->ex.info.channels /
                              d->ex.info.hz);
        }
        /* No Xing/LAME header and no index: fall through to the probe,
         * which will find nothing for a bare MP3 but costs one fread. */
    }

    /* The backup. esp_audio_codec's simple decoder cannot answer this --
     * frame_size is not stream length and its parsers are forward-only --
     * but the container states its own length in a fixed place, so read
     * it there instead. Cached because the UI asks once per track and the
     * answer cannot change. */
    if (!d->probed) {
        d->probed = true;
        /* PLAYBACK: this runs from decoder_duration_sec() on the
         * decode loop, in the pause before the first sample. */
        d->probe_sec = duration_probe(d->f, STORAGE_IO_PLAYBACK);
        /*
         * And the inference, when the container did not state one. This
         * is the length raw ADTS and AMR lost when the frame walk was
         * deleted in 0206 -- they state nothing about their own length
         * and had no source for it but a whole-file count. A byte rate
         * that has been PROVEN constant over a known extent is that
         * count without the read.
         *
         * Second, not first: a stated length is a fact and this is a
         * derivation, and the two only ever disagree when the derivation
         * is wrong.
         */
        if (!d->probe_sec && d->cbr_ok) {
            d->probe_sec = cbr_duration_sec(&d->cbr);
        }
        /* And a transport stream, which duration.c cannot answer at all
         * -- there is no header stating a length, only the span between
         * the first and last presentation timestamps, which the seek
         * probe has already read for its own clamp. */
        if (!d->probe_sec && d->ts.ok) {
            d->probe_sec = ts_seek_duration_sec(&d->ts);
        }
        if (!d->probe_sec && d->mp4.ok) {
            d->probe_sec = mp4_duration_sec(&d->mp4);
        }
    }
    return d->probe_sec;
}

bool decoder_can_seek(decoder_t *d)
{
    if (!d) return false;

    /* minimp3, once its index exists. */
    if (d->backend == BACKEND_MINIMP3) {
        return d->ex.info.hz && d->ex.info.channels;
    }

    /*
     * esp_audio_codec has no seek entry point, so what is seeked is the
     * FILE, not the decoder -- which only works where the mapping from
     * time to offset is a straight line and has been shown to be one.
     * cbr_ok is that showing. Everything else on this backend is still
     * unseekable: FLAC would want its SEEKTABLE and Ogg a bisection over
     * page granulepos, and neither is written.
     */
    return d->cbr_ok || d->flac.ok || d->ogg.ok || d->ts.ok || d->mp4.ok ||
           d->tbl_ok;
}

esp_err_t decoder_seek_sec(decoder_t *d, uint32_t sec)
{
    return decoder_seek_sec_at_cs(d, sec, NULL);
}

esp_err_t decoder_seek_sec_at_cs(decoder_t *d, uint32_t sec, uint32_t *landed_cs)
{
    if (!d) return ESP_ERR_NOT_SUPPORTED;
    if (landed_cs) *landed_cs = sec * 100;

    if (d->backend != BACKEND_MINIMP3) return esp_codec_seek(d, sec, landed_cs);

    if (!d->ex.info.hz || !d->ex.info.channels) return ESP_ERR_INVALID_STATE;

    /* mp3dec_ex_seek() counts in int16 values across all channels, the
     * same units as ex.samples -- not frames. Seeking to
     * sec * hz would land at half the intended point on stereo. */
    uint64_t target = (uint64_t)sec * d->ex.info.hz * d->ex.info.channels;
    if (d->ex.samples && target >= d->ex.samples) {
        target = d->ex.samples ? d->ex.samples - 1 : 0;
    }

    /*
     * Timed, because this is where the cost of a coarse table lands.
     *
     * With minimp3's own per-frame index this is a lookup and a short
     * read. With a ten-second table it is that plus up to thirty
     * seconds of forward MP3 decode -- see minimp3_install_index(). The
     * decode loop cannot look at a button while it is in here, so this
     * number IS control latency, and it is the one that says whether
     * the spacing wants to come down.
     *
     * It also covers the case nobody plans for: an open with no table
     * and no Xing header, where the first seek is minimp3 building its
     * own index and this reads as the whole file.
     */
    const int64_t t0 = esp_timer_get_time();
    const int rc = mp3dec_ex_seek(&d->ex, target);
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    if (rc != 0) {
        ESP_LOGW(TAG, "seek failed (err %d) after %" PRIu32 " ms",
                 d->ex.last_error, ms);
        return ESP_FAIL;
    }
    if (ms > 100) {
        ESP_LOGW(TAG, "mp3 seek took %" PRIu32 " ms (%s)", ms,
                 d->indexed ? "table" : "no table; index built here");
    }
    return ESP_OK;
}

/*
 * Decimate minimp3's index into a caller's arrays.
 *
 * This is where the table comes from, and it costs nothing: minimp3
 * builds a per-frame index either at open or on the first seek, and
 * everything here is arithmetic on a structure that already exists. The
 * same shape as the loudness measurement -- the expensive step is one
 * the player was having anyway.
 *
 * Not extracted when the index was installed rather than built: that
 * would rewrite the sidecar with the bytes it was just read from,
 * every play, and the dirty flag would be decoration.
 */
bool decoder_needs_table(decoder_t *d)
{
    return d && d->tbl_wanted && !d->tbl_ok;
}

long decoder_stream_pos(decoder_t *d)
{
    if (!d || !d->f) return -1;
    const long at = ftell(d->f);
    if (at < 0) return -1;
    /* Minus the window's unread tail: what has been READ is ahead of
     * what has been DECODED, and the pair being recorded is about the
     * latter. */
    const long unread = (long)(d->in_len - d->in_pos);
    long pos = at - (unread > 0 ? unread : 0);
    /* And minus the preamble still queued, which is not file bytes at
     * all and would otherwise push the offset backwards past the audio.
     */
    if (d->pre && d->pre_pos < d->pre_len) {
        pos = at;                       /* the window is preamble, not file */
    }
    return pos < 0 ? 0 : pos;
}

bool decoder_index_extract(decoder_t *d, uint32_t *offset, uint32_t *frame,
                           int max, uint32_t want_spacing_sec,
                           int *count, uint32_t *spacing_sec)
{
    if (!d || !offset || !frame || max <= 0 || !count || !spacing_sec) {
        return false;
    }
    if (d->backend != BACKEND_MINIMP3) return false;

    /*
     * Normally an installed table is not written back -- that would
     * rewrite the sidecar with the bytes it was read from, every play.
     * The exception is when minimp3 has since built its own index on
     * top: a table stored at the old ten-second spacing stays coarse
     * for ever otherwise, and coarse is what cost 1705 ms on the board.
     *
     * "Has since built one" is exactly "the index is finer than what
     * was installed", which is one comparison and needs no flag.
     */
    if (d->indexed && d->ex.index.num_frames <= d->index_count) return false;
    if (!d->ex.index.frames || d->ex.index.num_frames < 2) return false;
    if (!d->ex.info.hz || !d->ex.info.channels) return false;

    const uint64_t per_sec = (uint64_t)d->ex.info.hz * d->ex.info.channels;
    if (!per_sec) return false;

    uint32_t spacing = want_spacing_sec ? want_spacing_sec : 10;
    const mp3dec_frame_t *f = d->ex.index.frames;
    const size_t n = d->ex.index.num_frames;

    /* Double the spacing until the whole file fits, the same way the
     * envelope's columns are merged. A reader must use the stored value
     * rather than the constant, which is why it is returned. */
    for (;;) {
        const uint64_t step = (uint64_t)spacing * per_sec;
        const uint64_t last = f[n - 1].sample;
        if ((last / step) + 1 <= (uint64_t)max) break;
        if (spacing > (1u << 20)) return false;         /* absurd; give up */
        spacing *= 2;
    }

    const uint64_t step = (uint64_t)spacing * per_sec;
    int out = 0;
    uint64_t next = 0;
    for (size_t i = 0; i < n && out < max; i++) {
        if (f[i].sample < next) continue;
        /* An offset past 4 GB cannot be stored, and a file that large is
         * not on a FAT volume. Stop rather than truncate the value into
         * a wrong offset. */
        if (f[i].offset > UINT32_MAX) break;
        if (out && f[i].offset <= offset[out - 1]) continue;
        offset[out] = (uint32_t)f[i].offset;
        frame[out] = (uint32_t)(f[i].sample / d->ex.info.channels);
        out++;
        next = f[i].sample + step;
    }

    if (out < 2) return false;
    *count = out;
    *spacing_sec = spacing;
    ESP_LOGI(TAG, "mp3: seek table built, %d entries every %" PRIu32 " s "
                  "from %u frames",
             out, spacing, (unsigned)n);
    return true;
}

void decoder_close(decoder_t *d)
{
    if (!d) return;
    if (d->backend == BACKEND_MINIMP3) {
        mp3dec_ex_close(&d->ex);
    } else {
        if (d->esp_dec) esp_audio_simple_dec_close(d->esp_dec);
        ogg_seek_free(&d->ogg);
        mp4_free(&d->mp4);
        free(d->tbl_off);
        free(d->tbl_frm);
        free(d->inbuf);
    }
    if (d->f) storage_io_close(d->f);
    free(d);
}

bool decoder_install_index(decoder_t *d, const decoder_index_t *ix)
{
    if (!d) return false;
    return install_table(d, ix, "table from the walk");
}
