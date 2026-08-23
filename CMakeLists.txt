cmake_minimum_required(VERSION 3.16)

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
