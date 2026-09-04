# ark12

Ark Pixel Font, 12px, converted to a C bitmap table.

Upstream: https://github.com/TakWolf/ark-pixel-font
Copyright (c) 2021, TakWolf. SIL Open Font License, Version 1.1.

## Licence

**This component is OFL-1.1. The rest of this project is MIT.**

`ark12.c` and `ark12.h` are a format conversion of OFL-licensed Font
Software — PNG glyphs to C arrays — which makes them a Modified Version
under OFL section 5, and section 5 requires Modified Versions to remain
under the OFL. That is not a problem for this project: the OFL explicitly
permits bundling with software under any licence, and only the font files
themselves are bound by it. But it does mean three things:

- `LICENSE-OFL` must ship with any redistribution of these files, and
  with any binary built from them. Firmware images count.
- The tables cannot be relicensed MIT, and no amount of "we typed it out
  as C" changes that.
- They must not be sold on their own, only as part of something.

Ark Pixel declares **no Reserved Font Name**, so this derivative does not
have to be renamed. The name `ark12` is used anyway, because it is not
the Original Version and should not be mistaken for it.

This is the difference from the `font8x8` component it replaces: that
font was public domain, so nothing had to travel with it.

## Coverage

**20,669 glyphs, about 545 KB of flash.** Two cell sizes, and the table
records which each glyph is rather than inferring it from the codepoint:

- **6×12 halfwidth** (531 glyphs) — Basic Latin (U+0020–U+007E),
  Latin-1 Supplement, Latin Extended-A, Cyrillic.
- **12×12 fullwidth** (20,138 glyphs) — CJK Symbols and Punctuation,
  Hiragana, Katakana, CJK Unified Ideographs and Extension A, and
  Halfwidth and Fullwidth Forms.

CJK Unified is the bulk of that: 18,299 codepoints, effectively the whole
block. It is also the obvious thing to cut if the app partition ever gets
tight — dropping `(0x4E00, 0x9FFF)` from `RANGES` takes the table to about
62 KB while keeping every Latin, Cyrillic, kana and fullwidth-punctuation
glyph. (62 and not nearer 20: Extension A is 1,480 codepoints of its own
and stays behind.)

The five Latin-1 characters that were missing at 10px — © ® ¼ ½ ¾ — are
present now. They are drawn fullwidth by Ark, which the old
halfwidth-only table could not store and this one can. U+00A0 is drawn as
a space and U+00AD is skipped, both handled in `gfx.c`.

A small number of CJK codepoints have no glyph in Ark at **any** pixel
size — U+8B77 護 and U+90CE 郎 among them, both ordinary characters in
Japanese names. They render as the notdef box. That is a gap upstream,
not one this table can close by regenerating.

Cyrillic is present at this pixel size and was not at 10px, where Ark
ships it only in a variable-height proportional cut. Hangul is still
absent: the Compatibility Jamo block exists, but Hangul Syllables — what
Korean is actually written in — has no glyphs at this size, and jamo alone
cannot compose a title.

Latin Extended-B and Latin Extended Additional (Vietnamese) are
available upstream and excluded here. Add the range to `RANGES` in
`tools/gen_ark12.py` and rerun.

## Regenerating

    ./tools/gen_ark12.py

Standard library only. Do not hand-edit the generated files.

The generator prints a per-range glyph count, which is worth reading: a
range that contributes zero glyphs still produces a clean build, and that
silent failure is the one this script actually has.
