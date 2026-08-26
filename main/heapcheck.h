/*
 * heapcheck.h -- named heap integrity checkpoints.
 *
 * The problem this exists for: a heap corruption is reported by whoever
 * next touches the damaged block, not by whoever damaged it. The
 * player's most-churned PSRAM allocation is the PCM ring -- created and
 * freed once per track -- so it is the block that keeps discovering the
 * damage and the one that keeps appearing in the backtrace. That says
 * nothing about the culprit, and two plausible-sounding theories built
 * on that backtrace were both wrong.
 *
 * So instead of reasoning about it: check the heap at named points and
 * let the first failing checkpoint say which subsystem ran last. Bisect
 * by evidence rather than by argument.
 *
 * Cost when enabled is real -- heap_caps_check_integrity_all() walks
 * every block -- so this is off unless HEAPCHECK is defined, and the
 * checkpoints are placed at track-rate events rather than in any inner
 * loop.
 *
 * Enable with:
 *     idf.py -DHEAPCHECK=1 build
 * or by adding -DHEAPCHECK to the component's compile options.
 *
 * For finding the actual write, pair this with
 * CONFIG_HEAP_POISONING_COMPREHENSIVE in menuconfig
 * (Component config -> Heap memory debugging -> Comprehensive), which
 * catches an overrun at the moment of the write rather than at the next
 * free.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef HEAPCHECK

#include "esp_heap_caps.h"
#include "esp_log.h"

/*
 * Checks every heap and logs the checkpoint name if any is broken.
 *
 * Deliberately does not abort: the point is to see the SEQUENCE of
 * checkpoints -- the last good one and the first bad one -- and aborting
 * on the first failure throws away the half of the information that says
 * what came before it.
 */
#define HEAP_CHECK(name)                                                      \
    do {                                                                      \
        if (!heap_caps_check_integrity_all(true)) {                           \
            ESP_LOGE("heapchk", "HEAP BROKEN at checkpoint: %s", (name));     \
        } else {                                                              \
            ESP_LOGD("heapchk", "ok: %s", (name));                            \
        }                                                                     \
    } while (0)

#else
#define HEAP_CHECK(name)    do { } while (0)
#endif
