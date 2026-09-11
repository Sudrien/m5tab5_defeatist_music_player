/*
 * brightness.h -- one brightness setting, split between the backlight and
 * a filter on the pixels.
 *
 * Measured on hardware: the backlight's bottom is 1% PWM duty. Brightness
 * 5% (1% duty) was where the Sleep page's slider hit the cliff, and 1% is
 * the least the mapping ever wrote. For a screen left on beside a bed
 * that is still too bright, and the backlight cannot go lower.
 *
 * So below the backlight's floor the slider keeps going by darkening the
 * picture instead: the backlight holds at its floor and every pixel is
 * scaled down as it is sent to the panel. One slider, one setting; the
 * split is this file's business.
 *
 * The setting is a position in what is seen. Light output wanted is
 * (level/100)^GAMMA of full. Where that is at or above the backlight
 * floor, it is all backlight. Below it, the backlight stays at the floor
 * and the filter supplies the rest: filter = wanted / floor.
 *
 * Header-only and pure, so texttest compiles this file rather than a copy.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BRIGHTNESS_GAMMA        (2.2)
/* Lowest PWM duty the backlight is driven at, in percent. Measured: the
 * cliff is at 1%. */
#define BRIGHTNESS_FLOOR_PCT    (1)
/* The filter is a multiplier out of 256. RGB565 has 5 bits of red and
 * blue, so below about 8/256 those channels round to black and the
 * picture stops being dim and starts being wrong. */
#define BRIGHTNESS_FILTER_MIN   (8)
#define BRIGHTNESS_FILTER_FULL  (256)

typedef struct {
    int duty_pct;       /* backlight, BRIGHTNESS_FLOOR_PCT..100 */
    int filter;         /* pixels, BRIGHTNESS_FILTER_MIN..256; 256 = untouched */
} brightness_t;

static inline brightness_t brightness_map(int level)
{
    brightness_t b = { 100, BRIGHTNESS_FILTER_FULL };
    if (level >= 100) return b;
    if (level < 0) level = 0;

    const double wanted = 100.0 * pow((double)level / 100.0, BRIGHTNESS_GAMMA);
    if (wanted >= (double)BRIGHTNESS_FLOOR_PCT) {
        b.duty_pct = (int)(wanted + 0.5);
        if (b.duty_pct < BRIGHTNESS_FLOOR_PCT) b.duty_pct = BRIGHTNESS_FLOOR_PCT;
        return b;
    }

    b.duty_pct = BRIGHTNESS_FLOOR_PCT;
    int f = (int)(256.0 * wanted / (double)BRIGHTNESS_FLOOR_PCT + 0.5);
    if (f < BRIGHTNESS_FILTER_MIN) f = BRIGHTNESS_FILTER_MIN;
    if (f > BRIGHTNESS_FILTER_FULL) f = BRIGHTNESS_FILTER_FULL;
    b.filter = f;
    return b;
}

/* One RGB565 pixel scaled by filter/256, channel by channel. */
static inline uint16_t brightness_dim565(uint16_t c, int filter)
{
    if (filter >= BRIGHTNESS_FILTER_FULL) return c;
    if (filter <= 0) return 0;
    const unsigned r = ((c >> 11) & 0x1F) * (unsigned)filter >> 8;
    const unsigned g = ((c >> 5) & 0x3F) * (unsigned)filter >> 8;
    const unsigned b = (c & 0x1F) * (unsigned)filter >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

#ifdef __cplusplus
}
#endif
