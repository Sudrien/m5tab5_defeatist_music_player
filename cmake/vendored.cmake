# Fetch the header-only libraries this project vendors, at configure time.
#
# Both are pinned to a commit, not a branch, and every file is checked
# against a SHA256. Nothing here is committed to the repository, so the
# pin is the only thing that decides what a fresh clone gets -- a branch
# name would mean two clones a month apart build different code, and the
# minimp3 symbol collision this project already had to work around would
# come back for whoever cloned second.
#
# tools/fetch_vendored.sh does the same thing from a shell, for anyone
# who would rather see the files land before building. This runs
# automatically; that one is manual. Neither is required if the files are
# already present.
#
# Downloading during a build is a real cost: it needs the network the
# first time, and it fails on an air-gapped machine. That is why the
# error message below names the script rather than just reporting a
# failed download -- the files can be carried in by any means and the
# build will not care.
#
# SPDX-License-Identifier: MIT

set(MINIMP3_COMMIT ea99364f61c14656440e8d77e9c233ccf3124633)
set(PNGLE_COMMIT   b1c68193f1d3f8642b3e0e095d457a828038e6fb)
set(FONT8X8_COMMIT 8e279d2d864e79128e96188a6b9526cfa3fbfef9)

# vendored_fetch(<url> <destination> <sha256>)
function(vendored_fetch url dest sha)
    get_filename_component(name "${dest}" NAME)

    if(EXISTS "${dest}")
        file(SHA256 "${dest}" have)
        if(have STREQUAL "${sha}")
            return()
        endif()
        # A stale file from an older pin, or a half-written download.
        # Replacing it is safe: it is gitignored, so nothing local is
        # lost, and leaving it would fail the build with a confusing
        # mismatch on every subsequent configure.
        message(STATUS "vendored: ${name} does not match the pin, refetching")
        file(REMOVE "${dest}")
    endif()

    message(STATUS "vendored: fetching ${name}")

    # Downloaded to a temporary name and checked by hand rather than with
    # EXPECTED_HASH. EXPECTED_HASH raises its own FATAL_ERROR the instant
    # the hash differs, which skips everything below -- so the message
    # never appears and, worse, the rejected file is left in place for
    # the next configure to trip over. Verifying afterwards keeps both
    # the cleanup and the explanation.
    file(DOWNLOAD "${url}" "${dest}.tmp" TLS_VERIFY ON STATUS status)

    list(GET status 0 code)
    if(code EQUAL 0)
        file(SHA256 "${dest}.tmp" got)
        if(NOT got STREQUAL "${sha}")
            set(code 1)
            set(reason "SHA256 mismatch: expected ${sha}, got ${got}")
        endif()
    else()
        list(GET status 1 reason)
    endif()

    if(NOT code EQUAL 0)
        file(REMOVE "${dest}.tmp")
        message(FATAL_ERROR
            "Could not fetch ${name}: ${reason}\n"
            "\n"
            "This file is not committed to the repository. If this "
            "machine has no network access, fetch it elsewhere with "
            "./tools/fetch_vendored.sh and copy components/ across; the "
            "build only cares that the files exist and match the pin.\n"
            "\n"
            "A mismatch rather than a network failure means upstream "
            "moved or the pin is wrong. Do not just update the hash: "
            "minimp3 adding an exported symbol is exactly the case "
            "components/minimp3/minimp3_prefix.h has to be updated for.")
    endif()

    file(RENAME "${dest}.tmp" "${dest}")
endfunction()

set(_minimp3_base
    "https://raw.githubusercontent.com/lieff/minimp3/${MINIMP3_COMMIT}")
set(_minimp3_dir "${CMAKE_CURRENT_LIST_DIR}/../components/minimp3")

vendored_fetch("${_minimp3_base}/minimp3.h"
               "${_minimp3_dir}/minimp3.h"
               57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad)
vendored_fetch("${_minimp3_base}/minimp3_ex.h"
               "${_minimp3_dir}/minimp3_ex.h"
               8437f3fc1d4d8ab2269a1624f5380a08df1967a048f8887789bae6e25db7db79)

set(_pngle_base
    "https://raw.githubusercontent.com/kikuchan/pngle/${PNGLE_COMMIT}/src")
set(_pngle_dir "${CMAKE_CURRENT_LIST_DIR}/../components/pngle")

vendored_fetch("${_pngle_base}/pngle.c" "${_pngle_dir}/pngle.c"
               cfd047df6464ac299ecd47aeb13fecfb55f1d841da56dc57085248ef43655049)
vendored_fetch("${_pngle_base}/pngle.h" "${_pngle_dir}/pngle.h"
               d8aa264f80cdd884c02ca1e28b696e8bbfbc16074c9c5265fcec4463f563bb17)
vendored_fetch("${_pngle_base}/miniz.c" "${_pngle_dir}/miniz.c"
               97efe2132abc8346a3d11f29f84b574f5da78a1b94432ea42b0c754c7c39da4e)
vendored_fetch("${_pngle_base}/miniz.h" "${_pngle_dir}/miniz.h"
               8a638511f9f98e8240fc00522989fe7908a06f73a6386191e4374046c993cfa0)

# font8x8, public domain. Vendored rather than hand-written: a bitmap font
# typed from memory is a table of plausible-looking wrong glyphs, and the
# wrongness only shows up as one bad letter in one filename.
vendored_fetch(
    "https://raw.githubusercontent.com/dhepper/font8x8/${FONT8X8_COMMIT}/font8x8_basic.h"
    "${CMAKE_CURRENT_LIST_DIR}/../components/font8x8/font8x8_basic.h"
    49d8df366296b203ca3211bc0672cf2a762135bf12710735b6292756b19dffd5)
