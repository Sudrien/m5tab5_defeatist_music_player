/*
 * ark12.h -- Ark Pixel Font, 12px, as a bitmap table.
 *
 * GENERATED FILE. Do not edit. Regenerate with:
 *     ./tools/gen_ark12.py
 * from ark-pixel-font @ a local checkout.
 *
 * Ark Pixel Font
 * https://github.com/TakWolf/ark-pixel-font
 * Copyright (c) 2021, TakWolf (https://takwolf.com).
 * Licensed under the SIL Open Font License, Version 1.1.
 *
 * NOTE ON LICENSING: the rest of this project is MIT. This file is not,
 * and cannot be. It is a format conversion of OFL-licensed Font Software,
 * which makes it a Modified Version under the OFL, and OFL section 5
 * requires Modified Versions to stay under the OFL. The full text is in
 * components/ark12/LICENSE-OFL and must ship with any redistribution of
 * this file or of a binary containing it. Ark Pixel declares no Reserved
 * Font Name, so this derivative does not have to be renamed -- but it
 * also must not be sold on its own, and this header must stay attached.
 *
 * Every glyph is 12 rows tall and either 6px (Latin and its
 * relatives -- one cell) or 12px (CJK-adjacent scripts and fullwidth forms -- two cells, the
 * same shape a CJK terminal font calls fullwidth) wide. A row is one
 * value with bit 0 leftmost, wide enough to hold either -- one row type
 * rather than two, because a caller drawing text does not want to carry
 * a width-dependent branch through every blit. ark12_w[] says which
 * width each entry actually is; nothing here infers it from the
 * codepoint.
 *
 * SPDX-License-Identifier: OFL-1.1
 */
#pragma once

#include <stdint.h>

#define ARK12_H       12
#define ARK12_HALF_W  6
#define ARK12_FULL_W  12
#define ARK12_COUNT   20669

/*
 * Three parallel arrays rather than one struct per glyph. A struct with
 * a uint16_t codepoint, a uint8_t width and a uint16_t[10] bitmap pads
 * to a multiple of 2 either way, so this is not a packing saving -- it
 * is so ark12_glyph()'s binary search walks a bare uint16_t array rather
 * than striding through bitmaps it has not decided it wants yet.
 *
 * ark12_cp is sorted, direct indexing is not used because the gap from
 * U+007F to U+00A0, and now the much larger gap from U+017F to U+3000,
 * would waste far more than the lookup saves.
 */
extern const uint16_t ark12_cp[ARK12_COUNT];
extern const uint8_t  ark12_w[ARK12_COUNT];      /* ARK12_HALF_W or ARK12_FULL_W */
extern const uint16_t ark12_bits[ARK12_COUNT][ARK12_H];

/*
 * Returns NULL for a codepoint outside the subset -- callers decide what
 * that means; gfx.c draws a notdef box sized from the codepoint's own
 * expected width, which this function has no opinion on for a lookup
 * that failed.
 *
 * *w_out is written only on a successful lookup. A caller that reads it
 * unconditionally on a NULL return is reading whatever was on the stack
 * before the call, same as ignoring any other out-param on failure.
 */
static inline const uint16_t *ark12_glyph(uint32_t cp, int *w_out)
{
    if (cp > 0xFFFFu) return 0;
    int lo = 0, hi = ARK12_COUNT - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint16_t v = ark12_cp[mid];
        if (v == (uint16_t)cp) {
            *w_out = ark12_w[mid];
            return ark12_bits[mid];
        }
        if (v < (uint16_t)cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
