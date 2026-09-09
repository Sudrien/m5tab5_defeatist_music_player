# Remove esp_hosted's auto-init constructor.
#
# port_esp_hosted_host_init.c does this, unconditionally:
#
#     static void __attribute__((constructor)) esp_hosted_host_init(void)
#     { ESP_ERROR_CHECK(esp_hosted_init()); }
#
# A constructor runs before app_main(). On this board that is wrong twice
# over, and the second reason is the one that matters here.
#
# ORDERING. WLAN_3.3V is a load switch enabled by P0 of the expander at
# 0x44, and nothing writes that bit until app_main() has an I2C handle to
# write it with. So a constructor brings SDIO up against a slave that has
# no power, and ESP_ERROR_CHECK turns that into an abort. The failure is
# a player that does not boot, reported as sdmmc_init_ocr returning
# ESP_ERR_TIMEOUT, which reads like a wiring fault and is not one.
#
# THE SWITCH. settings_wifi_enabled() defaults false and its whole claim
# is that with it off nothing starts the radio -- one bit rather than an
# audit. A constructor runs before settings_init() has read the file, so
# the claim would be false the moment this dependency was added, and
# false in a way no amount of reading wifi.c would reveal. A switch that
# does not switch is worse than no switch.
#
# wifi.c calls esp_hosted_init() itself, after the power-up.
#
# WHY THIS IS A PATCH AND NOT A CONFIG
#
# There is no Kconfig for it. The constructor is unconditional in the
# source.
#
# WHY IT PATCHES managed_components/ AND WHY THAT IS SAFE
#
# Same shape as exfat.cmake and vendored.cmake: a configure-time side
# effect on the source tree, honest about what it is. The component
# manager rewrites managed_components/ whenever a dependency resolves, so
# an edit made by hand silently reverts -- and reverts back into a build
# that aborts before app_main(). Running it from cmake means it cannot be
# forgotten and cannot survive being undone.
#
# WHY IT RUNS AFTER project(), UNLIKE ITS NEIGHBOURS
#
# The other two must run before project.cmake because IDF scans
# components/ there and collects Kconfig in the same pass. This is the
# opposite: managed_components/ does not exist until the component
# manager has resolved dependencies, which happens DURING project.cmake.
# Running before it would find nothing on a fresh clone, patch nothing,
# and leave the constructor in place on exactly the tree least likely to
# notice.
#
# After project() is early enough. The file is compiled at build time,
# not configure time, so editing it during configure is in time for the
# compiler either way.

set(_hosted_ctor
    "${CMAKE_CURRENT_BINARY_DIR}/../managed_components/espressif__esp_hosted/host/port/esp/freertos/src/port_esp_hosted_host_init.c")
get_filename_component(_hosted_ctor "${_hosted_ctor}" ABSOLUTE)

if(NOT EXISTS "${_hosted_ctor}")
    # Not an error. esp_hosted is only pulled in when the registry
    # resolves it, and a tree that has never configured with the network
    # dependency present has nothing to patch. Say so once rather than
    # failing a build that is otherwise fine.
    message(STATUS "esp_hosted: constructor not found, nothing to patch "
                   "(${_hosted_ctor})")
    return()
endif()

file(READ "${_hosted_ctor}" _hosted_src)

if(_hosted_src MATCHES "__attribute__\\(\\(constructor\\)\\) esp_hosted_host_init")
    string(REPLACE
        "static void __attribute__((constructor)) esp_hosted_host_init(void)"
        "static void __attribute__((unused)) esp_hosted_host_init(void)  /* patched by cmake/hosted.cmake: wifi.c calls esp_hosted_init() after powering the C6 */"
        _hosted_src "${_hosted_src}")
    string(REPLACE
        "static void __attribute__((destructor)) esp_hosted_host_deinit(void)"
        "static void __attribute__((unused)) esp_hosted_host_deinit(void)  /* patched by cmake/hosted.cmake: removed with the constructor */"
        _hosted_src "${_hosted_src}")
    file(WRITE "${_hosted_ctor}" "${_hosted_src}")
    message(STATUS "esp_hosted: removed the auto-init constructor")
else()
    # Either already patched, or upstream changed the spelling. Both are
    # quiet, and the second one is a build that aborts before app_main()
    # with no clue as to why -- so it gets a warning rather than silence.
    if(_hosted_src MATCHES "patched by cmake/hosted.cmake")
        message(STATUS "esp_hosted: constructor already removed")
    else()
        message(WARNING
            "esp_hosted: could not find the auto-init constructor to remove.\n"
            "If the build now aborts before app_main() with an sdmmc "
            "timeout, upstream has changed the spelling in\n"
            "  ${_hosted_ctor}\n"
            "and this file needs updating. See its header for why the "
            "constructor cannot stay.")
    endif()
endif()
