Vendored from https://github.com/dhepper/font8x8 (public domain).

`font8x8_basic.h` is not committed. `cmake/vendored.cmake` fetches it at
configure time, pinned to a commit and checked against a SHA256.

ASCII 0-127 only, which is why `ui.c` transliterates anything above 0x7F
to `?` rather than pretending to render it.
