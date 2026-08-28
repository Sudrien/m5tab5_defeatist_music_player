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
