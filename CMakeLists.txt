cmake_minimum_required(VERSION 3.16)

# Pull down minimp3 and pngle if they are not here yet. Must run before
# project.cmake, which scans components/ -- pngle registers pngle.c and
# miniz.c as SRCS and those files have to exist by the time it looks.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/vendored.cmake)

# Vendor a patched, exFAT-enabled fatfs into components/ if it is not
# there yet. Must run before project.cmake for the same reason as the
# line above, and one more: IDF collects component Kconfig files during
# that scan, so a fatfs that appears afterwards contributes no CONFIG_*
# symbols and ffconf.h fails to compile against them.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/exfat.cmake)

# Vendor an esp_lcd whose DSI bridge underrun ISR increments a counter
# instead of calling esp_rom_printf. Before project.cmake for the same
# two reasons as the line above: the component scan, and the Kconfig
# collected during it.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/dpi_instrument.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m5tab5_defeatist_music_player)

# esp_driver_sdmmc greets every mount attempt with
#
#   I (nnn) SD_HOST: src_freq_hz: 160000000
#
# via ESP_EARLY_LOGI, the pre-scheduler variant, resolved against
# LOG_LOCAL_LEVEL at compile time -- esp_log_level_set() cannot reach it.
# Raising LOG_LOCAL_LEVEL for that one component compiles it out while
# leaving warnings and errors intact.
idf_component_get_property(sdmmc_lib esp_driver_sdmmc COMPONENT_LIB)
target_compile_definitions(${sdmmc_lib} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_WARN)

# --- the optimisation level, checked rather than assumed ---------------
#
# sdkconfig.defaults asks for COMPILER_OPTIMIZATION_PERF (-O2). It only
# gets it on a tree that has never been built.
#
# sdkconfig.defaults SEEDS a new sdkconfig and never revisits an existing
# one. That is the documented behaviour and it is the right behaviour --
# a defaults file that reached in and overwrote menuconfig choices would
# be unusable -- but it means every symbol added to defaults after the
# first build is silently inert on the machine that did that build. The
# tree looks configured. The comments in sdkconfig.defaults describe a
# build nobody is running.
#
# It is worth catching for this project specifically because the audio
# path has deadlines. The PCM ring absorbs card latency, not decode
# latency: a decoder running at -Og against a 20 s ring is fine until a
# 480 kbps FLAC on a slow drive, and the failure is a stutter that looks
# like an IO problem and is not one. "first sound 1983 ms" and every
# timeout constant in player.c were tuned against -O2.
#
# A warning rather than an error. Deliberately -Og is a real thing to
# want -- stepping through the decode loop in a debugger is hopeless at
# -O2, where half the locals are registers and the line table is a
# suggestion -- and a build system that refuses to produce a debuggable
# binary is worse than one that mentions the trade.
if(NOT CONFIG_COMPILER_OPTIMIZATION_PERF)
    if(CONFIG_COMPILER_OPTIMIZATION_DEBUG)
        set(opt_now "-Og (CONFIG_COMPILER_OPTIMIZATION_DEBUG)")
    elseif(CONFIG_COMPILER_OPTIMIZATION_SIZE)
        set(opt_now "-Os (CONFIG_COMPILER_OPTIMIZATION_SIZE)")
    elseif(CONFIG_COMPILER_OPTIMIZATION_NONE)
        set(opt_now "-O0 (CONFIG_COMPILER_OPTIMIZATION_NONE)")
    else()
        set(opt_now "something other than -O2")
    endif()
    message(WARNING
        "Building at ${opt_now}, but sdkconfig.defaults asks for -O2.\n"
        "sdkconfig.defaults only seeds a NEW sdkconfig, so this and any "
        "other symbol added to it since your first build are being "
        "ignored.\n"
        "To take the defaults:  rm sdkconfig && idf.py build\n"
        "To choose explicitly:  idf.py menuconfig  "
        "(Compiler options -> Optimization Level)\n"
        "Ignore this if you are deliberately building for a debugger.")
endif()
