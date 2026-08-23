#!/usr/bin/env sh
#
# Fetch the two header-only libraries this project vendors:
#
#   minimp3  -- MP3 decode (Layers I/II/III, free format, gapless)
#   pngle    -- streaming PNG decode for cover art
#
# Both land under components/ and are meant to be COMMITTED afterwards.
# Same reasoning as tools/enable_exfat.sh: a build input that is fetched
# at build time is a build that breaks differently on someone else's
# machine.
#
# --revert removes them.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MINIMP3="$ROOT/components/minimp3"
PNGLE="$ROOT/components/pngle"

MINIMP3_REF=master
PNGLE_REF=master

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

Done. Now commit components/minimp3/ and components/pngle/.

Pinned to a branch name, not a tag, because neither project tags
releases. If reproducibility matters more than freshness to you, replace
MINIMP3_REF and PNGLE_REF above with commit SHAs.
MSG
