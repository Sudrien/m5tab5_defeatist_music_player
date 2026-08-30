# Vendor a copy of IDF's esp_lcd with the DSI bridge underrun ISR counted
# rather than logged, at configure time.
#
# Unconditional, and deliberately so. This is the measurement every
# bandwidth theory about the cyan flash has been assuming the answer to
# for five patches, and a build that has it only when someone remembers
# a flag is a build that will be flashed without it on the run that
# mattered. It costs one counter and one log line a second.
#
# WHY THIS RUNS BEFORE project.cmake
#
# Same reason as exfat.cmake: IDF scans components/ during project.cmake
# and collects Kconfig in the same pass. esp_lcd carries its own Kconfig
# (CONFIG_LCD_*), so a copy appearing after that scan contributes no
# symbols and the vendored source compiles against a config header that
# does not describe it.
#
# It also has to shadow IDF's esp_lcd rather than sit beside it. That is
# what a same-named directory under components/ does, and it is why this
# is a whole-component copy rather than one file added to main.
#
# REMOVING IT
#
# ./tools/instrument_dpi.sh --revert
#
# There is no environment opt-out. If the counter has done its job the
# answer is known and the right move is to revert this and delete both
# files, not to leave a switch behind that quietly changes what the next
# person measures.
#
# SPDX-License-Identifier: MIT

set(_dpi_dest "${CMAKE_CURRENT_LIST_DIR}/../components/esp_lcd")
set(_dpi_script "${CMAKE_CURRENT_LIST_DIR}/../tools/instrument_dpi.sh")

if(EXISTS "${_dpi_dest}")
    # Already vendored. Not re-run and not re-checked: the copy is
    # editable and the whole point of it is that somebody is debugging
    # inside it. Silently reverting that on the next configure would be
    # a genuinely nasty thing to do.
    message(STATUS "dpi: components/esp_lcd present, leaving it alone")
elseif(CMAKE_HOST_WIN32)
    # The script is POSIX sh with a heredoc'd python3. Rather than
    # half-port it, say what is missing -- the result is a directory, and
    # a directory copied in from anywhere works exactly as well.
    message(WARNING
        "dpi: tools/instrument_dpi.sh needs a POSIX shell and is being "
        "skipped on Windows. The DSI underrun counter will not exist and "
        "player.c will report nothing. Run the script under WSL or Git "
        "Bash, or copy a components/esp_lcd from a machine that has, "
        "then reconfigure.")
else()
    find_program(BASH_EXECUTABLE bash)
    if(NOT BASH_EXECUTABLE)
        message(WARNING
            "dpi: no bash found, skipping tools/instrument_dpi.sh. The "
            "DSI underrun counter will not exist.")
    else()
        message(STATUS "dpi: vendoring an instrumented esp_lcd component")

        # WORKING_DIRECTORY because the script addresses everything
        # relative to the project root, as enable_exfat.sh does.
        execute_process(
            COMMAND "${BASH_EXECUTABLE}" "${_dpi_script}" --no-clean
            WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
            RESULT_VARIABLE _dpi_result
            OUTPUT_VARIABLE _dpi_out
            ERROR_VARIABLE _dpi_err)

        if(NOT _dpi_result EQUAL 0)
            # FATAL_ERROR rather than a warning. A half-copied
            # components/esp_lcd shadows IDF's and builds into something
            # worse than either, and the failure would otherwise arrive
            # as a link error against g_tab5_dpi_underruns with no
            # connection to this file.
            message(FATAL_ERROR
                "dpi: tools/instrument_dpi.sh failed.\n"
                "${_dpi_out}${_dpi_err}\n"
                "Delete components/esp_lcd if it was partly created, "
                "then fix the above.")
        endif()
    endif()
endif()
