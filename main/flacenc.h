/*
 * flacenc -- a fixed-predictor FLAC encoder, for the recorder.
 *
 * What libFLAC's -0 to -2 do and nothing else: fixed predictors of order
 * 0-4, partitioned Rice residuals (method 1, 5-bit parameters, when a
 * 24-bit side channel needs them), constant and verbatim fallbacks, and
 * all four stereo decorrelations chosen per block. No LPC, so no
 * floating point anywhere -- which is the point on a core with only a
 * single-precision FPU. See 5104 in ARCHITECTURE.md for why this and not
 * libFLAC (its LPC levels are soft-double and cost more than a core).
 *
 * The caller owns the file. flacenc_open() writes "fLaC" and a
 * placeholder STREAMINFO through the callback; flacenc_close() returns
 * the final one, which the caller writes back over bytes
 * FLACENC_STREAMINFO_OFFSET.. of the file. A file whose writer never got
 * that far is still a valid FLAC stream: the placeholder says "total
 * samples unknown", and every frame carries its own CRC.
 *
 * No MD5: the STREAMINFO signature is left zero, which the format
 * defines as "not computed". It would cost as much as the encoding.
 *
 * Not thread-safe per instance; one encoder is one caller's.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLACENC_STREAMINFO_OFFSET 8
#define FLACENC_STREAMINFO_BYTES  34
#define FLACENC_MAX_BLOCK         4096

typedef bool (*flacenc_write_fn)(void *ctx, const uint8_t *buf, size_t n);
typedef struct flacenc flacenc_t;

/* channels 1-2, bps 16 or 24, 16 <= blocksize <= FLACENC_MAX_BLOCK.
 * The header goes out through wr before this returns. Allocates about
 * 26 * blocksize bytes for stereo, 9 * blocksize for mono. */
flacenc_t *flacenc_open(unsigned channels, unsigned bps, unsigned rate,
                        unsigned blocksize, flacenc_write_fn wr, void *ctx);

/* Interleaved samples, sign-extended in int32. Any frame count; a frame
 * is encoded and written each time a block fills. False if wr refused. */
bool flacenc_write(flacenc_t *e, const int32_t *pcm, unsigned frames);

/* Encodes the last partial block, fills streaminfo, frees e. With
 * streaminfo NULL it only frees (the abandon path). */
bool flacenc_close(flacenc_t *e, uint8_t streaminfo[FLACENC_STREAMINFO_BYTES]);
