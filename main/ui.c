/*
 * ui.c -- the transport bar.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "gfx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui.h"
#include "waveform.h"

static const char *TAG = "tab5_ui";

/* RGB() and every drawing primitive now live in gfx.c, so the chooser can
 * use the same ones. Nothing here changed except the names. */

#define C_BG        RGB(0x11, 0x11, 0x11)
#define C_TRACK     RGB(0x3A, 0x3A, 0x3A)   /* unplayed / unfilled */
#define C_FILL      RGB(0xD1, 0x3B, 0x2C)   /* played / set volume */
#define C_THUMB     RGB(0xFF, 0xFF, 0xFF)
#define C_ICON      RGB(0xCC, 0xCC, 0xCC)
/* Album row. Dimmer than artist, but its own value rather than reusing
 * C_TRACK -- that is 0x3A, chosen to be a slider groove that does not
 * compete with the fill, and it is too dark to read as text. */
#define C_ALBUM     RGB(0x88, 0x88, 0x88)

/* A disabled icon. Dark enough to read as off at arm's length, light
 * enough not to look like a rendering fault -- the same distance below
 * C_ICON that C_ALBUM sits below C_THUMB, so the bar has one idea of
 * what "de-emphasised" means rather than two. */
#define C_ICON_OFF  RGB(0x55, 0x55, 0x55)

/*
 * ReplayGain. Yellow because nothing else on the bar is: C_FILL owns
 * "the level you set" and C_ICON owns "a control", so a gain that is
 * neither needs a third idea rather than a shade of one of theirs.
 */
#define C_RG        RGB(0xE8, 0xC0, 0x30)
/* The envelope's two halves.
 *
 * Played reuses C_FILL exactly -- it is the same statement the slider fill
 * was making, in the shape of the song instead of a rectangle. Unplayed is
 * its own grey rather than C_TRACK: 0x3A was chosen to be a groove that
 * disappears behind the fill, and a 64 px shape drawn in it reads as a
 * smudge. 0x6E is the dimmest grey that still resolves as a waveform at
 * arm's length on this panel.
 */
#define C_WAVE_PAST   C_FILL
#define C_WAVE_FUTURE RGB(0x6E, 0x6E, 0x6E)
/* The playhead, where the two meet. The colour boundary alone marks the
 * position, but only where the envelope is tall; across a quiet passage it
 * is a 5 px change of colour, so there is a line as well. */
#define C_PLAYHEAD  RGB(0xFF, 0xFF, 0xFF)


/* Two thicknesses, and the asymmetry is the point: the filled part is
 * thick so progress reads from arm's length, the remainder is thin so it
 * does not compete with it. */
#define TRACK_THIN  (4)
#define TRACK_THICK (13)
#define THUMB_R     (16)

/* Drawn geometry, relative to the top of the bar. */
/*
 * Sizes here are set for a 5" 720x1280 panel -- about 294 PPI, where an
 * 8 px font glyph is 1.4 mm tall and unreadable at arm's length. Scale 5
 * puts the title at roughly 3.5 mm, which is about a phone's body text.
 * Everything else is sized to match rather than left at the values that
 * looked fine in a mockup rendered at 96 PPI.
 */
/*
 * Eight rows, top to bottom, each one thing:
 *
 *   1  the cover, 720x720, above the bar entirely
 *   2  the seek bar -- the envelope, full panel width
 *   3  elapsed left, remaining right
 *   4  title
 *   5  album
 *   6  artist
 *   7  folder | prev, play/pause, next | sleep
 *   8  volume
 *
 * The previous layout stacked title, artist and album at the top of the
 * bar and then put the seek bar under them, which meant the two things
 * that change while a track plays -- the position and the clock -- were
 * the two furthest from the artwork they belong to. Reading down is now
 * reading outward: what is playing, where in it, what it is called, and
 * then the controls, which are the only rows a finger goes near.
 *
 * The clocks moved to a row of their own because the envelope took the
 * full width. They used to flank it, which is what SEEK_X0 was for: 142 px
 * of margin at each end so the MM:SS runs had somewhere to sit. That was
 * 284 px of the panel spent on two five-character numbers, taken out of
 * the middle of the one element that wants width.
 *
 * Sizes are for a 5" 720x1280 panel -- about 294 PPI, where an 8 px font
 * glyph is 1.4 mm tall and unreadable at arm's length. Scale 4 puts the
 * title at roughly 2.8 mm, about a phone's body text, and fits 19
 * characters across the panel -- which is why the title bounces.
 */
#define SEEK_Y      (96)    /* row 2: the envelope's baseline */
#define SEEK_X0     (0)     /* full width, edge to edge */
#define TIME_Y      (112)   /* row 3 */
#define TIME_PAD    (16)    /* clocks in from each edge */
#define TITLE_Y     (186)   /* row 4 */
#define ALBUM_Y     (238)   /* row 5 */
#define ARTIST_Y    (278)   /* row 6 */
#define ROW_Y       (380)   /* row 7, centres */
#define VOL_Y       (490)   /* row 8 */
#define TEXT_X      (16)

#define BTN_R       (46)    /* play/pause circle */
#define ICON_HALF   (26)
#define SKIP_HALF   (35)    /* prev/next: 26 px triangle plus a 7 px bar */

/* Hit targets are padded well beyond the drawn shapes. A 22 px slider on
 * a 5" panel is a small thing to hit with a thumb, and there is nothing
 * adjacent to steal the press from -- the same reasoning as the map
 * project's BTN_PAD_TOP. */
#define HIT_PAD_Y   (30)
#define HIT_PAD_X   (14)

/*
 * There is no finger bubble any more, and this is where it was.
 *
 * It was a 128 px disc raised above the finger during a drag, showing the
 * value being set: MM:SS for seek, a percentage for volume. The argument
 * for it was that the thing being adjusted should not sit under the hand.
 *
 * That argument was answered better by a row that already existed. What a
 * seek drag adjusts is a position, and row 3 shows positions -- in
 * seven-segment digits at 20x38, at the two ends of the panel, in the
 * place the eye is already going for that number. The bubble was a second
 * and smaller rendering of the same value somewhere worse. Volume needs
 * no readout at all: the slider's own fill is the level, and volume
 * applies live during the drag, so the feedback is in the ears.
 *
 * What it cost was out of proportion to that. The bubble was the only
 * thing ui_task ever drew above s_bar_top, and everything awkward here
 * followed from it:
 *
 *   - It reached onto the cover art, which is not cleared each frame, so
 *     erasing it needed a saved strip -- s_bubble_bg, captured by
 *     ui_capture_background() from five call sites across three tasks,
 *     freed and reallocated by media_task while ui_task memcpy'd 190 KB
 *     out of it. A shared pointer with no owner, which is the one thing
 *     this project has a rule against.
 *   - It forced a second gfx_blit() per drag poll, plus two 190 KB
 *     memcpys, at 50 Hz. Around 40 MB/s of PSRAM bandwidth on a bus the
 *     DPI peripheral is already reading flat out.
 *   - It made "the two writers own disjoint bands" false. It is true now:
 *     media_task owns rows 0..UI_ART_H-1 and ui_task owns the rest, and
 *     the only thing they contend for is the transfer.
 *
 * BUBBLE_ABOVE had to exceed SEEK_Y or the bubble overlapped the bar, and
 * row 7's spacing had to keep the padded hit boxes apart. The second
 * constraint is still real. The first is gone with the thing it
 * constrained; do not reintroduce it by putting something else up there.
 */
static uint16_t *s_fb;
static int s_w, s_h;
static int s_bar_top;

/* Live drag state. -1 = nothing being dragged. */
static int s_drag = -1;         /* 0 = seek, 1 = volume */
/* s_drag_x is still tracked: draw_slider_c() and the envelope both need
 * the finger's x to show where the value is. Only the bubble wanted it in
 * order to follow the hand. */
static int s_drag_x, s_drag_y;
static int s_drag_pct;
static bool s_was_down;

/*
 * When the previous-track button was last tapped, for double-tap
 * detection. 0 means "no tap pending a partner".
 *
 * 400 ms: comfortably longer than two deliberate taps take, and shorter
 * than the gap between two separate decisions to go back one track.
 */
const char *ui_action_name(ui_action_kind_t k)
{
    switch (k) {
    case UI_ACTION_NONE:        return "none";
    case UI_ACTION_PLAY_PAUSE:  return "play/pause";
    case UI_ACTION_CHOOSE_FILE: return "folder";
    case UI_ACTION_SETTINGS:    return "gear (settings)";
    case UI_ACTION_SCREEN_OFF:  return "moon (screen off)";
    case UI_ACTION_SCREEN_ON:   return "wake";
    case UI_ACTION_PREV:        return "prev";
    case UI_ACTION_PREV_AGAIN:  return "prev x2";
    case UI_ACTION_NEXT:        return "next";
    case UI_ACTION_SEEK:        return "seek";
    case UI_ACTION_VOLUME:      return "volume";
    case UI_ACTION_MUTE:        return "mute";
    }
    return "?";
}

#define DOUBLE_TAP_MS   (400)
static TickType_t s_prev_tick;

/*
 * Marquee state for the title.
 *
 * s_marq_title is compared by pointer, not by strcmp: the player hands
 * the UI a pointer into its own tag buffer, which is rewritten in place
 * between tracks, so the string can change without the pointer changing.
 * Length is checked alongside it for exactly that case. Both are cheap
 * and neither is reliable alone.
 */
static const char *s_marq_title;
static int  s_marq_len;
static int  s_marq_off;         /* pixels the string is shifted left */
static int  s_marq_dir = 1;
static int  s_marq_hold;
static bool s_marq_active;

/*
 * Bounce, rather than wrap.
 *
 * A wrapping marquee needs the string drawn twice with a separator and it
 * never shows the beginning and end together; a bounce shows the head,
 * travels, shows the tail, and comes back. On a title -- where the front
 * is usually the part that identifies the song and the back is usually
 * "(Remastered 2011)" -- the head is worth returning to.
 *
 * Advanced from ui_draw() rather than from a timer, so it moves at
 * whatever rate the bar is being repainted. ui_animating() is what makes
 * that rate 25 Hz instead of 10.
 */
#define MARQ_STEP   (3)
#define MARQ_HOLD   (14)        /* frames paused at each end */

static void marquee_step(const char *title, int over)
{
    if (over <= 0) {
        s_marq_active = false;
        s_marq_off = 0;
        return;
    }
    s_marq_active = true;

    if (s_marq_hold > 0) { s_marq_hold--; return; }

    s_marq_off += MARQ_STEP * s_marq_dir;
    if (s_marq_off >= over) {
        s_marq_off = over;
        s_marq_dir = -1;
        s_marq_hold = MARQ_HOLD;
    } else if (s_marq_off <= 0) {
        s_marq_off = 0;
        s_marq_dir = 1;
        s_marq_hold = MARQ_HOLD;
    }
    (void)title;
}

bool ui_animating(void)
{
    return s_marq_active;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/* Seek runs nearly the full width; volume sits between the folder icon
 * and the play button. Returned in absolute screen coordinates. */
/* Row 2: the envelope, edge to edge. Nothing shares the row now, which
 * is what the clocks moving to row 3 bought. */
static void seek_bounds(int *x0, int *x1, int *y)
{
    *x0 = SEEK_X0;
    *x1 = s_w - SEEK_X0;
    *y  = s_bar_top + SEEK_Y;
}

/* Row 8. The speaker sits in the left margin, so the groove starts clear
 * of it and stops the same distance from the right edge -- an asymmetric
 * slider reads as a mistake even when the icon explains it. */
static void vol_bounds(int *x0, int *x1, int *y)
{
    *x0 = 96;
    *x1 = s_w - 96;
    *y  = s_bar_top + VOL_Y;
}

/*
 * Row 7, five controls in three groups: the file chooser at the left
 * edge, transport in the middle, sleep at the right.
 *
 * The centres are spaced so the padded hit boxes do not touch. Play is
 * BTN_R + HIT_PAD_X = 60 either side; prev and next are 38. At 248, 360
 * and 472 the gaps are 300-288 and 420-434, which is the margin the
 * ordering in ui_touch() no longer has to provide. Boxes that overlap and
 * are disambiguated by test order work until the order changes.
 */
static void play_centre(int *cx, int *cy)
{
    *cx = s_w / 2;
    *cy = s_bar_top + ROW_Y;
}

static void prev_centre(int *cx, int *cy)
{
    *cx = s_w / 2 - 112;
    *cy = s_bar_top + ROW_Y;
}

static void next_centre(int *cx, int *cy)
{
    *cx = s_w / 2 + 112;
    *cy = s_bar_top + ROW_Y;
}

static void folder_centre(int *cx, int *cy)
{
    *cx = 64;
    *cy = s_bar_top + ROW_Y;
}

/*
 * The gear, in the gap between the folder and prev.
 *
 * 156 rather than anywhere else because row 7's boxes must not touch and
 * this is the only slack left in it. The folder's padded box ends at
 * 64 + ICON_HALF + HIT_PAD_X = 104; prev's starts at 360 - 112 - SKIP_HALF
 * - HIT_PAD_X = 199. A gear at 156 spans 116..196, which clears both.
 *
 * The right-hand side has no such gap: next ends at 521 and the moon
 * starts at 616, so a sixth control over there would have had to move
 * the transport, and the transport is where every finger already goes.
 */
static void gear_centre(int *cx, int *cy)
{
    *cx = 156;
    *cy = s_bar_top + ROW_Y;
}

static void moon_centre(int *cx, int *cy)
{
    *cx = s_w - 64;
    *cy = s_bar_top + ROW_Y;
}

/* ------------------------------------------------------------------ */
/* Widgets                                                             */
/* ------------------------------------------------------------------ */

static void draw_slider_c(int x0, int x1, int y, int pct, uint16_t fill)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const int w = x1 - x0;
    const int split = x0 + (w * pct) / 100;

    /* Thin remainder first, then the thick fill over its left end, so the
     * two never disagree by a pixel at the join. */
    gfx_fill_rect(x0, y - TRACK_THIN / 2, w, TRACK_THIN, C_TRACK);
    gfx_fill_rect(x0, y - TRACK_THICK / 2, split - x0, TRACK_THICK, fill);
    gfx_fill_circle(split, y, THUMB_R, C_THUMB);
}

static void draw_play_pause(bool playing)
{
    int cx, cy;
    play_centre(&cx, &cy);
    gfx_fill_circle(cx, cy, BTN_R, C_THUMB);

    if (playing) {
        /* Pause: two bars. */
        gfx_fill_rect(cx - 15, cy - 20, 10, 40, C_BG);
        gfx_fill_rect(cx + 5, cy - 20, 10, 40, C_BG);
    } else {
        /* Play: a triangle, nudged right so it looks centred rather than
         * measuring centred. */
        for (int dy = -20; dy <= 20; dy++) {
            const int a = dy < 0 ? -dy : dy;
            gfx_fill_rect(cx - 11, cy + dy, 34 - (a * 34) / 20, 1, C_BG);
        }
    }
}

/*
 * Prev and next: a triangle with a bar on the leading side.
 *
 * Drawn rather than glyphs because font8x8 has no transport symbols and
 * an ASCII "|<" at scale 3 is two characters that read as punctuation.
 * The bar is what distinguishes them from the play triangle at a glance,
 * which matters when all three sit in a row 112 px apart.
 */
static void draw_skip(int cx, int cy, bool forward, bool enabled)
{
    const int h = 22;               /* half-height of the triangle */
    const int w = 26;               /* base to apex */
    const uint16_t c = enabled ? C_ICON : C_ICON_OFF;

    for (int dy = -h; dy <= h; dy++) {
        const int a = dy < 0 ? -dy : dy;
        const int run = w - (a * w) / h;
        if (run <= 0) continue;
        if (forward) gfx_fill_rect(cx - w, cy + dy, run, 1, c);
        else         gfx_fill_rect(cx + w - run, cy + dy, run, 1, c);
    }

    /* The bar goes just past the apex -- the wall the tape stops against,
     * which is the convention every transport since a cassette deck has
     * used. Past the apex and not behind the base: behind the base it
     * reads as an underline on an arrow. */
    if (forward) gfx_fill_rect(cx + 2, cy - h, 7, 2 * h + 1, c);
    else         gfx_fill_rect(cx - 9, cy - h, 7, 2 * h + 1, c);
}

static void draw_folder(void)
{
    int cx, cy;
    folder_centre(&cx, &cy);
    gfx_fill_rect(cx - ICON_HALF, cy - 18, 21, 7, C_ICON);          /* tab */
    gfx_fill_rect(cx - ICON_HALF, cy - 12, 2 * ICON_HALF, 32, C_ICON);
    gfx_fill_rect(cx - ICON_HALF + 4, cy - 7, 2 * ICON_HALF - 8, 23, C_BG);
}

/*
 * A gear: a disc with a hole, and eight teeth around it.
 *
 * Drawn from rectangles like everything else on this bar -- there is no
 * path renderer and does not need to be. The teeth are four rectangles
 * (two of them crossing at the diagonals would need rotation, which
 * gfx.c has no notion of), so the diagonals are drawn as short stepped
 * blocks instead. At 294 PPI the result reads as a gear at arm's length,
 * which is the whole requirement for an icon.
 */
static void draw_gear(void)
{
    int cx, cy;
    gear_centre(&cx, &cy);

    const int r = ICON_HALF - 4;        /* body */
    const int t = 8;                    /* tooth half-width */
    const int o = ICON_HALF;            /* tooth outer reach */

    /* Four square teeth on the axes. */
    gfx_fill_rect(cx - t, cy - o, 2 * t, o - r + 6, C_ICON);
    gfx_fill_rect(cx - t, cy + r - 6, 2 * t, o - r + 6, C_ICON);
    gfx_fill_rect(cx - o, cy - t, o - r + 6, 2 * t, C_ICON);
    gfx_fill_rect(cx + r - 6, cy - t, o - r + 6, 2 * t, C_ICON);

    /* Four on the diagonals, as single blocks set out at 45 degrees.
     * A rotated rectangle would be nicer and would need a rasteriser. */
    const int d = (r * 7) / 10;         /* r / sqrt(2), near enough */
    gfx_fill_rect(cx + d - 6, cy - d - 6, 13, 13, C_ICON);
    gfx_fill_rect(cx - d - 7, cy - d - 6, 13, 13, C_ICON);
    gfx_fill_rect(cx + d - 6, cy + d - 7, 13, 13, C_ICON);
    gfx_fill_rect(cx - d - 7, cy + d - 7, 13, 13, C_ICON);

    gfx_fill_circle(cx, cy, r, C_ICON);
    gfx_fill_circle(cx, cy, 8, C_BG);   /* the hole */
}

static void draw_moon(void)
{
    int cx, cy;
    moon_centre(&cx, &cy);
    gfx_fill_circle(cx, cy, ICON_HALF, C_ICON);
    gfx_fill_circle(cx + 13, cy - 10, ICON_HALF, C_BG); /* bite out the crescent */
}

/* A speaker is a small rectangle (the body) with a cone flaring out to the
 * right. The first version drew the cone as a triangle whose height shrank
 * with x, which is an arrow pointing right -- the flare has to grow with
 * x, not shrink. */
/*
 * Row 8's left margin. The icon was decoration; it is a button now.
 *
 * SPK_HALF is 26 and the test below adds no padding, so the box is
 * 28..80 and the volume slider's padded box starts at 82. They do not
 * touch, which is the rule row 7 already follows: boxes that overlap and
 * are disambiguated by test order work right up until the order changes.
 * A 52 px target is 4.5 mm at 294 PPI, which is smaller than the
 * transport buttons and larger than a fingertip needs for an icon that
 * sits alone in a margin.
 */
#define SPK_HALF    (26)

static void spk_centre(int *cx, int *cy)
{
    int x0, x1, y;
    vol_bounds(&x0, &x1, &y);
    *cx = x0 - 42;
    *cy = y;
}

/*
 * Muted draws a slash through the cone, not a greyed icon.
 *
 * Grey is what this file uses for "this button does nothing" -- next at
 * the end of a folder, the battery outline with no reading. Mute is the
 * opposite: the control is working and is the reason there is no sound.
 * A greyed speaker would say the mute button is unavailable.
 */
static void draw_speaker(bool muted)
{
    int cx, cy;
    spk_centre(&cx, &cy);
    const uint16_t c = muted ? C_FILL : C_ICON;

    gfx_fill_rect(cx - 13, cy - 6, 9, 13, c);       /* body */
    for (int dx = 0; dx <= 15; dx++) {              /* cone, flaring right */
        const int half = 4 + dx;
        gfx_fill_rect(cx - 4 + dx, cy - half, 1, 2 * half + 1, c);
    }

    if (!muted) return;

    /* A 3 px diagonal, drawn as one short run per row so it needs no
     * line primitive -- the same trick draw_skip() uses for its
     * triangles. */
    for (int i = -20; i <= 20; i++) {
        gfx_fill_rect(cx + i - 1, cy - i - 1, 3, 3, C_FILL);
    }
}

/*
 * "RG" under the speaker, and a mark on the slider for the offset.
 *
 * Two parts because they answer two questions. The badge says a
 * measured gain is being applied at all, which is otherwise invisible
 * -- a track playing 4 dB down with the slider untouched looks like a
 * quiet file or a fault. The mark says how much and which way, placed
 * where the thumb WOULD be if the gain were part of the slider, so the
 * gap between the mark and the thumb is the adjustment drawn to the
 * same scale as the control it modifies.
 *
 * The mark is not a second thumb and is deliberately not round: it is
 * not draggable, and a thing that looks like the thumb invites a drag
 * that would do nothing.
 *
 * The dB scale here is the slider's own, which is linear in percent and
 * therefore linear in amplitude (see the note on apply_gain() in
 * audio_out.c). A gain in dB has to be converted to the same units
 * before it can be drawn beside it, or the mark would be right only at
 * one volume.
 */
static void draw_rg(const ui_state_t *st)
{
    if (!st->rg_active) return;

    int cx, cy;
    spk_centre(&cx, &cy);

    /* Under the speaker, in the margin it already owns. Scale 2 is the
     * smallest the font stays legible at arm's length. */
    gfx_draw_text(cx - 17, cy + 20, "RG", 2, 40, C_RG);

    if (st->rg_gain_db == 0.0f) return;

    int x0, x1, y;
    vol_bounds(&x0, &x1, &y);

    /* Where the slider sits now, and where it would sit with the gain
     * folded in. Amplitude ratio, because that is the slider's curve. */
    const int base = st->volume;
    float scaled = (float)base * powf(10.0f, st->rg_gain_db / 20.0f);
    if (scaled < 0.0f) scaled = 0.0f;
    if (scaled > 100.0f) scaled = 100.0f;

    const int w = x1 - x0;
    const int mark = x0 + (int)(((float)w * scaled) / 100.0f);
    const int here = x0 + (w * (base < 0 ? 0 : base > 100 ? 100 : base)) / 100;

    /* The span between the two, so a small offset is still visible when
     * the mark itself would sit under the thumb. */
    if (mark != here) {
        const int a = mark < here ? mark : here;
        const int b = mark < here ? here : mark;
        gfx_fill_rect(a, y - TRACK_THIN / 2, b - a, TRACK_THIN, C_RG);
    }

    /* A 3 px bar, full slider height. Not a circle: see above. */
    gfx_fill_rect(mark - 1, y - THUMB_R, 3, 2 * THUMB_R + 1, C_RG);
}

/*
 * The battery, at the right end of the volume row.
 *
 * Opposite the speaker on purpose: that row already has an icon in the
 * left margin and 96 px of unused panel in the right one, and the two
 * things being reported -- how loud it is, how much is left -- are both
 * states of the device rather than of the track. Everything above this
 * row is about the song.
 *
 * Icon above, digits below, both centred on the same x. Side by side
 * would need 76 px of width in a 96 px margin, which leaves the outline
 * touching the slider groove.
 *
 * The fill is proportional and the outline is not: an outline that
 * shrinks reads as a smaller battery rather than as a flatter one.
 */
#define BATT_W      (46)
#define BATT_H      (24)
#define BATT_NUB_W  (5)
#define BATT_NUB_H  (10)
#define BATT_WALL   (3)

/* Below 20% the fill turns red -- the same red as the seek bar's played
 * portion, because it is the same statement: this much is spent. */
#define BATT_LOW_PCT (20)
#define C_BATT_LOW   C_FILL
#define C_BATT_CHG   RGB(0x4C, 0xC0, 0x5E)

/*
 * EXTERNAL POWER: a bolt, a lamp, and a Type-C connector seen end on.
 *
 * The marker means the battery has stopped being the answer to "how
 * long has this got" -- something is feeding the device and the reading
 * that used to be here is no longer the interesting number.
 *
 * Two earlier attempts drew the connector in profile. 0723 drew a
 * hollow stadium the height of the battery with a lead beside it, which
 * at this size is a body with a cap on it: a memory stick, which is a
 * thing you also plug into this device, so the icon named the wrong
 * object. 0806 flattened the shell and ran the cable to the edge, which
 * fixed the silhouette and left the icon saying only "a cable".
 *
 * A cable is not the message. THE MESSAGE IS POWER, and the thing that
 * says power in one glance is a lightning bolt -- so the bolt is the
 * subject, the connector underneath says which kind of power, and a
 * green lamp beside the bolt says it is arriving. Type-C seen end on is
 * a shape nothing else on this panel resembles: a flat stadium with a
 * bar down the middle, which is the receptacle and its tongue. In
 * profile it competes with every other plug ever drawn; end on it does
 * not.
 *
 * Stacked rather than side by side. The battery it replaces is an
 * outline with digits under it, so the corner already reads top to
 * bottom, and keeping that means the eye lands in the same place
 * whether or not the pack is in.
 *
 * The bolt is solid. An outlined one at 26 px is four strokes meeting
 * at two acute angles, and the angles fill in.
 */
#define USB_BOLT_W   (18)
#define USB_BOLT_H   (26)
#define USB_DOT_R    (4)
#define USB_CONN_W   (46)   /* the battery's own width: the two icons
                             * occupy the same column and must not make
                             * the corner shift when power is connected */
#define USB_CONN_H   (18)
#define USB_CONN_WALL (3)   /* the battery's wall, for the same reason */
#define USB_TONGUE_W (26)
#define USB_TONGUE_H (4)

/*
 * The bolt, as a closed path in a USB_BOLT_W x USB_BOLT_H box.
 *
 * Six points: down the left face, across the notch, down to the tip,
 * back up the right face, across the other notch. The two notches are
 * what make it a bolt rather than a Z -- without them the strokes meet
 * flush and it reads as a lightning-shaped arrow.
 */
static const int8_t k_bolt[][2] = {
    { 13,  0 }, {  2, 14 }, {  9, 14 }, {  7, 26 }, { 18, 11 }, { 11, 11 },
};

/* gfx has rectangles and circles; a rounded rectangle is four of one and
 * two of the other. Corners first, then the cross, so nothing lands on
 * a corner already placed. */
static void fill_rrect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    gfx_fill_circle(x + r, y + r, r, c);
    gfx_fill_circle(x + w - 1 - r, y + r, r, c);
    gfx_fill_circle(x + r, y + h - 1 - r, r, c);
    gfx_fill_circle(x + w - 1 - r, y + h - 1 - r, r, c);
    gfx_fill_rect(x + r, y, w - 2 * r, h, c);
    gfx_fill_rect(x, y + r, w, h - 2 * r, c);
}

/*
 * Scanline fill of a small closed polygon, in whole pixels.
 *
 * One row at a time: find where the edges cross this row, sort the
 * crossings, fill between them in pairs. Six points means at most three
 * pairs and the insertion sort is over a list that never exceeds a
 * handful, so this is a loop over 26 rows and nothing more.
 *
 * Edges are counted half-open in y -- a vertex exactly on the row
 * belongs to the edge below it and not the one above -- which is what
 * stops a vertex being counted twice and leaving a row unfilled.
 */
#define POLY_MAX_X  8

static void fill_poly(const int8_t pts[][2], int n, int ox, int oy, uint16_t c)
{
    int ymin = pts[0][1], ymax = pts[0][1];
    for (int i = 1; i < n; i++) {
        if (pts[i][1] < ymin) ymin = pts[i][1];
        if (pts[i][1] > ymax) ymax = pts[i][1];
    }

    for (int y = ymin; y <= ymax; y++) {
        int xs[POLY_MAX_X];
        int nx = 0;

        for (int i = 0; i < n && nx < POLY_MAX_X; i++) {
            const int x1 = pts[i][0], y1 = pts[i][1];
            const int j = (i + 1 == n) ? 0 : i + 1;
            const int x2 = pts[j][0], y2 = pts[j][1];
            if ((y1 <= y && y < y2) || (y2 <= y && y < y1)) {
                /* Rounded rather than truncated: at this size half a
                 * pixel of error on an edge is a visible step. */
                const int num = (x2 - x1) * (y - y1);
                const int den = y2 - y1;
                xs[nx++] = x1 + (num + den / 2) / den;
            }
        }

        for (int i = 1; i < nx; i++) {
            const int v = xs[i];
            int k = i - 1;
            while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; }
            xs[k + 1] = v;
        }

        for (int i = 0; i + 1 < nx; i += 2) {
            const int w = xs[i + 1] - xs[i] + 1;
            if (w > 0) gfx_fill_rect(ox + xs[i], oy + y, w, 1, c);
        }
    }
}

static void draw_usb_c(int cx, int cy)
{
    /* Top of the bolt sits where the battery's outline starts, and the
     * connector where its digits were, so the block occupies the same
     * rows either way. */
    const int top = cy - 30;

    /* Bolt left of centre, lamp to its right: the pair balances around
     * the same axis the connector below is centred on. */
    fill_poly(k_bolt, (int)(sizeof(k_bolt) / sizeof(k_bolt[0])),
              cx - USB_BOLT_W + 1, top + 1, C_ICON);

    gfx_fill_circle(cx + 16, top + 12, USB_DOT_R, C_BATT_CHG);

    /* The receptacle: a stadium, hollowed to its wall. */
    const int conn_x = cx - USB_CONN_W / 2;
    const int conn_y = top + 35;
    fill_rrect(conn_x, conn_y, USB_CONN_W, USB_CONN_H,
               USB_CONN_H / 2, C_ICON);
    fill_rrect(conn_x + USB_CONN_WALL, conn_y + USB_CONN_WALL,
               USB_CONN_W - 2 * USB_CONN_WALL,
               USB_CONN_H - 2 * USB_CONN_WALL,
               (USB_CONN_H - 2 * USB_CONN_WALL) / 2, C_BG);

    /* The tongue. Square ends, not rounded: rounded ones at 4 px tall
     * turn the bar into a double-headed arrow. */
    gfx_fill_rect(cx - USB_TONGUE_W / 2,
                  conn_y + USB_CONN_H / 2 - USB_TONGUE_H / 2,
                  USB_TONGUE_W, USB_TONGUE_H, C_ICON);
}

static void draw_battery(int pct, bool charging, bool ext)
{
    int x0, x1, y;
    vol_bounds(&x0, &x1, &y);
    const int cx = x1 + 42, cy = y;

    /*
     * Nothing of the battery is drawn when there is no battery. Not an
     * outline with a connector next to it, and not a connector inside a
     * battery -- one icon, saying one thing.
     */
    if (ext) {
        draw_usb_c(cx, cy);
        return;
    }

    const int left = cx - BATT_W / 2;
    const int top  = cy - 30;

    /* Outline: four walls rather than a filled rect with a hole punched
     * in it, so nothing is drawn twice and the interior can be filled
     * without clearing it first. */
    gfx_fill_rect(left, top, BATT_W, BATT_WALL, C_ICON);
    gfx_fill_rect(left, top + BATT_H - BATT_WALL, BATT_W, BATT_WALL, C_ICON);
    gfx_fill_rect(left, top, BATT_WALL, BATT_H, C_ICON);
    gfx_fill_rect(left + BATT_W - BATT_WALL, top, BATT_WALL, BATT_H, C_ICON);

    /* The nub, which is what makes 46x24 read as a battery rather than
     * as a text field. */
    gfx_fill_rect(left + BATT_W, cy - 30 + (BATT_H - BATT_NUB_H) / 2,
                  BATT_NUB_W, BATT_NUB_H, C_ICON);

    if (pct < 0) {
        /* No reading. An empty outline and no digits -- see ui_state_t.
         * Drawing 0% here would be a claim, and the wrong one. */
        return;
    }

    const int inner_x = left + BATT_WALL + 1;
    const int inner_w = BATT_W - 2 * (BATT_WALL + 1);
    const int inner_y = top + BATT_WALL + 1;
    const int inner_h = BATT_H - 2 * (BATT_WALL + 1);

    const uint16_t c = charging ? C_BATT_CHG
                     : (pct <= BATT_LOW_PCT ? C_BATT_LOW : C_ICON);

    int w = (inner_w * pct) / 100;
    /* A nonzero charge always shows at least a sliver. Rounding 4% down
     * to nothing draws the same picture as a flat pack. */
    if (w == 0 && pct > 0) w = 1;
    gfx_fill_rect(inner_x, inner_y, w, inner_h, c);

    /* Same seven-segment digits as the clocks, so the two numbers on the
     * panel that are not part of a song look like each other. */
    gfx_draw_pct_centred(cx, cy + 4, pct, c);
}

/* ------------------------------------------------------------------ */

void ui_clear_art(void)
{
    if (!s_fb) return;
    gfx_fill_rect(0, 0, s_w, s_bar_top, C_BG);
    gfx_blit(0, s_bar_top);
}

/*
 * The format card, drawn where the cover would be.
 *
 * Scale 6 for the heading and 3 for the rest, laid out around the centre
 * of the square rather than from its top: the number of lines varies
 * with what the decoder has managed to say about the file so far, and a
 * block that grows downward from a fixed top drifts off centre as it
 * does.
 */
#define ART_INFO_HEAD_SCALE (5)
#define ART_INFO_BODY_SCALE (3)
#define ART_INFO_GAP        (18)

void ui_show_art_info(const char *const *lines, int n)
{
    if (!s_fb || !lines || n <= 0) return;

    gfx_fill_rect(0, 0, s_w, s_bar_top, C_BG);

    int total = GFX_GLYPH_H(ART_INFO_HEAD_SCALE);
    for (int i = 1; i < n; i++) {
        total += ART_INFO_GAP + GFX_GLYPH_H(ART_INFO_BODY_SCALE);
    }

    int y = (s_bar_top - total) / 2;
    if (y < 0) y = 0;

    for (int i = 0; i < n; i++) {
        const char *t = lines[i] ? lines[i] : "";
        const int scale = i ? ART_INFO_BODY_SCALE : ART_INFO_HEAD_SCALE;
        const int w = gfx_text_w(t, scale);
        int x = (s_w - w) / 2;
        if (x < TEXT_X) x = TEXT_X;
        gfx_draw_text(x, y, t, scale, s_w - 2 * TEXT_X,
                      i ? C_ICON : C_THUMB);
        y += GFX_GLYPH_H(scale) + ART_INFO_GAP;
    }

    gfx_blit(0, s_bar_top);
}

esp_err_t ui_init(esp_lcd_panel_handle_t panel, int w, int h)
{
    /* gfx owns the framebuffer lookup now; this file keeps its own copies
     * of the geometry because every layout function reads them. */
    ESP_RETURN_ON_ERROR(gfx_init(panel, w, h), TAG, "gfx");
    s_w = w;
    s_h = h;
    s_bar_top = h - UI_BAR_H;
    s_fb = gfx_fb();
    return ESP_OK;
}

void ui_draw(const ui_state_t *st)
{
    if (!s_fb) return;

    if (st->screen_off) {
        /* Nothing at all -- the backlight is off, and drawing into a dark
         * panel just burns PSRAM bandwidth the decoder wants. */
        return;
    }

    gfx_fill_rect(0, s_bar_top, s_w, UI_BAR_H, C_BG);

    int x0, x1, y;
    seek_bounds(&x0, &x1, &y);
    /*
     * A track whose numbers are not in yet gets the empty state, not the
     * previous track's. See ui_state_t::stats_valid -- everything below
     * that reads pos_sec or len_sec is gated on it.
     */
    const bool stats = st->stats_valid;
    const int pos_pct = (stats && st->len_sec > 0)
                      ? (int)((st->pos_sec * 100) / st->len_sec)
                      : 0;
    /*
     * Three states, not two:
     *   - seekable: full slider with a thumb, draggable
     *   - known length, not seekable: progress fills, no thumb. Honest --
     *     the position is real -- and the missing thumb is what says not
     *     to try dragging it.
     *   - no length: a bare groove.
     */
    const int shown_pct = (s_drag == 0) ? s_drag_pct : pos_pct;

    if (stats && waveform_ready() && st->len_sec > 0) {
        /*
         * The envelope is the bar. Played columns red, unplayed grey, and
         * the split is the position -- which is the whole reason for
         * merging the two: the shape of the song and the point reached in
         * it were always the same axis drawn twice.
         *
         * The thumb is gone with the groove. A slider needs one because
         * there is nothing else to grab; this is a 64 px tall target with
         * a hard colour edge in it, and a circle sitting on top of the
         * envelope obscured the columns nearest the position -- the ones
         * being looked at.
         */
        waveform_draw_bar(x0, x1, y, UI_WAVE_H, shown_pct,
                          C_WAVE_PAST, C_WAVE_FUTURE);

        const int split = x0 + ((x1 - x0) * shown_pct) / 100;
        gfx_fill_rect(split - 1, y - UI_WAVE_H, 3, UI_WAVE_H + 4,
                      st->can_seek ? C_PLAYHEAD : C_WAVE_FUTURE);
    } else if (stats && st->len_sec > 0) {
        /*
         * No envelope yet -- the scan takes a few seconds on a long
         * track and may never produce one at all for a format with no
         * per-frame loudness.
         *
         * Drawn as a flat block at full height rather than as a slider
         * with a thumb: the same geometry the envelope will occupy, with
         * every column at 100%. Two reasons.
         *
         * The row stops jumping. A thin groove with a circle on it,
         * replaced seconds later by a 72 px tall waveform, is a layout
         * change in the middle of a glance -- and the thumb was the only
         * part that moved, so it read as the control being replaced
         * rather than as detail arriving. Now the block simply acquires
         * a shape.
         *
         * And the thumb was misleading here anyway. It says "grab me",
         * which is exactly what the envelope version deliberately does
         * not say -- the whole 72 px block is the target, and it is the
         * same target before and after the scan finishes.
         */
        /*
         * Unshaped and grey, both halves the same: there is no envelope
         * to divide, and colouring the played part red would draw a
         * progress bar that happens to be the exact size and place the
         * waveform will occupy -- which reads as a waveform of a track
         * that is uniformly loud, rather than as one not measured yet.
         * Grey says "nothing known here" in a way red cannot.
         */
        gfx_fill_rect(x0, y - UI_WAVE_H, x1 - x0, UI_WAVE_H, C_WAVE_FUTURE);

        /*
         * And say why -- but only when it is true.
         *
         * An unshaped bar on first play and a shaped one ever after is
         * otherwise unexplained, and looks like the waveform failed on
         * this track. This is the one moment the measurement is worth
         * mentioning, in the same yellow the RG badge uses so the two
         * read as one feature.
         *
         * Gated on rg_measuring, not on the envelope being missing.
         * A track whose envelope is already in its sidecar is not being
         * listened to, and the gap between the track starting and the
         * bar being handed that envelope would otherwise put the words
         * on screen for a few seconds of every replay -- claiming work
         * that is not happening, on exactly the tracks that already did
         * it. Same for a track whose measurement was dropped by a seek:
         * nothing is listening any more, so nothing says it is.
         *
         * Behind the playhead, not above or below: the bar is 72 px of
         * empty grey and the text has nowhere better to be, and it is
         * drawn first so the playhead crosses over it rather than the
         * text sitting on top of the position.
         */
        if (st->rg_measuring) {
            gfx_draw_text(x0 + 12, y - UI_WAVE_H / 2 - 8,
                          "ReplayGain is listening...", 2, x1 - x0 - 24, C_RG);
        }

        const int split = x0 + ((x1 - x0) * shown_pct) / 100;
        gfx_fill_rect(split - 1, y - UI_WAVE_H, 3, UI_WAVE_H + 4,
                      st->can_seek ? C_PLAYHEAD : C_WAVE_FUTURE);
    } else {
        /* No duration at all: nothing to show a position against, so a
         * bare groove. Not a flat block -- a full-height bar with no
         * playhead in it would claim the track had a length and that the
         * position was zero. */
        gfx_fill_rect(x0, y - 3, x1 - x0, 6, C_TRACK);
    }

    /*
     * Row 3: elapsed at the left edge, remaining at the right.
     *
     * Remaining rather than total, and negative rather than bare. The
     * total was the same five characters for the whole song and said
     * nothing the bar was not already showing; how long is left is the
     * question people actually ask of a player, and it is the one number
     * on screen that the seek bar cannot answer by looking at it.
     *
     * With no duration there is nothing to subtract from, so BOTH
     * clocks dash -- the elapsed one too. A running elapsed beside a
     * dashed remaining invites the arithmetic that would finish the
     * sentence, and there is no total to finish it with; dashes on both
     * match the bare groove above, which is also refusing to claim a
     * position it does not have.
     */
    const int ty = s_bar_top + TIME_Y;

    if (!stats) {
        /* Both clocks, both dashed. The elapsed one especially: it is
         * the number that was counting a moment ago, and leaving it at
         * the old track's value for the length of an open is the single
         * most convincing way to look like the press did nothing. */
        gfx_draw_time_dashes(TIME_PAD, ty, C_TRACK);
        gfx_draw_time_dashes(s_w - GFX_TIME_W - TIME_PAD, ty, C_TRACK);
    } else {
        /*
         * While a seek drag is in progress these two show where the
         * finger is, not where the audio is, and are drawn in the fill
         * colour to say so.
         *
         * This is what replaced the bubble, and the colour is the whole
         * of what makes it honest. The digits are in the same place and
         * the same size either way, so without it a dragged clock is
         * indistinguishable from a counting one -- it would read as the
         * seek having already happened, several seconds before the
         * decode loop has even been asked. Red says requested; grey says
         * playing. Same distinction the envelope already draws with the
         * same two colours, one row up.
         *
         * Only the seek drag. A volume drag leaves these alone: it is not
         * a position, the slider's own fill is already showing the level,
         * and it applies live so the answer arrives in the ears before
         * any of this is repainted.
         *
         * Both clocks move together. Showing the target on the left and
         * the real remainder on the right would put two numbers on screen
         * that do not add up to the length.
         */
        const bool seeking = (s_drag == 0 && st->len_sec > 0);
        const uint32_t shown_sec = seeking
            ? (uint32_t)((uint64_t)st->len_sec * s_drag_pct / 100)
            : st->pos_sec;
        const uint16_t clock_c = seeking ? C_FILL : C_ICON;

        if (st->len_sec > 0) {
            gfx_draw_time(TIME_PAD, ty, shown_sec, clock_c);
            const uint32_t left = (st->len_sec > shown_sec)
                                ? st->len_sec - shown_sec : 0;
            gfx_draw_time_neg(s_w - GFX_TIME_NEG_W - TIME_PAD, ty, left,
                              clock_c);
        } else {
            /* No duration: dashes on both, matching the bare groove.
             * The elapsed number is real and is still withheld, because
             * on its own beside a blank it is an invitation to work out
             * what is left, which is the one thing not known. */
            gfx_draw_time_dashes(TIME_PAD, ty, C_TRACK);
            gfx_draw_time_dashes(s_w - GFX_TIME_W - TIME_PAD, ty, C_TRACK);
        }
    }

    /* Title big, then artist, then album -- a row each.
     *
     * The joined "artist  -  album" string is gone, and with it the 96
     * byte buffer it was built in: the two tag fields are 64 bytes each,
     * so anything approaching full length was silently truncated, and it
     * truncated the album first only by luck of ordering.
     *
     * Album is dimmer than artist rather than the same grey. Three rows
     * of equal weight read as a paragraph; the hierarchy is what makes it
     * scannable at arm's length. */
    /*
     * Rows 4, 5, 6: title, album, artist.
     *
     * The title bounces when it does not fit rather than being cut with
     * an ellipsis. It is the one string on screen that is not
     * interchangeable with the file it came from -- an album can be
     * truncated because the cover above it says the same thing, and an
     * artist because the album implies it, but "Everything In Its Right
     * Pl..." is a song nobody can name.
     *
     * Album above artist, which is the reverse of what this used to do.
     * Reading downward the rows now go from most specific to least: this
     * track, the record it is on, the person who made the record.
     */
    const char *title = st->title ? st->title : "";
    const int title_w = gfx_text_w(title, 3);
    const int win_w = s_w - 2 * TEXT_X;
    const int over = title_w - win_w;
    const int len = (int)strlen(title);

    /* A new title starts from the left, not from wherever the last one
     * had got to. Without this a short title inherits the previous long
     * one's offset and is drawn off the side of the panel. */
    if (title != s_marq_title || len != s_marq_len) {
        s_marq_title = title;
        s_marq_len = len;
        s_marq_off = 0;
        s_marq_dir = 1;
        s_marq_hold = MARQ_HOLD;
    }
    marquee_step(title, over);

    gfx_draw_text_clipped(TEXT_X - s_marq_off, s_bar_top + TITLE_Y,
                          TEXT_X, win_w, title, 3, C_THUMB);

    if (st->album && *st->album) {
        gfx_draw_text(TEXT_X, s_bar_top + ALBUM_Y, st->album, 3, win_w, C_ALBUM);
    }
    if (st->artist && *st->artist) {
        gfx_draw_text(TEXT_X, s_bar_top + ARTIST_Y, st->artist, 3, win_w, C_ICON);
    }

    vol_bounds(&x0, &x1, &y);
    /*
     * The slider keeps showing the level, dimmed, rather than dropping
     * to zero.
     *
     * Muting is not setting the volume to nothing; it is suspending it.
     * A slider that ran to the left end would lose the only record of
     * where it is going back to, and unmuting would look like the player
     * had picked a number. Dim says "this is not in effect right now",
     * which is what is true.
     */
    draw_slider_c(x0, x1, y, s_drag == 1 ? s_drag_pct : st->volume,
                  st->muted ? C_ICON_OFF : C_FILL);

    draw_speaker(st->muted);
    draw_rg(st);
    draw_battery(st->battery_pct, st->battery_charging, st->ext_power);
    draw_folder();
    draw_gear();
    draw_moon();

    int cx, cy;
    /* Prev is never greyed: it always does something -- restart the
     * track if nothing else -- so dimming it would be a lie about a
     * working button. Next genuinely stops working at the end of a
     * folder, which is the case worth showing. */
    prev_centre(&cx, &cy);
    draw_skip(cx, cy, false, true);
    next_centre(&cx, &cy);
    draw_skip(cx, cy, true, st->has_next);
    draw_play_pause(st->playing);

    gfx_blit(s_bar_top, s_h);
}

/* ------------------------------------------------------------------ */

static bool in_box(int x, int y, int cx, int cy, int half)
{
    return x >= cx - half - HIT_PAD_X && x <= cx + half + HIT_PAD_X &&
           y >= cy - half - HIT_PAD_Y && y <= cy + half + HIT_PAD_Y;
}

static int pct_from_x(int x, int x0, int x1)
{
    if (x <= x0) return 0;
    if (x >= x1) return 100;
    return ((x - x0) * 100) / (x1 - x0);
}

ui_action_t ui_touch(const ui_state_t *st, bool down, int x, int y)
{
    ui_action_t act = { UI_ACTION_NONE, 0 };
    const bool tapped = down && !s_was_down;
    const bool released = !down && s_was_down;
    s_was_down = down;

    /* Screen off: the only thing a touch does is wake, and it does not
     * also press whatever is under it. Waking straight into a button
     * would let one tap turn the screen off again. */
    if (st->screen_off) {
        if (tapped) act.kind = UI_ACTION_SCREEN_ON;
        return act;
    }

    if (s_drag >= 0) {
        int x0, x1, sy;
        if (s_drag == 0) seek_bounds(&x0, &x1, &sy);
        else             vol_bounds(&x0, &x1, &sy);

        if (down) {
            s_drag_x = x;
            s_drag_y = y;
            s_drag_pct = pct_from_x(x, x0, x1);
            /* Volume tracks live -- you want to hear it while moving.
             * Seek does not: decoding to a new position on every poll
             * would thrash the SD card, so it fires once on release. */
            if (s_drag == 1) {
                act.kind = UI_ACTION_VOLUME;
                act.value = s_drag_pct;
            }
            return act;
        }

        if (released) {
            act.kind = (s_drag == 0) ? UI_ACTION_SEEK : UI_ACTION_VOLUME;
            act.value = s_drag_pct;
            s_drag = -1;
            return act;
        }
        s_drag = -1;
        return act;
    }

    if (!tapped) return act;

    int cx, cy;

    play_centre(&cx, &cy);
    if (in_box(x, y, cx, cy, BTN_R)) {
        act.kind = UI_ACTION_PLAY_PAUSE;
        return act;
    }

    prev_centre(&cx, &cy);
    if (in_box(x, y, cx, cy, SKIP_HALF)) {
        /*
         * A second tap inside the window is a different intent, not a
         * repeat of the first: single means "the track before this one
         * in the list", double means "whatever I was actually listening
         * to". Under shuffle those are unrelated answers.
         *
         * The first tap is not held back waiting to see whether a second
         * arrives. Doing that would put DOUBLE_TAP_MS of lag on
         * every single press to serve the rarer one; instead both fire
         * and the player treats the second as a correction. That is why
         * the second action is PREV_AGAIN rather than PREV: the player
         * needs to know it is undoing its own last move.
         */
        const TickType_t now = xTaskGetTickCount();
        const bool again = s_prev_tick &&
            (now - s_prev_tick) < pdMS_TO_TICKS(DOUBLE_TAP_MS);

        s_prev_tick = again ? 0 : now;   /* a triple tap is two doubles */
        act.kind = again ? UI_ACTION_PREV_AGAIN : UI_ACTION_PREV;
        return act;
    }

    next_centre(&cx, &cy);
    if (in_box(x, y, cx, cy, SKIP_HALF)) {
        act.kind = UI_ACTION_NEXT;
        return act;
    }

    folder_centre(&cx, &cy);
    if (in_box(x, y, cx, cy, ICON_HALF)) {
        act.kind = UI_ACTION_CHOOSE_FILE;
        return act;
    }

    gear_centre(&cx, &cy);
    if (in_box(x, y, cx, cy, ICON_HALF)) {
        act.kind = UI_ACTION_SETTINGS;
        return act;
    }

    moon_centre(&cx, &cy);
    if (in_box(x, y, cx, cy, ICON_HALF)) {
        act.kind = UI_ACTION_SCREEN_OFF;
        return act;
    }

    /* Before the volume slider, and with a box that does not reach it --
     * see SPK_HALF. in_box() is not used because it adds HIT_PAD_X, and
     * the padded box would overlap the slider's. */
    spk_centre(&cx, &cy);
    if (x >= cx - SPK_HALF && x <= cx + SPK_HALF &&
        y >= cy - ICON_HALF - HIT_PAD_Y && y <= cy + ICON_HALF + HIT_PAD_Y) {
        act.kind = UI_ACTION_MUTE;
        return act;
    }

    /* Sliders last: their padded hit boxes are wide, so a button landing
     * inside one has to win first. */
    int x0, x1, sy;
    vol_bounds(&x0, &x1, &sy);
    if (x >= x0 - HIT_PAD_X && x <= x1 + HIT_PAD_X &&
        y >= sy - HIT_PAD_Y && y <= sy + HIT_PAD_Y) {
        s_drag = 1;
        s_drag_x = x;
        s_drag_y = y;
        s_drag_pct = pct_from_x(x, x0, x1);
        act.kind = UI_ACTION_VOLUME;
        act.value = s_drag_pct;
        return act;
    }

    /*
     * Seek, but only when there is something to seek within.
     *
     * With no duration the bar has no scale, so a drag has nothing to
     * mean: the split followed the finger, the clocks had nothing to
     * count against, and on release the player logged "seek ignored" and
     * everything snapped back. That is a control that looks live and is
     * not, which is worse than one that plainly does nothing.
     *
     * The test is seekability, not length. Those came apart the moment
     * duration.c started reading lengths out of containers: an Ogg has a
     * full, correct, moving seek bar and no way to seek within it, and
     * gating on len_sec let the drag through to a player that logged
     * "seek ignored" and snapped the thumb back.
     */
    if (!st->can_seek) return act;

    /* The hit box now covers the envelope's full height, not a padded
     * band around a line. Pressing a tall column and having nothing
     * happen would read as the bar having gone dead -- the drawn shape
     * has to be the target. */
    seek_bounds(&x0, &x1, &sy);
    if (x >= x0 - HIT_PAD_X && x <= x1 + HIT_PAD_X &&
        y >= sy - UI_WAVE_H - HIT_PAD_Y && y <= sy + HIT_PAD_Y) {
        s_drag = 0;
        s_drag_x = x;
        s_drag_y = y;
        s_drag_pct = pct_from_x(x, x0, x1);
        return act;
    }

    return act;
}
