/*
 * brightnesstest.c -- main/brightness.h, compiled from the header: the
 * split between backlight duty and the pixel filter, and the RGB565
 * scaling gfx.c applies at blit time.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>

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
    b = brightness_map(6);
    CHECK(b.duty_pct == 1 && b.filter < brightness_map(8).filter && b.filter >= 8,
          "6, the minimum: %d/%d", b.duty_pct, b.filter);

    printf("light out never rises as the slider goes down\n");
    long prev = 100L * 256;
    for (int l = 100; l >= 0; l--) {
        b = brightness_map(l);
        const long light = (long)b.duty_pct * b.filter;
        CHECK(light <= prev, "level %d: %ld > %ld", l, light, prev);
        prev = light;
    }

    printf("the fade starts where the screen is and ends dark\n");
    {
        const uint32_t floor = 4095u / 100u;        /* 40 counts: 1% */
        for (int level = 6; level <= 100; level++) {
            const brightness_t s = brightness_map(level);
            const uint32_t d0 = 4095u * (uint32_t)s.duty_pct / 100u;
            brightness_raw_t r = brightness_fade_at(d0, s.filter, floor, 0.0);
            const long l0 = (long)d0 * s.filter;
            const long got0 = (long)r.duty * r.filter;
            CHECK(labs(got0 - l0) <= 256, "level %d starts at %ld, wanted %ld", level, got0, l0);

            r = brightness_fade_at(d0, s.filter, floor, 1.0);
            CHECK(r.duty == 0 && r.filter == 0, "level %d does not end off", level);

            /* Light never rises, the backlight is never between off and
             * the floor, and a level already on the floor still fades
             * through many filter values rather than dropping. */
            long prev = l0 + 256;
            int distinct = 0, last_f = -1, bad_mono = 0, bad_floor = 0;
            for (int i = 0; i < 1000; i++) {
                r = brightness_fade_at(d0, s.filter, floor, i / 1000.0);
                const long light = (long)r.duty * r.filter;
                if (light > prev) bad_mono++;
                prev = light;
                if (r.duty != 0 && r.duty < floor) bad_floor++;
                if (r.filter != last_f) { distinct++; last_f = r.filter; }
            }
            CHECK(bad_mono == 0, "level %d: light rose %d times", level, bad_mono);
            CHECK(bad_floor == 0, "level %d: under the floor %d times", level, bad_floor);
            CHECK(distinct >= 20, "level %d: only %d filter steps -- a jump, not a fade", level, distinct);
        }
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
