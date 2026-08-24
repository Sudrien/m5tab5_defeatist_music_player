#!/usr/bin/env python3
"""
gen_ark10.py -- turn Ark Pixel Font's 10px monospaced glyph PNGs into the
C table in components/ark10/ark10.h.

Why this exists at all, and why its output is committed while font8x8's is
not: Ark Pixel does not ship a single header, or even a single font file
in its source tree. It ships one PNG per glyph -- tens of thousands of
them -- and builds the .otf/.bdf at release time. cmake/vendored.cmake's
model of "fetch one pinned file and check its SHA256" does not survive
that, and fetching a 17 MB repository archive on every fresh configure to
extract 400 files from it is worse than committing the 6 KB of output.

So: this script is run by hand, rarely (only when bumping ARK_COMMIT), and
its output goes into git. The pin below is what makes that honest -- the
header records the commit it came from, so it can be regenerated and
diffed byte for byte by anyone who doubts it.

Usage:
    ./tools/gen_ark10.py                     # downloads the pinned archive
    ./tools/gen_ark10.py --src path/to/repo  # uses a checkout you have

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
ARK_COMMIT = "master"  # replace with a commit sha before committing output
ARK_URL = f"https://codeload.github.com/TakWolf/ark-pixel-font/zip/{ARK_COMMIT}"

# The subset. Basic Latin is what font8x8 covered; the other two are the
# entire point of the exercise. Latin Extended-A is where Polish, Czech,
# Hungarian, Turkish and the Baltic languages live -- ł, ř, ő, ı, ų are
# all past U+00FF, so stopping at Latin-1 Supplement would fix French and
# German and leave half of Europe still reading '?'.
#
# Latin Extended-B and Latin Extended Additional (Vietnamese) are
# deliberately excluded: another ~700 glyphs, ~7 KB, for a long tail this
# player is unlikely to meet on an SD card. Add the range here and rerun
# if that turns out to be wrong.
RANGES = [
    (0x0020, 0x007E),  # Basic Latin, minus the control codes and DEL
    (0x00A0, 0x00FF),  # Latin-1 Supplement
    (0x0100, 0x017F),  # Latin Extended-A
]

GLYPH_W = 5
GLYPH_H = 10

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


def collect(src: str) -> dict[int, bytes]:
    """codepoint -> 10 bytes of bitmap, one per row, bit 0 = leftmost."""
    root = os.path.join(src, "assets", "glyphs", "10")
    if not os.path.isdir(root):
        sys.exit(f"no glyph directory under {src}")

    wanted = {cp for lo, hi in RANGES for cp in range(lo, hi + 1)}

    # monospaced first, common second. 'common' holds glyphs shared with
    # the proportional cut, and for CJK-adjacent blocks those are
    # fullwidth -- 10 px, not 5. The width check below drops them rather
    # than squeezing them, because a half-drawn © is worse than a missing
    # one. This is why e.g. U+00A9 does not appear in the output.
    found: dict[int, str] = {}
    for flavour in ("common", "monospaced"):
        base = os.path.join(root, flavour)
        if not os.path.isdir(base):
            continue
        for block in sorted(os.listdir(base)):
            # notdef.png sits loose beside the block directories.
            if not os.path.isdir(os.path.join(base, block)):
                continue
            for name in os.listdir(os.path.join(base, block)):
                m = re.fullmatch(r"([0-9A-F]{4,6})\.png", name)
                if not m:
                    continue
                cp = int(m.group(1), 16)
                if cp in wanted:
                    found[cp] = os.path.join(base, block, name)

    glyphs: dict[int, bytes] = {}
    skipped_width = []
    for cp in sorted(found):
        w, h, alpha = png_alpha(open(found[cp], "rb").read())
        if (w, h) != (GLYPH_W, GLYPH_H):
            skipped_width.append((cp, w, h))
            continue
        rows = bytearray()
        for y in range(GLYPH_H):
            bits = 0
            for x in range(GLYPH_W):
                if alpha[y][x] >= ALPHA_ON:
                    bits |= 1 << x
            rows.append(bits)
        glyphs[cp] = bytes(rows)

    if skipped_width:
        print(f"skipped {len(skipped_width)} non-{GLYPH_W}x{GLYPH_H} glyphs: "
              + ", ".join(f"U+{c:04X}({w}x{h})" for c, w, h in skipped_width[:8])
              + (" ..." if len(skipped_width) > 8 else ""), file=sys.stderr)

    missing = sorted(wanted - set(glyphs))
    if missing:
        print(f"{len(missing)} codepoints absent from the source: "
              + ", ".join(f"U+{c:04X}" for c in missing[:8])
              + (" ..." if len(missing) > 8 else ""), file=sys.stderr)
    return glyphs


HEADER = '''/*
 * ark10.h -- Ark Pixel Font, 10px monospaced, Basic Latin + Latin-1
 * Supplement + Latin Extended-A, as a bitmap table.
 *
 * GENERATED FILE. Do not edit. Regenerate with:
 *     ./tools/gen_ark10.py
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
 * components/ark10/LICENSE-OFL and must ship with any redistribution of
 * this file or of a binary containing it. Ark Pixel declares no Reserved
 * Font Name, so this derivative does not have to be renamed -- but it
 * also must not be sold on its own, and this header must stay attached.
 *
 * Each glyph is {w} wide and {h} tall, one byte per row, bit 0 leftmost --
 * the same bit order font8x8_basic used, so the drawing loops did not
 * have to change when this replaced it.
 *
 * SPDX-License-Identifier: OFL-1.1
 */
#pragma once

#include <stdint.h>

#define ARK10_W {w}
#define ARK10_H {h}
#define ARK10_COUNT {count}

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
{{
    if (cp > 0xFFFFu) return 0;
    int lo = 0, hi = ARK10_COUNT - 1;
    while (lo <= hi) {{
        const int mid = (lo + hi) / 2;
        const uint16_t v = ark10_cp[mid];
        if (v == (uint16_t)cp) return ark10_bits[mid];
        if (v < (uint16_t)cp) lo = mid + 1; else hi = mid - 1;
    }}
    return 0;
}}
'''

SOURCE = '''/*
 * ark10.c -- the tables declared by ark10.h.
 *
 * GENERATED FILE. Do not edit. Regenerate with ./tools/gen_ark10.py
 * from ark-pixel-font @ {commit}.
 *
 * Ark Pixel Font, Copyright (c) 2021, TakWolf. See ark10.h and
 * LICENSE-OFL. This file is OFL-1.1, not MIT.
 *
 * SPDX-License-Identifier: OFL-1.1
 */

#include "ark10.h"

const uint16_t ark10_cp[ARK10_COUNT] = {{
{cps}}};

const uint8_t ark10_bits[ARK10_COUNT][ARK10_H] = {{
{bits}}};
'''


def emit(glyphs: dict[int, bytes], outdir: str, commit: str) -> None:
    order = sorted(glyphs)

    cps = ""
    for i in range(0, len(order), 12):
        cps += "    " + " ".join(f"0x{c:04X}," for c in order[i:i + 12]) + "\n"

    bits = ""
    for cp in order:
        row = ", ".join(f"0x{b:02X}" for b in glyphs[cp])
        ch = chr(cp) if 0x20 < cp < 0x7F and cp != 0x22 else " "
        bits += f"    {{ {row} }},  /* U+{cp:04X} {ch} */\n"

    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "ark10.h"), "w") as f:
        f.write(HEADER.format(commit=commit, w=GLYPH_W, h=GLYPH_H,
                              count=len(order)))
    with open(os.path.join(outdir, "ark10.c"), "w") as f:
        f.write(SOURCE.format(commit=commit, cps=cps, bits=bits))

    print(f"{len(order)} glyphs, {len(order) * GLYPH_H} bytes of bitmap, "
          f"{len(order) * 2} bytes of index", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", help="an ark-pixel-font checkout; downloads if absent")
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "components", "ark10"))
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
