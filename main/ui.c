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
#include "esp_timer.h"

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
/* Starred. The same gold the chooser's rows use -- one mark, one
 * colour, two screens. Not C_FILL, which means "played" on the seek bar
 * eight pixels away. */
#define C_STAR      RGB(0xE8, 0xB3, 0x2C)
#define C_STAR_OFF  RGB(0xFF, 0xFF, 0xFF)   /* the thin ring: nothing starred */
/* Album row. Dimmer than artist, but its own value rather than reusing
 * C_TRACK -- that is 0x3A, chosen to be a slider groove that does not
 * compete with the fill, and it is too dark to read as text. */
#define C_ALBUM     RGB(0x88, 0x88, 0x88)

/* A disabled icon. Dark enough to read as off at arm's length, light
 * enough not to look like a rendering fault -- the same distance below
 * C_ICON that C_ALBUM sits below C_THUMB, so the bar has one idea of
 * what "de-emphasised" means rather than two. */
#define C_ICON_OFF  RGB(0x55, 0x55, 0x55)

/* The play toggle's lit trough. The same green as a charging battery --
 * see draw_play_pause() for why that is a reuse and not a collision. */
#define C_PLAY_ON   RGB(0x4C, 0xC0, 0x5E)

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
/*
 * The LIVE badge, and it is C_FILL rather than a fifth idea.
 *
 * The rule this file has followed so far is that a new kind of statement
 * gets a new colour -- C_RG exists because a gain is neither a level nor
 * a control. A broadcast indicator would qualify, except that red is
 * already what an on-air light is everywhere else, and the one thing that
 * could be confused with it is not on screen: while a stream plays there
 * is no envelope and no seek fill, because there is no position for
 * either to describe. The only other red left in the bar is a slider,
 * and a filled slider is not confusable with a word in a pill.
 *
 * An alias rather than the bare constant so that a later patch deciding
 * this was wrong changes one line and finds every use.
 */
#define C_LIVE      C_FILL
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
/*
 * Every offset here is from the SQUARE's top-left corner, not the
 * panel's. That is what lets portrait and landscape share the table:
 * the square is 720x720 at both angles, so a row at y=392 is 392 px
 * down the square wherever the square happens to be.
 *
 * Nine rows now, not eight -- row 7 split in two when the square grew
 * from 560 to 720. Seven controls on one line was what 560 px forced,
 * and it put the file chooser and the sleep page in the same sweep as
 * the transport. Two lines puts the transport alone on its own row,
 * which is the row every finger goes to.
 *
 *   1  the cover, beside or above the square entirely
 *   2  the envelope, full square width
 *   3  elapsed left, remaining right
 *   4  title
 *   5  album
 *   6  artist
 *   7  prev, play/pause, next
 *   8  folder, gear, star, sleep -- four on one pitch
 *   9  output icon, volume, battery
 */
#define BAR_PAD     (24)    /* square edge to content, both sides */
#define SEEK_Y      (96)    /* row 2: the envelope's baseline */
#define TIME_Y      (112)   /* row 3: TOP of the cell, not a baseline */
#define TITLE_Y     (186)   /* row 4 */
#define ALBUM_Y     (238)   /* row 5 */
#define ARTIST_Y    (278)   /* row 6 */
#define ROW_Y       (392)   /* row 7, transport centres */
#define AUX_Y       (500)   /* row 8, the four icons' centres */
#define VOL_Y       (623)   /* row 9 */
/*
 * The art overlay's inset. NOT the bar's -- the bar's text starts at
 * bar_x0(), which is the square's content edge and is 560+24 in
 * landscape. This one is for the notice card and the no-artwork lines,
 * which live in the artwork band and so are still measured from the
 * panel's own left edge.
 */
#define TEXT_X      (24)

/*
 * Play/pause is a toggle, not a disc.
 *
 * A disc with a triangle in it says what tapping does; this says what
 * the player IS doing, which is the thing a glance across the room
 * actually wants. The knob's position carries it -- right for playing,
 * left for paused -- and the trough's colour says the same thing again
 * so it survives being seen out of the corner of an eye: green for
 * running, C_FILL for stopped.
 *
 * The green is C_BATT_CHG, already in the palette for "power is coming
 * in", which is near enough to "this is going" to not be a fifth idea.
 * The red is C_FILL, which already means played on the envelope and set
 * on the volume track; a filled pill is not confusable with either, and
 * the alternative was a fourth red.
 *
 * 168x92 with a 45 px knob: the proportion from the mockup, with the
 * knob nearly the trough's full height so the trough reads as a track
 * rather than a border. Wider than the 92 px disc it replaces, which is
 * what moved prev and next out to +/-132.
 */
#define PILL_W      (168)
#define PILL_H      (92)
#define KNOB_R      (45)
/* BTN_R, the disc's radius, is gone with the disc. Its last user was the
 * hit test, which is the pill's own box now -- see ui_touch(). */

#define ICON_HALF   (26)
#define SKIP_HALF   (35)    /* prev/next: 26 px triangle plus a 7 px bar */
/*
 * Prev and next, either side of the pill.
 *
 * 150 and not 132, and the difference is a bug the layout test caught.
 * The pill's padded box reaches PILL_W/2 + HIT_PAD_X = 98 from the
 * centre and a skip glyph's reaches SKIP_HALF + HIT_PAD_X = 49 back
 * towards it, so the centres must be at least 147 apart. At 132 they
 * overlapped by 15 px and the ordering in ui_touch() was the only thing
 * deciding which control a press near the pill's edge hit -- which is
 * exactly the failure mode row 7's own comment warns about, reintroduced
 * by making the button wider without redoing its arithmetic.
 *
 * 150 leaves 3 px. The glyph's outer edge is then 185 from the centre,
 * well inside the content box's 336.
 */
#define SKIP_DX     (150)

/*
 * The two blocks that flank the volume groove, declared up here because
 * vol_bounds() derives the gutters from them and it is above the icons
 * themselves. SPK_HALF's own argument -- a 52 px target for an icon
 * alone in a margin -- is with draw_speaker().
 *
 * BATT_BLOCK is the battery's drawn width INCLUDING the nub: the
 * outline is BATT_W and the nub hangs off its right edge, and a groove
 * that stopped at the outline would run under the nub.
 */
#define SPK_HALF    (26)

/*
 * The battery's own dimensions, up here rather than beside draw_battery()
 * because batt_cx() needs them and C compiles top to bottom -- the same
 * trap fill_rrect() fell into in 5001. BATT_BLOCK is what the row's
 * arithmetic spends on it: the outline plus the nub hanging off its
 * right edge, because a groove that stopped at the outline would run
 * under the nub.
 */
#define BATT_W      (46)
#define BATT_H      (24)
#define BATT_NUB_W  (5)
#define BATT_NUB_H  (10)
#define BATT_WALL   (3)
#define BATT_BLOCK  (BATT_W + BATT_NUB_W)

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

/*
 * The control square's origin. s_bar_top is the portrait name and kept
 * because every row offset below is written against it; s_bar_x is the
 * other axis, 0 in portrait and 560 in landscape. Both are set by
 * ui_relayout() and by nothing else.
 */
static int s_bar_top;
static int s_bar_x;
static bool s_landscape;

/* Live drag state. -1 = nothing being dragged. */
/*
 * The notice card. Declared up here with the rest of the file's state
 * rather than beside ui_show_notice(), because ui_clear_art() -- which
 * is above it -- drops the card, and a static cannot be used before it
 * is declared.
 */
static bool s_notice_up;
static bool s_notice_dismissible;
static int  s_notice_x, s_notice_y, s_notice_w, s_notice_h;

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
    case UI_ACTION_SCREEN_OFF:  return "moon (sleep page)";
    case UI_ACTION_FAVORITE:    return "star (favourite)";
    case UI_ACTION_DISMISS_NOTICE: return "notice dismissed";
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
/*
 * The square's content box. Everything below is laid out inside it, and
 * it is the only place s_bar_x and s_bar_top appear together.
 */
static inline int bar_x0(void) { return s_bar_x + BAR_PAD; }
static inline int bar_x1(void) { return s_bar_x + UI_SQUARE - BAR_PAD; }
static inline int bar_cx(void) { return s_bar_x + UI_SQUARE / 2; }

/* Row 2: the envelope, content edge to content edge. Nothing shares the
 * row. It used to run the full panel width; inside the square it stops
 * at the padding, so it lines up with the title below it rather than
 * bleeding past it. */
static void seek_bounds(int *x0, int *x1, int *y)
{
    *x0 = bar_x0();
    *x1 = bar_x1();
    *y  = s_bar_top + SEEK_Y;
}

/*
 * Row 9. Three blocks and two gutters, the gutters equal and the outer
 * blocks flush to the content box.
 *
 * The margins used to be a flat 96 px either side, which was sized when
 * the bar was the full 720-wide panel. Inside a padded square that left
 * dead air at the output icon's end and crowded the battery at the
 * other, so the groove is now derived: content width less the two
 * icons, the rest split evenly.
 */
#define VOL_BLOCKS  (2 * SPK_HALF + BATT_BLOCK)

/*
 * The two icons anchor to the CONTENT BOX, and the groove is derived
 * from them -- which is the opposite of how this worked and is the whole
 * of this patch.
 *
 * spk_centre() and draw_battery() used to hang off vol_bounds() by a
 * fixed 42 px each, from when the groove's margins were a flat 96 and
 * the icons sat inside them. 5001 rebalanced the groove and the icons
 * came along for the ride: the output icon landed at 105 rather than
 * flush at 50, leaving 55 px of dead air at the left end -- the very
 * thing the rebalance was supposed to remove -- while the comment
 * claiming the outer blocks were flush was simply false.
 *
 * A BOOT LOG IS WHAT CAUGHT IT. A mute press logged at x=83, which is
 * inside 79..131 and nowhere near the 24..76 a flush icon occupies. The
 * arithmetic had been checked against itself and agreed with itself;
 * only the device knew where the icon actually was.
 *
 * So the dependency runs one way now. These two are the anchors, both
 * expressed against bar_x0()/bar_x1() and neither against the groove,
 * and vol_bounds() starts from where they end.
 */
static int spk_cx(void)
{
    return bar_x0() + SPK_HALF;
}

static int batt_cx(void)
{
    /* Drawn from cx - BATT_W/2 and extending BATT_NUB_W past
     * cx + BATT_W/2, so flush right means the NUB touches the content
     * edge, not the outline. */
    return bar_x1() - BATT_NUB_W - BATT_W / 2;
}

static void vol_bounds(int *x0, int *x1, int *y)
{
    /* Two gutters out of what the blocks leave. The /8 is not a typo
     * for /2: an eighth each side separates the blocks from the groove
     * without letting the gutters dominate the row. */
    const int gut = ((bar_x1() - bar_x0()) - VOL_BLOCKS) / 8;
    *x0 = spk_cx() + SPK_HALF + gut;
    *x1 = batt_cx() - BATT_W / 2 - gut;
    *y  = s_bar_top + VOL_Y;
}

/*
 * Row 7: the transport, alone.
 *
 * The pill is 168 wide against the disc's 92, so prev and next moved out
 * from +/-112 to +/-150 -- see SKIP_DX, where the arithmetic is, and
 * where a first attempt at +/-132 is recorded because it overlapped and
 * the layout test is what said so. Tighter than it was, and the reason
 * row 7 now has nothing else on it.
 */
static void play_centre(int *cx, int *cy)
{
    *cx = bar_cx();
    *cy = s_bar_top + ROW_Y;
}

static void prev_centre(int *cx, int *cy)
{
    *cx = bar_cx() - SKIP_DX;
    *cy = s_bar_top + ROW_Y;
}

static void next_centre(int *cx, int *cy)
{
    *cx = bar_cx() + SKIP_DX;
    *cy = s_bar_top + ROW_Y;
}

/*
 * Row 8: folder, gear, star, sleep, on ONE pitch.
 *
 * The old row put these in the gaps left over by the transport, at 64,
 * 156, 568 and s_w-64 -- four positions chosen one at a time so their
 * padded boxes cleared whatever was beside them, and a comment arguing
 * the row was full. With the transport on its own row there is nothing
 * to clear and nothing to argue: four centres, evenly spaced across the
 * content box, inset by the icon's half-width so no glyph overhangs.
 *
 * Pitch works out at (672 - 52) / 3 = 206 px, and every box is the same
 * distance from its neighbours, which is the thing the eye actually
 * checks.
 */
static void aux_centre(int idx, int *cx, int *cy)
{
    const int x0 = bar_x0() + ICON_HALF;
    const int span = (bar_x1() - ICON_HALF) - x0;

    /*
     * The pitch is computed once and multiplied, rather than
     * interpolating each centre across the span.
     *
     * Interpolating -- x0 + span*idx/3 -- truncates differently at each
     * index: a 620 px span gives gaps of 206, 207, 207, so the row is
     * a pixel out of true in two places. Nobody sees one pixel, but the
     * whole point of this row is that the spacing is equal, and a
     * comment claiming equal spacing over code that computes unequal
     * spacing is the kind of thing that stays wrong for years.
     *
     * So: an exact pitch, and the remainder spent on the leading margin
     * so the group stays centred in the box.
     */
    const int pitch = span / 3;
    const int lead = (span - pitch * 3) / 2;

    *cx = x0 + lead + pitch * idx;
    *cy = s_bar_top + AUX_Y;
}

static void folder_centre(int *cx, int *cy) { aux_centre(0, cx, cy); }
static void gear_centre(int *cx, int *cy)   { aux_centre(1, cx, cy); }
static void star_centre(int *cx, int *cy)   { aux_centre(2, cx, cy); }
static void moon_centre(int *cx, int *cy)   { aux_centre(3, cx, cy); }

/*
 * Row 3, both clocks, in one place because they are laid out against
 * each other.
 *
 * ark12 rather than seven segments -- see gfx_time_text(). The left one
 * sits at the content edge and the right one is MEASURED and
 * right-justified, because these do not zero-pad and so have no fixed
 * width: "2:41" is 84 px at scale 3 and "101:23" is 126.
 *
 * NULL for either draws dashes in its place. The dashes are wider than
 * a short time and narrower than a long one, so the right-hand run
 * shifts when a duration resolves. That is accepted: the alternative is
 * padding every clock to the widest form it could take, which puts a
 * leading zero on every track under ten minutes to keep a placeholder
 * still.
 */
#define CLOCK_SCALE (3)
#define CLOCK_DASH  "--:--"

static void draw_clocks(const char *elapsed, const char *remaining, uint16_t c)
{
    const int ty = s_bar_top + TIME_Y;
    const char *l = elapsed   ? elapsed   : CLOCK_DASH;
    const char *r = remaining ? remaining : CLOCK_DASH;

    gfx_draw_time_text(bar_x0(), ty, l, CLOCK_SCALE, c);
    gfx_draw_time_text(bar_x1() - gfx_time_text_w(r, CLOCK_SCALE), ty,
                       r, CLOCK_SCALE, c);
}

/* ------------------------------------------------------------------ */
/* Widgets                                                             */
/* ------------------------------------------------------------------ */

/* Defined with the battery's icons further down, because that is where
 * most of its users are; declared HERE, at the top of the widgets,
 * because C compiles top to bottom and this file has shipped a build
 * failure for exactly this before. The play toggle's trough is now the
 * first user and it is above all of them, which is why the declaration
 * moved up rather than staying beside the output icons. */
static void fill_rrect(int x, int y, int w, int h, int r, uint16_t c);

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

    /* Trough, then knob, then the glyph in it -- see PILL_W's note for
     * why this is a switch and not a button. */
    fill_rrect(cx - PILL_W / 2, cy - PILL_H / 2, PILL_W, PILL_H, PILL_H / 2,
               playing ? C_PLAY_ON : C_FILL);

    const int kx = cx + (playing ? 1 : -1) * (PILL_W / 2 - KNOB_R - 1);
    gfx_fill_circle(kx, cy, KNOB_R, C_THUMB);

    if (playing) {
        /* Play: a triangle, nudged right so it looks centred rather than
         * measuring centred. */
        for (int dy = -20; dy <= 20; dy++) {
            const int a = dy < 0 ? -dy : dy;
            gfx_fill_rect(kx - 11, cy + dy, 34 - (a * 34) / 20, 1, C_BG);
        }
    } else {
        /* Pause: two bars. */
        gfx_fill_rect(kx - 15, cy - 20, 10, 40, C_BG);
        gfx_fill_rect(kx + 5, cy - 20, 10, 40, C_BG);
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

    /*
     * The ink spans -w..+2+bar on the forward glyph and the mirror of
     * that on the back one, so a triangle drawn with its apex ON cx
     * sits visibly left of centre in its slot. Shifting by half the
     * difference centres what the eye sees rather than what the
     * arithmetic measures -- the same correction the play triangle
     * makes for the same reason.
     */
    const int off = (w - (7 + 2)) / 2;
    const int ax = forward ? cx + off : cx - off;

    for (int dy = -h; dy <= h; dy++) {
        const int a = dy < 0 ? -dy : dy;
        const int run = w - (a * w) / h;
        if (run <= 0) continue;
        if (forward) gfx_fill_rect(ax - w, cy + dy, run, 1, c);
        else         gfx_fill_rect(ax + w - run, cy + dy, run, 1, c);
    }

    /* The bar goes just past the apex -- the wall the tape stops against,
     * which is the convention every transport since a cassette deck has
     * used. Past the apex and not behind the base: behind the base it
     * reads as an underline on an arrow. */
    if (forward) gfx_fill_rect(ax + 2, cy - h, 7, 2 * h + 1, c);
    else         gfx_fill_rect(ax - 9, cy - h, 7, 2 * h + 1, c);
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

/*
 * The star, filled when this station is starred and a ring when it is
 * not. Nothing at all when there is no station -- see ui_state_t's
 * `fav`.
 *
 * The same shape the chooser's rows use, at row 7's icon size, so the
 * two stars are recognisably one mark. Both were two overlapping
 * triangles until 0932, which is a hexagram and not a star.
 *
 * Not shared code with browser.c's: that one punches its ring in the
 * row colour and this one in the panel background, which is the only
 * thing that differs, and a helper taking a background is longer than
 * the two lines it would save.
 */
static void draw_star_btn(int state)
{
    if (state == UI_FAV_HIDDEN) return;

    /*
     * Three looks, told apart at arm's length: solid gold for starred,
     * a thick gold ring for a file whose folder is starred, a thin
     * white ring for neither. The rings are the same star with a
     * smaller or larger one punched out of it in the background.
     */
    int cx, cy;
    star_centre(&cx, &cy);
    switch (state) {
    case UI_FAV_ON:
        gfx_fill_star(cx, cy, ICON_HALF, C_STAR);
        break;
    case UI_FAV_FOLDER:
        gfx_fill_star(cx, cy, ICON_HALF, C_STAR);
        gfx_fill_star(cx, cy, (ICON_HALF * 50) / 100, C_BG);
        break;
    default:
        gfx_fill_star(cx, cy, ICON_HALF, C_STAR_OFF);
        gfx_fill_star(cx, cy, (ICON_HALF * 82) / 100, C_BG);
        break;
    }
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
 *
 * The icon SHOWS the output route and the tap MUTES. Those are two
 * different things and it is worth being deliberate that they share a
 * control: the route is not a user choice on this device -- audio_out.c
 * arbitrates it and says why -- so there is nothing here for a tap to
 * cycle through. If a manual override is ever added it does not belong
 * on this tap, which already has a meaning people rely on; it belongs in
 * the settings panel's AUDIO tab, next to a note about the cases where
 * the override cannot be honoured.
 */

static void spk_centre(int *cx, int *cy)
{
    int x0, x1, y;
    vol_bounds(&x0, &x1, &y);
    (void)x0; (void)x1;             /* the row's y, not its groove */
    *cx = spk_cx();
    *cy = y;
}

/*
 * The icon in the volume row's left margin is the OUTPUT, not the
 * speaker.
 *
 * It used to be a speaker unconditionally, which was a picture of the
 * output only when the output happened to be the speaker. With
 * headphones in -- the common case -- the panel was drawing a device
 * that was deliberately silent. audio_out.c already arbitrates between
 * three of them and logs which one won; this draws the same answer.
 *
 * One icon, three shapes, rather than a speaker plus a badge saying what
 * it really is. Same argument draw_battery() makes for not drawing a
 * connector next to an outline: one icon, saying one thing.
 *
 * They share a centre and a colour, so mute keeps working as a slash
 * over whatever is current, and draw_rg() keeps hanging its badge
 * underneath without knowing which shape it is under.
 */


/* Headphones: a band and two cups. The cups hang below the band's ends
 * and are deeper than the band is thick, which is what stops the
 * silhouette reading as a croquet hoop. */
#define HP_R        (15)    /* band radius */
#define HP_BAND_T   (4)     /* band thickness */
#define HP_CUP_W    (8)
#define HP_CUP_H    (17)

/*
 * USB audio: the letters, not a picture.
 *
 * Two silhouettes were tried and are in the history -- the trident,
 * which collapses into a smudge at this size, and the A receptacle seen
 * end on, which does not. The receptacle is a perfectly good shape and
 * is still the wrong answer, for a reason that has nothing to do with
 * how it draws: a rectangle with a bar in it has to be learned before it
 * says anything, and what it says once learned is less than what is
 * actually known here. The route is not "the USB port". It is a UAC
 * device that enumerated, offered a format this player can clock, and
 * won the arbitration in audio_out.c. "UAC" says that, to anyone who
 * would know what a USB audio dongle is -- which is everyone who has
 * plugged one into this thing.
 *
 * It also stops competing with draw_usb_c(). A rounded stadium for the C
 * port and a squared rectangle for the A port are distinguishable side
 * by side, but they are two silhouettes to tell apart in a corner that
 * already asks the eye to read a battery.
 *
 * Scale 2 is forced, not chosen. Three glyphs at ark12's 7 px halfwidth
 * advance is 42 px, so 20 px either side of the centre, inside SPK_HALF
 * (26). Scale 3 would be 30 px either side: outside the hit box this
 * shares with the mute button, and into the slider's padded box at 82.
 */
#define UAC_SCALE   (2)

static void draw_out_speaker(int cx, int cy, uint16_t c)
{
    gfx_fill_rect(cx - 13, cy - 6, 9, 13, c);       /* body */
    for (int dx = 0; dx <= 15; dx++) {              /* cone, flaring right */
        const int half = 4 + dx;
        gfx_fill_rect(cx - 4 + dx, cy - half, 1, 2 * half + 1, c);
    }
}

static void draw_out_headphones(int cx, int cy, uint16_t c)
{
    const int top = cy - 12;

    /* The band, one column at a time off the circle equation -- gfx has
     * no arc primitive and this needs no line one. Upper half only: the
     * lower half is where a head would be. */
    for (int dx = -HP_R; dx <= HP_R; dx++) {
        const int dy = (int)(0.5f + __builtin_sqrtf((float)(HP_R * HP_R - dx * dx)));
        gfx_fill_rect(cx + dx, top + HP_R - dy, 1, HP_BAND_T, c);
    }

    fill_rrect(cx - HP_R - 1, top + HP_R - 2, HP_CUP_W, HP_CUP_H, 3, c);
    fill_rrect(cx + HP_R - HP_CUP_W + 2, top + HP_R - 2, HP_CUP_W, HP_CUP_H, 3, c);
}

static void draw_out_usb(int cx, int cy, uint16_t c)
{
    /* gfx_text_w() includes the gap column after the last glyph, which
     * nothing draws into. Centring on the measured width would sit the
     * run a pixel right of the speaker and headphones it alternates
     * with, and the three swap in place often enough for that to read as
     * a twitch. */
    const int run = gfx_text_w("UAC", UAC_SCALE) - UAC_SCALE;

    gfx_draw_text(cx - run / 2, cy - GFX_GLYPH_H(UAC_SCALE) / 2,
                  "UAC", UAC_SCALE, run + UAC_SCALE, c);
}

/*
 * Muted draws a slash through the icon, not a greyed one.
 *
 * Grey is what this file uses for "this button does nothing" -- next at
 * the end of a folder, the battery outline with no reading. Mute is the
 * opposite: the control is working and is the reason there is no sound.
 * A greyed icon would say the mute button is unavailable.
 */
static void draw_speaker(bool muted, audio_out_route_t route)
{
    int cx, cy;
    spk_centre(&cx, &cy);
    const uint16_t c = muted ? C_FILL : C_ICON;

    switch (route) {
    case AUDIO_OUT_USB:        draw_out_usb(cx, cy, c);        break;
    case AUDIO_OUT_HEADPHONES: draw_out_headphones(cx, cy, c); break;
    default:                   draw_out_speaker(cx, cy, c);    break;
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

/* ------------------------------------------------------------------ */
/* Row 2, when there is no position to draw there                      */
/* ------------------------------------------------------------------ */

/*
 * LIVE, and what the stream is doing.
 *
 * This occupies the envelope's band -- the 72 px above the seek baseline
 * -- because that is the row whose contents a stream cancels, and leaving
 * it empty would put a 72 px hole in the middle of the bar. The hole is
 * also the wrong statement: absence reads as something not having loaded
 * yet, and the whole point is that nothing is coming.
 *
 * A pill and not just the word. At scale 3 on this panel four glyphs in
 * grey is a caption, indistinguishable in weight from the artist row
 * below; filled, it is an indicator, which is what it is. The text inside
 * it is C_BG so the pill reads as lit rather than outlined.
 *
 * The status sits beside the badge rather than under it, on one baseline,
 * because the two are one sentence -- this is live, and here is what it is
 * doing about it. It is absent most of the time: streamplan_status()
 * returns NONE whenever sound is coming out, so the badge alone is the
 * normal state and any word next to it means there is no audio right now.
 *
 * Nothing here is a hit target. ui_touch() gates the seek drag on
 * can_seek, which play_stream() holds false for the life of the stream,
 * so the row is inert without a second check.
 */
#define LIVE_PAD_X  (14)    /* pill padding around the word */
#define LIVE_PAD_Y  (8)
#define LIVE_GAP    (24)    /* pill to status text */

/*
 * The level strip: a minute of output, a mark for now, and the reserve.
 *
 * THE ENVELOPE'S OWN TWO COLOURS, and that is the whole design argument.
 * A file's envelope says heard behind, not heard ahead, in C_WAVE_PAST
 * and C_WAVE_FUTURE. This strip says exactly the same thing about a
 * stream -- the only difference is that a stream's "ahead" is seconds of
 * buffer rather than minutes of file. Inventing a third palette for the
 * same statement would make two displays look unrelated when they are
 * the same idea. C_PLAYHEAD marks now, as it does there.
 *
 * The second red beside the LIVE pill is deliberate and separated by
 * LIVE_GAP. A pill with a word in it and a waveform are not confusable,
 * and consistency with the envelope is worth more than avoiding a
 * colour.
 *
 * SINGLE SIDEBAND, up from the baseline, and this used to be centred
 * and symmetric.
 *
 * The argument for symmetry was that it is what a waveform looks like
 * and that a bottom-anchored strip would read as a graph of something
 * else. The second half of that is true of a strip drawn on its own and
 * false of this one: the file envelope one row up in the same place is
 * drawn from the baseline by waveform_draw_bar(), and has been all
 * along. So the two displays of the same idea disagreed about which way
 * a level grows, and the stream was the odd one -- the mirrored half
 * was not carrying information either, only doubling what the top half
 * already said at the cost of halving its resolution.
 *
 * Drawn from the baseline the full height is available for the value
 * instead of half of it, which is worth more than the symmetry was. The
 * reserve keeps its flat-block fallback -- deliberately NOT
 * waveform-shaped, because nothing is known about what it sounds like,
 * only that it exists -- at a third of the height, which is the same
 * fraction of the strip it was before.
 */
static void draw_level_strip(const ui_state_t *st, int sx, int x1, int y)
{
    const int w = x1 - sx;
    if (w < LEVELHIST_COLUMNS / 4) return;   /* no room to say anything */

    const int top = y - UI_WAVE_H;
    const int full = UI_WAVE_H;

    for (int i = 0; i < LEVELHIST_COLUMNS; i++) {
        /* Both edges from the same expression, so columns tile exactly
         * and rounding never leaves a one-pixel gap between them. */
        const int cx0 = sx + (int)((int64_t)i * w / LEVELHIST_COLUMNS);
        const int cx1 = sx + (int)((int64_t)(i + 1) * w / LEVELHIST_COLUMNS);
        int cw = cx1 - cx0;
        if (cw < 1) cw = 1;

        if (i < LEVELHIST_NOW_COLUMN) {
            /* The LAST 160 columns of the minute, not the first. See
             * LEVELHIST_HISTORY_OFFSET: the newest sample is at the end
             * of the read, and it belongs beside the mark. */
            const int v = st->strip[LEVELHIST_HISTORY_OFFSET + i];
            if (!v) continue;
            /* At least one pixel for anything non-zero: the difference
             * between quiet and silent is the difference between playing
             * and not, and it must not round away. */
            int h = v * full / 255;
            if (h < 1) h = 1;
            gfx_fill_rect(cx0, y - h, cw, h, C_WAVE_PAST);
            continue;
        }

        if (i == LEVELHIST_NOW_COLUMN) {
            gfx_fill_rect(cx0, top, cw < 2 ? 2 : cw, UI_WAVE_H, C_PLAYHEAD);
            continue;
        }

        if (i - LEVELHIST_NOW_COLUMN <= st->strip_ahead_cols) {
            /*
             * The reserve, drawn as its own waveform when there is one.
             *
             * Same geometry as the history above and the same units --
             * the publisher scales these by the applied gain so the two
             * halves meet at the mark without a step -- so the strip is
             * one continuous shape through "now" rather than a picture
             * and a bar. A silence or an ad break in the buffer is then
             * visible before it is audible, which is the whole point of
             * drawing the future at all.
             *
             * A zero column means no measurement rather than silence:
             * a file, or a stream whose carrier could not be allocated.
             * That draws the flat band this used to be, so the strip
             * degrades to its old self instead of to an empty one.
             */
            const int v = st->strip_ahead[i - LEVELHIST_NOW_COLUMN - 1];
            int h = v ? (v * full / 255) : (UI_WAVE_H / 3);
            if (h < 1) h = 1;
            gfx_fill_rect(cx0, y - h, cw, h, C_WAVE_FUTURE);
        }
    }

    /*
     * The reserve is deeper than the strip can show, so say so rather
     * than let the edge imply it stops there. The edge is exactly where
     * a listener looks to see whether it is still growing.
     */
    if (st->strip_clipped) {
        gfx_fill_rect(x1 - 3, top, 3, UI_WAVE_H, C_WAVE_FUTURE);
    }
}

static void draw_live(const ui_state_t *st)
{
    int x0, x1, y;
    seek_bounds(&x0, &x1, &y);

    const int tw = gfx_text_w("LIVE", 3);
    const int pw = tw + 2 * LIVE_PAD_X;
    const int ph = GFX_GLYPH_H(3) + 2 * LIVE_PAD_Y;
    /* Centred in the band the envelope would have stood in, not sat on
     * the baseline: the baseline is where a waveform's floor is, and
     * there is no waveform to share a floor with. */
    const int py = y - UI_WAVE_H / 2 - ph / 2;

    fill_rrect(bar_x0(), py, pw, ph, ph / 2, C_LIVE);
    gfx_draw_text(bar_x0() + LIVE_PAD_X, py + LIVE_PAD_Y, "LIVE", 3, pw, C_BG);

    /*
     * THE MINUTE, in what is left of the band.
     *
     * Starts after the pill rather than under it, which is the layout
     * chosen at the board: LIVE keeps the left. It costs about eight
     * seconds off the oldest end of the history, which is the cheapest
     * eight seconds on the strip -- the question is always what the
     * reserve is doing NOW and what it did a moment ago.
     *
     * Not drawn while there is a status to show. Those two cannot share
     * the space, and when the player has something to say about why
     * there is no sound, that outranks a minute of how loud it used to
     * be -- which at that moment is a minute of silence anyway.
     */
    const int sx0 = bar_x0() + pw + LIVE_GAP;
    if ((!st->stream_status || !*st->stream_status) && st->strip_valid) {
        draw_level_strip(st, sx0, x1, y);
        return;
    }

    if (!st->stream_status || !*st->stream_status) return;

    /*
     * C_THUMB, the same white as the title, and deliberately not the
     * artist row's grey. While this line is on screen it is the answer to
     * the only question being asked -- why is there no sound -- and grey
     * at this size is the weight of a detail.
     *
     * One colour for all four states. "No signal" is terminal and the
     * other three are not, and that difference is worth drawing, but it
     * cannot be drawn from here: this is a string, by design, and the
     * distinction would have to arrive as its own flag rather than be
     * recovered by comparing prose in a draw call.
     */
    const int sx = bar_x0() + pw + LIVE_GAP;
    gfx_draw_text(sx, py + LIVE_PAD_Y, st->stream_status, 3,
                  x1 - sx, C_THUMB);

    /* 5067: waiting for the network. After the words, on the pill's
     * centre line, turning on the clock; this bar is redrawn every frame
     * while streaming, so nothing else has to ask for it. */
    if (st->stream_spinner) {
        const int r = GFX_GLYPH_H(3) / 2 + 4;
        const int cx = sx + gfx_text_w(st->stream_status, 3) + LIVE_GAP + r;
        if (cx + r <= x1) {
            gfx_draw_spinner(cx, py + ph / 2, r,
                             (uint32_t)(esp_timer_get_time() / 1000),
                             C_THUMB, C_ALBUM);
        }
    }
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
    (void)x0; (void)x1;             /* the row's y, not its groove */
    const int cx = batt_cx(), cy = y;

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
    /* The card lived on the artwork, so painting the artwork out takes
     * it. Dropped here rather than left for the caller because every
     * caller would have to remember, and one that forgot would leave
     * ui_notice_hit() answering for a card nobody can see. */
    s_notice_up = false;

    if (!s_fb) return;
    int aw, ah;
    ui_art_band(NULL, NULL, &aw, &ah);
    gfx_fill_rect(0, 0, aw, ah, C_BG);
    ui_blit_art();
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

    /*
     * This paints the whole square, so a card that was up is gone from
     * the screen -- and if the flag stayed set, ui_notice_hit() would
     * go on answering for a card nobody can see, and the artwork tap
     * swallow would never lift. ui_clear_art() drops it for the same
     * reason. ANY function that paints over this square has to.
     */
    s_notice_up = false;

    /* The artwork BAND, which is the full width and 560 tall in
     * portrait and 560 wide and full height in landscape. Centring on
     * the panel instead would put these lines under the controls. */
    int aw, ah;
    ui_art_band(NULL, NULL, &aw, &ah);
    gfx_fill_rect(0, 0, aw, ah, C_BG);

    int total = GFX_GLYPH_H(ART_INFO_HEAD_SCALE);
    for (int i = 1; i < n; i++) {
        total += ART_INFO_GAP + GFX_GLYPH_H(ART_INFO_BODY_SCALE);
    }

    int y = (ah - total) / 2;
    if (y < 0) y = 0;

    for (int i = 0; i < n; i++) {
        const char *t = lines[i] ? lines[i] : "";
        const int scale = i ? ART_INFO_BODY_SCALE : ART_INFO_HEAD_SCALE;
        const int w = gfx_text_w(t, scale);
        int x = (aw - w) / 2;
        if (x < TEXT_X) x = TEXT_X;
        gfx_draw_text(x, y, t, scale, aw - 2 * TEXT_X,
                      i ? C_ICON : C_THUMB);
        y += GFX_GLYPH_H(scale) + ART_INFO_GAP;
    }

    ui_blit_art();
}

/* ------------------------------------------------------------------ */
/* The notice card                                                      */
/* ------------------------------------------------------------------ */

/*
 * Lighter than C_BG so the card reads as something laid ON the screen
 * rather than a hole in it, and edged so it still has a boundary
 * against a dark cover. C_NOTICE_WARN is the head colour for a
 * dismissible card: something went wrong is worth a different colour
 * from an address somebody asked to see.
 */
#define C_NOTICE_BG     RGB(0x26, 0x26, 0x26)
#define C_NOTICE_EDGE   RGB(0x4A, 0x4A, 0x4A)
#define C_NOTICE_WARN   RGB(0xE8, 0x9A, 0x3C)

/*
 * The card's inset from the artwork square, and its padding.
 *
 * MOST of the art and not all of it: a margin is what says the cover is
 * still there behind this. 56 px each side leaves 608 of 720, which is
 * wide enough for an address at body scale with room to spare.
 */
#define NOTICE_INSET    (56)
#define NOTICE_PAD      (40)
#define NOTICE_HEAD_SC  (4)
#define NOTICE_BODY_SC  (3)
#define NOTICE_GAP      (20)
#define NOTICE_CLOSE    (22)    /* half-width of the close box */

bool ui_notice_active(void)      { return s_notice_up; }
bool ui_notice_dismissible(void) { return s_notice_up && s_notice_dismissible; }
void ui_notice_clear(void)       { s_notice_up = false; }

bool ui_notice_hit(int x, int y)
{
    if (!s_notice_up || !s_notice_dismissible) return false;
    return x >= s_notice_x && x < s_notice_x + s_notice_w &&
           y >= s_notice_y && y < s_notice_y + s_notice_h;
}

/* An x, for the close box. Two strokes, drawn as a stack of short rows
 * so that gfx needs no line primitive -- the same trick the folder and
 * the note icons use. */
static void draw_close(int cx, int cy, int r, uint16_t c)
{
    for (int i = -r; i <= r; i++) {
        gfx_fill_rect(cx + i - 1, cy + i - 1, 3, 3, c);
        gfx_fill_rect(cx + i - 1, cy - i - 1, 3, 3, c);
    }
}

void ui_show_notice(const char *head, const char *const *body, int n,
                    bool dismissible)
{
    if (!s_fb || !head) return;
    if (n < 0) n = 0;
    if (n > 4) n = 4;   /* see ui.h: four body lines, and the card is sized
                         * for them rather than growing off the square */

    /* Height from the content, so a one-line card is not a tall box
     * with a sentence floating in it. */
    int text_h = GFX_GLYPH_H(NOTICE_HEAD_SC);
    for (int i = 0; i < n; i++) {
        text_h += NOTICE_GAP + GFX_GLYPH_H(NOTICE_BODY_SC);
    }

    int aw, ah;
    ui_art_band(NULL, NULL, &aw, &ah);

    s_notice_w = aw - 2 * NOTICE_INSET;
    s_notice_h = text_h + 2 * NOTICE_PAD;
    s_notice_x = NOTICE_INSET;
    s_notice_y = (ah - s_notice_h) / 2;
    if (s_notice_y < NOTICE_INSET) s_notice_y = NOTICE_INSET;
    /* A card taller than the square is clamped rather than allowed to
     * run under the transport bar, which it would otherwise cover. */
    if (s_notice_h > ah - 2 * NOTICE_INSET) {
        s_notice_h = ah - 2 * NOTICE_INSET;
    }

    gfx_fill_rect(s_notice_x, s_notice_y, s_notice_w, s_notice_h, C_NOTICE_EDGE);
    gfx_fill_rect(s_notice_x + 2, s_notice_y + 2, s_notice_w - 4,
                  s_notice_h - 4, C_NOTICE_BG);

    int y = s_notice_y + NOTICE_PAD;
    const int avail = s_notice_w - 2 * NOTICE_PAD;

    const int hw = gfx_text_w(head, NOTICE_HEAD_SC);
    int hx = s_notice_x + (s_notice_w - hw) / 2;
    if (hx < s_notice_x + NOTICE_PAD) hx = s_notice_x + NOTICE_PAD;
    gfx_draw_text(hx, y, head, NOTICE_HEAD_SC, avail,
                  dismissible ? C_NOTICE_WARN : C_THUMB);
    y += GFX_GLYPH_H(NOTICE_HEAD_SC) + NOTICE_GAP;

    for (int i = 0; i < n; i++) {
        const char *t = body && body[i] ? body[i] : "";
        const int tw = gfx_text_w(t, NOTICE_BODY_SC);
        int tx = s_notice_x + (s_notice_w - tw) / 2;
        if (tx < s_notice_x + NOTICE_PAD) tx = s_notice_x + NOTICE_PAD;
        gfx_draw_text(tx, y, t, NOTICE_BODY_SC, avail, C_ICON);
        y += GFX_GLYPH_H(NOTICE_BODY_SC) + NOTICE_GAP;
    }

    if (dismissible) {
        draw_close(s_notice_x + s_notice_w - NOTICE_PAD,
                   s_notice_y + NOTICE_PAD, NOTICE_CLOSE / 2, C_ICON);
    }

    s_notice_up = true;
    s_notice_dismissible = dismissible;

    /* The whole square, for ui_show_art_info()'s reason: this blits
     * itself rather than waiting for ui_draw(), which never comes up
     * this far. */
    ui_blit_art();
}

esp_err_t ui_init(esp_lcd_panel_handle_t panel, int w, int h)
{
    /* gfx owns the framebuffer lookup now; this file keeps its own copies
     * of the geometry because every layout function reads them. */
    ESP_RETURN_ON_ERROR(gfx_init(panel, w, h), TAG, "gfx");
    s_fb = gfx_fb();
    ui_relayout();
    return ESP_OK;
}

/*
 * The square's origin, and everything derived from it.
 *
 * One function, called at init and after every rotation change, so there
 * is exactly one place that knows where the square is. Every bound in
 * this file is s_bar_x/s_bar_top plus a constant from the row table --
 * which is why the rows did not have to be written twice.
 */
void ui_relayout(void)
{
    s_w = gfx_w();
    s_h = gfx_h();
    s_landscape = (s_w > s_h);

    if (s_landscape) {
        /* Square on the right, artwork in the 560-wide column left of
         * it. s_bar_top is 0: the square is the full height. */
        s_bar_x = s_w - UI_SQUARE;
        s_bar_top = 0;
    } else {
        /* Square at the bottom, artwork in the 560-tall band above. */
        s_bar_x = 0;
        s_bar_top = s_h - UI_SQUARE;
    }
}

bool ui_landscape(void) { return s_landscape; }

void ui_art_band(int *x, int *y, int *w, int *h)
{
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = s_landscape ? (s_w - UI_SQUARE) : s_w;
    if (h) *h = s_landscape ? s_h : (s_h - UI_SQUARE);
}

/*
 * Blit the artwork region.
 *
 * In portrait this is rows 0..UI_ART_H-1 and nothing else moves. In
 * landscape the artwork is a COLUMN, and gfx_blit() takes rows -- so the
 * whole screen goes, because there is no way to send part of a row. That
 * is the honest cost of the square: a cover change in landscape repaints
 * the controls too. It happens once per track, and gfx splits it into
 * bands on the way out.
 */
esp_err_t ui_blit_art_err(void)
{
    int ay, ah;
    ui_art_band(NULL, &ay, NULL, &ah);
    /* Landscape sends every row: the artwork is a column, and there is
     * no way to send part of a row. The controls ride along. */
    if (s_landscape) return gfx_blit_err(0, s_h);
    return gfx_blit_err(ay, ay + ah);
}

void ui_blit_art(void)
{
    (void)ui_blit_art_err();
}

/*
 * And the control square, for the same reason in the other direction.
 *
 * Portrait: rows s_bar_top..s_h, which is the bar and nothing else.
 * Landscape: the square is a column, so every row goes -- the artwork
 * rides along. That is the cost named in ui_blit_art(), paid at 25 Hz
 * here rather than once a track, and it is why landscape is the angle
 * to measure if the panel ever underruns.
 */
void ui_blit_bar(void)
{
    if (s_landscape) gfx_blit(0, s_h);
    else             gfx_blit(s_bar_top, s_h);
}

/* Whether a logical point is over the artwork rather than the square. */
bool ui_in_art(int x, int y)
{
    int aw, ah;
    ui_art_band(NULL, NULL, &aw, &ah);
    return x < aw && y < ah;
}

void ui_draw(const ui_state_t *st)
{
    if (!s_fb) return;

    if (st->screen_off) {
        /* Nothing at all -- the backlight is off, and drawing into a dark
         * panel just burns PSRAM bandwidth the decoder wants. */
        return;
    }

    gfx_fill_rect(s_bar_x, s_bar_top, UI_SQUARE, UI_SQUARE, C_BG);

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

    if (st->live) {
        /* No position exists, so none of the three states below applies
         * -- not even the bare groove, which claims there is a position
         * and that it is unknown. See ui_state_t::live. */
        draw_live(st);
    } else if (stats && waveform_ready() && st->len_sec > 0) {
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
     *
     * Both are drawn by draw_clocks(), which owns the row's layout --
     * the right-hand one is measured, not offset from a constant.
     */
    if (st->live) {
        /*
         * Nothing, and blank rather than dashed.
         *
         * Dashes are this bar's way of saying a number is not known yet,
         * and they are right for a file being scanned -- the length
         * exists and is being looked for. A live stream has no length to
         * find and no end to count down to, so two dashed clocks would
         * report a lookup that is never going to finish. The badge one
         * row up has already said why the row is empty.
         *
         * How long this station has been playing IS a real number and is
         * arguably worth the left-hand slot. It is not free: nothing
         * counts it today, so it needs a counter in play_stream() and a
         * decision about whether a reconnect resets it. Left for its own
         * patch rather than guessed at in this one.
         */
    } else if (!stats) {
        /* Both clocks, both dashed. The elapsed one especially: it is
         * the number that was counting a moment ago, and leaving it at
         * the old track's value for the length of an open is the single
         * most convincing way to look like the press did nothing. */
        draw_clocks(NULL, NULL, C_TRACK);
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
            const uint32_t left = (st->len_sec > shown_sec)
                                ? st->len_sec - shown_sec : 0;
            char el[GFX_TIME_TEXT_MAX], rem[GFX_TIME_TEXT_MAX];
            gfx_time_text(el, sizeof(el), shown_sec, false);
            gfx_time_text(rem, sizeof(rem), left, true);
            draw_clocks(el, rem, clock_c);
        } else {
            /* No duration: dashes on both, matching the bare groove.
             * The elapsed number is real and is still withheld, because
             * on its own beside a blank it is an invitation to work out
             * what is left, which is the one thing not known. */
            draw_clocks(NULL, NULL, C_TRACK);
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
    /* The square's content box, not the panel. In landscape the panel
     * is 1280 wide and the text is 672 -- measuring against the panel
     * would stop the marquee ever running and let long titles run out
     * over the artwork. */
    const int win_w = bar_x1() - bar_x0();
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

    gfx_draw_text_clipped(bar_x0() - s_marq_off, s_bar_top + TITLE_Y,
                          bar_x0(), win_w, title, 3, C_THUMB);

    if (st->album && *st->album) {
        gfx_draw_text(bar_x0(), s_bar_top + ALBUM_Y, st->album, 3, win_w, C_ALBUM);
    }
    if (st->artist && *st->artist) {
        gfx_draw_text(bar_x0(), s_bar_top + ARTIST_Y, st->artist, 3, win_w, C_ICON);
    }

    /*
     * The ICY title, on the album row, when there is one.
     *
     * The row is free: a stream has no tags, and since the previous patch
     * play_stream() clears the ones the last file left behind, so the two
     * conditions above are already false here. Gated on `live` anyway --
     * the flag, not the emptiness of the other rows -- so that a file can
     * never draw this line no matter what a future path leaves in the
     * field.
     *
     * The album row rather than the title row, keeping the order
     * streamplan_lines() already chose: top is the station, bottom is the
     * title. The station is what was chosen and what stays; this changes
     * underneath it.
     *
     * C_ICON and not C_ALBUM. Three levels of grey exist to rank three
     * rows against each other and there are two rows here, so the dimmest
     * of the three is ranking this against nothing -- and a now-playing
     * line is the most interesting string on a radio screen, not the
     * least.
     *
     * Clipped with an ellipsis rather than given the marquee. It is the
     * string most likely to need one -- "Artist - Title (Remastered)"
     * overruns 19 characters easily -- but there is one marquee and the
     * title row owns it, and handing it to whichever line is longest is a
     * change to how the marquee is owned. Not in the patch that first
     * draws this line.
     */
    if (st->live && st->stream_title && *st->stream_title) {
        gfx_draw_text(bar_x0(), s_bar_top + ALBUM_Y, st->stream_title, 3,
                      win_w, C_ICON);
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

    draw_speaker(st->muted, st->route);
    draw_rg(st);
    draw_battery(st->battery_pct, st->battery_charging, st->ext_power);
    draw_folder();
    draw_gear();
    draw_star_btn(st->fav);
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

    ui_blit_bar();
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

    /*
     * The card, first, because it is drawn over everything above the
     * bar and a control that is covered must not still be pressable.
     *
     * Only on the tap edge, and only for a dismissible card: a
     * persistent one is describing something still happening, and a tap
     * that made the address vanish while the server was still up would
     * leave no way back to it.
     *
     * Ahead of the drag branch as well -- a drag cannot have started
     * under a card, since this returns before any drag could begin.
     */
    if (tapped && ui_notice_hit(x, y)) {
        act.kind = UI_ACTION_DISMISS_NOTICE;
        return act;
    }

    /*
     * A tap anywhere on the artwork while a card is up does nothing.
     * There is nothing under there to press -- the cover is not a
     * control -- and swallowing it means a person jabbing at a card
     * that cannot be dismissed does not also do something else.
     */
    if (s_notice_up && ui_in_art(x, y)) return act;

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

    /*
     * The toggle's box is the PILL, not the disc it replaced.
     *
     * in_box() takes one half-extent because every other control on
     * this row is square; the pill is 168x92 and a BTN_R box would be a
     * 92 px target inside a 168 px control -- the 38 px either end that
     * look pressable would not be. Written out rather than adding a
     * rectangular in_box() for one caller.
     */
    play_centre(&cx, &cy);
    if (x >= cx - PILL_W / 2 - HIT_PAD_X && x <= cx + PILL_W / 2 + HIT_PAD_X &&
        y >= cy - PILL_H / 2 - HIT_PAD_Y && y <= cy + PILL_H / 2 + HIT_PAD_Y) {
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

    /*
     * The star, and only when there is one. With no station the box is
     * not tested at all, so the gap between next and the moon is dead
     * space rather than an invisible button -- which is what a hidden
     * control that still answers would be.
     */
    if (st->fav != UI_FAV_HIDDEN) {
        star_centre(&cx, &cy);
        if (in_box(x, y, cx, cy, ICON_HALF)) {
            act.kind = UI_ACTION_FAVORITE;
            return act;
        }
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
