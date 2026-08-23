# minimp3: MIT, from https://github.com/lieff/minimp3
#
# Header-only. minimp3.h and minimp3_ex.h are fetched by
# tools/fetch_vendored.sh and are expected to be committed alongside this
# file -- see the exFAT note in the README for why build inputs get
# committed rather than fetched at build time.
#
# No SRCS: the implementation is emitted into decoder.c by its
# MINIMP3_IMPLEMENTATION define. Registering the component with an empty
# source list is what makes INCLUDE_DIRS propagate.
idf_component_register(
    INCLUDE_DIRS "."
)
