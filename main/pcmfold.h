/*
 * pcmfold.h -- 24-bit PCM folded to the 16 bits everything downstream is
 * built from.
 *
 * WHY THIS IS A HEADER AND NOT A STATIC IN decoder.c
 *
 * It was a static in decoder.c, which was right while the file path was
 * the only path that could meet a 24-bit sample. It is not any more:
 * LapFox Radio is 24-bit Ogg FLAC, netdec.c refused it, and the fold
 * that would have played it was forty lines away behind a `static`.
 *
 * Copying it into netdec.c was the other option and is the worse one.
 * The arithmetic below is the kind that is wrong quietly -- a sign
 * extension that depends on the signedness of a right shift, a rounding
 * step that biases every sample toward zero if it is dropped, a clamp
 * that only fires within 128 of full scale -- and two copies means one
 * of them gets fixed. Header-only and inline, so both paths compile the
 * same instructions, and host-tested in texttest/pcmfoldtest.c, which a
 * static in a file full of ESP-IDF includes could never be.
 *
 * WHERE THE CALL BELONGS, in either path: the first moment the samples
 * exist and the last moment before they belong to the rest of the
 * player. The ring is int16, the ReplayGain scale is int16, the
 * crossfade, the envelope, the loudness gate and the volume are all
 * int16, and the I2S slot config is 16-bit. Converting there means none
 * of them learn the source was ever anything else.
 *
 * 32 IS NOT A WIDER 24 AND IS NOT HANDLED HERE.
 * esp_audio_simple_dec_info_t reports a bit count and nothing about how
 * to read it, so a 32-bit stream is integer or IEEE float and the struct
 * cannot say which. Folding float samples as integers is full-scale
 * noise into headphones, and guessing has no safe side. Both callers
 * refuse 32 before reaching this file; see test files 19 and 20, which
 * are the same audio in both forms and are indistinguishable from here.
 */
#ifndef PCMFOLD_H
#define PCMFOLD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fold `bytes` of little-endian 24-bit PCM in `buf` to int16, in place.
 * Returns the number of int16 samples written.
 *
 * IN PLACE, FORWARD, three bytes read for every two written, so the
 * destination trails the source by a growing margin and never catches
 * it. The count returned shrinks accordingly -- bytes / 3 rather than
 * bytes / 2 -- and is smaller than a buffer sized for int16 at the same
 * byte count, so there is nothing to overflow. A caller that hands over
 * a buffer of N int16 can be handed back at most (N * 2) / 3 samples.
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
 *
 * A byte count that is not a multiple of three leaves the remainder
 * unread rather than reading past it: a partial sample is the decoder
 * having produced something this cannot interpret, and a third of a
 * sample is not worth a special case.
 */
static inline int pcmfold_24_to_16(int16_t *buf, uint32_t bytes)
{
    const uint8_t *src = (const uint8_t *)buf;
    int16_t *dst = buf;
    const uint32_t n = bytes / 3;

    for (uint32_t i = 0; i < n; i++) {
        /*
         * Little-endian, sign in the top byte.
         *
         * ASSEMBLED UNSIGNED, THEN SIGN-EXTENDED BY SUBTRACTION. The
         * obvious version -- casting the top byte to int8_t and
         * shifting it left by 16 -- is undefined behaviour whenever the
         * sample is negative, because C says nothing about left-shifting
         * a negative value. It shipped that way from 0803 and worked on
         * every compiler it met, which is how that class of bug spends
         * its time; UBSan in this file's own test is what finally said
         * so. Nothing below depends on the signedness of a right shift
         * either, for the same reason.
         */
        const uint32_t raw = ((uint32_t)src[2] << 16) |
                             ((uint32_t)src[1] << 8) | (uint32_t)src[0];
        int32_t s = (int32_t)raw;
        if (s & 0x800000) s -= 0x1000000;
        src += 3;

        s = (s + 128) >> 8;
        if (s > 32767) s = 32767;
        else if (s < -32768) s = -32768;
        *dst++ = (int16_t)s;
    }
    return (int)n;
}

#ifdef __cplusplus
}
#endif

#endif /* PCMFOLD_H */
