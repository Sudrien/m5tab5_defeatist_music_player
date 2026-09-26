/*
 * heapmap -- where the memory is, when it runs out (5097).
 *
 * The question this answers is the one 5095's log could not: with the SD
 * card, a USB drive and Wi-Fi up, `USBH: EP Alloc error: ESP_ERR_NO_MEM`
 * and `DMA 15351 free (largest 4096)` while internal free read ~40 KB --
 * so most of the free internal RAM was in regions DMA cannot use, and
 * something ordinary was filling the ones it can.
 *
 * heapmap_init() registers a failed-allocation hook. Every failure logs
 * one line (size, caps, function, task). The first failure, and then at
 * most one every HEAPMAP_GAP_MS, also prints the map:
 *
 *   - free / largest / minimum-ever for internal, DMA-capable internal,
 *     and PSRAM;
 *   - IDF's per-region table for internal RAM (heap_caps_print_heap_info),
 *     which is what tells L2MEM (DMA) from RETENT_RAM and RTCRAM;
 *   - per-task totals, only if CONFIG_HEAP_TASK_TRACKING is enabled. It
 *     is off: it costs bytes on every allocation, in the RAM that is
 *     short. Switch it on in menuconfig for a diagnostic build.
 *
 * The map is printed from the failing task if its stack has room, and
 * otherwise from heapmap_poll(), which ui_task calls every pass.
 * heapmap_log() prints it on demand -- once at the end of boot and once
 * at the first station's first sound, as baselines.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define HEAPMAP_GAP_MS  (30000)

void heapmap_init(void);
void heapmap_log(const char *why);
void heapmap_poll(void);

#ifdef __cplusplus
}
#endif
