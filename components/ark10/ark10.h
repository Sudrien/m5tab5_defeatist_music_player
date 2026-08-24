/*
 * ark10.h -- Ark Pixel Font, 10px monospaced, Basic Latin + Latin-1
 * Supplement + Latin Extended-A, as a bitmap table.
 *
 * GENERATED FILE. Do not edit. Regenerate with:
 *     ./tools/gen_ark10.py
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
 * components/ark10/LICENSE-OFL and must ship with any redistribution of
 * this file or of a binary containing it. Ark Pixel declares no Reserved
 * Font Name, so this derivative does not have to be renamed -- but it
 * also must not be sold on its own, and this header must stay attached.
 *
 * Each glyph is 5 wide and 10 tall, one byte per row, bit 0 leftmost --
 * the same bit order font8x8_basic used, so the drawing loops did not
 * have to change when this replaced it.
 *
 * SPDX-License-Identifier: OFL-1.1
 */
#pragma once

#include <stdint.h>

#define ARK10_W 5
#define ARK10_H 10
#define ARK10_COUNT 310

/*
 * Two parallel arrays rather than one 0x180-entry direct-indexed table.
 * Direct indexing would be simpler and, at this subset size, barely
 * larger -- but the gap from U+007F to U+00A0 and the eventual wish to
 * add Latin Extended Additional (U+1E00) both argue for a lookup that
 * does not care how sparse the set is. ark10_cp is sorted, so
 * ark10_glyph() binary searches it.
 */
extern const uint16_t ark10_cp[ARK10_COUNT];
extern const uint8_t ark10_bits[ARK10_COUNT][ARK10_H];

/* Returns NULL for a codepoint outside the subset. Callers decide what
 * that means; gfx.c draws U+FFFD's stand-in box. */
static inline const uint8_t *ark10_glyph(uint32_t cp)
{
    if (cp > 0xFFFFu) return 0;
    int lo = 0, hi = ARK10_COUNT - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint16_t v = ark10_cp[mid];
        if (v == (uint16_t)cp) return ark10_bits[mid];
        if (v < (uint16_t)cp) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
