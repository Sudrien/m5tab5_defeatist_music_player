/*
 * browser.c -- the file chooser.
 *
 * SPDX-License-Identifier: MIT
 */

#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "browser.h"
#include "decoder.h"
#include "gfx.h"
#include "storage.h"

static const char *TAG = "tab5_browser";

#define C_BG        RGB(0x0C, 0x0C, 0x0C)
#define C_ROW       RGB(0x1A, 0x1A, 0x1A)
#define C_ROW_ALT   RGB(0x14, 0x14, 0x14)
#define C_TEXT      RGB(0xEE, 0xEE, 0xEE)
#define C_DIM       RGB(0x77, 0x77, 0x77)
#define C_DISABLED  RGB(0x44, 0x44, 0x44)
#define C_ACCENT    RGB(0xD1, 0x3B, 0x2C)
#define C_TAB_ON    RGB(0x22, 0x22, 0x22)
#define C_TAB_OFF   RGB(0x10, 0x10, 0x10)
#define C_BTN       RGB(0x26, 0x26, 0x26)
#define C_RULE      RGB(0x33, 0x33, 0x33)
/* Between C_RULE and C_DIM. The prefix note has to be readable when
 * looked for and easy to look past when not, which neither of the
 * existing greys manages -- one disappears, the other competes with the
 * path above it. */
#define C_FAINT     RGB(0x55, 0x55, 0x55)

/* Layout. Sized for the same 294 PPI the transport bar is: a 88 px row is
 * about 7.5 mm, which is a comfortable thumb target, and the 27 px text in
 * it is the same height as the artist line on the player. */
#define TAB_H       (96)
#define PATH_H      (60)
#define LIST_TOP    (TAB_H + PATH_H)
#define FOOT_H      (120)
#define ROW_H       (88)

/*
 * The scrollbar: how wide it is drawn, and how much wider it is to hit.
 *
 * The two differ on purpose. A 16 px bar is the right weight next to
 * 64 px rows; a 16 px target is not, at 294 PPI with a fingertip. The
 * grab zone is the outer 72 px of the list, which is dead space in every
 * row anyway -- filenames are drawn from the left and the ones long
 * enough to reach the right edge are already being clipped there.
 */
#define SCROLL_W        (16)
#define SCROLL_HIT_W    (72)
#define NAME_SCALE  (3)
#define LABEL_SCALE (2)

/* Entries held for the open directory. Beyond this the listing is
 * truncated, for the same reason PLAYLIST_MAX exists: this is a strdup
 * per name on a touch event. */
#define MAX_ENTRIES (512)

typedef struct {
    char *name;
    bool is_dir;
} entry_t;

static bool s_open;
static bool s_dirty;
static bool s_was_down;

static storage_id_t s_tab = STORAGE_SD;
static char s_dir[512];
static char s_result[512];

static entry_t *s_entries;
static int s_count;
static int s_top;               /* first visible row */

/*
 * A drag on the scrollbar, in progress.
 *
 * The bar was drawn from the first version and has never been touchable:
 * eight pixels of decoration reporting a position on a list that could
 * only be moved a page at a time from the footer. On a 9-track folder
 * that is a curiosity. On a folder with two hundred files in it, it is
 * twenty-two presses to reach the end of something the bar has been
 * showing you the shape of the whole time.
 *
 * Held across polls because a drag is not a press: browser_touch() is
 * otherwise built entirely on the down edge, which is right for buttons
 * and useless for anything that has to follow a finger.
 */
static bool s_scroll_drag;

/* The shared leading text of this folder's filenames, and its length.
 * Declared up here rather than beside find_common_prefix() because
 * entries_free() has to be able to drop it. */
static char s_prefix[128];
static int  s_prefix_len;

static play_order_t s_order = PLAY_ORDER_ALL;
static uint32_t s_seen_generation = UINT32_MAX;

play_order_t browser_order(void) { return s_order; }
bool browser_is_open(void) { return s_open; }

static int rows_visible(void)
{
    return (gfx_h() - FOOT_H - LIST_TOP) / ROW_H;
}

/*
 * The scrollbar's geometry, in one place.
 *
 * Drawing and hit-testing computed this independently in the first
 * version of this patch and it was wrong within a day -- the thumb was
 * drawn from LIST_TOP and grabbed from the top of the screen, so the
 * finger led the bar by the height of the tab strip. One function, two
 * callers, no chance to drift.
 *
 * Returns false when the list fits, which is also when there is nothing
 * to drag.
 */
static bool scroll_geom(int *track_y, int *track_h, int *bar_h, int *max_top)
{
    const int rows = rows_visible();
    if (s_count <= rows) return false;

    *track_y = LIST_TOP;
    *track_h = rows * ROW_H;
    *bar_h = (*track_h * rows) / s_count;
    if (*bar_h < 24) *bar_h = 24;
    *max_top = s_count - rows;
    return true;
}

/* ------------------------------------------------------------------ */
/* Directory listing                                                   */
/* ------------------------------------------------------------------ */

static void entries_free(void)
{
    for (int i = 0; i < s_count; i++) free(s_entries[i].name);
    s_count = 0;
    /* The prefix belonged to those names. A volume going away without a
     * reload -- which is what calls this -- would otherwise leave the
     * header claiming an elision over an empty list. */
    s_prefix[0] = '\0';
    s_prefix_len = 0;
}

/* Folders first, then files, each run sorted case-insensitively. Mixing
 * them alphabetically buries a disc subfolder in the middle of the track
 * list, and the two are different kinds of thing to tap. */
static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

/* Defined below, next to the rules it enforces; called from the bottom
 * of load_dir() because it can only run once the listing is complete
 * and sorted. */
static void find_common_prefix(void);

static void load_dir(const char *dir)
{
    if (!s_entries) {
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_entries) {
            ESP_LOGE(TAG, "out of memory for the listing");
            return;
        }
    }

    entries_free();
    s_top = 0;
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    s_dirty = true;

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "cannot open %s", dir);
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < MAX_ENTRIES) {
        const bool is_dir = (e->d_type == DT_DIR);
        /* Everything the decoder cannot open is hidden rather than
         * greyed. A card root is mostly System Volume Information and
         * stray .txt files, and a list where two thirds of the rows are
         * untappable is a worse list. */
        if (storage_is_hidden(e->d_name)) continue;   /* . .. and dotfiles */
        if (!is_dir && !decoder_supports(e->d_name)) continue;

        s_entries[s_count].name = strdup(e->d_name);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = is_dir;
        s_count++;
    }
    closedir(d);

    qsort(s_entries, (size_t)s_count, sizeof(entry_t), cmp_entry);
    find_common_prefix();
}

/*
 * The bit of every filename in this folder that is not telling you
 * anything.
 *
 * Ripped albums come off a CD as
 *
 *   Advent_Chamber_Orchestra_-_04_-_Mozart_-_Eine_Kleine_Nachtmusik.mp3
 *
 * and a folder of them is nine rows that agree for the first
 * twenty-seven characters. At NAME_SCALE the row holds about forty, so
 * the artist and the album spend two thirds of every line repeating
 * what the folder name above them already says, and the part that
 * differs -- the track number and the title, the only reason to look at
 * the list -- is pushed off the right edge and clipped.
 *
 * So it is found once per folder and dropped from the drawing. THE
 * ENTRIES ARE NOT MODIFIED: s_entries[i].name stays the real filename,
 * because it is what gets opened, what is compared against the playing
 * track, and what the playlist is built from. Only the row's starting
 * offset moves.
 *
 * WHERE IT STOPS
 *
 * At a separator, never mid-word. The raw common prefix of 04_ and 05_
 * includes the "0", which would leave rows reading "4_-_Mozart" and
 * "5_-_Handel" -- technically shorter and actively worse, because a
 * truncation that lands inside a number reads as data loss rather than
 * as tidying. Backing up to the last _ - . or space costs a couple of
 * characters and keeps every row starting on something whole.
 *
 * WHEN IT DECLINES
 *
 * Fewer than two files, a prefix shorter than MIN_PREFIX, or any file
 * left with almost nothing after it. That last one is the case that
 * matters: a folder holding 01.mp3 through 09.mp3 has a common prefix
 * of "0", and hiding it turns a legible list into the digits 1 to 9.
 * The point is to remove what is redundant, not to remove as much as
 * possible.
 *
 * Directories are neither counted nor shortened. A subfolder is not part
 * of the album's naming scheme and the prefix rarely applies to it.
 */
#define MIN_PREFIX      (8)     /* below this it is not worth the elision */
#define MIN_REMAINDER   (4)     /* what every row must still have left */

static bool is_sep(char c)
{
    return c == '_' || c == '-' || c == '.' || c == ' ';
}

static void find_common_prefix(void)
{
    s_prefix[0] = '\0';
    s_prefix_len = 0;

    const char *first = NULL;
    int files = 0, lcp = 0;

    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].is_dir) continue;
        const char *n = s_entries[i].name;
        if (!first) { first = n; lcp = (int)strlen(n); files = 1; continue; }
        files++;

        int k = 0;
        while (k < lcp && first[k] && n[k] == first[k]) k++;
        lcp = k;
        if (lcp < MIN_PREFIX) return;   /* cannot recover; nothing to do */
    }

    if (files < 2 || lcp < MIN_PREFIX) return;

    /* Back up to a separator, and include it -- the row should start on
     * the next real character, not on the underscore before it. */
    while (lcp > 0 && !is_sep(first[lcp - 1])) lcp--;
    if (lcp < MIN_PREFIX) return;

    /* Every row has to be left with something worth reading. */
    for (int i = 0; i < s_count; i++) {
        if (s_entries[i].is_dir) continue;
        if ((int)strlen(s_entries[i].name) - lcp < MIN_REMAINDER) return;
    }

    if (lcp >= (int)sizeof(s_prefix)) lcp = (int)sizeof(s_prefix) - 1;
    memcpy(s_prefix, first, (size_t)lcp);
    s_prefix[lcp] = '\0';
    s_prefix_len = lcp;
    ESP_LOGI(TAG, "hiding a shared prefix on %d files: \"%s\"", files, s_prefix);
}

/* Path of the volume root for the active tab. */
static const char *tab_root(void) { return storage_mount_path(s_tab); }

static bool at_root(void)
{
    return strcmp(s_dir, tab_root()) == 0;
}

static void go_up(void)
{
    if (at_root()) return;
    char up[512];
    snprintf(up, sizeof(up), "%s", s_dir);
    char *slash = strrchr(up, '/');
    if (!slash) return;
    if (slash == up) return;
    *slash = '\0';
    load_dir(up);
}

/*
 * An absent volume's tab is still selectable, which is the one place
 * this differs from a normal tab strip. The grey means "nothing here
 * yet", not "not a button".
 *
 * It used to mean more than that: selecting the USB tab was what powered
 * the port, so the tab HAD to be selectable or there was no way to ask.
 * The port comes up at boot now -- a USB audio device cannot announce
 * itself through a dark port, and waiting for someone to open the
 * chooser was never a sensible gate on that -- so this is back to being
 * an ordinary reason rather than a load-bearing one. The tab stays
 * selectable anyway: a tab that ignores taps while the drive spins up
 * reads as a lost tap.
 */
static void select_tab(storage_id_t id)
{
    const bool same = (id == s_tab);
    s_tab = id;

    if (!storage_present(id)) {
        entries_free();
        s_dir[0] = '\0';
        s_dirty = true;
        return;
    }

    if (same && s_count > 0) return;
    load_dir(storage_mount_path(id));
}

void browser_open(const char *start)
{
    s_open = true;
    s_was_down = false;
    /* A drag cannot survive the screen it was on. Left set, the first
     * press after reopening would be treated as the continuation of a
     * gesture that ended somewhere else. */
    s_scroll_drag = false;
    s_dirty = true;
    s_seen_generation = storage_generation();

    /* Reopen where the current track lives, when that volume is still
     * there. Coming back to the root of the card every time is the thing
     * that makes a chooser tedious on an album you are picking through. */
    storage_id_t want = STORAGE_COUNT;
    if (start && *start) want = storage_of_path(start);

    if (want < STORAGE_COUNT && storage_present(want)) {
        s_tab = want;
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", start);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) *slash = '\0';
        load_dir(dir);
        return;
    }

    for (int i = 0; i < STORAGE_COUNT; i++) {
        if (storage_present((storage_id_t)i)) {
            s_tab = (storage_id_t)i;
            load_dir(storage_mount_path(s_tab));
            return;
        }
    }

    /* Nothing mounted. Both tabs grey, no rows. */
    entries_free();
    s_dir[0] = '\0';
}

void browser_close(void)
{
    s_open = false;
    entries_free();
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static const char *status_line(void)
{
    if (s_dir[0]) return s_dir;
    if (s_tab == STORAGE_USB) {
        /* The false branch is a few milliseconds of bring-up at boot,
         * not a state anyone can tap their way into any more. */
        return storage_usb_powered() ? "USB port on - waiting for a drive"
                                     : "USB port coming up";
    }
    return "no card in the slot";
}

static void draw_tab(storage_id_t id, int x, int w)
{
    const bool present = storage_present(id);
    /* Selected is selected even with nothing mounted: the USB tab can be
     * the active one while the port is still coming up, and a strip with
     * no underline at all would read as a lost tap. */
    const bool active = (id == s_tab);

    gfx_fill_rect(x, 0, w, TAB_H, active ? C_TAB_ON : C_TAB_OFF);
    if (active) gfx_fill_rect(x, TAB_H - 5, w, 5, present ? C_ACCENT : C_DISABLED);

    const char *label = storage_label(id);
    const int tw = gfx_text_w(label, NAME_SCALE);
    gfx_draw_text(x + (w - tw) / 2, (TAB_H - GFX_GLYPH_H(NAME_SCALE)) / 2, label,
                  NAME_SCALE, w - 16,
                  present ? (active ? C_TEXT : C_DIM) : C_DISABLED);
}

/* A folder, drawn the same way ui.c draws the one on the transport bar so
 * the button and the rows it produces are visibly the same idea. */
static void draw_folder_icon(int cx, int cy, uint16_t c)
{
    gfx_fill_rect(cx - 20, cy - 14, 16, 5, c);
    gfx_fill_rect(cx - 20, cy - 9, 40, 25, c);
    gfx_fill_rect(cx - 16, cy - 5, 32, 17, C_ROW);
}

/* A note: stem and head. Two rectangles and a circle is enough to read as
 * "this one is a track" at a glance, which is all the icon is for. */
static void draw_note_icon(int cx, int cy, uint16_t c)
{
    gfx_fill_rect(cx + 6, cy - 16, 4, 24, c);
    gfx_fill_rect(cx + 6, cy - 16, 14, 5, c);
    gfx_fill_circle(cx + 1, cy + 9, 7, c);
}

/*
 * The track being heard, as told by the player. See browser_set_playing().
 *
 * Held here rather than asked for, because the answer is not derivable
 * on this side: playlist_current() is where the DECODER is, and at a
 * boundary that is twenty seconds ahead of the speaker. 0513 dirtied
 * the list when that changed, which worked and was too early by exactly
 * one ring.
 */
static char s_playing[512];

void browser_set_playing(const char *path)
{
    const char *p = (path && *path) ? path : "";
    if (strcmp(s_playing, p) == 0) return;

    snprintf(s_playing, sizeof(s_playing), "%s", p);

    /* The redraw goes with it. Every other thing that moves the marker
     * is a press, which dirties the list on its own; this one arrives
     * from another task and has nothing else to ride on. */
    s_dirty = true;
}

/* Is this row the track that is playing? Compared as a whole path: a
 * same-named track in a different folder is a different track, and on a
 * card full of "01 Intro.mp3" that is not a hypothetical. */
static bool is_current(const char *name)
{
    const char *path = s_playing[0] ? s_playing : NULL;
    if (!path || !s_dir[0]) return false;

    char full[512];
    /* A name too long to join is a name that cannot be the current track
     * either, since the current track was opened through the same join. */
    if (!storage_join_path(full, sizeof(full), s_dir, name)) return false;
    return strcmp(full, path) == 0;
}

static void draw_button(int x, int y, int w, int h, const char *label, bool on)
{
    gfx_fill_rect(x, y, w, h, on ? C_BTN : C_TAB_OFF);
    const int tw = gfx_text_w(label, LABEL_SCALE);
    gfx_draw_text(x + (w - tw) / 2, y + (h - GFX_GLYPH_H(LABEL_SCALE)) / 2, label,
                  LABEL_SCALE, w - 8, on ? C_TEXT : C_DISABLED);
}

static const char *order_label(void)
{
    switch (s_order) {
    case PLAY_ORDER_ONE:     return "ONE";
    case PLAY_ORDER_SHUFFLE: return "RND";
    default:                 return "ALL";
    }
}

/* Six buttons across the bottom, equal width. The arithmetic is done from
 * the panel width rather than written out, because a 720 px panel divides
 * evenly and nothing here should assume that twice. */
#define FOOT_BUTTONS (6)

static void foot_box(int i, int *x, int *w)
{
    const int bw = gfx_w() / FOOT_BUTTONS;
    *x = i * bw;
    *w = (i == FOOT_BUTTONS - 1) ? (gfx_w() - *x) : bw;
}

void browser_draw(void)
{
    if (!s_open) return;

    /* A card going in or a drive coming out while the chooser is up has
     * to be visible without a touch, so the generation counter is the
     * other thing that dirties the screen. */
    const uint32_t gen = storage_generation();
    if (gen != s_seen_generation) {
        s_seen_generation = gen;
        s_dirty = true;
        /* The tab stays where the user put it. Jumping to whatever else
         * is mounted would undo the tap that powered this port about a
         * second before the drive it was waiting for turned up. */
        if (!storage_present(s_tab)) {
            if (s_dir[0]) {                 /* it was there and went away */
                entries_free();
                s_dir[0] = '\0';
            } else {
                /*
                 * Never landed anywhere, so there is no tap to respect.
                 *
                 * This is the boot case: the chooser comes up before
                 * anything is mounted, defaults to the SD tab, and with
                 * no card there is nothing behind it. A drive turning
                 * up a second later left the user looking at an empty
                 * SD listing with their music one untapped tab away.
                 *
                 * Adopting the volume that just appeared is not
                 * overriding a choice, because none has been made.
                 */
                for (int i = 0; i < STORAGE_COUNT; i++) {
                    if (!storage_present((storage_id_t)i)) continue;
                    s_tab = (storage_id_t)i;
                    load_dir(tab_root());
                    break;
                }
            }
        } else if (s_count == 0 || !s_dir[0]) {
            load_dir(s_dir[0] ? s_dir : tab_root());
        }
    }

    if (!s_dirty) return;
    s_dirty = false;

    const int w = gfx_w(), h = gfx_h();
    gfx_fill_rect(0, 0, w, h, C_BG);

    draw_tab(STORAGE_SD, 0, w / 2);
    draw_tab(STORAGE_USB, w / 2, w - w / 2);

    /* Current directory, tail kept -- the end of the path is the part
     * that says where you are. With nothing mounted it says why, because
     * "no drive" and "no power" are the difference between waiting and
     * tapping. */
    /*
     * One line normally, two when something is being hidden -- and the
     * second line is not optional politeness. A list quietly showing
     * names that are not the names on the card is a list you cannot
     * trust; saying what came off the front makes it an abbreviation
     * rather than a discrepancy.
     *
     * Both fit in PATH_H at LABEL_SCALE without moving LIST_TOP, so the
     * list does not lose a row on folders that happen to be tidy.
     */
    if (s_prefix_len) {
        const int gh = GFX_GLYPH_H(LABEL_SCALE);
        const int pad = (PATH_H - 2 * gh) / 3;
        gfx_draw_text_tail(16, TAB_H + pad, status_line(), LABEL_SCALE,
                           w - 32, C_DIM);

        char note[160];
        snprintf(note, sizeof(note), "all start with  %s", s_prefix);
        /* Head kept, not tail: the front of the prefix is what identifies
         * it, and it is the front that the rows are missing. */
        gfx_draw_text(16, TAB_H + 2 * pad + gh, note, LABEL_SCALE,
                      w - 32, C_FAINT);
    } else {
        gfx_draw_text_tail(16, TAB_H + (PATH_H - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                           status_line(), LABEL_SCALE, w - 32, C_DIM);
    }
    gfx_fill_rect(0, LIST_TOP - 2, w, 2, C_RULE);

    const int rows = rows_visible();
    for (int r = 0; r < rows; r++) {
        const int i = s_top + r;
        const int y = LIST_TOP + r * ROW_H;
        if (i >= s_count) {
            gfx_fill_rect(0, y, w, ROW_H, C_BG);
            continue;
        }
        gfx_fill_rect(0, y, w, ROW_H, (r & 1) ? C_ROW_ALT : C_ROW);

        const bool playing = !s_entries[i].is_dir && is_current(s_entries[i].name);

        if (s_entries[i].is_dir) {
            draw_folder_icon(48, y + ROW_H / 2, C_DIM);
        } else {
            draw_note_icon(48, y + ROW_H / 2, playing ? C_ACCENT : C_DIM);
        }
        /* Files only. See find_common_prefix() -- the name itself is
         * untouched; this is where the elision happens and the only
         * place it happens. */
        const char *label = s_entries[i].name
                          + (s_entries[i].is_dir ? 0 : s_prefix_len);
        gfx_draw_text(96, y + (ROW_H - GFX_GLYPH_H(NAME_SCALE)) / 2, label,
                      NAME_SCALE, w - 112 - SCROLL_W, playing ? C_ACCENT : C_TEXT);
    }

    /*
     * Scroll position, as a bar down the right edge -- and, since 0606,
     * the way to change it. A number of pages would need a font; a bar
     * says the same thing in sixteen pixels and can be dragged.
     *
     * The call IS the test. s_count > rows is the same question
     * scroll_geom() already answers, and asking it separately while
     * discarding the answer is what let the outputs be read on a path
     * the compiler could not prove they had been written on. One
     * condition, and it is the one that fills the variables.
     */
    int track_y, track_h, bar_h, max_top;
    if (scroll_geom(&track_y, &track_h, &bar_h, &max_top)) {
        const int bar_y = track_y + (max_top > 0 ? ((track_h - bar_h) * s_top) / max_top : 0);

        /*
         * Wider than it was, because it is a control now rather than a
         * readout. Eight pixels is legible; it is not something a finger
         * aims at. SCROLL_W is still narrow enough to leave the row taps
         * the whole of the rest of the width.
         */
        gfx_fill_rect(w - SCROLL_W, track_y, SCROLL_W, track_h, C_ROW);
        gfx_fill_rect(w - SCROLL_W, bar_y, SCROLL_W, bar_h,
                      s_scroll_drag ? C_TEXT : C_DIM);
    }

    const int fy = h - FOOT_H;
    gfx_fill_rect(0, fy, w, FOOT_H, C_BG);
    gfx_fill_rect(0, fy, w, 2, C_RULE);

    int bx, bw;
    foot_box(0, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, "UP",   !at_root() && s_dir[0]);
    foot_box(1, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, "FLDR", s_dir[0] != '\0');
    foot_box(2, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, "UP^",  s_top > 0);
    foot_box(3, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, "DN",   s_top + rows < s_count);
    foot_box(4, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, order_label(), true);
    foot_box(5, &bx, &bw); draw_button(bx + 4, fy + 8, bw - 8, FOOT_H - 16, "X",    true);

    gfx_blit(0, h);
}

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

browser_result_t browser_touch(bool down, int x, int y)
{
    browser_result_t res = { BROWSER_NONE, NULL };
    const bool tapped = down && !s_was_down;
    s_was_down = down;

    if (!s_open) return res;

    const int w = gfx_w(), h = gfx_h();
    const int rows = rows_visible();

    /*
     * The scrollbar, before anything else and outside the tapped test.
     *
     * Outside it because a drag is a sequence of downs with one edge at
     * the front, and everything else in this function wants only that
     * edge. Before the rows because the grab zone overlaps them: a press
     * inside it is a scroll, not a file, and a press that starts a drag
     * must not also open whatever it happened to land on.
     */
    if (s_scroll_drag || (tapped && x >= w - SCROLL_HIT_W)) {
        int track_y, track_h, bar_h, max_top;
        if (down && scroll_geom(&track_y, &track_h, &bar_h, &max_top) &&
            y >= track_y && y < track_y + track_h) {

            /*
             * The thumb centres on the finger rather than keeping the
             * offset it was grabbed at.
             *
             * Grab-offset is the desktop behaviour and it is the right
             * one for a mouse, where the pointer is a pixel and the
             * thumb is visible under it. Here the thumb is under a
             * fingertip that covers it entirely, so preserving an offset
             * preserves something nobody can see, and a press on the
             * track above the thumb would do nothing at all instead of
             * going there. Centring makes press-anywhere and drag the
             * same gesture.
             */
            const int span = track_h - bar_h;
            int pos = y - track_y - bar_h / 2;
            if (pos < 0) pos = 0;
            if (pos > span) pos = span;

            const int want = span > 0 ? (pos * max_top + span / 2) / span : 0;
            if (want != s_top) {
                s_top = want;
                s_dirty = true;
            }
            /* Set after the move, so the first frame of a drag already
             * draws the thumb in its held colour. */
            s_scroll_drag = true;
            return res;
        }

        if (!down && s_scroll_drag) {
            s_scroll_drag = false;
            s_dirty = true;          /* back to the resting colour */
            ESP_LOGI(TAG, "scrolled to row %d of %d", s_top, s_count);
            return res;
        }
    }

    if (!tapped) return res;

    if (y < TAB_H) {
        const storage_id_t want = (x < w / 2) ? STORAGE_SD : STORAGE_USB;
        ESP_LOGI(TAG, "button: tab %s", want == STORAGE_SD ? "SD" : "USB");
        select_tab(want);
        return res;
    }

    if (y >= h - FOOT_H) {
        const int bw = w / FOOT_BUTTONS;
        int which = x / bw;
        if (which >= FOOT_BUTTONS) which = FOOT_BUTTONS - 1;

        /* One line per footer press, named, before the switch acts on
         * it -- so a press that turns out to do nothing (page up at the
         * top of the list, play-folder on an empty folder) still shows
         * up as having been received. A button that is working and a
         * button that is not both look like silence otherwise. */
        static const char *const foot_name[FOOT_BUTTONS] = {
            "up", "play folder", "page up", "page down", "order", "cancel"
        };
        ESP_LOGI(TAG, "button: %s", foot_name[which]);

        switch (which) {
        case 0:
            go_up();
            break;
        case 1:
            if (s_dir[0]) {
                snprintf(s_result, sizeof(s_result), "%s", s_dir);
                res.kind = BROWSER_PLAY_FOLDER;
                res.path = s_result;
            }
            break;
        case 2:
            if (s_top > 0) {
                s_top -= rows;
                if (s_top < 0) s_top = 0;
                s_dirty = true;
            }
            break;
        case 3:
            if (s_top + rows < s_count) {
                s_top += rows;
                s_dirty = true;
            }
            break;
        case 4:
            s_order = (s_order == PLAY_ORDER_ONE)     ? PLAY_ORDER_ALL
                    : (s_order == PLAY_ORDER_ALL)     ? PLAY_ORDER_SHUFFLE
                                                      : PLAY_ORDER_ONE;
            ESP_LOGI(TAG, "play order now %s", order_label());
            s_dirty = true;
            break;
        default:
            res.kind = BROWSER_CANCELLED;
            break;
        }
        return res;
    }

    const int r = (y - LIST_TOP) / ROW_H;
    if (r < 0 || r >= rows) return res;
    const int i = s_top + r;
    if (i >= s_count) return res;

    ESP_LOGI(TAG, "button: row %d (%s) \"%s\"", i,
             s_entries[i].is_dir ? "dir" : "file", s_entries[i].name);

    if (s_entries[i].is_dir) {
        char sub[512];
        if (!storage_join_path(sub, sizeof(sub), s_dir, s_entries[i].name)) {
            ESP_LOGW(TAG, "path too long: %s/%s", s_dir, s_entries[i].name);
            return res;
        }
        load_dir(sub);
        return res;
    }

    if (!storage_join_path(s_result, sizeof(s_result), s_dir, s_entries[i].name)) {
        ESP_LOGW(TAG, "path too long: %s/%s", s_dir, s_entries[i].name);
        return res;
    }
    res.kind = BROWSER_PLAY_FILE;
    res.path = s_result;
    return res;
}
