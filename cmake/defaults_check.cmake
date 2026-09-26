# Compare every CONFIG_ line in sdkconfig.defaults with the value the
# build is actually using, and name the ones that differ.
#
# sdkconfig.defaults only seeds a NEW sdkconfig. A key added to it after
# the first build on a machine is inert there until `rm sdkconfig`, and
# nothing said so: 5033 was "not working" for that reason, and 5072's
# 64 KB DMA pool was reported as working (5075) from a board whose boot
# log said `Reserving pool of 32K`. The -O2 check in CMakeLists.txt
# catches one key; this catches all of them.
#
# Include AFTER project(), where the CONFIG_* variables exist. Sets
# SDKCONFIG_DRIFT in the caller's scope to a comma-separated list of the
# keys whose value differs (empty when none) and warns with the details.
#
# Values are compared as sdkconfig.cmake spells them: `y` as y, a bool
# that is off as the empty string (the defaults file writes it `=n`),
# strings without their quotes.
#
# A key the build does not define at all -- renamed upstream, or never
# a symbol -- is warned about separately and kept out of
# SDKCONFIG_DRIFT. `rm sdkconfig` cannot fix that one; only editing the
# defaults can, so it does not belong in a boot line whose advice is
# `rm sdkconfig`, and a boot line that fires on every build would be
# read past.

set(_defaults_file "${CMAKE_CURRENT_LIST_DIR}/../sdkconfig.defaults")
get_filename_component(_defaults_file "${_defaults_file}" ABSOLUTE)

# IDF does not reconfigure when sdkconfig.defaults changes -- only when
# sdkconfig does -- so a patch that edits the defaults would never reach
# this check on an existing build directory. Now it does.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_defaults_file}")

set(SDKCONFIG_DRIFT "")
set(_drift_text "")
set(_unknown_text "")
if(EXISTS "${_defaults_file}")
    file(STRINGS "${_defaults_file}" _lines REGEX "^CONFIG_[A-Za-z0-9_]+=")
    foreach(_line IN LISTS _lines)
        if(NOT _line MATCHES "^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
            continue()
        endif()
        set(_key "${CMAKE_MATCH_1}")
        string(STRIP "${CMAKE_MATCH_2}" _want)
        if(_want MATCHES "^\"(.*)\"$")
            set(_want "${CMAKE_MATCH_1}")
        endif()
        if(_want STREQUAL "n")
            set(_want "")
        endif()

        if(NOT DEFINED ${_key})
            if(NOT _want STREQUAL "")   # asked for off and absent: off
                string(APPEND _unknown_text "  ${_key}=${_want}\n")
            endif()
            continue()
        endif()
        set(_have "${${_key}}")
        if(_have STREQUAL _want)
            continue()
        endif()
        if(_have STREQUAL "")
            set(_have "n")
        endif()
        if(_want STREQUAL "")
            set(_want "n")
        endif()

        if(SDKCONFIG_DRIFT STREQUAL "")
            set(SDKCONFIG_DRIFT "${_key}")
        else()
            set(SDKCONFIG_DRIFT "${SDKCONFIG_DRIFT},${_key}")
        endif()
        string(APPEND _drift_text "  ${_key}: defaults say ${_want}, sdkconfig has ${_have}\n")
    endforeach()
endif()

if(NOT SDKCONFIG_DRIFT STREQUAL "")
    message(WARNING
        "sdkconfig differs from sdkconfig.defaults:\n"
        "${_drift_text}"
        "sdkconfig.defaults only seeds a NEW sdkconfig, so these lines are "
        "not in this build.\n"
        "To take the defaults:  rm sdkconfig && idf.py build\n"
        "The firmware will say the same at boot, under the banner.")
endif()

if(NOT _unknown_text STREQUAL "")
    message(WARNING
        "sdkconfig.defaults sets symbols this build does not have:\n"
        "${_unknown_text}"
        "They do nothing. A component renamed them, or they never existed; "
        "rm sdkconfig will not help. Fix or remove the line.")
endif()
