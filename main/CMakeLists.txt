idf_component_register(
    SRCS "player.c" "decoder.c" "albumart.c" "ui.c" "gfx.c" "touch.c"
         "storage.c" "playlist.c" "browser.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2c esp_driver_i2s esp_driver_sdmmc
             esp_driver_jpeg esp_lcd esp_lcd_touch fatfs sdmmc
             usb usb_host_msc
    PRIV_REQUIRES pngle minimp3 font8x8
)

# minimp3 is a header-only library compiled into decoder.c, so its
# warnings land on our source. Upstream is clean under -Wall but trips
# -Wunused-function when MINIMP3_ONLY_SIMD-style paths are compiled out,
# which they are on RISC-V.
set_source_files_properties(decoder.c PROPERTIES COMPILE_FLAGS
    "-Wno-unused-function")
