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

/* Layout. Sized for the same 294 PPI the transport bar is: a 88 px row is
 * about 7.5 mm, which is a comfortable thumb target, and the 27 px text in
 * it is the same height as the artist line on the player. */
#define TAB_H       (96)
#define PATH_H      (60)
#define LIST_TOP    (TAB_H + PATH_H)
#define FOOT_H      (120)
#define ROW_H       (88)
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

static play_order_t s_order = PLAY_ORDER_ALL;
static uint32_t s_seen_generation = UINT32_MAX;

play_order_t browser_order(void) { return s_order; }
bool browser_is_open(void) { return s_open; }

static int rows_visible(void)
{
    return (gfx_h() - FOOT_H - LIST_TOP) / ROW_H;
}

/* ------------------------------------------------------------------ */
/* Directory listing                                                   */
/* ------------------------------------------------------------------ */

static void entries_free(void)
{
    for (int i = 0; i < s_count; i++) free(s_entries[i].name);
    s_count = 0;
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
        if (!is_dir && !decoder_supports(e->d_name)) continue;
        if (e->d_name[0] == '.') continue;      /* . .. and dotfiles */

        s_entries[s_count].name = strdup(e->d_name);
        if (!s_entries[s_count].name) break;
        s_entries[s_count].is_dir = is_dir;
        s_count++;
    }
    closedir(d);

    qsort(s_entries, (size_t)s_count, sizeof(entry_t), cmp_entry);
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
 * Rule 2: selecting the USB tab is what powers the port.
 *
 * So an absent volume's tab is still selectable, which is the one place
 * this differs from a normal tab strip. A greyed tab that ignored the tap
 * would leave the port dark with no way to ask for it -- the grey has to
 * mean "nothing here yet", not "not a button".
 */
static void select_tab(storage_id_t id)
{
    const bool same = (id == s_tab);
    s_tab = id;

    if (id == STORAGE_USB && !storage_usb_powered()) {
        storage_usb_enable();
        entries_free();
        s_dir[0] = '\0';
        s_dirty = true;
        return;
    }

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
        return storage_usb_powered() ? "USB port on - waiting for a drive"
                                     : "tap again to power the USB port";
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

/* Is this row the track that is playing? Compared as a whole path: a
 * same-named track in a different folder is a different track, and on a
 * card full of "01 Intro.mp3" that is not a hypothetical. */
static bool is_current(const char *name)
{
    const int cur = playlist_current();
    if (cur < 0) return false;
    const char *path = playlist_path(cur);
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
    gfx_draw_text_tail(16, TAB_H + (PATH_H - GFX_GLYPH_H(LABEL_SCALE)) / 2,
                       status_line(), LABEL_SCALE, w - 32, C_DIM);
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
        gfx_draw_text(96, y + (ROW_H - GFX_GLYPH_H(NAME_SCALE)) / 2, s_entries[i].name,
                      NAME_SCALE, w - 112, playing ? C_ACCENT : C_TEXT);
    }

    /* Scroll position, as a bar down the right edge. A number of pages
     * would need a font; a bar says the same thing in eight pixels. */
    if (s_count > rows) {
        const int track_h = rows * ROW_H;
        int bar_h = (track_h * rows) / s_count;
        if (bar_h < 24) bar_h = 24;
        const int span = track_h - bar_h;
        const int max_top = s_count - rows;
        const int bar_y = LIST_TOP + (max_top > 0 ? (span * s_top) / max_top : 0);
        gfx_fill_rect(w - 8, LIST_TOP, 8, track_h, C_ROW);
        gfx_fill_rect(w - 8, bar_y, 8, bar_h, C_DIM);
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

    if (!s_open || !tapped) return res;

    const int w = gfx_w(), h = gfx_h();
    const int rows = rows_visible();

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
