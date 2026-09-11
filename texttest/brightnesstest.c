/*
 * brightnesstest.c -- main/brightness.h, compiled from the header: the
 * split between backlight duty and the pixel filter, and the RGB565
 * scaling gfx.c applies at blit time.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "brightness.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

int main(void)
{
    printf("the top of the range is all backlight, as before\n");
    brightness_t b = brightness_map(100);
    CHECK(b.duty_pct == 100 && b.filter == 256, "100: %d/%d", b.duty_pct, b.filter);
    b = brightness_map(90);
    CHECK(b.duty_pct == 79 && b.filter == 256, "90 (the default): %d/%d", b.duty_pct, b.filter);
    b = brightness_map(50);
    CHECK(b.duty_pct == 22 && b.filter == 256, "50: %d/%d", b.duty_pct, b.filter);

    printf("the backlight never goes under its floor\n");
    for (int l = -5; l <= 105; l++) {
        b = brightness_map(l);
        CHECK(b.duty_pct >= BRIGHTNESS_FLOOR_PCT && b.duty_pct <= 100, "level %d duty %d", l, b.duty_pct);
        CHECK(b.filter >= BRIGHTNESS_FILTER_MIN && b.filter <= 256, "level %d filter %d", l, b.filter);
        CHECK(b.filter == 256 || b.duty_pct == BRIGHTNESS_FLOOR_PCT,
              "level %d filters with the backlight above its floor (%d%%)", l, b.duty_pct);
    }

    printf("below the floor it keeps getting darker, through the filter\n");
    b = brightness_map(5);
    CHECK(b.duty_pct == 1 && b.filter < 256, "5, the old cliff: %d/%d", b.duty_pct, b.filter);
    b = brightness_map(3);
    CHECK(b.duty_pct == 1 && b.filter < brightness_map(5).filter && b.filter >= 8,
          "3, the new minimum: %d/%d", b.duty_pct, b.filter);

    printf("light out never rises as the slider goes down\n");
    long prev = 100L * 256;
    for (int l = 100; l >= 0; l--) {
        b = brightness_map(l);
        const long light = (long)b.duty_pct * b.filter;
        CHECK(light <= prev, "level %d: %ld > %ld", l, light, prev);
        prev = light;
    }

    printf("RGB565 scaling is per channel and exact at the ends\n");
    CHECK(brightness_dim565(0xFFFF, 256) == 0xFFFF, "full is identity");
    CHECK(brightness_dim565(0x0000, 128) == 0x0000, "black stays black");
    CHECK(brightness_dim565(0xFFFF, 128) == ((15 << 11) | (31 << 5) | 15),
          "white at half: %04x", brightness_dim565(0xFFFF, 128));
    CHECK(brightness_dim565(0xF800, 128) == (15 << 11), "red stays red");
    CHECK(brightness_dim565(0x07E0, 128) == (31 << 5), "green stays green");
    CHECK(brightness_dim565(0x001F, 128) == 15, "blue stays blue");
    CHECK(brightness_dim565(0x1234, 0) == 0, "zero is black");
    {
        int bad = 0;
        for (unsigned c = 0; c < 0x10000; c += 7) {
            for (int f = 8; f <= 256; f += 8) {
                const uint16_t d = brightness_dim565((uint16_t)c, f);
                if (((d >> 11) & 31) > ((c >> 11) & 31) || ((d >> 5) & 63) > ((c >> 5) & 63) ||
                    (d & 31) > (c & 31)) bad++;
            }
        }
        CHECK(bad == 0, "%d pixels got brighter in a channel", bad);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
