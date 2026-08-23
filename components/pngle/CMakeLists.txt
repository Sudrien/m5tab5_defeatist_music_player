# pngle: streaming PNG decoder, MIT, from https://github.com/kikuchan/pngle
#
# miniz.h is a one-line shim that #includes miniz.c with
# MINIZ_HEADER_FILE_ONLY defined, so miniz.c is both the header and the
# implementation. That is by design upstream; compiling miniz.c once as
# a source here gives the implementation exactly once.
idf_component_register(
    SRCS "pngle.c" "miniz.c"
    INCLUDE_DIRS "."
)

# Upstream miniz trips -Wunused-function and friends on newer GCC.
set_source_files_properties(miniz.c PROPERTIES COMPILE_FLAGS
    "-Wno-unused-function -Wno-unused-parameter -Wno-sign-compare -Wno-attributes")

# pngle.c/pngle.h/miniz.c/miniz.h are fetched by tools/fetch_vendored.sh.
