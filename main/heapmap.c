/*
 * heapmap -- see heapmap.h (5097).
 */
#include "heapmap.h"

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_HEAP_TASK_TRACKING
#include "esp_heap_task_info.h"
#endif

static const char *TAG = "tab5_heap";

/* Stack the failing task must have left to print the map itself: the
 * per-region table is a printf per region, and printf wants ~1.5 KB. */
#define HEAPMAP_MIN_STACK  (3072)

static volatile bool    s_pending;
static const char      *s_pending_why = "";
static int64_t          s_last_map_us;
static volatile uint32_t s_failures;

static void line(const char *name, uint32_t caps)
{
    multi_heap_info_t i;
    heap_caps_get_info(&i, caps);
    ESP_LOGI(TAG, "  %-8s free %7u  largest %7u  min-ever %7u  allocated %7u  blocks %u",
             name, (unsigned)i.total_free_bytes, (unsigned)i.largest_free_block,
             (unsigned)i.minimum_free_bytes, (unsigned)i.total_allocated_bytes,
             (unsigned)i.allocated_blocks);
}

void heapmap_log(const char *why)
{
    s_last_map_us = esp_timer_get_time();
    s_pending = false;
    ESP_LOGI(TAG, "heap map (%s), %u allocation failures so far:",
             why ? why : "", (unsigned)s_failures);
    line("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    line("DMA", MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    line("PSRAM", MALLOC_CAP_SPIRAM);
    /* IDF's own table, one entry per region: the addresses say which is
     * L2MEM (DMA-capable) and which RETENT_RAM / RTCRAM (not). */
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
#if CONFIG_HEAP_TASK_TRACKING
    heap_caps_print_all_task_stat_overview(stdout);
#endif
}

static void on_alloc_failed(size_t size, uint32_t caps, const char *fn)
{
    if (xPortInIsrContext()) return;            /* nothing safe to do here */
    const uint32_t n = ++s_failures;

    const char *task = pcTaskGetName(NULL);
    /* One line each, up to a burst: the esp_hosted OOM storms before
     * 5092 were dozens a second, and a log of nothing else helps nobody. */
    if (n <= 20 || (n % 100) == 0) {
        ESP_LOGW(TAG, "allocation failed: %u bytes, caps 0x%08x, in %s, task %s (#%u)",
                 (unsigned)size, (unsigned)caps, fn ? fn : "?",
                 task ? task : "?", (unsigned)n);
    }

    const int64_t now = esp_timer_get_time();
    if (s_last_map_us && now - s_last_map_us < (int64_t)HEAPMAP_GAP_MS * 1000) return;
    if (uxTaskGetStackHighWaterMark(NULL) >= HEAPMAP_MIN_STACK) {
        heapmap_log("allocation failed");
    } else {
        s_pending_why = "allocation failed (printed later)";
        s_pending = true;
    }
}

void heapmap_poll(void)
{
    if (!s_pending) return;
    heapmap_log(s_pending_why);
}

void heapmap_init(void)
{
    const esp_err_t err = heap_caps_register_failed_alloc_callback(on_alloc_failed);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no failed-allocation hook: %s", esp_err_to_name(err));
    }
}
