/*
 * tailplantest.c -- the boundary rules in main/tailplan.h, compiled from
 * that header rather than transcribed. What player.c does with the
 * answers (the decode cut, the writer's overlap, the dip) runs on the
 * board and is not covered here; the rules themselves are.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "tailplan.h"

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
    tail_t t;
    tail_xfade_t x;
    tail_dip_t d;

    printf("an unexamined track is the crossfade as it always was\n");
    t = tail_from_record(false, false, 0, 0, 0);
    CHECK(t.kind == TAIL_PLAIN && t.cut_ms == 0, "kind %d cut %u", t.kind, t.cut_ms);
    x = tail_xfade(t.kind, t.fade_ms, t.after_ms, 12000);
    CHECK(x.allow && x.overlap_ms == 12000 && !x.out_unity && x.lead_ms == 0, "overlap %u", x.overlap_ms);
    d = tail_dip(t.kind, t.fade_ms, 6000);
    CHECK(d.down_ms == 6000 && d.up_ms == 6000, "dip %u/%u", d.down_ms, d.up_ms);

    printf("a breath of silence is not a silence\n");
    t = tail_from_record(true, false, 0, 239500, 240000);
    CHECK(t.kind == TAIL_PLAIN && t.cut_ms == 0 && t.after_ms == 500, "kind %d after %u", t.kind, t.after_ms);

    printf("perceived silence: kept whole under three seconds, no fade in\n");
    t = tail_from_record(true, false, 0, 238000, 240000);
    CHECK(t.kind == TAIL_SILENCE && t.cut_ms == 0 && t.after_ms == 2000, "kind %d cut %u after %u", t.kind, t.cut_ms, t.after_ms);
    x = tail_xfade(t.kind, t.fade_ms, t.after_ms, 12000);
    CHECK(!x.allow && x.overlap_ms == 0, "allowed %d", x.allow);
    d = tail_dip(t.kind, t.fade_ms, 6000);
    CHECK(d.down_ms == 0 && d.up_ms == 0, "dip %u/%u", d.down_ms, d.up_ms);

    printf("longer silence is cut to three seconds\n");
    t = tail_from_record(true, false, 0, 230000, 240000);
    CHECK(t.kind == TAIL_SILENCE && t.cut_ms == 233000 && t.after_ms == 3000, "cut %u after %u", t.cut_ms, t.after_ms);

    printf("exactly three seconds is not cut\n");
    t = tail_from_record(true, false, 0, 237000, 240000);
    CHECK(t.cut_ms == 0 && t.after_ms == 3000, "cut %u", t.cut_ms);

    printf("a recorded fade: fade in only, half the crossfade at most\n");
    t = tail_from_record(true, true, 230000, 238000, 238000);
    CHECK(t.kind == TAIL_FADE && t.fade_ms == 8000 && t.cut_ms == 0, "kind %d fade %u", t.kind, t.fade_ms);
    x = tail_xfade(t.kind, t.fade_ms, t.after_ms, 12000);
    CHECK(x.allow && x.out_unity && x.overlap_ms == 6000 && x.lead_ms == 0, "overlap %u lead %u", x.overlap_ms, x.lead_ms);
    d = tail_dip(t.kind, t.fade_ms, 6000);
    CHECK(d.down_ms == 0 && d.up_ms == 6000, "dip %u/%u", d.down_ms, d.up_ms);

    printf("a short recorded fade: the fade in is as short as the fade\n");
    t = tail_from_record(true, true, 235000, 238000, 238000);
    x = tail_xfade(t.kind, t.fade_ms, t.after_ms, 12000);
    CHECK(x.overlap_ms == 3000 && x.out_unity, "overlap %u", x.overlap_ms);
    d = tail_dip(t.kind, t.fade_ms, 6000);
    CHECK(d.down_ms == 0 && d.up_ms == 3000, "dip %u/%u", d.down_ms, d.up_ms);

    printf("a fade then silence is a fade; the kept silence is dropped by the overlap\n");
    t = tail_from_record(true, true, 222000, 230000, 240000);
    CHECK(t.kind == TAIL_FADE && t.cut_ms == 233000 && t.after_ms == 3000, "kind %d cut %u after %u", t.kind, t.cut_ms, t.after_ms);
    x = tail_xfade(t.kind, t.fade_ms, t.after_ms, 12000);
    CHECK(x.overlap_ms == 6000 && x.lead_ms == 3000 && x.out_unity, "overlap %u lead %u", x.overlap_ms, x.lead_ms);

    printf("an odd crossfade length halves down, never up\n");
    x = tail_xfade(TAIL_FADE, 8000, 0, 1000);
    CHECK(x.overlap_ms == 500, "overlap %u", x.overlap_ms);

    printf("crossfade off means no overlap and no dip, whatever the ending\n");
    for (int k = TAIL_PLAIN; k <= TAIL_FADE; k++) {
        x = tail_xfade((tail_kind_t)k, 8000, 1000, 0);
        CHECK(!x.allow, "kind %d allowed with crossfade off", k);
        d = tail_dip((tail_kind_t)k, 8000, 0);
        CHECK(d.down_ms == 0 && d.up_ms == 0, "kind %d dip %u/%u", k, d.down_ms, d.up_ms);
    }
    printf("...but silence is still cut with the crossfade off\n");
    t = tail_from_record(true, false, 0, 230000, 240000);
    CHECK(t.cut_ms == 233000, "cut %u", t.cut_ms);

    printf("a damaged record is plain\n");
    t = tail_from_record(true, false, 0, 250000, 240000);
    CHECK(t.kind == TAIL_PLAIN && t.cut_ms == 0 && t.after_ms == 0, "end past total: kind %d cut %u", t.kind, t.cut_ms);
    t = tail_from_record(true, true, 238000, 238000, 238000);
    CHECK(t.kind != TAIL_FADE, "zero-length fade taken as a fade");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
