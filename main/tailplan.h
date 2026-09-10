/*
 * tailplan.h -- what a track boundary does with the end of the track
 * leaving, given what the sidecar says that end sounds like.
 *
 * Pure functions of a few numbers, and header-only on purpose: player.c
 * applies them on two tasks with a DMA deadline between them, and none
 * of that can run on a host. What can is the decision, so the decision
 * lives here and texttest/tailplantest.c compiles this file rather than
 * a copy of it.
 *
 * THE RULES
 *
 *   Recorded silence after the audio is cut to TAIL_SILENCE_KEEP_MS.
 *   The decoder simply stops there; nothing past it reaches a ring.
 *
 *   An end in silence the listener would hear as silence -- at least
 *   TAIL_SILENCE_PERCEIVED_MS of it, with no fade before it -- gets no
 *   fade into the next track. The gap is part of the record; the next
 *   track starts at full level when it is over.
 *
 *   An end in a fade that is already in the recording is not faded
 *   again. The outgoing track plays at its own level to the end of its
 *   audio while the incoming track fades in underneath, and that fade
 *   in is half the configured crossfade or the recorded fade's own
 *   length, whichever is shorter. It is placed to FINISH where the
 *   outgoing audio does, so whatever silence was kept after the fade is
 *   dropped by the overlap rather than heard as a gap after it.
 *
 *   Anything else is the crossfade exactly as before.
 *
 * A fade followed by silence is a fade. The silence after a fade is the
 * fade's own bottom, and a hard start after it would undo the reason
 * for respecting the fade.
 *
 * The level match is still applied, with one change: in a fade-in the
 * outgoing track is never attenuated. A step down in the middle of a
 * recorded fade is the artefact this exists to stop. If the incoming
 * track is the louder one it is still brought down to match.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESTIMATES, not measurements. Below a second a gap between songs reads
 * as a pause for breath rather than as a silence; three seconds is the
 * longest a recorded silence is allowed to hold up the next track. */
#define TAIL_SILENCE_PERCEIVED_MS  (1000u)
#define TAIL_SILENCE_KEEP_MS       (3000u)

typedef enum {
    TAIL_PLAIN = 0,     /* unknown, or ends with sound: crossfade as before */
    TAIL_SILENCE,       /* ends in perceived silence: no fade in */
    TAIL_FADE,          /* ends in a recorded fade: fade in only */
} tail_kind_t;

typedef struct {
    tail_kind_t kind;
    uint32_t fade_ms;       /* recorded fade, start to end of audio */
    uint32_t after_ms;      /* silence left after the audio once cut */
    uint32_t cut_ms;        /* stop decoding here; 0 = do not cut */
} tail_t;

/* From the sidecar's fade section. `present` false is an unexamined
 * track and is plain. */
static inline tail_t tail_from_record(bool present, bool has_fade,
                                      uint32_t start_ms, uint32_t audio_end_ms,
                                      uint32_t total_ms)
{
    tail_t t = { TAIL_PLAIN, 0, 0, 0 };
    if (!present || !total_ms || audio_end_ms > total_ms) return t;

    const uint32_t after = total_ms - audio_end_ms;
    if (after > TAIL_SILENCE_KEEP_MS) t.cut_ms = audio_end_ms + TAIL_SILENCE_KEEP_MS;
    t.after_ms = after < TAIL_SILENCE_KEEP_MS ? after : TAIL_SILENCE_KEEP_MS;

    if (has_fade && start_ms < audio_end_ms) {
        t.kind = TAIL_FADE;
        t.fade_ms = audio_end_ms - start_ms;
    } else if (after >= TAIL_SILENCE_PERCEIVED_MS) {
        t.kind = TAIL_SILENCE;
    }
    return t;
}

typedef struct {
    bool     allow;         /* false: plain handoff, no overlap */
    uint32_t overlap_ms;    /* how long the two tracks sound together */
    uint32_t lead_ms;       /* outgoing audio after the overlap, dropped */
    bool     out_unity;     /* outgoing track is not ramped or trimmed */
} tail_xfade_t;

/* `xfade_ms` is the configured crossfade, 0 when it is off. */
static inline tail_xfade_t tail_xfade(tail_kind_t kind, uint32_t fade_ms,
                                      uint32_t after_ms, uint32_t xfade_ms)
{
    tail_xfade_t x = { false, 0, 0, false };
    if (!xfade_ms) return x;

    switch (kind) {
    case TAIL_SILENCE:
        return x;
    case TAIL_FADE: {
        const uint32_t half = xfade_ms / 2;
        x.allow = true;
        x.overlap_ms = fade_ms < half ? fade_ms : half;
        x.lead_ms = after_ms;
        x.out_unity = true;
        return x;
    }
    case TAIL_PLAIN:
    default:
        x.allow = true;
        x.overlap_ms = xfade_ms;
        return x;
    }
}

/*
 * The rate-change dip: the same rules, for the boundary where no overlap
 * is possible. `half_ms` is half the configured crossfade, which is
 * what each half of the dip already was.
 */
typedef struct {
    uint32_t down_ms;
    uint32_t up_ms;
} tail_dip_t;

static inline tail_dip_t tail_dip(tail_kind_t kind, uint32_t fade_ms,
                                  uint32_t half_ms)
{
    tail_dip_t d = { half_ms, half_ms };
    if (kind == TAIL_SILENCE) {
        d.down_ms = 0;
        d.up_ms = 0;
    } else if (kind == TAIL_FADE) {
        d.down_ms = 0;
        if (fade_ms < d.up_ms) d.up_ms = fade_ms;
    }
    return d;
}

#ifdef __cplusplus
}
#endif
