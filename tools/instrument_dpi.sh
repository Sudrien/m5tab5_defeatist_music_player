#!/usr/bin/env sh
#
# Vendor a copy of IDF's esp_lcd component with the DSI bridge underrun
# ISR counted rather than logged.
#
# WHY
#
# esp_lcd/dsi/esp_lcd_panel_dpi.c raises
#
#     can't fetch data from external memory fast enough, underrun happens
#
# from its bridge ISR when the DPI peripheral cannot pull a line out of
# PSRAM in time. Every bandwidth theory about the cyan flash rests on
# that line, and it has never once appeared in a captured log -- which
# could mean the underruns are not happening, or could mean the ISR's
# esp_rom_printf is not reaching the console this device is monitored
# over. Those two readings point in opposite directions and no patch
# should be written until they are told apart.
#
# A printf in an ISR that may fire at frame rate is also its own hazard:
# it can only ever be lossy, and the loss is worst exactly when the
# thing being measured is worst. So this replaces the log with a plain
# DRAM counter that a task reads at its leisure.
#
# WHAT IT DOES
#
#   components/esp_lcd/          copy of $IDF_PATH/components/esp_lcd
#   dsi/esp_lcd_panel_dpi.c      the ESP_DRAM_LOGE call becomes an
#                                increment of g_tab5_dpi_underruns
#
# player.c declares that symbol extern and prints it from ui_task once a
# second when it moves. A run with no flashes reads zero; a flash with a
# zero counter means the flash is not a bridge underrun and the whole
# bandwidth line of attack is wrong.
#
# The directory is gitignored, generated and disposable, exactly like
# components/fatfs. --revert removes it.
#
# SPDX-License-Identifier: MIT

set -eu

DEST="components/esp_lcd"

# See the same flag in enable_exfat.sh: right for a human running this
# against a built tree, catastrophic for cmake, which is executing from
# inside build/.
CLEAN=1
if [ "${1:-}" = "--no-clean" ]; then
    CLEAN=0
    shift || true
fi

if [ "${1:-}" = "--revert" ]; then
    rm -rf "$DEST" build sdkconfig
    echo "removed $DEST, build/ and sdkconfig; back to stock esp_lcd"
    echo "now run: idf.py build flash monitor"
    exit 0
fi

if [ -z "${IDF_PATH:-}" ]; then
    echo "IDF_PATH is not set. Source export.sh first." >&2
    exit 1
fi

SRC="$IDF_PATH/components/esp_lcd"
if [ ! -d "$SRC" ]; then
    echo "no esp_lcd component at $SRC" >&2
    exit 1
fi

# Already vendored. An error for a human, who asked for this and should
# be told it was a no-op; a silent success for cmake, which asks on every
# configure and would otherwise fail every build after the first.
if [ -e "$DEST" ]; then
    if [ "$CLEAN" = "0" ]; then
        exit 0
    fi
    echo "$DEST already exists; delete it or run --revert first" >&2
    exit 1
fi

mkdir -p components
cp -r "$SRC" "$DEST"
chmod -R u+w "$DEST"

DPI="$DEST/dsi/esp_lcd_panel_dpi.c"
if [ ! -f "$DPI" ]; then
    echo "expected $DPI; IDF layout may have changed" >&2
    exit 1
fi

# The ISR line this whole exercise is about. Matched on the message text
# rather than on ESP_DRAM_LOGE, because that macro is used elsewhere in
# the file and the message is what identifies this one call site.
if ! grep -q "underrun happens" "$DPI"; then
    echo "error: no underrun log call in $DPI; IDF layout may have changed" >&2
    exit 1
fi

# The counter itself. DRAM_ATTR because it is written from an ISR that
# may run with the cache disabled, and non-static because player.c reads
# it. Inserted after the last #include so it lands at file scope, ahead
# of every function.
python3 - "$DPI" <<'PYEOF'
import re, sys

path = sys.argv[1]
text = open(path).read()

decl = (
    '\n'
    '/* tools/instrument_dpi.sh: DSI bridge underruns, counted rather than\n'
    ' * logged. Read by player.c. DRAM_ATTR because the ISR below can run\n'
    ' * with the cache disabled. */\n'
    '#include "esp_attr.h"\n'
    'DRAM_ATTR uint32_t g_tab5_dpi_underruns;\n'
)

# After the final #include of the file's include block.
includes = list(re.finditer(r'^#include[^\n]*\n', text, re.MULTILINE))
if not includes:
    sys.exit('error: no #include lines in %s' % path)
at = includes[-1].end()
text = text[:at] + decl + text[at:]

# The log call becomes an increment. Matched from ESP_DRAM_LOGE up to the
# semicolon so a wrapped call is replaced whole rather than leaving its
# tail behind as a syntax error.
#
# Every call site, not exactly one. IDF is free to log this from more
# than one place and the count is not the interesting fact -- what
# matters is that none is left.
call = re.compile(r'ESP_DRAM_LOGE\s*\([^;]*underrun happens[^;]*\);', re.DOTALL)
text, n = call.subn('g_tab5_dpi_underruns++;', text)
if n < 1:
    sys.exit('error: found no underrun log call to replace')

# And the verification, here rather than in a grep outside.
#
# The first version of this script grepped the file for "underrun
# happens" afterwards and failed if it appeared at all. It appears in a
# comment as well as in the call, so a substitution that had worked
# perfectly was reported as a failure -- in CI, at the end of a build,
# with "counted 1 underrun call site" printed immediately above the
# error saying it had not worked.
#
# The check has to ask the same question the substitution did.
if call.search(text):
    sys.exit('error: an underrun log call survived the substitution')

open(path, 'w').write(text)
print('counted %d underrun call site' % n)
PYEOF

# Prove it took. A sed or a regex that matches nothing exits 0, and a
# failed patch would otherwise look exactly like a successful one -- with
# the symptom being a counter that reads zero forever, which is also the
# most interesting possible result. Nothing would be more misleading.
#
# This is the increment only. Whether a log call survived is checked in
# the python above, against the same pattern that did the replacing --
# a plain grep for the message text matches the comment beside the call
# as well, which failed a build over a substitution that had worked.
if ! grep -q "g_tab5_dpi_underruns++" "$DPI"; then
    echo "error: the counter increment is not in $DPI" >&2
    exit 1
fi

echo "patched $DPI:"
grep -n "g_tab5_dpi_underruns" "$DPI" | sed 's/^/  /'
echo

if [ "$CLEAN" = "1" ]; then
    rm -rf build sdkconfig
    echo "removed build/ and sdkconfig so Kconfig is regenerated"
    echo
    echo "now run: idf.py build flash monitor"
fi
