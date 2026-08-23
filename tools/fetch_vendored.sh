#!/usr/bin/env sh
#
# Fetch the two header-only libraries this project vendors:
#
#   minimp3  -- MP3 decode (Layers I/II/III, free format, gapless)
#   pngle    -- streaming PNG decode for cover art
#
# You do not normally need to run this: cmake/vendored.cmake fetches the
# same files, from the same pinned commits, at configure time. This is
# here for fetching them ahead of a build, or on a machine with network
# access so they can be carried to one without.
#
# The files are gitignored, not committed.
#
# --revert removes them.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MINIMP3="$ROOT/components/minimp3"
PNGLE="$ROOT/components/pngle"

# Same pins as cmake/vendored.cmake. If you change one, change both --
# CMake verifies a SHA256 per file and will refetch anything this script
# leaves behind that does not match.
MINIMP3_REF=ea99364f61c14656440e8d77e9c233ccf3124633
PNGLE_REF=b1c68193f1d3f8642b3e0e095d457a828038e6fb

if [ "${1:-}" = "--revert" ]; then
    rm -f "$MINIMP3/minimp3.h" "$MINIMP3/minimp3_ex.h"
    rm -f "$PNGLE/pngle.c" "$PNGLE/pngle.h" "$PNGLE/miniz.c" "$PNGLE/miniz.h"
    echo "removed vendored headers (CMakeLists.txt kept)"
    exit 0
fi

fetch() {
    echo "  $2"
    curl -fsSL "$1" -o "$2"
}

echo "minimp3 -> components/minimp3/"
mkdir -p "$MINIMP3"
BASE="https://raw.githubusercontent.com/lieff/minimp3/$MINIMP3_REF"
fetch "$BASE/minimp3.h"    "$MINIMP3/minimp3.h"
fetch "$BASE/minimp3_ex.h" "$MINIMP3/minimp3_ex.h"

echo "pngle -> components/pngle/"
mkdir -p "$PNGLE"
BASE="https://raw.githubusercontent.com/kikuchan/pngle/$PNGLE_REF/src"
fetch "$BASE/pngle.c" "$PNGLE/pngle.c"
fetch "$BASE/pngle.h" "$PNGLE/pngle.h"
fetch "$BASE/miniz.c" "$PNGLE/miniz.c"
fetch "$BASE/miniz.h" "$PNGLE/miniz.h"

cat <<'MSG'

Done. These files are gitignored -- do not commit them.

Pinned to commits rather than a branch because neither project tags
releases, and because an unpinned minimp3 can reintroduce the symbol
collision that components/minimp3/minimp3_prefix.h exists to fix.
MSG
