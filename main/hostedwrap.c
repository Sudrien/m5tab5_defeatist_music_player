/*
 * 5092: esp_hosted's per-packet SDIO buffers, from PSRAM.
 *
 * esp_hosted 3.0.8 allocates every packet buffer on the SDIO path -- one
 * per received frame while it waits for the process task, one per frame
 * sent -- with eh_host_port_dma_alloc(1536), which is
 * heap_caps_malloc(n, MALLOC_CAP_DMA): internal RAM. When that fails the
 * log says `mempool OOM start (RX)` / `(TX)`, a received frame is dropped
 * or an outgoing one (ACKs included) is not sent. 5078 moved the frames
 * lwIP holds to PSRAM and 5079 the two staging buffers
 * (EH_HOST_PORT_DMA_PREFER_SPIRAM, which covers only the _aligned
 * allocator), and on Wi-Fi at 512 kbit/s with the SD card and a USB drive
 * mounted it was still dozens of OOM pairs and `DMA 4135 free`. The same
 * station over the USB Ethernet adapter -- no esp_hosted in the path --
 * logged none, with 40 KB of DMA memory free.
 *
 * The link option in main/CMakeLists.txt (--wrap) sends esp_hosted's
 * calls here instead, without changing a line of it. PSRAM first, with
 * the original internal allocation as the fallback:
 *
 *   - RX: in streaming mode the packet buffer is only ever a memcpy
 *     target (the SDIO read lands in a staging buffer), so where it lives
 *     does not matter to the hardware.
 *   - TX: the SDMMC host DMAs from it. The P4's SDMMC reaches PSRAM when
 *     address and length are cache-line multiples (sdmmc_host.c,
 *     sdmmc_host_check_buffer_alignment) and syncs the cache around the
 *     transfer; IDF serves SPIRAM|DMA cache-line aligned and rounds the
 *     size up (heap_align_hw.c), and esp_hosted sends whole 512-byte
 *     blocks from the start of the 1536-byte buffer. Same terms as 5079's
 *     staging buffers, which have run on the board since.
 *
 * The fallback is the original's own body -- heap_caps_malloc(n,
 * MALLOC_CAP_DMA) -- rather than __real_eh_host_port_dma_alloc(), so this
 * file does not reach back into esp_hosted's archive and the link order
 * does not matter. eh_host_port_dma_free() is heap_caps_free(), which
 * takes either.
 */
#include <stdbool.h>
#include <stddef.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "tab5_hosted";

void *__wrap_eh_host_port_dma_alloc(size_t n);   /* for -Wmissing-prototypes */

void *__wrap_eh_host_port_dma_alloc(size_t n)
{
    static bool said_psram, said_fallback;

    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (p) {
        if (!said_psram) {
            said_psram = true;
            ESP_LOGI(TAG, "esp_hosted packet buffers from PSRAM (%u bytes each)",
                     (unsigned)n);
        }
        return p;
    }
    if (!said_fallback) {
        said_fallback = true;
        ESP_LOGW(TAG, "no PSRAM for an esp_hosted packet buffer (%u bytes); "
                      "using internal RAM", (unsigned)n);
    }
    return heap_caps_malloc(n, MALLOC_CAP_DMA);    /* the original */
}
