#!/usr/bin/env python3
"""
gen_ark12.py -- turn Ark Pixel Font's 12px glyph PNGs into the C table in
components/ark12/ark12.h.

Why this exists at all, and why its output is committed while font8x8's is
not: Ark Pixel does not ship a single header, or even a single font file
in its source tree. It ships one PNG per glyph -- tens of thousands of
them -- and builds the .otf/.bdf at release time. cmake/vendored.cmake's
model of "fetch one pinned file and check its SHA256" does not survive
that, and fetching a 17 MB repository archive on every fresh configure to
extract twenty thousand files from it is worse than committing the output.

So: this script is run by hand, rarely (only when bumping ARK_COMMIT), and
its output goes into git. The pin below is what makes that honest -- the
header records the commit it came from, so it can be regenerated and
diffed byte for byte by anyone who doubts it.

Usage:
    ./tools/gen_ark12.py                     # downloads the pinned archive
    ./tools/gen_ark12.py --src path/to/repo  # uses a checkout you have

Requires only the standard library. The PNGs are tiny RGBA images, so the
decoder here is a complete but deliberately unclever implementation of the
five PNG filter types rather than a Pillow dependency.

SPDX-License-Identifier: MIT
"""

import argparse
import io
import os
import re
import struct
import sys
import tempfile
import urllib.request
import zipfile
import zlib

# Pinned the same way cmake/vendored.cmake pins minimp3 and pngle: to a
# commit, never a branch. Two people regenerating this header a month
# apart must get the same bytes out.
ARK_COMMIT = "d8bd8345fd80f8cf48527aac7718ee1e280870aa"
ARK_URL = f"https://codeload.github.com/TakWolf/ark-pixel-font/zip/{ARK_COMMIT}"

# The subset. Two widths coexist here, not one: Ark ships glyphs at this
# pixel size in three cuts -- "monospaced" (6x12, one cell), "common"
# (12x12, one cell doubled -- CJK-shaped scripts and the fullwidth forms
# of a few Latin-1 symbols) and "proportional" (variable width AND
# variable height, which is a different rendering model than this file
# has and is not what RANGES below draws from). A range only belongs here
# if every codepoint in it resolves to one of the first two.
#
# Cyrillic is in RANGES now and was not at 10px. The reason is entirely
# about which cuts exist at which size: at 10px Cyrillic is shipped only
# in the "proportional" cut -- 16 rows tall with per-glyph ascenders and
# descenders, spanning rows 2..13, which no 10-row window can crop without
# clipping real letters (measured: the best-placed window still clips 25
# of 153 glyphs). At 12px there is a genuine monospaced Cyrillic cut at
# the same 6x12 cell as Latin, so it drops in with no special handling at
# all. Nothing clever happened here; the data simply exists at this size
# and did not at the last one.
#
# Hangul is still out, and for an unchanged reason: the Compatibility Jamo
# block (individual letters) exists, but Hangul Syllables -- the composed
# characters real Korean text is written in -- has no glyphs at this size
# either. Jamo alone cannot render a Korean title; Unicode's algorithmic
# Hangul composition would have to run on top of glyphs this font does not
# have, so Korean stays out rather than shipping half of it.
#
# What *is* available clean, checked against the archive: Hiragana and
# Katakana (93 + 96 codepoints, both entirely 12x12 in the "common" cut),
# CJK Symbols and Punctuation (the fullwidth quote marks and brackets
# Japanese and Chinese titles actually use), and -- the reason this size
# was worth the move -- 18,299 unique CJK Unified Ideograph codepoints,
# against 1,076 at 10px. That is effectively the whole block rather than a
# subset, so a Chinese or Japanese title is no longer a coin flip on
# whether its particular characters happen to be drawn.
#
# "Effectively" and not "entirely": Ark is hand-drawn and crowd-
# contributed, and a few codepoints simply have no glyph at any size --
# U+8B77 and U+90CE among them, both ordinary characters in Japanese
# names. They come out of glyph_for() as a notdef box, which is the
# correct outcome and not a bug in this script; there is nothing to
# generate. Expect a small number of boxes in CJK text rather than none.
#
# Halfwidth and Fullwidth Forms (FF00-FFEF) is in RANGES because gfx.c's
# cp_is_wide() already treats that block as fullwidth, and a block the
# renderer reserves double-width space for but the table has no glyphs
# for is the worst of both: a double-wide notdef box where a fullwidth
# parenthesis belongs. Japanese and Chinese taggers use these constantly
# -- a title with (2017) in fullwidth parens is completely ordinary --
# so the block was a real omission rather than a nicety, caught by
# rendering a sampler sheet and seeing the boxes.
#
# That coverage is not free: see the size report emit() prints. The table
# is roughly 545 KB against a 3 MB app partition. It is affordable, it is
# the single largest thing in the binary, and it is the first thing to
# trim if that ever stops being true -- dropping (0x4E00, 0x9FFF) alone
# takes it to about 62 KB while keeping every Latin, Cyrillic, kana and
# fullwidth-punctuation glyph. Note that 62 and not something nearer 20:
# Extension A is 1,480 codepoints of its own and stays behind, so cutting
# the main block is most but not all of the saving.
#
# Latin Extended-B and Latin Extended Additional (Vietnamese) are still
# excluded on the original grounds -- a long tail this player is unlikely
# to meet on an SD card. Add the range here and rerun if that turns out to
# be wrong.
RANGES = [
    (0x0020, 0x007E),  # Basic Latin, minus the control codes and DEL
    (0x00A0, 0x00FF),  # Latin-1 Supplement
    (0x0100, 0x017F),  # Latin Extended-A
    (0x0400, 0x04FF),  # Cyrillic (halfwidth; monospaced cut, 12px only)
    (0x3000, 0x303F),  # CJK Symbols and Punctuation (fullwidth)
    (0x3040, 0x309F),  # Hiragana (fullwidth)
    (0x30A0, 0x30FF),  # Katakana (fullwidth)
    (0x3400, 0x4DBF),  # CJK Unified Ideographs Extension A (fullwidth)
    (0x4E00, 0x9FFF),  # CJK Unified Ideographs (fullwidth)
    (0xFF00, 0xFFEF),  # Halfwidth and Fullwidth Forms
]

# 12px, not the 10px this started at. Ark ships three sizes -- 10, 12 and
# 16 -- and they are not the same font at three scales; they are three
# separately drawn sets with very different coverage, because this is a
# hand-drawn crowd-contributed font and contributors did not spread evenly
# across sizes. Counted from the archive rather than assumed:
#
#                        10px        12px        16px
#   CJK Unified          1076       18299          97
#   Cyrillic (mono)         0         151         151
#   Hiragana/Katakana   93/96       93/96       93/96
#
# 12px is simply where this font is finished. It is the only size with a
# monospaced Cyrillic cut at the same fixed cell as Latin (10px has only
# a variable-height "proportional" cut, which is a different rendering
# model and was why Cyrillic was excluded before), and its CJK coverage is
# effectively the whole block rather than a curated subset. 16px regresses
# hard on CJK -- 97 codepoints -- so it buys resolution and loses the
# script that motivated the exercise.
#
# There is a display argument on top of the coverage one. The panel is
# 720x1280 on 5", about 294 PPI, and CLAUDE.md's "Sizes are set for 294
# PPI" section scales everything up to compensate. Scaling up a 10px glyph
# by 4 gives 40px of very blocky letterform; a 12px glyph by 3 gives 36px
# with more drawn detail underneath and a smaller scale multiplier, so the
# blocks are 3x3 rather than 4x4. Same physical size, more fidelity --
# which is the whole point of having pixels this small.
GLYPH_H = 12
HALF_W = 6    # one cell -- Latin, Latin-1, Latin Extended-A, Cyrillic
FULL_W = 12   # two cells -- CJK-adjacent scripts and fullwidth forms

# Anything drawn at least this opaque is on. Ark's glyphs are hard-edged
# 1-bit art stored in an 8-bit alpha channel, so every pixel is 0 or 255
# and the threshold never actually has to decide anything. It is here so
# that a future antialiased source degrades to something legible instead
# of to noise.
ALPHA_ON = 128


def png_alpha(data: bytes) -> tuple[int, int, list[list[int]]]:
    """Decode an 8-bit RGBA PNG, returning (w, h, alpha rows)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    idat = bytearray()
    w = h = depth = color = 0
    off = 8
    while off < len(data):
        (length,) = struct.unpack(">I", data[off:off + 4])
        kind = data[off + 4:off + 8]
        body = data[off + 8:off + 8 + length]
        if kind == b"IHDR":
            w, h, depth, color, _, interlace, _ = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color != 6 or interlace != 0:
                raise ValueError(f"unsupported PNG: depth={depth} color={color}")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
        off += 12 + length

    raw = zlib.decompress(bytes(idat))
    stride = w * 4
    out = []
    prev = bytearray(stride)
    pos = 0
    for _ in range(h):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if ftype == 0:
                pass
            elif ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
            else:
                raise ValueError(f"bad filter {ftype}")
        out.append([line[x * 4 + 3] for x in range(w)])
        prev = line
    return w, h, out


def collect(src: str) -> dict[int, tuple[int, list[int]]]:
    """codepoint -> (width, 10 rows of bitmap, one uint per row, bit 0 = leftmost).

    width is HALF_W or FULL_W, read from the PNG rather than assumed --
    see the RANGES comment above for why that distinction cannot be made
    from the codepoint alone at the RANGES-selection stage and has to be
    settled per glyph, here, against the actual pixel data.
    """
    root = os.path.join(src, "assets", "glyphs", "12")
    if not os.path.isdir(root):
        sys.exit(f"no glyph directory under {src}")

    wanted = {cp for lo, hi in RANGES for cp in range(lo, hi + 1)}

    # monospaced first, common second -- in PRIORITY, which is the
    # opposite of iteration order below: iterating common first and
    # monospaced second means a codepoint present in both ends up with
    # the monospaced path, because the second loop's assignment wins.
    # 'common' holds glyphs shared with the proportional cut, and for
    # CJK-adjacent blocks those are fullwidth -- 10 px, not 5. Nothing in
    # the new ranges has a monospaced entry to prefer, so this only
    # matters for the original three Latin ranges, unchanged from before.
    found: dict[int, str] = {}
    for flavour in ("common", "monospaced"):
        base = os.path.join(root, flavour)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            # CJK Unified Ideographs is itself split into per-prefix
            # subdirectories (4E-/, 4F-/, ...) rather than one flat
            # directory of 1244 files -- os.walk() rather than a single
            # os.listdir() is what makes that transparent here.
            for name in filenames:
                m = re.fullmatch(r"([0-9A-F]{4,6})\.png", name)
                if not m:
                    continue
                cp = int(m.group(1), 16)
                if cp in wanted:
                    found[cp] = os.path.join(dirpath, name)

    glyphs: dict[int, tuple[int, list[int]]] = {}
    skipped_width = []
    for cp in sorted(found):
        w, h, alpha = png_alpha(open(found[cp], "rb").read())
        if h != GLYPH_H or w not in (HALF_W, FULL_W):
            skipped_width.append((cp, w, h))
            continue
        rows = []
        for y in range(GLYPH_H):
            bits = 0
            for x in range(w):
                if alpha[y][x] >= ALPHA_ON:
                    bits |= 1 << x
            rows.append(bits)
        glyphs[cp] = (w, rows)

    if skipped_width:
        print(f"skipped {len(skipped_width)} glyphs of neither {HALF_W}x{GLYPH_H} "
              f"nor {FULL_W}x{GLYPH_H}: "
              + ", ".join(f"U+{c:04X}({w}x{h})" for c, w, h in skipped_width[:8])
              + (" ..." if len(skipped_width) > 8 else ""), file=sys.stderr)

    # Not a warning. RANGES names whole Unicode blocks, and Ark draws
    # those blocks sparsely -- CJK Unified is 20,992 codepoints of which
    # 18,299 are drawn, and the rest were never expected. Reported as a
    # count, and only as a count, because the previous phrasing ("N
    # codepoints absent from the source") read as breakage on every run
    # and trained the eye to skip it. The per-range table emit() prints
    # is where a genuinely empty range shows up.
    missing = wanted - set(glyphs)
    if missing:
        print(f"{len(glyphs)} of {len(wanted)} requested codepoints drawn at "
              f"this size; {len(missing)} are not in the font (expected -- "
              f"RANGES names whole blocks)", file=sys.stderr)
    return glyphs


HEADER = '''/*
 * ark12.h -- Ark Pixel Font, 12px, as a bitmap table.
 *
 * GENERATED FILE. Do not edit. Regenerate with:
 *     ./tools/gen_ark12.py
 * from ark-pixel-font @ {commit}.
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
 * Every glyph is {h} rows tall and either {half}px (Latin and its
 * relatives -- one cell) or {full}px ({full_desc} -- two cells, the
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

#define ARK12_H       {h}
#define ARK12_HALF_W  {half}
#define ARK12_FULL_W  {full}
#define ARK12_COUNT   {count}

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
{{
    if (cp > 0xFFFFu) return 0;
    int lo = 0, hi = ARK12_COUNT - 1;
    while (lo <= hi) {{
        const int mid = (lo + hi) / 2;
        const uint16_t v = ark12_cp[mid];
        if (v == (uint16_t)cp) {{
            *w_out = ark12_w[mid];
            return ark12_bits[mid];
        }}
        if (v < (uint16_t)cp) lo = mid + 1; else hi = mid - 1;
    }}
    return 0;
}}
'''

SOURCE = '''/*
 * ark12.c -- the tables declared by ark12.h.
 *
 * GENERATED FILE. Do not edit. Regenerate with ./tools/gen_ark12.py
 * from ark-pixel-font @ {commit}.
 *
 * Ark Pixel Font, Copyright (c) 2021, TakWolf. See ark12.h and
 * LICENSE-OFL. This file is OFL-1.1, not MIT.
 *
 * SPDX-License-Identifier: OFL-1.1
 */

#include "ark12.h"

const uint16_t ark12_cp[ARK12_COUNT] = {{
{cps}}};

const uint8_t ark12_w[ARK12_COUNT] = {{
{widths}}};

const uint16_t ark12_bits[ARK12_COUNT][ARK12_H] = {{
{bits}}};
'''


def emit(glyphs: dict[int, tuple[int, list[int]]], outdir: str, commit: str) -> None:
    order = sorted(glyphs)

    cps = ""
    for i in range(0, len(order), 12):
        cps += "    " + " ".join(f"0x{c:04X}," for c in order[i:i + 12]) + "\n"

    widths = ""
    for i in range(0, len(order), 16):
        row_cps = order[i:i + 16]
        widths += "    " + " ".join(f"{glyphs[c][0]}," for c in row_cps) + "\n"

    bits = ""
    for cp in order:
        w, rows = glyphs[cp]
        row = ", ".join(f"0x{b:03X}" for b in rows)
        ch = chr(cp) if 0x20 < cp < 0x7F and cp != 0x22 else " "
        bits += f"    {{ {row} }},  /* U+{cp:04X} {ch} */\n"

    n_half = sum(1 for c in order if glyphs[c][0] == HALF_W)
    n_full = len(order) - n_half
    bitmap_bytes = len(order) * GLYPH_H * 2
    index_bytes = len(order) * (2 + 1)

    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "ark12.h"), "w") as f:
        f.write(HEADER.format(commit=commit, h=GLYPH_H, half=HALF_W, full=FULL_W,
                              full_desc="CJK-adjacent scripts and fullwidth forms",
                              count=len(order)))
    with open(os.path.join(outdir, "ark12.c"), "w") as f:
        f.write(SOURCE.format(commit=commit, cps=cps, widths=widths, bits=bits))

    print(f"{len(order)} glyphs ({n_half} halfwidth, {n_full} fullwidth), "
          f"{bitmap_bytes} bytes of bitmap, {index_bytes} bytes of index, "
          f"{bitmap_bytes + index_bytes} bytes total", file=sys.stderr)

    # Per-range, because a range silently contributing nothing is the
    # failure mode this script actually has -- adding a block to RANGES
    # that the font does not draw at this size produces a working build
    # and no glyphs, and nothing else here would say so.
    print("  per range:", file=sys.stderr)
    for lo, hi in RANGES:
        n = sum(1 for cp in order if lo <= cp <= hi)
        flag = "   <-- EMPTY, is this range drawn at this size?" if n == 0 else ""
        print(f"    U+{lo:04X}..U+{hi:04X}  {n:6d}{flag}", file=sys.stderr)

    # The number the RANGES comment quotes for trimming. Printed rather
    # than left as a claim in a comment that nobody re-checks after
    # changing RANGES.
    kept = [cp for cp in order if not (0x4E00 <= cp <= 0x9FFF)]
    trimmed = len(kept) * (GLYPH_H * 2 + 3)
    print(f"  without U+4E00..U+9FFF: {len(kept)} glyphs, {trimmed} bytes",
          file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", help="an ark-pixel-font checkout; downloads if absent")
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "components", "ark12"))
    args = ap.parse_args()

    if args.src:
        emit(collect(args.src), args.out, "a local checkout")
        return

    print(f"fetching {ARK_URL}", file=sys.stderr)
    blob = urllib.request.urlopen(ARK_URL).read()
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            z.extractall(tmp)
        root = os.path.join(tmp, os.listdir(tmp)[0])
        emit(collect(root), args.out, ARK_COMMIT)


if __name__ == "__main__":
    main()
