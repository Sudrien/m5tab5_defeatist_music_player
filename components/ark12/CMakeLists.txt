# ark12: Ark Pixel Font, 12px, as a bitmap table.
#
# Unlike components/font8x8 and components/minimp3, this one has SRCS.
# font8x8 was header-only with the definition in the header, so it had to
# be included in exactly one translation unit; ark12 splits declaration
# from definition, so the table is compiled once here and gfx.c just
# includes the header. That is the ordinary arrangement, and it was
# available here because this file is generated rather than vendored --
# we choose its shape.
#
# The .c and .h are committed, not fetched. See tools/gen_ark12.py for
# why, and components/ark12/README.md for the licence consequence: this
# component is OFL-1.1 while the rest of the project is MIT.
idf_component_register(
    SRCS "ark12.c"
    INCLUDE_DIRS "."
)
