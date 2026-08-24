# ark10

Ark Pixel Font, 10px monospaced cut, converted to a C bitmap table.

Upstream: https://github.com/TakWolf/ark-pixel-font
Copyright (c) 2021, TakWolf. SIL Open Font License, Version 1.1.

## Licence

**This component is OFL-1.1. The rest of this project is MIT.**

`ark10.c` and `ark10.h` are a format conversion of OFL-licensed Font
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
have to be renamed. The name `ark10` is used anyway, because it is not
the Original Version and should not be mistaken for it.

This is the difference from the `font8x8` component it replaces: that
font was public domain, so nothing had to travel with it.

## Coverage

Basic Latin (U+0020–U+007E), Latin-1 Supplement, Latin Extended-A —
351 glyphs, 5×10 px each. About 3.7 KB of flash.

Five Latin-1 characters are absent because Ark draws them fullwidth
(10 px) and this table is monospaced-halfwidth only: © ® ¼ ½ ¾. They
render as the notdef box. U+00A0 is drawn as a space and U+00AD is
skipped, both handled in `gfx.c`.

Latin Extended-B and Latin Extended Additional (Vietnamese) are
available upstream and excluded here on size grounds. Add the range to
`RANGES` in `tools/gen_ark10.py` and rerun.

## Regenerating

    ./tools/gen_ark10.py

Standard library only. Do not hand-edit the generated files.
