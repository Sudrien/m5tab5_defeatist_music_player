# font8x8: 8x8 monochrome bitmap font, public domain, from
# https://github.com/dhepper/font8x8
#
# Header-only, and the header contains a definition rather than a
# declaration -- so it must be included in exactly one translation unit.
# ui.c is that unit. No SRCS here for the same reason as components/minimp3.
idf_component_register(
    INCLUDE_DIRS "."
)
