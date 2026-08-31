idf_component_register(
    SRCS "player.c" "decoder.c" "albumart.c" "covertag.c" "mediacache.c" "ui.c" "gfx.c" "touch.c" "audio_out.c"
         "battery.c" "storage.c" "storage_io.c" "usbhost.c" "uac.c" "hid.c" "playlist.c" "settings.c" "browser.c" "panel.c" "duration.c" "waveform.c" "replaygain.c" "loudness.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_app_format esp_driver_i2c esp_driver_i2s esp_driver_sdmmc
             esp_driver_jpeg esp_lcd esp_lcd_touch fatfs sdmmc
             usb usb_host_msc usb_host_uac esp_timer json
    PRIV_REQUIRES pngle minimp3 ark10
)

# Heap integrity checkpoints, off by default. Build with
#   idf.py -DHEAPCHECK=1 build
# to have main/heapcheck.h's HEAP_CHECK() name the last subsystem to run
# before the heap went bad. See its header for why that is the question
# worth asking. Pair with CONFIG_HEAP_POISONING_COMPREHENSIVE to catch
# the write itself rather than the next free.
# The waveform scan, off by default as of 0101. Build with
#   idf.py -DWAVEFORM=1 build
# to put it back. It reads the whole file to draw the seek bar's envelope,
# which on a Xing-less MP3 is the second whole-file read of the same track
# -- decoder_open()'s index build is the first. Off until that duplication
# is resolved; the code is intentionally still compiled, see do_walk().
if(WAVEFORM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WAVEFORM_SCAN=1)
    message(STATUS "waveform envelope scan ENABLED")
endif()

if(HEAPCHECK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HEAPCHECK=1)
    message(STATUS "heap integrity checkpoints ENABLED (slow)")
endif()

# minimp3 is a header-only library compiled into decoder.c, so its
# warnings land on our source. Upstream is clean under -Wall but trips
# -Wunused-function when MINIMP3_ONLY_SIMD-style paths are compiled out,
# which they are on RISC-V.
set_source_files_properties(decoder.c PROPERTIES COMPILE_FLAGS
    "-Wno-unused-function")
