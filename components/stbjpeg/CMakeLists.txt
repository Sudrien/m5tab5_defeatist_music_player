# stbjpeg: stb_image (public domain, https://github.com/nothings/stb),
# JPEG only, as the last JPEG decoder. See stbjpeg.h.
#
# stb_image.h is fetched by cmake/vendored.cmake at configure time, pinned
# and hash-checked, and is gitignored like minimp3 and pngle.
idf_component_register(
    SRCS "stbjpeg.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES heap
)

# -fwrapv: a damaged file can overflow signed arithmetic in stb_image's
# IDCT (texttest's corruption sweep found it under UBSan). Defined as
# wrapping, it is garbage pixels rather than undefined behaviour.
# Upstream is -Wall clean on most compilers, not all of these.
target_compile_options(${COMPONENT_LIB} PRIVATE -fwrapv
    -Wno-unused-function -Wno-sign-compare -Wno-unused-but-set-variable
    -Wno-type-limits -Wno-shift-negative-value -Wno-implicit-fallthrough)
